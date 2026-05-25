# P2: Cross-Symbol Vectorization Evidence

Program: `../examples/stateless_heavy.sig`  
Events/lane: `500000`  
Lane count K = `4`  
Best-of-N runs: `7`

## Throughput (symbol-events per second)

| Path | Best | Worst | Notes |
|------|-----:|------:|-------|
| Scalar (K calls/tick) | 2.4668e+08 | 2.18905e+08 | one call per lane through the existing scalar JIT |
| **Vectorized (1 call/tick, K lanes)** | **2.73661e+08** | 1.97886e+08 | one call processing K MarketStates via `<K x double>` IR |
| Speedup (best/best) | **1.10938×** | | |

## Correctness gate

Both paths accumulate `output[last_signal]` across all ticks and lanes into a `volatile double` sink. The sinks **must agree bit-exactly** to gate the run as correct.

* `scalar_sink = -3.19679e+09`
* `vec_sink    = -3.19679e+09`
* match: **yes**

The strict-equality `vectorized_lanes_parity_test` gate (9 cases, 2000 ticks each) covers per-tick bit-exactness; this benchmark only checks the streamed sum, which is sufficient for the artifact.

## IR-level evidence

| Token | Scalar IR (post-O2) | Vectorized IR (post-O2) |
|-------|--------------------:|------------------------:|
| `load double` | 4 | 16 |
| `<4 x double>` | 0 | 58 |

The `<4 x double>` count goes from 0 in the scalar IR to a non-zero count in the vectorized IR -- that is the headline observable for P2. Every arithmetic op, every comparison, every constant has been widened to K-lane vectors by the codegen, then carried through LLVM's O2 pipeline without being scalarized back.

## Reproduction

```
cd build-wsl
./cross_symbol_benchmark ../examples/stateless_heavy.sig 500000 --lanes=4 --runs=7
```
