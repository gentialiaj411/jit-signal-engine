# RESUME_CLAIMS.md

## Purpose
Token-efficient source of truth for resume-facing project claims in this repo.

## Resume Bullets (Verified - see bench/results.csv, CLAIMS_MATRIX.md, and new allocation/node-state tests)
Project: JIT-Compiled Signal Evaluation Engine

- Built a C++20 JIT compiler that translates streaming signal expressions into optimized x86-64 machine code via LLVM ORC, with benchmark artifacts showing JIT p99 latency in the 14-78 ns range on the current WSL run and 10-48 ns on the current Windows run (`bench/results.csv`, `bench/results_windows.csv`)
- Built pandas-based differential oracle on recorded NASDAQ ITCH data validating JIT signal output to >=99.7% within tolerance (`rtol=1e-6`) across 5 signals; caught and fixed cross-signal node-ID state collision uncovered by oracle (`CLAIMS_MATRIX.md`: "JIT signal output compared against pandas reference oracle...")
- Designed a dependency-aware compilation pass that compiles all 14 financial operators into a single native function per tick, while keeping market-data CSE claims explicitly artifact-bound
- Eliminated runtime hash-table lookups and heap allocation from compiled signal evaluation by lowering signal nodes onto stable integer IDs with preallocated node-indexed state arrays and rolling-window ring buffers

## Evidence Status
- Quantitative performance claims are benchmark-environment dependent; keep host/setup context with every metric.
- Node-indexed state and no-allocation hot-path claims are now directly covered by `node_state_layout_test` and `hot_path_allocation_test`.

## Scrutiny Checklist
- Confirm each metric/percentile/throughput claim has reproducible command + artifact.
- Confirm correctness claims map to explicit tests.
- Confirm implementation nouns in bullets exist in code paths.
- Replace or soften any claim that lacks direct evidence.

## Optimization Guidance
- Prefer one strong, reproducible metric per bullet over multiple weak metrics.
- Keep bullets outcome-first, mechanism-second, evidence-third.
- If evidence is partial, rewrite with scoped language (e.g., "in current benchmark setup").

## Candidate Upgrade (Phase 1-5)
- Built a C++20 signal DSL engine with LLVM ORC whole-program JIT fusion, verified interpreter/JIT parity via fuzz testing, and measured JIT p99 latency between 14-78 ns on the current WSL benchmark artifact (`bench/results.csv`).
- Refactored the runtime to multi-symbol struct-of-arrays evaluation (single compile, symbol-indexed execution), validated parity at 64 symbols, and benchmarked scaling from 1 to 10,000 symbols with end-to-end throughput/latency artifacts (`bench/results_multisymbol.csv`).
- Added explicit AVX2-gated vectorized rolling-window codegen with scalar fallback, plus dedicated scalar-vs-vector parity tests and per-operator SIMD benchmark artifacts (`bench/results_simd.csv`).
- Integrated with a sibling market-data pipeline by consuming canonical `mf::core::BookEvent` journals and producing deterministic per-signal IC reports on recorded data at 1/5/30-tick horizons (`bench/results/backtest/phase4_mdh_20260523/ic_report.json`).
