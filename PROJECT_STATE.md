# PROJECT_STATE.md

## Current Snapshot (2026-05-24)
`jit-signal-engine` is in a post-Phase-5 hardening state. The repo now has verified parity, SIMD, multi-symbol, and recorded-backtest evidence rails.

## Phase Status
- Phase 1 (audit + artifacts): complete
- Phase 2 (SIMD vectorization): complete
- Phase 3 (multi-symbol SoA): complete
- Phase 4 (recorded-data backtest + IC): complete
- Phase 5 (mdh integration closure): complete

## Verified Core Capabilities
- C++20 signal DSL with interpreter and LLVM ORC JIT backends.
- Whole-program fusion via `CompileProgram`.
- Interpreter/JIT parity preserved (`fuzz_parity_test` passing).
- SIMD path with runtime AVX2 detection + scalar fallback (`fuzz_parity_simd_test` passing).
- Multi-symbol SoA execution with single-compile reuse (`fuzz_parity_multisymbol_test` passing).
- Multi-symbol warmed hot path remains allocation-free (`hot_path_allocation_test` passing).
- Recorded-data backtest runner consuming mdh canonical journal and producing deterministic IC artifacts (`backtest_determinism_test` passing).
- Differential oracle CTest now runs a deterministic AAPL fixture with real numeric coverage and a 99.9% within-tolerance threshold.

## Benchmark Reality
- WSL artifact: `bench/results.csv` shows JIT p99 values from 14 ns to 78 ns.
- Windows-native artifact: `bench/results_windows.csv` shows JIT p99 values from 10 ns to 48 ns.
- The current all-signals fused path is the top-end p99 case in both artifacts.
- Differential oracle reports are now honest about NaN/degenerate rows and UNKNOWN exclusions; the latest 1M-event recorded-ITCH comparison is above 99% numeric within tolerance for `vol` and `filtered`.
- Current CTest differential-oracle fixture reports 100.00% within tolerance for all five signals, including 211 numeric `vol` rows and 211 numeric `filtered` rows.

## Key New Artifacts
- `bench/results/cse_evidence/{before.ll,after.ll,cse_diff_report.md}`
- `bench/results_simd.csv`
- `bench/results_multisymbol.csv`
- `bench/results_windows.csv`
- `bench/results/backtest/phase4_mdh_20260523/signals.csv`
- `bench/results/backtest/phase4_mdh_20260523/ic_report.json`

## Current Claim Boundary
- CSE load-elimination remains **Supported**, not fully **Verified**, because current IR diff did not show a relevant load-count drop in the tested fused case.
- All numeric claims should cite the specific artifact + environment.
- Differential oracle parity is verified for the current 1M-event artifact, with residual `vol`/`filtered` mismatches under 0.3% numeric rows at `rtol=1e-6`, `atol=1e-9`.
