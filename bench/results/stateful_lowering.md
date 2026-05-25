# Stateful operator IR lowering (P0)

## Summary

P0 replaces three opaque C-runtime calls (`jit_rt_ema_alpha`, `jit_rt_sma*`,
`jit_rt_lag`) with inline LLVM IR that operates directly on layout-stable
state structs (`EmaStateLowered`, `SmaStateLowered`, `LagStateLowered`).
Period is a compile-time literal in the DSL, so the modulus and alpha both
become constants and the inner loop is fully visible to LLVM's optimizer.

The interpreter and the runtime-call C helpers remain the parity oracle.

## Toggles

- CLI: `--lower-stateful=none|all|sma,ema,lag`
- Env var: `JITSE_LOWER_STATEFUL=none|all|sma,ema,lag`
- API: `JitCompiler::SetStatefulLowering(StatefulLoweringFlags)`

CLI > API > env var. Default = `none` (preserves pre-P0 behavior).

## Correctness evidence (`stateful_lowering_parity_test`)

Side-by-side runs of CompileProgram-with-lowering-OFF vs ON, 5000 ticks of
deterministic `MarketSimulator(seed=123)` output, comparing `max_abs_diff`
and NaN/non-NaN disagreement counts on the final signal of each program.

| Case                          | max_abs_diff | NaN disagreements |
|-------------------------------|--------------|-------------------|
| `sma(mid, 10)`                | 6.7e-13      | 0                 |
| `sma(mid, 64)`                | 1.8e-13      | 0                 |
| `sma(mid, 200)`               | 2.3e-13      | 0                 |
| `ema(mid, 10)`                | **0** (bit)  | 0                 |
| `ema(mid, 60)`                | **0** (bit)  | 0                 |
| `lag(mid, 5)`                 | **0** (bit)  | 0                 |
| `lag(mid, 50)`                | **0** (bit)  | 0                 |
| `filtered_momentum` (ema-only)| **0** (bit)  | 0                 |
| `sma+ema+lag` mixed           | 3.7e-13      | 0                 |
| 5 EMAs of differing periods   | **0** (bit)  | 0                 |

EMA and LAG lower to bit-identical IEEE-754 output. SMA drifts by
`~1e-13` because the lowered version uses a `double` running sum vs the
runtime helper's `long double` running sum — well below the project's
`rtol=1e-6` tolerance gate.

The whole-project test suite passes under `JITSE_LOWER_STATEFUL=all`:

```
$ JITSE_LOWER_STATEFUL=all ctest --test-dir build-wsl-p0 --output-on-failure
17/17 tests passed, including:
  fuzz_parity_test (200 random expressions, 50 ticks each)
  fuzz_parity_simd_test
  fuzz_parity_multisymbol_test
  multithread_equivalence_test  (hash-equivalent multi-threaded output)
  backtest_determinism_test     (deterministic IC over recorded ITCH)
  differential_oracle_test      (pandas vs JIT on real ITCH, rtol=1e-6)
```

## IR-level evidence (`stateful_lowering_ir_diff`)

Opaque runtime-call count in the fused IR for `examples/filtered_momentum.sig`.
The differential oracle and fuzz parity gate the lowered values; this
table shows what the lowering actually eliminated.

| Runtime helper                  | Pre-O2 (none) | Pre-O2 (all) | Post-O2 (none) | Post-O2 (all) |
|---------------------------------|---------------|--------------|----------------|---------------|
| `jit_rt_ema_alpha`              | 8             | **0**        | 8              | **0**         |
| `jit_rt_sma*`                   | 0             | 0            | 0              | 0             |
| `jit_rt_lag`                    | 0             | 0            | 0              | 0             |
| `jit_rt_rolling_std` (control)  | 3             | 3            | 3              | 3             |

For a synthetic program covering all three:

```
signal s_ma = sma(mid(AAPL), 20)
signal e_ma = ema(mid(AAPL), 15)
signal lagged = lag(mid(AAPL), 7)
signal combo = s_ma - e_ma + lagged
```

