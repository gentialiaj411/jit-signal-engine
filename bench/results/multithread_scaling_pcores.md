# Pinned-host multi-threaded multi-symbol scaling (pcores-only)

- Canonical host: `wsl2-ultra9-275hx-2026-05` — see `bench/PINNED_HOST.md`
- Git commit: `659f180d1a40f943f0a4ad4e2b543fde24de7a9d`
- Symbols: 10,000; sustained run: 60s per thread count
- Pin cores (host-detected): `1,2,3,4,5,6,7` (`taskset -c 1,2,3,4,5,6,7`; workers use `--pin-core-base 1` + thread index)
- Thread counts: `1,2,4,6`

## Throughput and scaling

| Threads | Symbols/sec | Scaling vs 1T | Efficiency | Pass p50 (ns) | Pass p99 (ns) |
|---:|---:|---:|---:|---:|---:|
| 1 | 1768850 | 1.00x | 100.0% | 5411870 | 10187400 |
| 2 | 2711940 | 1.53x | 76.7% | 3530420 | 5791600 |
| 4 | 7260540 | 4.10x | 102.6% | 1322310 | 2483220 |
| 6 | 9640490 | 5.45x | 90.8% | 914371 | 2261330 |

At **6** threads: **5.45x** throughput vs 1 thread, **90.8%** parallel efficiency (cores `1`–`6`).

## Reproduce

```bash
bash bench/run_pinned_multithread_scaling.sh build-wsl \
  --cores 1,2,3,4,5,6,7 --thread-counts 1,2,4,6 \
  --out-json bench/results/multithread_scaling_pcores.json \
  --out-md bench/results/multithread_scaling_pcores.md --label pcores-only
```

Raw log: `bench/results/multithread_scaling_pcores_run.log`. JSON: `bench/results/multithread_scaling_pcores.json`.
