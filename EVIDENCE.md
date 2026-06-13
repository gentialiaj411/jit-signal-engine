# EVIDENCE.md

Purpose: compact map from public project claims to the tests, source files, and artifacts that support them.

## Correctness

| Claim | Evidence |
|---|---|
| DSL parser exists and handles the supported grammar | `src/lexer.*`, `src/parser.*`, `lexer_test`, `parser_smoke_test` |
| DSL supports explicit scalar parameter declarations and formatter/lint round-trip for them | `src/signal_program.*`, `src/dsl_formatter.*`, `parser_smoke_test`, `dsl_formatter_roundtrip_test` |
| Interpreter is the semantic reference | `src/interpreter.*`, `interpreter_test`, `runtime_test` |
| JIT agrees with the interpreter | `jit_test`, `fuzz_parity_test` |
| Math built-ins have JIT/interpreter parity coverage | `jit_test` covers `abs`, `log`, and `sqrt` |
| Parameter gradients match finite-difference oracle checks across stateless and stateful operators | `gradient_parity_test` |
| Compiled gradients agree with interpreted gradients at tight tolerance | `gradient_parity_test` (compiled-vs-interpreted rel `1e-12` or abs `1e-12`) |
| Fixture-backed calibration improves a deterministic objective using the compiled gradient path | `jitse_calibrate`, `calibration_smoke_test`, `bench/results/autodiff/calibration_fixture_fit.{md,meta.json}` |
| SIMD path preserves scalar JIT semantics | `fuzz_parity_simd_test` |
| Multi-symbol execution preserves single-symbol semantics | `fuzz_parity_multisymbol_test` |
| Recorded-data/oracle workflow has deterministic gates | `backtest_determinism_test`, `differential_oracle_test` |

## Runtime Design

| Claim | Evidence |
|---|---|
| Stateful operators use stable integer `node_id` slots | `src/signal_program.*`, `src/runtime.*`, `node_state_layout_test` |
| Hot path avoids hash-map lookups for runtime state | `SignalContext` vectors in `src/runtime.h`, `node_state_layout_test` |
| Warmed evaluation avoids heap allocation | `hot_path_allocation_test` (interpreter, forward JIT, compiled forward+gradient JIT) |
| One compiled program can run across multiple symbols | `MultiSymbolSignalContext`, `fuzz_parity_multisymbol_test` |

## Performance Artifacts

| Artifact | Meaning |
|---|---|
| `bench/results/pinned_host_speedup.json` | Canonical pinned-host JIT vs interpreter speedups (30 repeats, bootstrap CIs). |
| `bench/results/pinned_host_speedup.md` | Headline pinned-host speedup table for resume claims. |
| `bench/PINNED_HOST.md` | Canonical host fingerprint, toolchain, and reproduce commands. |
| `bench/run_pinned_speedup.sh` | Host-validated driver for pinned speedup artifacts. |
| `bench/results/cse_evidence/cse_diff_verified.md` | Verified fused-program market load dedup (22→2 loads). |
| `bench/run_cse_ir_diff.sh` | Regenerates CSE IR artifacts and verified load-count report. |
| `bench/results/multithread_scaling_pcores.{json,md}` | P-core-only scaling (**5.45×** / **90.8%** at 6T) — resume headline. |
| `bench/results/multithread_scaling.{json,md}` | Hybrid P+E scaling (**5.69×** / **35.6%** at 16T). |
| `bench/results/multithread_scaling_ecores.{json,md}` | E-core-only scaling (**6.51×** / **40.7%** at 16T). |
| `bench/run_pinned_multithread_scaling.sh` | Pinned 60s/run scaling driver (`--cores`, `--thread-counts`, `--out-json`, `--out-md`). |
| `bench/PINNED_HOST.md`, `bench/detect_hybrid_cores.sh`, `bench/verify_core_clusters.sh` | Core mapping and reproduce commands. |
| `multithread_equivalence_test` | ST vs MT output hash parity for sharded execution. |
| `bench/results.csv` | WSL/Linux Release benchmark for interpreter/JIT/hardcoded paths (latency; historical speedup context). |
| `bench/results.csv.meta.json` | Provenance for `bench/results.csv`. |
| `bench/results_windows.csv` | Windows-native Release benchmark. |
| `bench/results_windows.csv.meta.json` | Provenance for `bench/results_windows.csv`. |
| `bench/results_multisymbol.csv` | Multi-symbol scaling benchmark through 10,000 symbols. |
| `bench/results_multisymbol.csv.meta.json` | Provenance for `bench/results_multisymbol.csv`. |
| `bench/results_simd.csv` | Per-operator SMA AVX2 vs scalar JIT (historical; SMA windows often memory-bound). |
| `bench/results_simd.csv.meta.json` | Provenance for `bench/results_simd.csv`. |
| `bench/results/avx2_speedup/` | Cross-symbol vectorized JIT vs scalar JIT (K=4); **~2.6×** on `stateless_compute_heavy.sig`, **~1.4×** on `stateless_heavy.sig`. |
| `bench/results/stateful_vec_speedup.md` | Cross-symbol vectorized JIT vs scalar JIT on stateful `filtered_momentum.sig` (K=4, opaque `jit_rt_*` fan-out). |
| `bench/results/stateful_vec_lowering_speedup.md` | P4 three-way: scalar+`kAll` vs vec+`kAll` vs vec+`kNone` on `filtered_momentum.sig` (K=4, `--phase4`). |
| `bench/results/rolling_std_lowering_speedup.md` | P0 `rolling_std` lowered fused-program speedup artifact (`signal_benchmark`, pinned core 2). |
| `bench/results/lowering_gap_phase3/phase3_table.md` | Phase 3 JIT÷hw gap with per-signal `hw_reference` baselines and ctx GEP lowered bases (fused **1.42×**). |
| `bench/results/lowering_gap_phase3/phase3_conclusion.md` | Phase 0→3 before/after table, perf profile delta, deferred items. |
| `bench/run_lowering_gap_phase3.sh` | Regenerates Phase 3 gap matrix + IR lowered-base check. |
| `bench/run_stateful_vec_lowering_phase4.sh` | Regenerates P4 three-way vec+lowering benchmark artifact. |
| `bench/results/autodiff/phase3_forward_gradient_tick.md` | Pinned-host forward-only vs compiled forward+gradient tick cost for one parameterized fused program (**77.26 ns/tick** vs **147.06 ns/tick**, **1.90×** on the measured host). |
| `bench/results/autodiff/phase3_forward_gradient_tick.meta.json` | Host/build/command provenance for the Phase 3 compiled-gradient benchmark artifact. |
| `bench/results/autodiff/calibration_fixture_fit.md` | Phase 4 fixture-backed calibration report: objective **0.06167875 -> 0.0012323651** under Adam using the compiled whole-program gradient path. |
| `bench/results/autodiff/calibration_fixture_fit.meta.json` | Host/build/command provenance for the Phase 4 calibration artifact. |
| `bench/hw_reference.{h,cpp}` | Per-signal hand-written C++ tick baselines for `signal_benchmark`. |
| `bench/results/spsc_pipeline/` | SPSC ring + JIT eval pipeline latency (enqueue→signal); p50 **~228 ns** (spread), **~280 ns** (filtered_momentum) at 2 MHz. |
| `bench/results/vec_thread_composition/` | Vector × thread composition (stateless vs stateful). |
| `bench/results/latency/` | Per-call and CO-aware latency histograms (`latency_bench`). |
| `src/spsc_ring.h`, `test/spsc_ring_test.cpp` | Lock-free SPSC ring correctness + alloc discipline. |
| `test/welford_stddev_parity_test.cpp` | Welford `rolling_std` vs two-pass reference. |
| `test/stateful_subtree_dedup_test.cpp` | P11 inliner + stateful dedup regression. |
| `test/jit_module_cache_test.cpp` | Persistent post-O2 bitcode cache. |
| `test/dsl_formatter_roundtrip_test.cpp` | `jitse fmt` round-trip + idempotency. |
| `ARCHITECTURE.md` | End-to-end pipeline reader's guide. |

