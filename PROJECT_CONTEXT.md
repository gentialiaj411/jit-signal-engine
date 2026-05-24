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

## Current Verified State (2026-05-24)
- Phase 1 through Phase 5 hardening is complete.
- The repo is post-Phase-5 and the current claims surface is backed by parity, SIMD, multi-symbol, and backtest artifacts.
- Interpreter/JIT parity is preserved (`fuzz_parity_test`).
- SIMD parity is preserved (`fuzz_parity_simd_test`).
- Multi-symbol parity is preserved (`fuzz_parity_multisymbol_test`).
- Warmed multi-symbol hot path remains allocation-free (`hot_path_allocation_test`).
- Recorded-data deterministic IC workflow passes (`backtest_determinism_test`).
- Recorded-data differential oracle on a 1M-event run now shows `short_ma`/`long_ma`/`raw` at 100.00% within tolerance, `vol` at 99.74%, and `filtered` at 99.72%.
- `differential_oracle_test` now gates a deterministic AAPL numeric fixture at `--min-within-rate 0.999`; latest fixture report is 100.00% within tolerance for all five signals.

## Reality of Performance Claims
Use artifact-specific numbers only.

- WSL benchmark artifact: `bench/results.csv`
  - JIT p99 spans 14-78 ns across the measured signals.
  - `<all_signals>` fused path is 78 ns p99 in the current artifact.
- Windows native benchmark artifact: `bench/results_windows.csv`
  - JIT p99 spans 10-48 ns across the measured signals.
  - `<all_signals>` fused path is 48 ns p99 in the current artifact.

Historical benchmark numbers are not current ceilings unless re-produced in a fresh artifact.

## Evidence Bundle Locations
- CSE evidence: `bench/results/cse_evidence/{before.ll,after.ll,cse_diff_report.md}`
- SIMD bench: `bench/results_simd.csv`
- Multi-symbol scaling: `bench/results_multisymbol.csv`
- Signal benches:
  - WSL: `bench/results.csv`
  - Windows: `bench/results_windows.csv`
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
- CSE claim is currently Supported, not fully Verified, unless IR-diff evidence shows measurable relevant load reduction.
- All speed/latency claims must cite environment plus artifact.
- No claim of universal portability for latency or speedup.

## Key Docs
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
