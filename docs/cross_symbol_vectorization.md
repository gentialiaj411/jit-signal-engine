# P2 — Cross-Symbol JIT Vectorization

**Status:** Landed (stateless widening P2; stateful per-lane fan-out P10; lowered stateful P4). Stateless speedup: `bench/results/avx2_speedup/` (~2.6× vs scalar JIT on `stateless_compute_heavy.sig` at K=4). Stateful+P4: `bench/results/stateful_vec_lowering_speedup.md`. See `docs/cross_symbol_vectorization_stateful.md`.
**Predecessors:** P0–P2 stateful-op IR lowering (production default `kAll`) and P1 tiered warm-loop specialization. P4 composes lowering with vector mode via per-lane `LaneEmitScope`.

## What problem this solves

Until P2, the JIT processed one symbol per call: the entry signature was

```c
void program(const MarketState*, MultiSymbolSignalContext*, uint32_t symbol_id, double* outputs);
```

To evaluate a signal across N symbols you called `program` N times. The compiled IR was scalar throughout, so the only way to amortize call/load overhead and increase ILP was task-level parallelism (which P-2 already exploits via per-thread arenas).

P2 adds the orthogonal **data-parallel** axis: a vectorized entry function processes K symbols at a time by widening every IR `double` to `<K × double>`:

```c
void program_vec(const MarketState* const* per_lane_market,
                 MultiSymbolSignalContext* arena,
                 uint32_t base_symbol,
                 double* outputs);
```

* `per_lane_market` points to K consecutive `MarketState*` pointers (one per lane).
* `outputs` is laid out signal-major / lane-minor: `outputs[signal_index * K + lane]`. A `<K × double>` vector store of a single signal writes K contiguous doubles.

K is one of `{2, 4, 8}`. On AVX2 (`<4 × double>` = one ymm), K = 4 is the canonical choice.

This is "data-parallel JIT" in the same lineage as Halide's vectorize-across-pixels, gather-style SIMD in databases, and the SoA passes in V8 / Cranelift. The headline is that **one compiled function processes K symbols**, not K iterations of the same scalar code.

## MVP scope

* Supported expressions: constants, unary, binary arithmetic, comparisons, boolean logic, `if`/`then`/`else` (as lane-wise `select`), market loads (`bid` / `ask` / `mid` / `spread`), `abs` / `sqrt` / `log`.
* **Stateful ops (P10):** supported via per-lane scalarized fan-out (`EmitScalarizedFanOut` in `jit_compiler.cpp`). Each lane calls the existing scalar `jit_rt_*` helper against its own `SignalContext` slot. Throughput on stateful-heavy programs is ~0.9–1.0× scalar (composition wins come from threading — see `bench/results/vec_thread_composition/`).

* **P4 (lowered stateful):** default `kAll` lowering composes with vector mode via per-lane `LaneEmitScope` inline IR fan-out. Parity: `vectorized_lanes_parity_test` + `vectorized_stateful_parity_test` with `kAll`. Perf: `bench/results/stateful_vec_lowering_speedup.md` — vec+lowered **2.13×** vs vec+opaque, **0.57×** vs scalar-lowered on `filtered_momentum` (K=4). True K-wide SIMD ring state is not implemented.

* **P11:** duplicate stateful subtrees in one signal body share `node_id` when the first occurrence is unconditional; required for correct `if` + `rolling_std` after inlining (`stateful_subtree_dedup_test`).

## How the codegen works

The vectorized codegen reuses `EmitExpr`, branching on `cg.lane_count` at the few sites where scalar-vs-vector semantics differ:

1. **`NumberLiteral`** — broadcast to a splat constant: `<K × constant>`.
2. **Binary arithmetic** — LLVM's `CreateFAdd` / `CreateFSub` / etc. are type-polymorphic, so they automatically operate on `<K × double>` when their operands are vectors.
3. **Comparisons** — `CreateFCmpOGT` etc. produce `<K × i1>`; the trailing `UIToFP` widens to `<K × double>` using the vectorized result type.
4. **Intrinsics (`abs`, `sqrt`, `log`)** — pick the `{<K × double>}` overload of the intrinsic via `Intrinsic::getDeclaration(&module, kind, {VecTy(cg)})`.
5. **`Conditional` (if/then/else)** — lane-wise `select`. Both branches evaluate; P11 dedup ensures stateful ops referenced in multiple branches share one `node_id` when the first use is unconditional (see `stateful_subtree_dedup_test`).
6. **Market load (`bid` / `ask` / `mid` / `spread`)** — built from K independent scalar reads:
   ```
   for lane in 0..K:
     mst_lane = load ptr, per_lane_market_arg[lane]
     scalar   = load double, mst_lane.instruments[sym_id].field
     result   = insertelement <K × double> result, double scalar, lane
   ```
   LLVM may further optimize the K independent loads into a gather instruction on AVX2; either way, the wider lanes amortize the ALU work after the load.
