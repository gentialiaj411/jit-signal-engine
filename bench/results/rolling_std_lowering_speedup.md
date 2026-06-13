# Rolling Std Lowering Speedup

Program: `../examples/filtered_momentum.sig`  
Mode: `signal_benchmark --all-signals --pin-core 2 --measure-runs 5 --lower-stateful=all`

## Result

| Metric | Value |
|---|---:|
| Interpreter throughput median | `2.03124e+06` events/s |
| JIT throughput median | `3.2372e+07` events/s |
| Speedup (median) | `15.601x` |
| Speedup (p99) | `16.1391x` |
| JIT p99 latency (median run) | `35 ns` |

## Raw summary

```
speedup_median=15.601
speedup_p99=16.1391
jit_throughput_median=3.2372e+07
interp_throughput_median=2.03124e+06
jit_lat_ns_p99_median=35
interp_lat_ns_p99_median=788
```
