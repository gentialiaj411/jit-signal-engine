# Pinned-host multi-threaded multi-symbol scaling

- Canonical host: `wsl2-ultra9-275hx-2026-05` — see `bench/PINNED_HOST.md`
- Git commit: `659f180d1a40f943f0a4ad4e2b543fde24de7a9d`
- Symbols: 10,000; sustained run: 60s per thread count
- Pin cores: `2` .. `17` (one thread per core)

## Throughput and scaling

| Threads | Symbols/sec | Scaling vs 1T | Efficiency | Pass p50 (ns) | Pass p99 (ns) |
|---:|---:|---:|---:|---:|---:|
| 1 | 831599 | 1.00x | 100.0% | 12136200 | 17355900 |
| 2 | 1423410 | 1.71x | 85.6% | 7271520 | 9417060 |
| 4 | 2353070 | 2.83x | 70.7% | 4205860 | 6021300 |
| 8 | 3664630 | 4.41x | 55.1% | 2636540 | 4288100 |
| 16 | 4730650 | 5.69x | 35.6% | 2024160 | 3618930 |

At **16** threads: **5.69x** throughput vs 1 thread, **35.6%** parallel efficiency.

## Reproduce

```bash
bash bench/run_pinned_multithread_scaling.sh build-wsl
```

Raw log: `bench/results/multithread_scaling_run.log`. JSON: `bench/results/multithread_scaling.json`.
