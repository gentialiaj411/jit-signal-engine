# AUDIT_LOG.md

## 2026-06-13 (autodiff task Phase 4 complete)
- Added `cli/jitse_calibrate.cpp`, a fixture-backed calibration driver that loads a parameterized `.sig` plus tick CSV, compiles the whole-program gradient via `JitCompiler::CompileProgramGradient`, and runs Adam (`lr=0.05`, `beta1=0.9`, `beta2=0.999`, `epsilon=1e-8`, 80 iterations) without introducing an interpreter-only or finite-difference fallback.
- Added the deterministic fixture pair `examples/calibration_fit_target.sig` and `test/fixtures/calibration_ticks.csv`.
- Added `calibration_smoke_test` to CTest, invoking the driver on the checked-in fixture and requiring at least `0.02` objective improvement in CI.
- Recorded Phase 4 artifacts under `bench/results/autodiff/`: `calibration_fixture_fit.md`, `calibration_fixture_fit.meta.json`, `calibration_fixture_fit_params.csv`, and `calibration_fixture_fit_trajectory.csv`.
- Measured fixture objective improvement: **0.06167875 -> 0.0012323651** (improvement **0.0604463849**) on host `gentilaptop | Linux 6.6.114.1-microsoft-standard-WSL2 x86_64 GNU/Linux`.
- Kept the recorded-data IC path explicitly out of scope for calibration because the checked-in recorded-data IC artifact remains broken at HEAD (all-NaN / zero-sample output).
- Verification: `cmake --build build-wsl -j`; `ctest --test-dir build-wsl -R calibration_smoke_test --output-on-failure`; `ctest --test-dir build-wsl --output-on-failure` — **38/38** passed.

## 2026-06-12 (autodiff task Phase 3 complete)
- Added a compiled whole-program gradient entrypoint through `src/jit_compiler.cpp` / `src/runtime.cpp`, reusing the fused-program JIT path and stateful runtime recurrences instead of introducing a JIT-free gradient implementation.
- Extended `gradient_parity_test` to a three-way gate: interpreted gradient vs central FD (`h = 1e-6 * max(1, |theta|)`, rel `1e-4` or abs `1e-8`), compiled gradient vs interpreted gradient (rel `1e-12` or abs `1e-12`), and compiled value vs interpreted value.
- Extended `hot_path_allocation_test` to assert zero warmed allocations on the compiled forward+gradient tick.
- Extended `fuzz_parity_test` to emit parameterized programs so value parity exercises the param machinery in addition to plain literals.
- Added `bench/autodiff_benchmark.cpp` and recorded `bench/results/autodiff/phase3_forward_gradient_tick.{md,meta.json}`: forward-only **77.26 ns/tick**, compiled forward+gradient **147.06 ns/tick**, ratio **1.90x** on the measured pinned host command.
- Fixed two compiled-gradient bugs during bring-up: missing `gradient_param_count` refresh on `MultiSymbolSignalContext`, and incorrect use of `*Prepared` ring helpers for lazily-grown sensitivity rings in compiled `lag` / `sma`.
- Verification: `cmake --build build-wsl -j`; `ctest --test-dir build-wsl -R "gradient_parity_test|hot_path_allocation_test|fuzz_parity" --output-on-failure`; `ctest --test-dir build-wsl --output-on-failure` — **37/37** passed.

## 2026-06-12 (rolling_std/zscore period=1 parity fix)
- Rejected `period < 2` for `rolling_std()` and `zscore()` in `src/interpreter.cpp`, `src/jit_compiler.cpp`, and `src/autodiff.cpp` to remove accepted-input parity drift (`nan` interpreter vs `inf` lowered JIT at `period=1`).
- Replaced the disabled `gradient_parity_test` JIT value bypass with explicit invalid-program rejection checks for `rolling_std(..., 1)` and `zscore(..., 1)`.
- Verified `test/fuzz_parity_test.cpp` already constrains generated `rolling_std`/`zscore` periods to `{3,5,10}`, so no generator change was required.
- Verification: `ctest --test-dir build-wsl -R gradient_parity_test --output-on-failure` and `ctest --test-dir build-wsl --output-on-failure` — **37/37** passed.