7. **Stateful ops (P10/P4)** — `EmitScalarizedFanOut`: K scalar extracts per lane, per-lane state update, K inserts back into `<K × double>`. With `kAll` lowering (P4), each lane emits inline lowered IR via `LaneEmitScope` instead of opaque `jit_rt_*` calls.

The vectorized function's output store is forced to `alignof(double)` so that LLVM emits the unaligned form (`movupd` on x86). This is required because the caller's `outputs` buffer comes from `std::vector<double>` or similar, which only guarantees 8-byte alignment; the default `<4 × double>` natural alignment would force a 32-byte-aligned `movapd` that segfaults on caller-supplied buffers.

## Layout invariants that gate correctness

The vectorized market load assumes the IR struct definition of `InstrumentState` matches the C++ struct exactly. Specifically:

* `sizeof(InstrumentState) == 64` (the C++ struct is `alignas(64)`, padding the natural 40-byte layout to a cache line).
* The IR struct includes a trailing `[24 × i8]` padding field so GEP arithmetic strides match (`instruments[i]` lives at offset `i × 64`, not `i × 40`).

These are gated by `static_assert`s in `runtime.cpp`. If `InstrumentState` changes layout, the build breaks until the IR struct definition in `EmitMarketFieldLoad` is updated in lockstep.

This invariant existed in the scalar JIT too but was latent — the previous codegen used `i × 40` for the IR stride. Single-ticker programs (every existing canonical signal) accessed only `instruments[0]`, where both strides agree at offset 0. The bug surfaced when P2 first tried a two-ticker stateless signal (`mid(AAPL) - mid(MSFT)`) and produced wildly wrong sinks. The fix landed alongside P2 and benefits scalar multi-ticker programs as well.

## Evidence

**Canonical speedup artifacts:** `bench/results/avx2_speedup/` (pinned P-core, best-of-15, 2M events/lane).

| Program | Speedup (vec vs scalar JIT, K=4) | Notes |
|---|---|---|
| `stateless_compute_heavy.sig` | **~2.6×** | FP-heavy; 122 `<4 x double>` tokens in vec IR |
| `stateless_heavy.sig` | **~1.4×** | Memory-bound; 58 vector tokens |

Older run (500k events, best-of-7): `bench/results/cross_symbol_vectorization/cross_symbol_vectorization.md` (~1.11× on `stateless_heavy.sig`).

Reproduce:

```
cd build-wsl
taskset -c 2 ./cross_symbol_benchmark ../examples/stateless_compute_heavy.sig 2000000 --lanes=4 --runs=15 \
  --md=../bench/results/avx2_speedup/stateless_compute_heavy.md
```

Speedup is program-dependent: arithmetic intensity per market load determines how much the vectorized ALU work outweighs K independent gathers.

## Parity gate

`test/vectorized_lanes_parity_test.cpp`: stateless positive cases (including `compute_heavy_sqrt_chain` mirroring `stateless_compute_heavy.sig`) plus P4 positive cases that `CompileProgramVectorized` succeeds with `kSma`/`kEma`/`kLag`/`kAll`.

Stateful programs use `test/vectorized_stateful_parity_test.cpp` (per-lane fan-out, ~1e-9 tolerance).

Bit-equality is the standard for stateless positive cases; P11 dedup is gated by `stateful_subtree_dedup_test.cpp`.

## API summary

```cpp
// in src/jit_compiler.h
class JitCompiler {
  // ...
  using ProgramFnVec = void (*)(
      const MarketState* const* per_lane_market,
      MultiSymbolSignalContext* arena,
      std::uint32_t base_symbol,
      double* outputs);

  bool CompileProgramVectorized(
      const std::vector<SignalDef>& signals,
      const SymbolTable& symbols,
      unsigned lane_count);

  ProgramFnVec GetProgramVectorizedFunction() const;
  unsigned VectorizedLaneCount() const;
};
```

## Future work

* **Gather lowering for market loads** — `llvm.masked.gather` vs K scalar loads + insertelement.
* **Mixed K per program** — pick lane count from compile-time properties.
* **Lower remaining stateful ops into IR** where profiling justifies it (see `runtime_call_profile` after Welford).
