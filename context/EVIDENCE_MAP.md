# EVIDENCE_MAP.md

## Supported Claims
- Custom DSL parser, type-check, constant-fold, formatter/lint.
- Explicit scalar `param` declarations with source-preserving parse/format round-trip.
- Interpreter + LLVM ORC JIT (single-signal, fused program, vectorized program).
- JIT/interpreter parity (fuzz + deterministic tests).
- Autodiff over parameterized programs, including recurrent/windowed operators, checked against central finite differences.
- Compiled whole-program gradients checked against interpreted gradients at rel `1e-12` / abs `1e-12`.
- Fixture-backed calibration driver uses the compiled whole-program gradient entrypoint and improves a deterministic fit-to-target objective under CI.
- Warmed compiled forward+gradient tick is allocation-free under the existing allocation counter gate.
- Whole-program fusion; market load dedup **22→2** on `filtered_momentum.sig`.
- Multi-thread 10k symbols, ~91% efficiency at 6 P-cores; hash-equivalent sharded output.
- Cross-symbol vectorized JIT ~2.6× vs scalar JIT on FP-heavy stateless program (K=4).
- Stateful lowering composable with cross-symbol vectorization (P4 parity under `kAll`).
- Lowering gap fused JIT÷hw **1.42×** on pinned host (`lowering_gap_phase3/`).
- O(1) Welford `rolling_std` gated vs two-pass reference.
- P11 stateful dedup for inlined duplicate subtrees.
- Tiered JIT (baseline + warm specialization).
- Persistent JIT module cache.
- SPSC ring + pipeline bench (p50 ingest→signal).
- Latency benchmarks (standalone JIT ~19/22 ns spread; pipeline ~228 ns p50).
- Pandas oracle + recorded ITCH; fuzz under sanitizers (local campaigns).

## Risky / Unsupported Claims
- Universal SIMD speedup (use `avx2_speedup/` with program name).
- Universal latency p99 on WSL without host caveat.
- Live trading platform or alpha from IC.
- Historical CSV speedups without **Historical** label.

## Benchmarks and Artifacts
| Artifact | Use |
|---|---|
| `pinned_host_speedup.*` | JIT vs interpreter (canonical) |
| `multithread_scaling_pcores.*` | Resume multi-thread headline |
| `avx2_speedup/` | Vec vs scalar JIT (stateless) |
| `stateful_vec_lowering_speedup.md` | P4 three-way: scalar+`kAll` vs vec+`kAll` vs vec+`kNone` |
| `lowering_gap_phase3/` | JIT÷hand-written C++ gap after Phase 3 |
| `autodiff/phase3_forward_gradient_tick.*` | Forward-only vs compiled forward+gradient tick cost with provenance |
| `autodiff/calibration_fixture_fit.*` | Fixture-backed Phase 4 calibration artifact with objective drop and Adam settings |
| `spsc_pipeline/` | Live ingest latency |
| `latency/` | Per-call + CO-aware latency |
| `cse_diff_verified.md` | 22→2 loads |
| `vec_thread_composition/` | Vec × threads |
| `results_simd.csv` | Per-op SMA only (historical) |

## Resume-Safe Wording (preferred three bullets)
See `RESUME_CLAIMS.md` — ~5× vs interpreter, fusion+10k×6 cores ~91%, parity/fuzz/oracle + inlining state fix. Optional fourth: stateful-op IR lowering (fused JIT÷hw **1.42×**, `jit_rt_*` profile drop on `filtered_momentum`).

## Claims To Avoid
- "AVX2 always faster"
- "Sub-20ns p99 pipeline" on WSL without qualification
- Quoting hybrid 16T efficiency as the main scaling story