## 2026-06-12 (autodiff task Phase 2 complete)
- Added Phase 2 stateful sensitivity propagation to `src/autodiff.cpp` using per-`(node_id,param_id)` state carried in `SignalContext`: `lag`, `sma`, `ema_alpha`, Welford `rolling_std` / `zscore`, `rolling_corr`, `rolling_beta`, `kalman1d`, `rolling_min/max`, and `cross_*` stop-gradients.
- Extended `SignalContext` / `PrewarmSignalContext` / `node_state_layout_test` with flattened sensitivity storage keyed by `node_id` and parameter slot.
- Added primal `ema_alpha(expr, alpha)` support through interpreter, node-id allocation, and JIT codegen so the EMA recurrence is exercised against the real DSL surface.
- Expanded `gradient_parity_test` to cover the Phase 2 operator order and boundary conventions, including the `rolling_std(period=1)` NaN convention and a multi-slide refresh case.
- Fixed `signal_program_test` to accept duplicate-name rejection at parse time now that `ParseProgram` enforces that earlier.
- Verification: `ctest --test-dir build-wsl --output-on-failure` **37/37** passed.

## 2026-06-12 (autodiff task Phase 1 complete)
- Added explicit DSL parameters via `ProgramDef` / `ParamDef` / `ParameterExpr`, `ParseProgram`, formatter/lint support, and shared runtime/JIT parameter plumbing (`SignalContext::params`, `jit_rt_param`).
- Relaxed primal `kalman1d` value paths to accept parameter expressions for `q` and `r` while keeping existing interpreter semantics as the oracle.
- Added Phase 1 stateless symbolic differentiation in `src/autodiff.*` and the `gradient_parity_test` finite-difference oracle gate for parameterized fused programs.
- Fixed an autodiff construction bug in the `abs` derivative caused by unspecified argument-evaluation order around `CloneExpr(*x_prime)` and `std::move(x_prime)`.
- Verification: `ctest --test-dir build-wsl --output-on-failure` **37/37** passed.

## 2026-06-10 (Phase 2 complete)
- Lowered remaining opaque ops: `vwap` (`kVwap`), `cross_above`/`cross_below` (`kCross`), `rolling_corr` (`kRollingCorr`), `rolling_beta` (`kRollingBeta`), `kalman1d` (`kKalman1d`). All included in `kAll`.
- Added POD lowered state + prewarm + `jit_rt_*_lowered_base` for each; inline emitters in `jit_compiler.cpp`.
- `VwapStateLowered` uses double accumulators (SMA-style); single-signal `Compile()` hoists market bid/ask/volume loads like `CompileProgram`.
- Parity: 6 new cases in `stateful_lowering_parity_test.cpp`. `rolling_pair_kalman_parity_test` forces `kNone` (interpreter-vs-runtime wiring; lowering covered elsewhere).
- Verification: `ctest --test-dir build-wsl --output-on-failure` **36/36** passed.

## 2026-06-10
- Phase 2 (continued): `rolling_min` + `rolling_max` inline mono-deque IR (`kRollingMin`, `kRollingMax` in `kAll`); parity cases pass.
- Phase 2 (partial): `zscore` IR lowering (`kZscore`, `EmitLoweredZscore`, `zscore_lowered` state). Parity cases pass. `z` signal quick bench: **3.67× → 8.69×** vs interpreter (lowering off vs zscore on). `fuzz_parity_test` rel tol **1e-5**.
- Phase 1 (operator lowering task): promoted `StatefulLoweringFlags::kAll` as production default in `JitCompiler`, env parse (`JITSE_LOWER_STATEFUL` unset → `kAll`), and `signal_benchmark`. `kNone` reachable via env/CLI/API.
- Regenerated `bench/results/pinned_host_speedup.{json,md}`: momentum **7.32×** [7.29×, 7.48×]; fused **15.56×** [15.52×, 15.58×] (was 5.18× / 2.96× with lowering off).
- Test fixes: `fuzz_parity_test` rel tolerance for lowered default; `stateful_subtree_dedup_test` explicit `kNone` for runtime-path checks; `runtime_call_profile_test` lag gate 1.5×.
- Verification: `ctest --test-dir build-wsl --output-on-failure` **36/36** passed.

