# SIMD Candidate Operators (Phase 2)

Based on [operator_taxonomy.md](C:/Users/bhask/Documents/PROJECTS/JIT/jit-signal-engine/docs/operator_taxonomy.md), rolling operators with reduction-style work are:

- `sma`: rolling mean reduction over the window buffer.
- `rolling_std`: rolling variance/stddev reduction (sum + sumsq style).
- `zscore`: rolling mean/stddev reduction plus normalization.

Phase 2 implementation scope in this run:

- Implemented explicit LLVM AVX2 vector IR lowering for `sma`.
- Kept scalar fallback for non-AVX2 hosts and forced-scalar mode.
- `rolling_std` / `zscore` remain scalar runtime-call lowering in this phase.
- Current benchmark refresh shows AVX2 still trails scalar JIT on `sma(64)` and `sma(128)`, with `zscore(128)` effectively tied, so this work documents vectorization support but not a measured speedup.
