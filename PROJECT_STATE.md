# PROJECT_STATE.md

## Current Snapshot (2026-06-13)

`jit-signal-engine` is a mature signal DSL + LLVM JIT engine: P0-P15 roadmap items are done, elite-plan P0-P2 is done, and recent work added Welford `rolling_std`, cross-symbol AVX2 speedup artifacts, SPSC live-ingest benchmarking, module cache, tiered specialization, fmt/lint tooling, and autodiff Phases 1-4 through a fixture-backed calibration driver using compiled whole-program gradients under the current **38**-target LLVM test suite.

## Phase Status

- Phase 1-5 (audit, SIMD, multi-symbol, backtest, mdh): **complete**
- Roadmap P0-P15 (fusion, vec, threads, profiling, tiered JIT, operators, observability, fuzz, DSL, inliner fix, vec x thread bench, module cache, architecture doc, fmt/lint): **complete**
- Resume-upgrade follow-ons (Welford production path, AVX2 wins artifact, SPSC pipeline): **complete**
- Operator lowering Phases 0-4: **complete** (all stateful ops lowered; ctx GEP bases; fused JIT vs hw **1.42x**; P4 per-lane lowered+vec parity green - perf **0.57x** vs scalar-lowered on `filtered_momentum`, see `stateful_vec_lowering_speedup.md`)
- Autodiff task: Phases 0-4 complete (parameterized DSL surface, stateless symbolic gradients, stateful forward sensitivities, compiled whole-program gradient path, finite-difference oracle gate, interpreted-vs-compiled gradient parity gate, fixture-backed calibration driver + CI smoke gate)

## Verified Core Capabilities

- C++20 DSL: lexer, parser, type-check, constant-fold, dependency inline, node-ID allocation (with P11 stateful dedup).
- Parameterized DSL programs via explicit `param` declarations; `jitse fmt` / `jitse lint` preserve them.
- Interpreter oracle + LLVM ORC JIT (`Compile`, `CompileProgram`, `CompileProgramVectorized`).
- Stateless symbolic differentiation plus stateful forward sensitivities over fused parameterized programs, with a compiled whole-program gradient entrypoint gated by `gradient_parity_test`.
- Fixture-backed calibration via `jitse_calibrate`, which optimizes a checked-in fit-to-target objective with Adam using `CompileProgramGradient`; `calibration_smoke_test` gates a minimum objective improvement on every run.
- Whole-program fusion + per-tick market load memoization (`22 -> 2` on `filtered_momentum.sig`).
- Tiered JIT: baseline + warm-specialized compile, atomic fn-pointer swap (`tiered_specialization_parity_test`).
- Multi-thread sharded evaluation: **10k symbols**, **6 P-cores**, **~91%** efficiency, hash-equivalent to ST (`multithread_equivalence_test`).
- Cross-symbol vectorization (K=2/4/8): stateless widened IR; stateful per-lane fan-out (P10); lowered stateful via P4 `LaneEmitScope`; **~2.6x** vs scalar JIT on `stateless_compute_heavy.sig` (`bench/results/avx2_speedup/`).
- `rolling_std` / `zscore`: O(1) Welford readout + periodic recompute; two-pass reference gated by `welford_stddev_parity_test`.
- Persistent post-O2 bitcode module cache (`jit_module_cache_test`).
- Fuzzing: parser + runtime harnesses; CI smoke; libFuzzer + sanitizers locally (`docs/fuzzing.md`).
- Latency: HdrHistogram-style buckets, open/closed-loop (`latency_bench`, `latency_histogram_test`).
- Live ingest: lock-free SPSC ring + producer/consumer pipeline bench (`spsc_ring_test`, `spsc_jit_pipeline_bench`).
- Tools: `jitse fmt`, `jitse lint`; `ARCHITECTURE.md` for end-to-end pipeline.
- Backtest + pandas differential oracle on recorded ITCH; deterministic IC (`backtest_determinism_test`, `differential_oracle_test`).
- Recorded-data IC calibration remains intentionally out of scope for Phase 4 because the checked-in recorded-data IC artifact is still broken at HEAD (all-NaN / zero-sample output); the calibration demo is the deterministic fixture instead.

## Benchmark Reality (headlines)

| Claim | Artifact |
|---|---|
| JIT vs interpreter (momentum) | **7.32x** median [7.29x, 7.48x] - `pinned_host_speedup.md` (lowering `kAll` default) |
| Fused all-signals vs interpreter | **15.56x** median [15.52x, 15.58x] - same |
| Fused all-signals JIT vs hand-written C++ | **1.42x** - `bench/results/lowering_gap_phase3/phase3_table.md` |
| Multi-thread (10k sym, 6 P-cores) | **5.45x**, **90.8%** efficiency - `multithread_scaling_pcores.md` |
| Vec JIT vs scalar JIT (FP-bound) | **~2.62x** at K=4 - `avx2_speedup/stateless_compute_heavy.md` |
| P4 vec+lowered vs vec+opaque (`filtered_momentum`) | **~2.1x** at K=4 - `stateful_vec_lowering_speedup.md` |
| P4 vec+lowered vs scalar-lowered (same) | **~0.55x** - per-lane fan-out ceiling without K-wide SIMD state |
| Standalone JIT call latency | p50 **19 ns**, p99 **22 ns** on `spread_signal` - `bench/results/latency/` |
| SPSC pipeline p50 (2 MHz, spread) | **~228 ns** enqueue->signal - `spsc_pipeline/spread_signal_2mhz.md` |
| Compiled forward+gradient vs forward-only tick | **147.06 ns** vs **77.26 ns** (**1.90x**) - `bench/results/autodiff/phase3_forward_gradient_tick.md` |
| Fixture-backed calibration objective | **0.06167875 -> 0.00123237** - `bench/results/autodiff/calibration_fixture_fit.md` |

## Still Optional

- Further profiler-driven lowering of remaining runtime helpers.
- Native Linux bare-metal rerun of SPSC/latency tails (WSL adds scheduler noise).

## Claim Boundary

- Not a live trading platform; backtest IC is methodology, not alpha.
- The Phase 4 calibration claim is only that differentiable IR + gradient descent improve a checked-in fixture objective through the compiled gradient path; it does not claim recorded-data IC calibration works.
- Speedups and latency are host- and program-specific; cite the artifact path.
- `bench/results_simd.csv` (per-op SMA AVX2) is historical; cross-symbol vec wins are in `bench/results/avx2_speedup/`.
