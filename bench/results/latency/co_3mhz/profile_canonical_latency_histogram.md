# Latency distribution: `profile_canonical`

Events: 400000  
Warmup: 20000  
Mode: open-loop CO-aware @ 3000000.000000 Hz  
Source: `examples/profile_canonical.sig`  
Methodology: per-call timing via `std::chrono::steady_clock::now()`. The histogram is HdrHistogram-style log-linear with 4 precision bits (~6% bucket width). See [`docs/latency_distribution.md`](../../../docs/latency_distribution.md) for full methodology.

## jit

Total samples: 400000  
Min: 130 ns  
Max: 226040 ns

| Percentile | Latency (ns) |
|---|---:|
| p50 | 196 |
| p75 | 204 |
| p90 | 212 |
| p99 | 92160 |
| p99.9 | 176128 |
| p99.99 | 225280 |
| p99.999 | 225280 |
| max | 226040 |

## Throughput cross-check

| Configuration | Wall time (s) | Throughput (events/s) | Sink |
|---|---:|---:|---:|
| jit | 0.1332 | 3.003e+06 | nan |

## CDF

![CDF](profile_canonical_latency_histogram.svg)
