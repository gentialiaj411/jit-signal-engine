# Pinned-host JIT vs interpreter speedup

- Canonical host: `wsl2-ultra9-275hx-2026-05` — see `bench/PINNED_HOST.md`
- Git commit: `eb661e0dbefe0e9e77a26df5f99c5d93b62156a0`
- Generated (UTC): 2026-06-10T01:56:44.839085+00:00
- Pin core: 2; events/run: 1,000,000; repeats: 30

## Headline speedups (JIT throughput / interpreter throughput)

| Case | Median | p99 of runs | 95% bootstrap CI (median) |
|---|---:|---:|---|
| Momentum (single signal) | 7.32x | 8.38x | [7.29x, 7.48x] |
| All-signals fused | 15.56x | 15.63x | [15.52x, 15.58x] |

## Reproduce

```bash
bash bench/run_pinned_speedup.sh build-wsl
```

Raw data: `bench/results/pinned_host_speedup.json`.
