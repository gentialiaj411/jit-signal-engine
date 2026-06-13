# NEXT_TASK.md

## Status

**Operator lowering (`OPERATOR_LOWERING_TASK.md`) — DONE.** Phases 0–4 complete; full `ctest` green.
**Autodiff (`AUTODIFF_TASK.md`) — Phases 1-4 DONE.** Parameterized DSL surface, stateless symbolic gradients, stateful forward sensitivities, compiled whole-program gradients, and the fixture-backed calibration driver with CI objective-improvement gate landed; full `ctest` green.

## Headline Evidence (pinned host)

| Phase | Result | Artifact |
|-------|--------|----------|
| 3 fused JIT÷hw | **1.42×** | `bench/results/lowering_gap_phase3/phase3_table.md` |
| 3 `jit_rt_*` profile drop | **80% → 2.8%** | `bench/results/perf/filtered_momentum/runtime_call_profile.md` |
| 4 parity (lowered+vec) | green under `kAll` | `vectorized_stateful_parity_test`, `vectorized_lanes_parity_test` |
| 4 perf | vec+lowered **2.13×** opaque; **0.57×** scalar-lowered | `bench/results/stateful_vec_lowering_speedup.md` |

Reproduce Phase 4 bench: `bash bench/run_stateful_vec_lowering_phase4.sh build-wsl`

## Next Task

- No additional autodiff implementation phase is planned here. The next relevant follow-on is a separate recorded-data IC fix/re-verification if you want recorded-data calibration to become claimable.
- Keep the recorded-data calibration objective out of scope until `backtest_runner` IC output is source-fixed and re-verified.

## Deferred (documented, not blocking)

- Cross-signal structural stateful CSE (breaks per-signal `Evaluate` parity).
- K-wide `<K x double>` SIMD ring-buffer state (needed for vec+lowered to beat scalar-lowered on stateful programs).
- Bare-metal SPSC/latency rerun (WSL p99 noise).

## Strict Scope

Parity suites must stay green. No commits unless explicitly requested.

## Optional Follow-On Prompts

1. **K-wide lowered `ema` at K=4** — SoA ring state + `vectorized_stateful_parity_test` green; re-benchmark vs scalar-lowered.
2. **Bare-metal latency/SPSC** — tighten p99 claims beyond WSL scheduler noise.
3. **Recorded-data IC fix** — make the backtest objective numerically valid again before attempting any recorded-data calibration claim.