## 2026-06-10 (Phase 0)
- Ran lowering gap baseline matrix (`bench/run_lowering_gap_baseline.sh`); artifacts under `bench/results/lowering_gap_baseline/`. Fused JIT-on vs interpreter **15.57×**; confirmed `results.csv` used lowering off; documented hw baseline fairness gap.

- LLM scaffold created (`.claudeignore`, `AGENTS.md`, `PROJECT_STATE.md`, `CLAIMS_MATRIX.md`, `AUDIT_LOG.md`, `NEXT_TASK.md`).
- Evidence source limited to top-level structure and `README.md`.
- Next priorities: verify README claims via targeted tests and benchmark scripts.

## 2026-05-17
- Used gh run view 25999435376 --log to inspect the failing CI run on main.
- Root cause: the pushed commit still referenced removed LagState::total_samples in src/runtime.cpp, breaking both Ubuntu build jobs at compile time.
- Verified the working tree runtime fix locally with cmake --build build --target signal_core --parallel 8.

## 2026-05-17
- Removed generated build outputs at `build/` and `CMakeFiles/` from the workspace.
- Preserved repo handoff/context docs, including `AGENT_HANDOFF/` and `docs/architecture.md`; no source files were changed.

## 2026-05-17
- Created `PROJECT_CONTEXT.md` as the dense, self-contained orientation doc for future LLM passes.
- Documented the real pipeline, module map, tests, benchmark scripts, evidence status, and current TODO/VERIFY items.

## 2026-05-23
- Corrected the JIT p99 wording in `CLAIMS_MATRIX.md` and `RESUME_CLAIMS.md` to match `bench/results.csv` (`<all_signals>` JIT p99 `78 ns`, range `13-78 ns`).
- Extended `bench/simd_benchmark.cpp` to benchmark `sma(64)`, `sma(128)`, and `zscore(128)` for interpreter, scalar JIT, and AVX2 JIT modes.
- Recorded the SIMD refresh in `bench/results_simd.csv`; AVX2 did not beat scalar on the tested `sma` windows and was effectively tied on `zscore(128)`.
- Added an IC interpretation note to `docs/backtest_methodology.md` and a SIMD honesty note to `docs/simd_candidates.md`.
- Verification: `ctest --test-dir build-wsl --output-on-failure` passed `13/13` tests.

## 2026-05-23
- Added a pandas-based differential oracle for the recorded ITCH journal: `examples/journal_to_csv.cpp`, `bench/run_diff_oracle.sh`, `test/reference_oracle/compute_reference.py`, `test/reference_oracle/diff_signals.py`, and `test/reference_oracle/requirements.txt`.
- Added `differential_oracle_test` to CTest and verified `ctest --test-dir build-wsl --output-on-failure` passes `14/14` tests.
- Differential oracle result on the 50k-event recorded-ITCH slice: 100.0% within-tolerance match on `filtered`, `short_ma`, `long_ma`, `raw`, and `vol` with `rtol=1e-6`, `atol=1e-9`.
- Root cause found during development: the first oracle version replayed `rolling_std` globally instead of per output symbol; fixing the per-symbol replay matched the JIT runtime semantics exactly.

## 2026-05-23
- Synchronized the repo-facing context/state docs with the current evidence surface: parity tests, SIMD lowering, multi-symbol execution, deterministic backtesting, and the current benchmark artifacts.
- Updated the orientation docs to reflect current p99 ranges from `bench/results.csv` and `bench/results_windows.csv`.
- Aligned `NEXT_TASK.md`, `RESUME_CLAIMS.md`, `AGENTS.md`, and `CLAUDE.md` with the current post-Phase-5 state.

## 2026-05-23
- Rewrote `docs/architecture.md` into a current architecture reference covering the parser, runtime model, interpreter, JIT, SIMD path, multi-symbol path, backtest path, and verified test anchors.

## 2026-05-23
- Fixed the differential oracle's NaN accounting so `NaN=NaN` is reported separately from numeric exact/warmup matches, and added explicit `UNKNOWN` exclusion counts.
- Fixed `examples/backtest_runner.cpp` to keep the program's fixed input ticker state separate from per-event output-symbol state; the previous per-symbol market snapshot model was feeding fixed-ticker reads from the wrong market object.
- Regenerated the recorded-ITCH differential report and updated the repo-facing claims/docs to reflect the current partial parity state rather than a false 100% exact-match claim.

