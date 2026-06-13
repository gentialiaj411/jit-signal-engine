# context/PROJECT_CONTEXT.md

## Purpose
`jit-signal-engine` is a C++20 trading-signal DSL engine with:
- interpreter execution as the correctness oracle
- LLVM ORC JIT execution for native performance when LLVM is available
- whole-program fusion, cross-symbol vectorization, multi-thread sharding, tiered specialization, module cache, and recorded-data backtesting

## Current Architecture Highlights
- Front-end: lexer/parser -> AST (`SignalDef`, `Expr`); type-check; constant-fold; fmt/lint CLI.
- Program transforms: dependency inlining + node-id allocation (P11 stateful structural dedup).
- Runtime model:
  - per-node state indexed by stable `node_id`
  - O(1) Welford `rolling_std` with periodic buffer refresh
  - multi-symbol arena via `MultiSymbolSignalContext`
- Execution:
  - interpreter (`Interpreter::Evaluate`)
  - JIT scalar (`Compile`, `CompileProgram`)
  - JIT scalar gradient (`CompileProgramGradient`)
  - fixture-backed calibration driver (`jitse_calibrate`) using the compiled gradient entrypoint
  - JIT vectorized (`CompileProgramVectorized`, K=2/4/8)
  - tiered baseline + warm-specialized compile
  - optional persistent post-O2 bitcode cache
- Ingest (benchmark): `SpscRing` + `spsc_jit_pipeline_bench` (not default CLI path).
- Backtest: mdh canonical journal or CSV fixture; pandas differential oracle.
- Stateful lowering: default `StatefulLoweringFlags::kAll` (all 14 stateful ops inline); bases via `SignalContext::lowered_bases` GEP (no `jit_rt_*_lowered_base` in IR). P4: lowered ops compose with `CompileProgramVectorized` via per-lane `LaneEmitScope` fan-out.

## Verified Tests (Current — 38 CTest targets with LLVM)
- Core: `lexer_test`, `parser_smoke_test`, `interpreter_test`, `runtime_test`, `jit_test`, `fuzz_parity_test`
- State/layout: `node_state_layout_test`, `hot_path_allocation_test`, `welford_stddev_parity_test`, `stateful_subtree_dedup_test`
- Vec/SIMD: `fuzz_parity_simd_test`, `vectorized_lanes_parity_test`, `vectorized_stateful_parity_test`
- Scale: `fuzz_parity_multisymbol_test`, `multithread_equivalence_test`, `cse_load_dedup_test`
- Tooling/cache: `jit_module_cache_test`, `dsl_formatter_roundtrip_test`, `tiered_specialization_parity_test`
- Autodiff/params: `gradient_parity_test`
- Calibration: `calibration_smoke_test`
- Profile/latency: `runtime_call_profile_test`, `latency_histogram_test`
- Ingest: `spsc_ring_test`
- Backtest/oracle: `backtest_determinism_test`, `differential_oracle_test`
- (Run `ctest` serially if parallel runs flake on cache/backtest tests.)

## Bench / Evidence Anchors
- Pinned host: `bench/PINNED_HOST.md`, `bench/results/pinned_host_speedup.{json,md}`
- Multi-thread: `multithread_scaling_pcores.*` (**5.45×** / **90.8%** @6T), hybrid + E-core variants
- Vec speedup: `bench/results/avx2_speedup/`
- SPSC pipeline: `bench/results/spsc_pipeline/`
- Latency: `bench/results/latency/`
- CSE: `bench/results/cse_evidence/cse_diff_verified.md`
- Operator lowering (Phases 0–4): `bench/results/lowering_gap_baseline/`, `bench/results/lowering_gap_phase3/` (fused JIT÷hw **1.42×**)
- P4 lowered+vec: `bench/results/stateful_vec_lowering_speedup.md` (parity green; vec+lowered **0.57×** scalar-lowered on `filtered_momentum`)
- Runtime profile: `bench/results/perf/filtered_momentum/runtime_call_profile.md`
- Vec×threads: `bench/results/vec_thread_composition/`
- Backtest/oracle: `bench/results/backtest/`, `bench/results/diff_test/`
- Autodiff bench: `bench/results/autodiff/`
- Autodiff calibration artifact: `bench/results/autodiff/calibration_fixture_fit.{md,meta.json}`
- Human index: `EVIDENCE.md`, `ARCHITECTURE.md`, `RESUME_CLAIMS.md`

## Critical Honesty Notes
- Interpreter/JIT parity is the non-negotiable correctness gate.
- Gradient correctness is finite-difference-gated via `gradient_parity_test`; compiled gradients are also gated against interpreted gradients at rel `1e-12` / abs `1e-12`, and stateful sensitivities cover recurrent and rolling operators as well as the stateless Phase 1 subset.
- Phase 4 calibration is intentionally fixture-backed. The recorded-data IC path remains broken at HEAD for calibration purposes and should not be claimed as a working calibration objective.
- Vec speedup is **vs scalar JIT**, program-dependent; cite `avx2_speedup/README.md`.
- SPSC p99 on WSL2 is scheduler-noisy; cite p50 + decomposition.
- Canonical interpreter speedups: pinned host only.
- Not a live trading platform; IC is methodology evidence.

## Commands
```bash
cmake -B build-wsl -DCMAKE_BUILD_TYPE=Release && cmake --build build-wsl -j
ctest --test-dir build-wsl --output-on-failure
bash bench/run_pinned_speedup.sh build-wsl
bash bench/run_pinned_multithread_scaling.sh build-wsl
bash bench/run_lowering_gap_phase3.sh build-wsl
bash bench/run_stateful_vec_lowering_phase4.sh build-wsl
```
