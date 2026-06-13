# P2: Cross-Symbol Vectorization Evidence

Program: `../examples/filtered_momentum.sig`  
Events/lane: `500000`  
Lane count K = `4`  
Best-of-N runs: `5`

## Throughput (symbol-events per second)

| Path | Best | Worst | Notes |
|------|-----:|------:|-------|
| Scalar (K calls/tick) | 1.2828e+07 | 1.18869e+07 | one call per lane through the existing scalar JIT |
| **Vectorized (1 call/tick, K lanes)** | **9.53971e+06** | 8.99386e+06 | one call processing K MarketStates via `<K x double>` IR |
| Speedup (best/best) | **0.743661×** | | |

## Correctness gate

Both paths accumulate `output[last_signal]` across all ticks and lanes into a `volatile double` sink. The sinks **must agree bit-exactly** to gate the run as correct.

* `scalar_sink = 7.16138e+12`
* `vec_sink    = 7.16138e+12`
* match (1e-9 relative): **yes**

The strict-equality `vectorized_lanes_parity_test` gate (9 cases, 2000 ticks each) covers per-tick bit-exactness; this benchmark only checks the streamed sum, which is sufficient for the artifact.

## IR-level evidence

| Token | Scalar IR (post-O2) | Vectorized IR (post-O2) |
|-------|--------------------:|------------------------:|
| `load double` | 2 | 8 |
| `<4 x double>` | 0 | 64 |

The `<4 x double>` count goes from 0 in the scalar IR to a non-zero count in the vectorized IR -- that is the headline observable for P2. Every arithmetic op, every comparison, every constant has been widened to K-lane vectors by the codegen, then carried through LLVM's O2 pipeline without being scalarized back.

## Reproduction

```
cd build-wsl
./cross_symbol_benchmark ../examples/filtered_momentum.sig 500000 --lanes=4 --runs=5
```
