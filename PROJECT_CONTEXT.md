# PROJECT_CONTEXT.md

Status: current high-signal orientation for contributors and downstream LLMs.

## What This Project Is
`jit-signal-engine` is a C++20 trading-signal DSL engine with:
- interpreter execution as the correctness oracle
- optional LLVM ORC JIT execution for native performance
- whole-program fusion via `CompileProgram`
- AVX2-gated SIMD lowering for eligible rolling-window codegen
- multi-symbol SoA evaluation (`MultiSymbolSignalContext` + `symbol_id`)
- recorded-data backtest execution over mdh canonical journals

## Current Verified State (2026-06-13)
- Phase 1 through Phase 5, roadmap P0–P15, and **operator lowering Phases 0–4** are complete.
- Fused JIT vs hand-written C++: **1.42×** on pinned host (`bench/results/lowering_gap_phase3/`).
- Stateful lowering default `kAll`; `jit_rt_*` hot-path share **80% → 2.8%** on `filtered_momentum` (`bench/results/perf/filtered_momentum/runtime_call_profile.md`).
- Recent: Welford `rolling_std`, cross-symbol vec speedup artifacts (`avx2_speedup/`), SPSC pipeline bench, module cache, fmt/lint, explicit `param` declarations + autodiff Phases 1-4 (including fixture-backed calibration), P11 stateful dedup, 38 CTests (LLVM).
- The claims surface is backed by parity, fuzz, multi-symbol, vector, threading, latency, and backtest artifacts.
- Interpreter/JIT parity is preserved (`fuzz_parity_test`).
- SIMD parity is preserved (`fuzz_parity_simd_test`).
- Multi-symbol parity is preserved (`fuzz_parity_multisymbol_test`).
- Warmed multi-symbol hot path remains allocation-free (`hot_path_allocation_test`).
- Recorded-data deterministic IC workflow passes (`backtest_determinism_test`).
- Recorded-data differential oracle on a 1M-event run now shows `short_ma`/`long_ma`/`raw` at 100.00% within tolerance, `vol` at 99.74%, and `filtered` at 99.72%.
- `differential_oracle_test` now gates a deterministic AAPL numeric fixture at `--min-within-rate 0.999`; latest fixture report is 100.00% within tolerance for all five signals.
- `jit_test` now directly covers `abs`, `log`, and `sqrt` JIT/interpreter parity.
- `gradient_parity_test` now checks stateless and stateful parameter gradients on parameterized programs against central finite differences, checks compiled gradients against interpreted gradients at rel `1e-12` / abs `1e-12`, and keeps primal JIT/interpreter parity on the non-degenerate cases.
- `jitse_calibrate` now runs Adam against a checked-in CSV fit-to-target fixture using the compiled whole-program gradient entrypoint, and `calibration_smoke_test` gates a fixed minimum objective improvement (`0.02`).
- `hot_path_allocation_test` now covers the compiled forward+gradient tick, not only forward evaluation.
- No-LLVM builds are preserved; `fuzz_parity_simd_test` skips cleanly when JIT is unavailable.
- Interview/readability package is current: `README.md`, `EVIDENCE.md`, `PROJECT_ROADMAP.md`, `docs/agent_architecture.md`, and demo scripts.
- Elite plan P0–P2: pinned JIT speedups, verified fused load dedup (`cse_load_dedup_test`), multi-thread scaling (`multithread_equivalence_test`).
- Vec vs scalar JIT: **~2.6×** on `stateless_compute_heavy.sig` at K=4 (`bench/results/avx2_speedup/`).
- P4 lowered+vec parity green; perf on `filtered_momentum`: vec+lowered **2.13×** vs vec+opaque, **0.57×** vs scalar-lowered (`bench/results/stateful_vec_lowering_speedup.md`).
- SPSC ingest pipeline p50 **~228 ns** spread at 2 MHz (`bench/results/spsc_pipeline/`).
- Operator lowering Phases 0–4: complete (`OPERATOR_LOWERING_TASK.md`, `NEXT_TASK.md`).
- The checked-in Phase 4 calibration artifact reports objective **0.06167875 -> 0.0012323651** on the deterministic fixture (`bench/results/autodiff/calibration_fixture_fit.md`).
- Recorded-data IC remains broken at HEAD for calibration purposes; the Phase 4 demo deliberately routes around it and does not claim recorded-data calibration works.
- Resume bullets: see `RESUME_CLAIMS.md` (three-bullet preferred shape).

## Reality of Performance Claims
Use artifact-specific numbers only.

