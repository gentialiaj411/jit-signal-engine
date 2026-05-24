# AUDIT_LOG.md

## 2026-05-16
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

