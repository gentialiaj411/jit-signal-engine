# Pinned-host multi-threaded multi-symbol scaling (ecores-only)

- Canonical host: `wsl2-ultra9-275hx-2026-05` — see `bench/PINNED_HOST.md`
- Git commit: `659f180d1a40f943f0a4ad4e2b543fde24de7a9d`
- Symbols: 10,000; sustained run: 60s per thread count
- Pin cores (host-detected): `8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23` (`taskset -c 8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23`; workers use `--pin-core-base 8` + thread index)
- Thread counts: `1,2,4,8,16`

## Throughput and scaling

| Threads | Symbols/sec | Scaling vs 1T | Efficiency | Pass p50 (ns) | Pass p99 (ns) |
|---:|---:|---:|---:|---:|---:|
| 1 | 2006230 | 1.00x | 100.0% | 4867820 | 7223080 |
| 2 | 2684340 | 1.34x | 66.9% | 2917120 | 12890800 |
| 4 | 1785340 | 0.89x | 22.2% | 3819390 | 18864300 |
| 8 | 7990880 | 3.98x | 49.8% | 782021 | 8993240 |
| 16 | 13051000 | 6.51x | 40.7% | 714678 | 1538350 |

At **16** threads: **6.51x** throughput vs 1 thread, **40.7%** parallel efficiency (cores `8`–`23`).

## Reproduce

```bash
bash bench/run_pinned_multithread_scaling.sh build-wsl \
  --cores 8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23 --thread-counts 1,2,4,8,16 \
  --out-json bench/results/multithread_scaling_ecores.json \
  --out-md bench/results/multithread_scaling_ecores.md --label ecores-only
```

Raw log: `bench/results/multithread_scaling_ecores_run.log`. JSON: `bench/results/multithread_scaling_ecores.json`.