- **Canonical pinned-host speedups (lowering default `kAll`):** `bench/results/pinned_host_speedup.md` (momentum median **7.32×**; fused **15.56×** on host `wsl2-ultra9-275hx-2026-05`). Reproduce: `bash bench/run_pinned_speedup.sh build-wsl`. Use `--lower-stateful=none` or `JITSE_LOWER_STATEFUL=none` for opaque-runtime baseline.
- **JIT vs hand-written C++ (pinned host):** `bench/results/lowering_gap_phase3/phase3_table.md` — fused **1.42×**; spread **1.05×**.
- **Pinned multi-thread scaling (P-cores, resume headline):** `bench/results/multithread_scaling_pcores.md` (**5.45×** at 6 threads, **90.8%** efficiency, 10k symbols, cores 1–7).
- **Hybrid (context):** `bench/results/multithread_scaling.md` (**5.69×** / **35.6%** at 16T). **E-cores:** `multithread_scaling_ecores.md` (**6.51×** / **40.7%** at 16T). Launcher: `bench/run_pinned_multithread_scaling.sh` (`--cores`, `--thread-counts`). Core map: `bench/PINNED_HOST.md`.
- WSL benchmark artifact: `bench/results.csv`
  - JIT p99 spans 14-78 ns across the measured signals.
  - `<all_signals>` fused path is 78 ns p99 in the current artifact.
- Windows native benchmark artifact: `bench/results_windows.csv`
  - JIT p99 spans 10-48 ns across the measured signals.
  - `<all_signals>` fused path is 48 ns p99 in the current artifact.

Historical benchmark numbers are not current ceilings unless re-produced in a fresh artifact.

## Evidence Bundle Locations
- Pinned host: `bench/PINNED_HOST.md`, `bench/results/pinned_host_speedup.{json,md}`
- Multi-thread scaling: `multithread_scaling.{json,md}` (hybrid), `multithread_scaling_pcores.*`, `multithread_scaling_ecores.*`
- CSE / load dedup: `bench/results/cse_evidence/cse_diff_verified.md` (supersedes `cse_diff_report.md`)
- SIMD bench (per-op): `bench/results_simd.csv`
- Cross-symbol vec speedup: `bench/results/avx2_speedup/`
- Operator lowering: `bench/results/lowering_gap_phase3/`, `bench/results/stateful_vec_lowering_speedup.md`
- Autodiff benchmark: `bench/results/autodiff/phase3_forward_gradient_tick.{md,meta.json}`
- Autodiff calibration: `bench/results/autodiff/calibration_fixture_fit.{md,meta.json}`
- SPSC pipeline: `bench/results/spsc_pipeline/`
- Latency histograms: `bench/results/latency/`
- Single-thread multi-symbol sweep: `bench/results_multisymbol.csv`
- Architecture: `ARCHITECTURE.md`
- Signal benches:
  - WSL: `bench/results.csv`
  - Windows: `bench/results_windows.csv`
- Benchmark provenance:
  - `bench/results.csv.meta.json`
  - `bench/results_windows.csv.meta.json`
  - `bench/results_multisymbol.csv.meta.json`
  - `bench/results_simd.csv.meta.json`
- Backtest outputs:
  - `bench/results/backtest/phase4_mdh_20260523/signals.csv`
  - `bench/results/backtest/phase4_mdh_20260523/ic_report.json`
- Differential oracle:
  - `bench/results/diff_test/divergence_report.json`
  - `bench/results/diff_test/divergence_report.md`
  - Current 1M-event comparison is above 99% numeric within tolerance for `vol` and `filtered`.
  - Current CTest fixture comparison writes under the build tree, has real numeric coverage, and passes a 99.9% within-tolerance threshold.

## Claim Guardrails
- Interpreter/JIT parity is the correctness oracle; do not relax it.
- Fused JIT market load dedup is Verified on `filtered_momentum.sig` (see `bench/results/cse_evidence/cse_diff_verified.md`).
- All speed/latency claims must cite environment plus artifact.
- No claim of universal portability for latency or speedup.

## Key Docs
- `README.md`: human-facing landing page.
- `EVIDENCE.md`: compact claim-to-artifact map.
- `PROJECT_ROADMAP.md`: completion/optimization roadmap and current remaining work.
- `docs/agent_architecture.md`: compact architecture brief for LLM agents.
- `CLAIMS_MATRIX.md`: authoritative claim status.
- `docs/operator_taxonomy.md`: 14-operator classification.
- `docs/backtest_methodology.md`: no-lookahead IC methodology.
- `docs/integration_with_mdh.md`: mdh -> jit integration boundary.

## Practical Build/Test Entry
- WSL:
  - `cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release`
  - `cmake --build build-wsl -j`
  - `ctest --test-dir build-wsl --output-on-failure`
- Windows native (LLVM 18.1.6 via vcpkg):
  - configure with `LLVM_DIR=C:\Users\bhask\Documents\TOOLS\vcpkg\installed\x64-windows\share\llvm`
  - include `C:\Users\bhask\Documents\TOOLS\vcpkg\installed\x64-windows\bin` in `PATH` for runtime DLLs
