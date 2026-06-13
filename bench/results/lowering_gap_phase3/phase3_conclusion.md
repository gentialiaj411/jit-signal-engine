# Phase 3 conclusion — operator lowering gap closed

**Host:** pinned core 2, canonical WSL2 (`bench/pinned_host_spec.json`)  
**Date:** 2026-06-09  
**Status:** Phase 3 **complete**; Phase 4 (lowered + cross-symbol vec) **deferred**.

## What shipped

1. **`SignalContext::lowered_bases`** — POD at offset 0; JIT loads array bases via GEP (no `jit_rt_*_lowered_base` in IR).
2. **Per-signal hw baselines** — `bench/hw_reference.{h,cpp}` wired into `signal_benchmark`.
3. **Gap matrix** — `phase3_table.md` (reproduce: `bash bench/run_lowering_gap_phase3.sh build-wsl`).
4. **Fresh runtime profile** — `bench/results/perf/filtered_momentum/runtime_call_profile.md` (post-GEP).

## JIT vs hand-written C++ (before → after)

| Signal | Phase 0 JIT÷hw | Phase 3 JIT÷hw | Notes |
|---|---:|---:|---|
| spread | 1.10× | **1.05×** | fair hw both phases |
| momentum | — | **0.95×** | new hw baseline |
| spread_z | 0.43×* | **2.18×** | *Phase 0 hw was spread-only (unfair) |
| z | — | **2.22×** | new hw baseline |
| dev | — | **2.62×** | new hw baseline |
| **all_signals (fused)** | **0.30×*** | **1.42×** | *Phase 0 hw was spread-only |

**Headline:** fused `filtered_momentum` JIT is **~1.42×** hand-written C++ on the pinned host (was not measurable vs fair hw in Phase 0).

## Runtime-call overhead (filtered_momentum, 40M events)

| Config | % in `jit_rt_*` | % in `[JIT]` | Wall (s) |
|---|---:|---:|---:|
| `lowering=none` | **80.2%** | 15.7% | 2.93 |
| `lowering=all` (Phase 2, with `_lowered_base` calls) | ~2.5% | ~83% | ~1.11 |
| `lowering=all` (Phase 3, GEP bases) | **1.6%** | **86.3%** | **1.03** |

Phase 3 removed `jit_rt_rolling_std_lowered_base` / `jit_rt_ema_lowered_base` from the hot profile (previously ~2.2% combined). Residual `jit_rt_symbol_ctx` (~2.8%) is the main extern call left.

## Deferred (documented, not shipped)

| Item | Reason |
|---|---|
| Cross-signal structural stateful CSE | Reusing lowered SSA across signals breaks per-signal `Evaluate` parity (`stateful_subtree_dedup_test`, 977 mismatches on signal `s`). Needs fused interpreter model or cross-signal node_id aliasing. |
| K-wide SIMD ring-buffer state (Phase 4 perf stretch) | P4 parity landed (`LaneEmitScope` per-lane lowered fan-out); vec+lowered **0.57×** scalar-lowered on `filtered_momentum` — see `bench/results/stateful_vec_lowering_speedup.md`. |
| `assume_warm` in gap matrix | Tiered specialization is orthogonal; baseline `kAll` already meets ~1.4× fused hw target. |

## IR evidence

- No `jit_rt_*_lowered_base` in fused IR: `bench/results/lowering_gap_phase3/ir/README.md`
- Market-load CSE (22→2) unchanged: `bench/results/cse_evidence/cse_diff_verified.md`

## Verification

```bash
cmake --build build-wsl -j
ctest --test-dir build-wsl --output-on-failure   # 36/36
bash bench/run_lowering_gap_phase3.sh build-wsl
cd build-wsl && ./runtime_call_profile ../examples/filtered_momentum.sig \
  --events=40000000 --sample-us=100 \
  --out-dir=../bench/results/perf/filtered_momentum
```
