# Pinned-host JIT vs interpreter speedup

- Canonical host: `wsl2-ultra9-275hx-2026-05` — see `bench/PINNED_HOST.md`
- Git commit: `659f180d1a40f943f0a4ad4e2b543fde24de7a9d`
- Generated (UTC): 2026-05-24T19:39:18.675936+00:00
- Pin core: 2; events/run: 1,000,000; repeats: 30

## Headline speedups (JIT throughput / interpreter throughput)

| Case | Median | p99 of runs | 95% bootstrap CI (median) |
|---|---:|---:|---|
| Momentum (single signal) | 5.18x | 5.94x | [4.92x, 5.36x] |
| All-signals fused | 2.96x | 7.62x | [2.50x, 3.53x] |

## Reproduce

```bash
bash bench/run_pinned_speedup.sh build-wsl
```

Raw data: `bench/results/pinned_host_speedup.json`.