Current safe performance wording:

- **Pinned host speedup (canonical, lowering default `kAll`):** momentum median **7.32×** [7.29×, 7.48×]; all-signals fused median **15.56×** [15.52×, 15.58×] (`bench/results/pinned_host_speedup.md`). Lowering-off historical fused **5.98×** / **2.96×** context: `bench/results/lowering_gap_baseline/baseline_table.md`.
- WSL latency artifact: JIT p99 spans 14-78 ns across measured rows in `bench/results.csv`.
- Windows latency artifact: JIT p99 spans 10-48 ns across measured rows in `bench/results_windows.csv`.
- Treat all numbers as artifact-specific; speedup ratios must cite the pinned host unless labeled historical.

## Backtest And Oracle Evidence

| Artifact | Meaning |
|---|---|
| `bench/results/backtest/phase4_mdh_20260523/signals.csv` | Recorded-data signal output artifact. |
| `bench/results/backtest/phase4_mdh_20260523/ic_report.json` | Deterministic IC report at fixed horizons. |
| `bench/results/diff_test/divergence_report.md` | Human-readable JIT-vs-reference oracle comparison. |
| `bench/results/diff_test/divergence_report.json` | Machine-readable oracle comparison. |
| `bench/run_diff_oracle.sh` | Reproducible oracle driver. |
| `test/reference_oracle/` | pandas reference and fixture generation scripts. |

Backtest IC should be described as methodology and determinism evidence, not proof of alpha.
The Phase 4 calibration artifact is fixture-backed on purpose; recorded-data IC calibration remains blocked at HEAD and should not be claimed as working.

## Known Limits

- JIT availability depends on LLVM being found at configure/build time.
- Per-operator SIMD (`bench/results_simd.csv`) does not always beat scalar JIT; **cross-symbol** vectorization can win on FP-heavy stateless programs — see `bench/results/avx2_speedup/README.md` (program-dependent).
- Stateful-heavy vector mode is for correctness and threading composition, not a universal per-tick speedup over scalar.
- Fused JIT market load dedup is verified for `filtered_momentum.sig` (22→2 bid/ask loads pre-O2): `bench/results/cse_evidence/cse_diff_verified.md`, `cse_load_dedup_test`.
- SPSC pipeline p99+ on WSL2 reflects host scheduler noise; treat p50 and the README decomposition as the honest headline.
- Benchmark values should always include artifact, host, build type, and command.
- Run `ctest` serially when debugging flakes (`jit_module_cache_test`, `backtest_determinism_test` use temp/output paths).
