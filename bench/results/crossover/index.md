# P5 — Compile-vs-interpret crossover artifacts

Where does the JIT actually start paying for itself? This directory
answers that with per-signal numbers, not back-of-the-envelope arguments.

## How these were produced

[`bench/compile_runtime_crossover.cpp`](../../../bench/compile_runtime_crossover.cpp)
measures three things on a fixed program:

1. **Compile time**, broken down by phase using the new
   `JitCompiler::LastCompileTimings()` accessor (P5). The phases map to
   the natural cost centers in an LLVM-based JIT:
   - **AST → IR emission**: walking the SignalDef AST, building the
     llvm::Module, verifyFunction().
   - **LLVM O2 pipeline**: `PassBuilder::buildPerModuleDefaultPipeline(O2)`
     — SROA, GVN, instcombine, the inliner, the loop unroller, etc.
   - **ORC codegen + lookup**: LLJIT's machine-code generation
     (ISel + register allocation + executable memory write). The first
     `lookup()` blocks until codegen completes; that's what we time.
2. **Interpreter per-event time** at steady state (5% warmup, then a
   tight closed loop of `interpreter.Evaluate(...)` over the rest).
3. **JIT per-event time** at steady state (same shape, with the JIT
   function pointer instead).

Each measurement is best-of-K (compile K=7, events K=5) to mitigate
scheduler/TurboBoost noise. Best is used (not mean) because all noise on
this kind of measurement is one-sided.

The crossover is the textbook one-step calc:

```
N* = T_compile / (per_event_interp − per_event_jit)
```

If `per_event_jit >= per_event_interp` (no per-event win), the crossover
is reported as **NONE** rather than negative-or-infinite-and-misleading.

## Headline numbers (this host)

| Signal | T_compile (best of 7) | interp ns/evt | JIT ns/evt | per-event speedup | **N\*** |
|---|---:|---:|---:|---:|---:|
| `spread_signal`     | 1.31 ms | 8.24 ns | 2.33 ns | 3.54× | **222 k** |
| `filtered_momentum` | 3.34 ms | 229 ns  | 101 ns  | 2.28× | **26 k**  |
| `profile_canonical` | 5.04 ms | 308 ns  | 121 ns  | 2.54× | **27 k**  |

### What these mean in plain language

- For `filtered_momentum`, **don't bother compiling unless you'll
  process at least 26 000 events.** Below that the interpreter (no
  compile cost) wins.
- For `spread_signal`, the per-event speedup is huge (3.5×) but the
  per-event budget is tiny (a few ns), so it takes 220 k events to
  amortize the 1.3 ms of compile work. This is the right answer — a
  one-line stateless arithmetic signal is in the "fast path is so
  short, compile cost dwarfs anything" regime.
- For `profile_canonical` (sma + ema + lag + rolling_std), the
  crossover is essentially the same as `filtered_momentum` despite the
  larger compile, because the per-event interp cost is also higher.

### How the compile budget is spent

For every signal here ORC codegen + lookup is the largest of the three
phases (~70% of total). LLVM O2 is ~25%. AST→IR is ~5%. If we wanted
to reduce JIT compile time, ORC codegen is where to look first — but at
1–3 ms for these programs it's not actually a problem; the artifact
exists to *quantify* the tradeoff, not motivate optimizing it.

## Layout

For each signal:

- `{stem}_crossover.csv` — tabular cumulative-cost samples for re-plotting
- `{stem}_crossover.md` — summary table + breakeven formula + plain-language interpretation
- `{stem}_crossover.svg` — log-log plot of cumulative wall time vs
  events; the green dashed line + dot marks N\*

## How to regenerate

```bash
make -j8 compile_runtime_crossover
bash bench/run_crossover_artifacts.sh
```

The smoke gate
[`test/compile_timings_test.cpp`](../../../test/compile_timings_test.cpp)
asserts that the instrumentation reports nonzero values for each phase
and that the sum approximately equals total — a non-numeric
"instrumentation works" gate. Numeric results are not gated because
they're host-dependent.

## Methodology and limitations

Full discussion: [`docs/compile_runtime_crossover.md`](../../../docs/compile_runtime_crossover.md).
The short version:

- The very first `JitCompiler` in a process pays a one-time LLVM
  initialization tax (target machine bring-up, MCJIT pipeline setup)
  that is unrelated to program size. The artifact does an explicit
  warmup compile before measuring, so the reported `T_compile` reflects
  the *per-program* steady-state cost, not the first-time-ever cost. In
  production code that does a single JIT compile per session, you'd add
  the warmup ~10 ms to the budget.
- These numbers are on a non-isolated WSL2 host with the default
  governor. On a pinned host the per-event numbers tighten by 10-30%;
  the compile timings shift by less because they're CPU-bound on a
  predictable codepath.
