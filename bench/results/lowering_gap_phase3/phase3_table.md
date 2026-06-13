# Lowering gap Phase 3

Pinned host; `signal_benchmark` with per-signal `hw_reference` baselines
and ctx GEP for lowered-state bases (no `jit_rt_*_lowered_base` calls in IR).

| signal | jit thr | hw thr | jit÷hw | phase0 jit÷hw |
|---|---:|---:|---:|---:|
| spread | 115.09M | 109.66M | 1.05 | 1.10 |
| momentum | 102.27M | 107.18M | 0.95 | nan |
| spread_z | 55.91M | 25.64M | 2.18 | 0.43 |
| z | 55.33M | 24.92M | 2.22 | nan |
| dev | 52.79M | 20.15M | 2.62 | nan |
| all_signals | 37.85M | 26.73M | 1.42 | 0.30 |

## Reproduce

```bash
bash bench/run_lowering_gap_phase3.sh build-wsl
```
