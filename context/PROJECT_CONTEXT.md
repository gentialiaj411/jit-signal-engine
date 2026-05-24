# context/PROJECT_CONTEXT.md

## Purpose
`jit-signal-engine` is a C++20 trading-signal DSL engine with:
- interpreter execution as the correctness oracle
- LLVM ORC JIT execution for native performance when LLVM is available
- whole-program fusion, SIMD lowering, multi-symbol execution, and recorded-data backtesting

## Current Architecture Highlights
- Front-end: lexer/parser -> AST (`SignalDef`, `Expr`).
- Program transforms: dependency inlining + node-id allocation.
- Runtime model:
  - per-node state indexed by stable `node_id`
  - multi-symbol arena via `MultiSymbolSignalContext`
  - shared helper state for rolling stats, VWAP, lag, and cross-over operators
- Execution:
  - interpreter (`Interpreter::Evaluate`)
  - JIT single-signal (`Compile`)
  - JIT fused program (`CompileProgram`)
  - SIMD lowering with runtime AVX2 detection and scalar fallback
- Backtest integration:
  - consumes mdh canonical journal (`mf::core::BookEvent`)
  - emits signal time series plus `ic_report.json`

## Verified Tests (Current)
- `fuzz_parity_test` (interpreter vs JIT)
- `fuzz_parity_simd_test` (scalar JIT vs SIMD JIT)
- `fuzz_parity_multisymbol_test` (single-symbol reference vs multi-symbol execution)
- `hot_path_allocation_test` (includes multi-symbol warmed hot path)
- `backtest_determinism_test` (identical inputs -> identical IC sections)

## Bench / Evidence Anchors
- Baseline signal bench (WSL): `bench/results.csv`
- Baseline signal bench (Windows native): `bench/results_windows.csv`
- SIMD bench: `bench/results_simd.csv`
- Multi-symbol scaling: `bench/results_multisymbol.csv`
- CSE evidence bundle: `bench/results/cse_evidence/`
- Backtest outputs: `bench/results/backtest/phase4_mdh_20260523/`
- Differential oracle: `bench/results/diff_test/{divergence_report.json,divergence_report.md}`; current 1M-event comparison is above 99% numeric within tolerance for `vol` and `filtered` and 100% for `short_ma`/`long_ma`/`raw`.
- Differential oracle CTest now runs a deterministic AAPL fixture under the build tree with repeated signal-bearing rows and a 99.9% within-tolerance threshold; latest fixture report is 100% within tolerance for all five signals.

## Critical Honesty Notes
- Interpreter/JIT parity is the non-negotiable correctness gate.
- CSE read-dedup remains **Supported** unless an IR-diff artifact shows measurable relevant load reduction.
- Latency/speedup are environment-bound; do not claim universal portability.
- Current verified p99 ceilings differ by environment:
  - WSL artifact max: 78 ns
  - Windows artifact max: 48 ns

## Most Useful Commands
- Build (WSL): `cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release && cmake --build build-wsl -j`
- Test (WSL): `ctest --test-dir build-wsl --output-on-failure`
- Bench (WSL): `bash bench/run_benchmarks.sh ./build-wsl ./bench/results.csv 0 1000000`
- Bench (Windows native):
  - configure with LLVM: `-DLLVM_DIR=C:\Users\bhask\Documents\TOOLS\vcpkg\installed\x64-windows\share\llvm`
  - ensure runtime DLL path includes `C:\Users\bhask\Documents\TOOLS\vcpkg\installed\x64-windows\bin`

## Current Integration State with mdh
- Phase 4 used the mdh canonical journal as the preferred source.
- Phase 5 closed the integration/documentation boundary.
- Integration boundary documented in `docs/integration_with_mdh.md`.
- Backtest methodology documented in `docs/backtest_methodology.md`.
