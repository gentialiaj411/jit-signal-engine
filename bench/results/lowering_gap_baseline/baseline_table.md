# Lowering gap baseline (Phase 0)

Pinned host; `signal_benchmark --measure-runs 30 --pin-core 2`.
Interpreter + hw columns are identical across `jit_none` / `jit_all` runs (same binary invocation aside from lowering flag).

| signal | interp thr | jit_off thr | jit_on thr | hw thr | jit_on÷hw | jit_off÷interp | jit_on÷interp |
|---|---:|---:|---:|---:|---:|---:|---:|
| spread | 40.97M | 100.78M | 116.80M | 106.44M | 1.10 | 2.46 | 2.85 |
| momentum | 12.07M | 81.71M | 89.64M | nan | nan | 6.77 | 7.43 |
| spread_z | 5.82M | 21.79M | 53.56M | 124.15M | 0.43 | 3.75 | 9.21 |
| z | 5.98M | 22.93M | 25.11M | nan | nan | 3.83 | 4.20 |
| dev | 6.45M | 18.47M | 35.61M | nan | nan | 2.87 | 5.52 |
| all_signals | 2.20M | 13.18M | 34.30M | 114.64M | 0.30 | 5.98 | 15.57 |

## Reproduce

```bash
bash bench/run_lowering_gap_baseline.sh build-wsl
```