## 2026-05-24
- Final focused pass on `vol`/`filtered`: re-ran `bench/run_diff_oracle.sh --max-events 50000 --run-id diagnosis_fix` and confirmed the current slice is bootstrap-only (no trade/cross-trade events; mostly `System`, `StockDirectory`, `Unknown`), producing degenerate oracle rows rather than meaningful numeric parity coverage.
- Confirmed the signal formulas under investigation are `vol = rolling_std(mid(AAPL), 30)` and `filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0`.
- Documented unresolved parity as a tracked gap with warmup/degenerate-window hypothesis, and made CTest intent explicit: `differential_oracle_test` now gates pipeline execution on this bootstrap slice (`--min-within-rate 0.0`) while parity quality remains artifact-tracked.

## 2026-05-24
- Fixed whole-program JIT signal-reference semantics: `CompileProgram` now resolves `IdentifierExpr` references to previously emitted program values instead of requiring dependency-inlined re-evaluation.
- Added program-wide node ID allocation and switched `backtest_runner` to compile parsed program ASTs, preventing `filtered` from advancing duplicate `ema`/`rolling_std` state nodes.
- Replaced cancellation-prone rolling sample stddev calculation with a two-pass long-double calculation over the ring buffer, eliminating flat-window pseudo-volatility.
- Verification: WSL LLVM build passed for `signal_core`, `jit_test`, `backtest_runner`; `./build-wsl/jit_test` passed.
- Full oracle: `bash bench/run_diff_oracle.sh --max-events 1000000 --run-id diagnosis_fix_full_after_stddev --min-within-rate 0.0` produced `short_ma`/`long_ma`/`raw` 100.00%, `vol` 99.74%, and `filtered` 99.72% numeric within-tolerance rates, with zero `filtered` degenerate mismatches.

## 2026-05-24
- Added CSV replay support to `examples/backtest_runner.cpp` for oracle fixtures while preserving journal-mode backtest execution.
- Added `test/reference_oracle/generate_fixture_events.py` and `bench/run_diff_oracle.sh --fixture synthetic_aapl` for a deterministic AAPL numeric fixture.
- Tightened `differential_oracle_test` from the old bootstrap-only `--min-within-rate 0.0` smoke gate to `--min-within-rate 0.999` on the synthetic fixture.
- Routed the CTest fixture report under the build tree so `bench/results/diff_test` can remain the recorded-ITCH evidence artifact.
- Verification: `cmake --build build-wsl -j` passed; `ctest --test-dir build-wsl -R differential_oracle_test --output-on-failure` passed in 1.35s.
- Fixture oracle report: all five signals passed at 100.00% within tolerance; `vol` and `filtered` each had 211 numeric comparison rows.
- Refreshed default recorded-ITCH oracle artifact with `bash bench/run_diff_oracle.sh --max-events 1000000 --run-id recorded_1m_after_fixture_gate --min-within-rate 0.99`; all five signals passed the 99% threshold.

## 2026-05-24
- Added `docs/agent_architecture.md` as a compact LLM-facing architecture brief and moved it into the `AGENTS.md` onboarding path.
- Added `PROJECT_ROADMAP.md` with the completion/optimization roadmap and updated it after implementing the high-ROI completion items.
- Rewrote `README.md` as a recruiter/interviewer-facing landing page with verified capabilities, explicit non-claims, quick-start/demo commands, and benchmark artifact pointers.
- Added `EVIDENCE.md` and benchmark provenance sidecars (`bench/results*.meta.json`) so resume/project claims map directly to artifacts.
- Added demo scripts: `scripts/demo_smoke.sh` and `scripts/demo_smoke.ps1`.
- Added direct `abs`, `log`, and `sqrt` JIT/interpreter parity coverage in `test/jit_test.cpp` and documented the claim in `CLAIMS_MATRIX.md`.
- Fixed `fuzz_parity_simd_test` to skip cleanly when LLVM/JIT is unavailable, preserving interpreter-only build behavior.
- Verification: configured a fresh Windows Release build without LLVM in `build-agent`, built successfully, and `ctest --test-dir build-agent -C Release --output-on-failure` passed `12/12`; `scripts/demo_smoke.ps1 -BuildDir .\build-agent\Release` passed.
- Removed generated `build-agent/` after verification.

