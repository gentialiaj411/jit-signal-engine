# Phase 0 conclusion

**Gate:** proceed to Phase 1 (lowering default promotion). Gaps with lowering **on** are materially better than §4 estimates for JIT-vs-interpreter; JIT-vs-hw ratios for non-spread signals are **not yet meaningful** until per-signal hw baselines exist.

## Measured baseline (pinned core 2, 1M events, 30 measure runs)

See `baseline_table.md` and `matrix.csv`.

| Signal | JIT off ÷ interp | JIT on ÷ interp | JIT on ÷ JIT off | Notes |
|---|---:|---:|---:|---|
| spread | 2.46× | 2.85× | 1.16× | JIT on ≈ 1.10× hw spread (fair hw) |
| momentum | 6.77× | 7.43× | 1.10× | no hw baseline |
| spread_z | 3.75× | 9.21× | 2.46× | hw is spread-only → jit_on÷hw **understates** gap |
| z | 3.83× | 4.20× | 1.10× | zscore not lowered; no hw |
| dev | 2.87× | 5.52× | 1.93× | vwap etc. not lowered; no hw |
| **all_signals** | **5.98×** | **15.57×** | **2.60×** | matches §4 2.96×→15.6× story; hw is spread-only |

## Dominant remainder (from existing perf artifacts, not re-run here)

For fused `filtered_momentum.sig` with lowering **off**, `profile_lowering_none.txt` shows ~69.6% `jit_rt_rolling_std` + ~8% `jit_rt_ema_alpha` (~78% in runtime calls). With lowering **on**, `profile_lowering_all.txt` drops runtime to ~2%.

Phase 2 worklist order (still valid): opaque ops driving `z` / `spread_z` / `dev` slowness — `zscore`, `rolling_min`, `rolling_max`, `vwap`, then corr/beta/kalman/cross ops.

## Checkpoint vs §4

- Fused JIT-on vs interpreter **15.57×** — consistent with `rolling_std_lowering_speedup.md` (15.6×).
- Fused JIT-off vs interpreter **5.98×** — consistent with pinned ~2.96× claim directionally (artifact/host differ; this run uses core 2 + 30 measure runs).
- **Do not** quote jit_on÷hw for fused or z-family signals until CONFIRM-2 hw baselines are implemented.

## Blockers before Phase 3 gap-to-hw claims

1. Add honest per-signal `hw_*` paths in `bench/signal_benchmark.cpp` (momentum, z, dev, fused eval_all).
2. Optional: capture fresh `perf` logs under this directory for `all_signals` none vs all (reuse `bench/results/perf/filtered_momentum/` methodology).