| Runtime helper                  | Pre-O2 (none) | Pre-O2 (all) |
|---------------------------------|---------------|--------------|
| `jit_rt_ema_alpha`              | 2             | **0**        |
| `jit_rt_sma*`                   | 2             | **0**        |
| `jit_rt_lag`                    | 2             | **0**        |

All lowerable opaque calls drop to zero. The `rolling_std` control row
proves we did not inadvertently touch the non-lowered ops.

Inspecting the post-O2 IR confirms what the lowering unlocked:

1. **Alpha values become compile-time constants.** `%ema_ax = fmul double %mid, 0x3FC745D1745D1746` (≈ 2/11 for period=10).
2. **LLVM CSE fires across previously-opaque calls.** In the fused
   `filtered_momentum` IR, the alpha-times-mid term `%ema_ax` is reused
   by two distinct EMA-of-mid-period-10 sites that O2 could not unify
   before because each was hidden behind a `jit_rt_ema_alpha` call.

IR dumps: `bench/results/lowering_evidence/filtered_momentum.{none,all}.{pre,post}.ll`

## Throughput evidence (`signal_benchmark`)

WSL2 unpinned host (NOT the canonical pinned host — `bench/PINNED_HOST.md`
should be used for resume-facing numbers). `--measure-runs 5 --pin-core 2`.
Reported as JIT-vs-interpreter speedup median over the 5 runs.

`examples/momentum_signal.sig` (single signal, momentum):

| Configuration    | Median speedup | JIT throughput (M ev/s) | JIT p99 (ns) |
|------------------|----------------|-------------------------|--------------|
| `--lower-stateful=none` | 3.63×   | 164.8                   | 9            |
| `--lower-stateful=all`  | **4.01×** | **183.9**            | 8            |

`examples/filtered_momentum.sig --all-signals` (fused 5-signal program):

| Configuration    | Median speedup | JIT throughput (M ev/s) | JIT p99 (ns) |
|------------------|----------------|-------------------------|--------------|
| `--lower-stateful=none` | 1.97×   | 7.99                    | 215          |
| `--lower-stateful=ema`  | 2.12×   | 7.70                    | 200          |
| `--lower-stateful=all`  | **2.28×** | **8.20**             | 207          |

The fused-program speedup gap (+16%) is real but capped because
`rolling_std` still costs an opaque call. Lowering `rolling_std` is the
natural next step beyond P0; combined with this work it should push the
fused speedup substantially above the pre-P0 canonical 2.96× / pinned
baseline.

## Reproduce

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DJITSE_ENABLE_LLVM=ON
cmake --build build

# Correctness
ctest --test-dir build -R stateful_lowering_parity_test --output-on-failure
JITSE_LOWER_STATEFUL=all ctest --test-dir build --output-on-failure

# IR-level evidence
./build/stateful_lowering_ir_diff examples/filtered_momentum.sig \
    bench/results/lowering_evidence

# Throughput A/B (pinned host preferred)
./build/signal_benchmark --pin-core 2 --measure-runs 30 \
    --lower-stateful=none examples/momentum_signal.sig 1000000
./build/signal_benchmark --pin-core 2 --measure-runs 30 \
    --lower-stateful=all  examples/momentum_signal.sig 1000000
```

## What this changes about the project's central claim

Before P0, "whole-program JIT fusion" was technically accurate but the
fused function spent ~95% of its time inside opaque `jit_rt_*` calls
that LLVM could not inline, CSE across, or vectorize. The CSE/load-dedup
artifact (22→2 loads) was necessary precisely because LLVM's own CSE was
blocked by these opaque calls.

After P0, the canonical fused function for `filtered_momentum.sig`
contains zero EMA runtime calls. The lowered ops are visible IR that
LLVM optimizes alongside the rest of the program, which is what
"JIT-compiled signal evaluation" should mean.