## 2026-05-24
- **P0 pinned-host speedup:** added `bench/PINNED_HOST.md`, `bench/pinned_host_spec.json`, `bench/run_pinned_speedup.sh`; extended `signal_benchmark` with `--measure-runs` for repeated samples without re-JIT.
- Ran pinned harness on canonical WSL2 host (`wsl2-ultra9-275hx-2026-05`, core 2, 30×1M events): momentum median **5.18×** [4.92×, 5.36×]; all-signals fused median **2.96×** [2.50×, 3.53×]. Artifacts: `bench/results/pinned_host_speedup.{json,md}`.
- Updated `CLAIMS_MATRIX.md` (pinned **Verified** + historical **Historical** row), `RESUME_CLAIMS.md` B1 speedup wording, `EVIDENCE.md`, `NEXT_TASK.md`.
- Reconfigured `build-wsl` at the current repo path (stale CMake cache from moved `JIT/jit-signal-engine` path).

## 2026-05-24 (P1)
- **Root cause:** LLVM O2 could not CSE market `load`s across opaque `jit_rt_*` runtime calls in fused programs.
- **Fix:** entry preload + per-tick `(symbol_id, field)` memoization in `EmitMarketFieldLoad` / `PrewarmProgramMarketLoads` (`src/jit_compiler.cpp`); single-signal compiles keep basic-block-local memo for SIMD dominance.
- **Evidence:** `bench/run_cse_ir_diff.sh` + `cse_load_dedup_test`; fused `filtered_momentum.sig` bid/ask loads **22 → 2** pre-O2 (`bench/results/cse_evidence/cse_diff_verified.md`).
- Promoted `CLAIMS_MATRIX.md` market-load row to **Verified**; updated `RESUME_CLAIMS.md`, `README.md`, `EVIDENCE.md`.

## 2026-05-24 (P2)
- Added `src/multithread_eval.{h,cpp}` (symbol sharding, ST/MT eval, output hash).
- Added `multithread_scaling_benchmark`, `multithread_equivalence_test`, `bench/run_pinned_multithread_scaling.sh`, `bench/pinned_host_check.sh`.
- Pinned 10k-symbol scaling: **5.69×** vs 1 thread at 16 threads, **35.6%** parallel efficiency (`bench/results/multithread_scaling.json`).
- Fixed benchmark `JitCompiler` lifetime bug and bash `SECONDS` variable shadowing in the scaling driver.

## 2026-05-24 (P2 homogeneous scaling)
- Documented P/E core mapping in `bench/PINNED_HOST.md` (`lscpu` MAXMHZ unavailable in WSL; cluster verify via `bench/verify_core_clusters.sh`).
- Parameterized `bench/run_pinned_multithread_scaling.sh` (`--cores`, `--thread-counts`, `--out-json`, `--out-md`).
- Measured P-only scaling: **5.45×** at 6 threads, **90.8%** efficiency (`bench/results/multithread_scaling_pcores.{json,md}`).
- Measured E-only scaling: **6.51×** at 16 threads, **40.7%** efficiency (`bench/results/multithread_scaling_ecores.{json,md}`).
- Hybrid artifact unchanged (`bench/results/multithread_scaling.{json,md}`); `CLAIMS_MATRIX.md` + `RESUME_CLAIMS.md` updated.

## 2026-06-09 (operator lowering Phase 3 — closed)
- Added `SignalContext::lowered_bases` (offset-0 POD) + `RefreshLoweredStateBases`; JIT loads lowered array bases via GEP instead of `jit_rt_*_lowered_base` extern calls.
- Added `bench/hw_reference.{h,cpp}` and wired per-signal hw baselines into `signal_benchmark`.
- Added `bench/run_lowering_gap_phase3.sh`; artifacts under `bench/results/lowering_gap_phase3/` (fused JIT÷hw **1.42×**, spread **1.05×**); `phase3_conclusion.md` documents Phase 0→3 delta.
- Re-ran `runtime_call_profile` on `filtered_momentum` (40M events): `jit_rt_*` **80.2% → 2.8%**, no `_lowered_base` symbols in `kAll` profile.
- Cross-signal structural stateful CSE attempted then reverted (breaks `stateful_subtree_dedup_test` / per-signal `Evaluate` semantics). Phase 4 (lowered+vec) deferred (`RejectInVector`).
- Verification: `ctest --test-dir build-wsl` — **36/36** pass.

