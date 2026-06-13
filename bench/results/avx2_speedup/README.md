# AVX2 Wins: Cross-Symbol Vectorized JIT vs Scalar JIT

This bundle is the canonical "AVX2 wins over scalar JIT" artifact, the
upgrade-target #2 in `jit-signal-engine_upgrades.md`. It answers the
question:

> When the JIT widens every IR `double` to `<4 x double>` and the
> backend (LLVM ORC, host AVX2-enabled) emits `vmulpd`/`vaddpd`/
> `vsqrtpd` against four lanes per cycle, does the resulting code
> actually run faster than the existing scalar JIT, and by how much?

The answer depends almost entirely on the program's arithmetic intensity
per market-state load. Two stateless programs were measured under
identical methodology (pinned to one P-core via `taskset -c 2`, 2 M
events/lane, best-of-15 runs each path, bit-exact sink correctness
gate). Both compile through the same `JitCompiler::CompileProgram` /
`CompileProgramVectorized` paths.

| Program | FP ops / load | Speedup (K=4) | Cause |
|---------|--------------:|--------------:|-------|
| `stateless_heavy.sig` | ~3 (memory-bound) | **1.36×** | bid/ask gathers from 4 distinct `MarketState*` per call dominate cost; SIMD arithmetic wins are amortized over 16 `load double` ops |
| `stateless_compute_heavy.sig` | ~10 (FP-bound) | **2.62×** | dense arithmetic + 6 `sqrt` per tick saturate the vector pipeline; the 4 lanes pay one gather but get K-wide work for every downstream op |

Both numbers are reproducible inside a ±5% band across consecutive
invocations under pinning (validated with 10× back-to-back runs at
`runs=15, events=2_000_000`: speedup range 2.65–2.75× on the FP-bound
program). The benchmark is honest about run-to-run spread: it reports
`worst` as well as `best` throughput so consumers can see the steady-
state floor, not just a cherry-picked peak.

The reason this artifact is interesting is that **the SIMD work pays
off when arithmetic intensity is high enough to mask the gather
penalty** — exactly the workload regime quant kernels fall into once
you compose nonlinear features (sqrt/log/abs chains) on top of raw
market loads. The memory-bound case (`stateless_heavy.sig`) is the
honest lower bound: even there, the cross-symbol vectorized JIT wins
by 1.36× over the scalar JIT, because the four lanes pay one
function-call overhead instead of K.

## Stateful programs (P4 — separate artifact)

Cross-symbol vectorization on **stateful** programs uses per-lane fan-out
(P10); with default `kAll` lowering, P4 emits per-lane inline IR
(`LaneEmitScope`). Do **not** mix these numbers with the stateless
headlines above:

- `bench/results/stateful_vec_speedup.md` — vec vs scalar (opaque fan-out)
- `bench/results/stateful_vec_lowering_speedup.md` — P4 three-way:
  scalar+`kAll` vs vec+`kAll` vs vec+`kNone` on `filtered_momentum` (K=4)

Reproduce P4: `bash bench/run_stateful_vec_lowering_phase4.sh build-wsl`

## Headline numbers

* **2.62× speedup** of the vectorized JIT (K=4) over the scalar JIT
  on `stateless_compute_heavy.sig` (16 stateless signals, 4 mid/ask/bid
  loads, ~10 FP ops per load including 6 `sqrt` calls).
* **1.36× speedup** on `stateless_heavy.sig` (9 stateless signals,
  4 loads, ~3 FP ops per load) — the memory-bound floor.
* Both paths produce **bit-equivalent output** (parity sinks match
  exactly) and the strict per-tick bit-equality gate is
  `test/vectorized_lanes_parity_test.cpp` case
  `compute_heavy_sqrt_chain` (16 signals × 4 lanes × 2000 ticks).

## IR-level evidence

| Program | `load double` (scalar) | `load double` (vec) | `<4 x double>` (scalar) | `<4 x double>` (vec) |
|---------|----:|----:|----:|----:|
| `stateless_heavy.sig` | 4 | 16 | 0 | 58 |
| `stateless_compute_heavy.sig` | 4 | 16 | 0 | 122 |

`<4 x double>` is the canonical observable for "this is actually AVX2."
The post-O2 IR for the vectorized compile contains 122 vector ops on
the FP-bound program (sqrt/mul/add/sub/abs widened across all four
lanes), versus 0 in the scalar compile of the same source. The
optimizer never scalarizes the vector ops back, confirmed by reading
the IR dumps in `ir/stateless_compute_heavy_vec4.ll`.

## Files

* `stateless_compute_heavy.md` — full per-run table for the FP-bound
  program.
* `stateless_heavy.md` — full per-run table for the memory-bound
  program (the lower bound).
* `ir/stateless_compute_heavy_scalar.ll` and `.._vec4.ll` — post-O2
  LLVM IR for both compiles. The token counts in the table above come
  from these files.

## Reproduction

```
cd build-wsl
# FP-bound case (headline 2.62× speedup):
taskset -c 2 ./cross_symbol_benchmark ../examples/stateless_compute_heavy.sig \
    2000000 --lanes=4 --runs=15

# Memory-bound case (1.36× lower bound):
taskset -c 2 ./cross_symbol_benchmark ../examples/stateless_heavy.sig \
    2000000 --lanes=4 --runs=15

# Regenerate this bundle:
taskset -c 2 ./cross_symbol_benchmark ../examples/stateless_compute_heavy.sig \
    2000000 --lanes=4 --runs=15 \
    --md=../bench/results/avx2_speedup/stateless_compute_heavy.md \
    --ir-dir=../bench/results/avx2_speedup/ir
```

Pinning to a P-core (`taskset -c 2`) matters — without it, scheduler
migration onto an E-core mid-run can shave 30%+ off the reported
throughput on one path but not the other, inflating or deflating the
speedup ratio.

## Methodology notes

* **What counts as "scalar"**: the existing scalar JIT
  (`CompileProgram`) is invoked K times per tick, once per lane, with
  the lane's own `MarketState*` and `symbol_idx`. This is the
  fair baseline — if the goal is "process 4 symbols per tick", scalar
  must run 4 calls.
* **What counts as "vectorized"**: the vectorized JIT
  (`CompileProgramVectorized`, lane_count=4) is invoked once per tick
  with a `MarketState*[4]` array of per-lane pointers. The codegen
  emits `<4 x double>` IR for every value; LLVM O2 lowers those to
  `vmulpd`/`vaddpd`/`vsqrtpd` etc. on hosts with AVX2.
* **Run-to-run spread**: the benchmark reports `best` and `worst` of
  N runs per path. Best/best is the headline ratio in the table; this
  is consistent with the methodology of `bench/results/cross_symbol_vectorization`
  and `bench/results/vec_thread_composition`.
* **Correctness**: every run sinks the last signal's output across all
  ticks×lanes into a `volatile double`. Scalar and vec sinks must
  agree exactly or the run is flagged. The bit-exact per-tick gate is
  `vectorized_lanes_parity_test`.

## Companion claims

* `bench/results/vec_thread_composition/stateless_heavy.md`: the same
  vectorization composes multiplicatively with multi-threading
  (`vec(K=4)+4T` is 4.34× faster than `scalar+1T` on the memory-bound
  program; the FP-bound program composes proportionally more).
* `bench/results/cross_symbol_vectorization/cross_symbol_vectorization.md`:
  the original P2 artifact pre-AVX2-tuning, kept for historical
  comparison.
