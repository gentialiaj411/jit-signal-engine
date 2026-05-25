# CSE / market-load dedup report (verified)

## Program
- Path: `examples/filtered_momentum.sig`
- Mode: `--all-signals` (fused `CompileProgram`)

## Environment
- Git commit: `659f180d1a40f943f0a4ad4e2b543fde24de7a9d`

## Artifacts
- `bench/results/cse_evidence/before.ll` (pre-O2 IR)
- `bench/results/cse_evidence/after.ll` (post-O2 IR)

## Market bid/ask load counts

| Stage | Bid/ask `load double` count |
|---|---:|
| Pre-O2 (emitter + memoization) | 2 |
| Post-O2 LLVM | 2 |
| Prior artifact (pre-memoization) | 22 |

## Mechanism
LLVM CSE cannot fold loads across opaque `jit_rt_*` calls or dependency-inlined control-flow blocks. The fused JIT emitter preloads each used symbol's `bid`/`ask` once at function entry and reuses those SSA values for all later `mid()`/`bid()`/`ask()` lowering (including inlined `then` branches).

## Conclusion
Post-memoization IR shows **2** market bid/ask loads for this fused case (down from **22**). Market-data read deduplication in whole-program JIT is **Verified** for `filtered_momentum.sig`.
