# RESUME_CLAIMS.md

## Purpose
Token-efficient source of truth for resume-facing project claims in this repo. Wording here matches the preferred three-bullet shape (readable metrics, not acronym-heavy). See `EVIDENCE.md` and `CLAIMS_MATRIX.md` for artifact links and verification status.

## Resume Bullets (Verified)

Project: **JIT-Compiled Signal Evaluation Engine**

- Built a C++ signal engine with an LLVM JIT that compiles a custom trading DSL to native x86-64, delivering **7.32x** higher throughput than an interpreted baseline on the pinned-host single-signal benchmark and **15.56x** on the fused all-signals benchmark (`bench/results/pinned_host_speedup.md`, `bench/PINNED_HOST.md`)
- Fused multi-signal programs into a single compiled function per market tick, cutting redundant bid/ask loads from 22 to 2, and scaled evaluation to 10,000 symbols across 6 CPU cores at ~91% parallel efficiency (5.5× vs single-thread with matching single-threaded output; `multithread_equivalence_test`, `bench/results/multithread_scaling_pcores.md`)
- Validated JIT output with automated parity, fuzz, and pandas oracle tests on recorded market data and fixed a whole-program inlining bug that caused cross-signal state to alias incorrectly (`CLAIMS_MATRIX.md`, `test/stateful_subtree_dedup_test.cpp`)

## Supporting Claims (use when you have a fourth bullet or in interviews)

| Topic | Safe headline | Primary evidence |
|---|---|---|
| Per-tick JIT latency | Low 20s of ns on `spread_signal` (standalone call) | `bench/results/latency/spread_signal_latency_histogram.md`, `latency_bench` |
| Vectorized multi-symbol JIT | ~2.6× vs scalar JIT on compute-heavy stateless programs (K=4) | `bench/results/avx2_speedup/`, `vectorized_lanes_parity_test` |
| Live ingest pipeline | ~228 ns p50 enqueue→signal (stateless), ~280 ns (stateful) at 2 MHz pacing | `bench/results/spsc_pipeline/`, `spsc_ring_test` |
| `rolling_std` runtime | O(1) Welford + periodic buffer refresh vs two-pass reference | `welford_stddev_parity_test`, `src/runtime.cpp` |
| Persistent JIT cache | Post-O2 bitcode reuse across runs | `jit_module_cache_test` |
| Compiled gradients + calibration | Used compiled whole-program gradients to drive Adam on a deterministic fit-to-target fixture; objective improved **0.06167875 -> 0.00123237** | `bench/results/autodiff/calibration_fixture_fit.md`, `calibration_smoke_test` |
| Developer tooling | `jitse fmt` / `jitse lint` round-trip safe formatter | `dsl_formatter_roundtrip_test` |
| Stateful-op IR lowering | Lowered all rolling/stateful ops to inline IR; cut opaque `jit_rt_*` hot-path share ~80%→3% on fused program; closed JIT÷hand-written C++ gap to **1.42×** | `lowering_gap_phase3/`, `runtime_call_profile` on `filtered_momentum` |
| Lowered + vectorized (P4) | Parity under `kAll`; vec+lowered **~2.1×** vs opaque fan-out on stateful program (does not beat scalar-lowered without K-wide SIMD state) | `stateful_vec_lowering_speedup.md`, `vectorized_stateful_parity_test` |

## Evidence Status

- Canonical JIT-vs-interpreter speedup: pinned host only (`bench/run_pinned_speedup.sh`, `bench/results/pinned_host_speedup.{json,md}`).
- Historical `bench/results.csv` / Windows CSV speedups: different host — label **Historical**, not canonical.
- Cross-symbol vectorized speedup is **vs scalar JIT**, not vs interpreter; program-dependent (see `bench/results/avx2_speedup/README.md`).
- SPSC pipeline tail percentiles on WSL2 are OS-noise dominated; lead with **p50** and the decomposition in `bench/results/spsc_pipeline/README.md`.
- **38** CTest targets when LLVM is enabled (serial `ctest` recommended; a few tests use temp dirs and can flake under `-j` parallelism).

## Scrutiny Checklist

- Confirm each metric has reproducible command + artifact.
- Check `EVIDENCE.md` before adding public claims.
- Map correctness claims to explicit tests.
- Prefer one strong metric per bullet over metric stacking.