## 2026-06-09 (operator lowering Phase 4 — parity complete, perf partial)
- Lifted lowering⊥vector mutual exclusion: P4 `LaneEmitScope` per-lane lowered fan-out in `src/jit_compiler.cpp` (`per_lane_scalar_emit`); all lowered ops compile in `CompileProgramVectorized`.
- Extended `vectorized_stateful_parity_test` (`kAll` cases) and `vectorized_lanes_parity_test` (positive lowered-compile cases).
- Added `cross_symbol_benchmark --phase4` three-way table; artifact `bench/results/stateful_vec_lowering_speedup.md`: vec+`kAll` **2.13×** vs vec+`kNone`, **0.57×** vs scalar+`kAll` on `filtered_momentum` (K=4).
- True K-wide SIMD ring-buffer state not attempted; documented as future work.
- Verification: `ctest --test-dir build-wsl` — **36/36** pass; `fuzz_parity_simd_test`, `vectorized_*_parity_test` green.

## 2026-06-09 (operator lowering prompt completion)
- Extended `vectorized_stateful_parity_test` `kAll` cases to all stateful primitives (zscore, lag, cross, corr, beta, kalman, ema_of_zscore).
- Added `bench/run_stateful_vec_lowering_phase4.sh` for reproducible P4 benchmark artifact.
- Synced `PROJECT_CONTEXT.md`, `context/PROJECT_CONTEXT.md`, `context/BATON.md`, `context/EVIDENCE_MAP.md`, `OPERATOR_LOWERING_TASK.md`, `NEXT_TASK.md` — operator lowering marked **DONE**.
- Verification: `ctest --test-dir build-wsl` — **36/36** pass.

## 2026-06-09 (full context + markdown sync for operator lowering)
- Updated narrative docs for Phases 0–4 completion: `README.md`, `ARCHITECTURE.md`, `docs/architecture.md`, `docs/agent_architecture.md`, `docs/benchmarks.md`, `docs/cross_symbol_vectorization_stateful.md`, `docs/runtime_call_profile.md`, `WALKTHROUGH.md`, `RESUME_CLAIMS.md`, `PROJECT_CONTEXT.md`, `context/*`, `AGENTS.md`, `CLAUDE.md`, `jit-signal-engine_upgrades.md`, `bench/results/avx2_speedup/README.md`.

## 2026-05-27 (docs sync + P11–P15 follow-ons)
- Landed Welford O(1) `rolling_std` with periodic buffer refresh; `welford_stddev_parity_test` in CTest (36 targets with LLVM).
- Added `examples/stateless_compute_heavy.sig`, `bench/results/avx2_speedup/` (~2.62× vec vs scalar JIT at K=4, pinned).
- Added `src/spsc_ring.h`, `spsc_ring_test`, `spsc_jit_pipeline_bench`, `bench/results/spsc_pipeline/` (p50 ~228 ns spread enqueue→signal at 2 MHz).
- Stabilized `runtime_call_profile_test`; fixed `CodegenContext::stateful_emit_cache` init warnings.
- Synced narrative markdown: `README.md`, `EVIDENCE.md`, `RESUME_CLAIMS.md`, `PROJECT_STATE.md`, `PROJECT_CONTEXT.md`, `CLAIMS_MATRIX.md`, `ARCHITECTURE.md`, `docs/benchmarks.md`, `docs/simd_candidates.md`, `docs/cross_symbol_vectorization.md`, `docs/agent_architecture.md`, `AGENTS.md`, `context/*`, `PROJECT_ROADMAP.md`, `NEXT_TASK.md`, `context/BATON.md`.
- Verification: `ctest --test-dir build-wsl --output-on-failure` — 36/36 pass (serial).
