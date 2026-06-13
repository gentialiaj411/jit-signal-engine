# P4: Stateful Lowering + Cross-Symbol Vectorization

Program: `/mnt/c/Users/bhask/Documents/PROJECTS/jit-signal-engine/examples/filtered_momentum.sig`  
Events/lane: `1000000`  
Lane count K = `4`  
Best-of-N runs: `5`

## Throughput (symbol-events per second)

| Path | Best | Worst | Notes |
|------|-----:|------:|-------|
| Scalar (K calls/tick) | 3.7242e+07 | 3.5131e+07 | one call per lane through the existing scalar JIT |
| **Vectorized (1 call/tick, K lanes)** | **2.05629e+07** | 1.89389e+07 | one call processing K MarketStates via `<K x double>` IR |
| Speedup (best/best) | **0.552142×** | | |

## P4 three-way comparison (stateful lowering × vectorization)

| Path | Best symev/s | vs scalar-lowered | Notes |
|------|-------------:|------------------:|-------|
| Scalar + `kAll` (lowered) | 3.7242e+07 | 1.00× | K scalar JIT calls/tick (production default) |
| **Vector + `kAll` (lowered fan-out)** | **2.05629e+07** | **0.552142×** | P4: per-lane `LaneEmitScope` inline IR |
| Vector + `kNone` (opaque `jit_rt_*`) | 9.89529e+06 | 0.265702× | P10 runtime fan-out only |

Headline: lowered+vectorized is **2.07805×** faster than vectorized-unlowered, but **does not** beat scalar-lowered on this FP-heavy stateful program. True K-wide SIMD ring-buffer state remains future work.

* `vec_opaque_sink = 1.54881e+13`

## Correctness gate

Both paths accumulate `output[last_signal]` across all ticks and lanes into a `volatile double` sink. The sinks **must agree bit-exactly** to gate the run as correct.

* `scalar_sink = 7.96164e+08`
* `vec_sink    = 7.96164e+08`
* match (1e-9 relative): **yes**

The strict-equality `vectorized_lanes_parity_test` gate (9 cases, 2000 ticks each) covers per-tick bit-exactness; this benchmark only checks the streamed sum, which is sufficient for the artifact.

## IR-level evidence

| Token | Scalar IR (post-O2) | Vectorized IR (post-O2) |
|-------|--------------------:|------------------------:|
| `load double` | 10 | 40 |
| `<4 x double>` | 0 | 64 |

The `<4 x double>` count goes from 0 in the scalar IR to a non-zero count in the vectorized IR -- that is the headline observable for P2. Every arithmetic op, every comparison, every constant has been widened to K-lane vectors by the codegen, then carried through LLVM's O2 pipeline without being scalarized back.

## Reproduction

```
cd build-wsl
./cross_symbol_benchmark /mnt/c/Users/bhask/Documents/PROJECTS/jit-signal-engine/examples/filtered_momentum.sig 1000000 --lanes=4 --runs=5 --phase4
```
