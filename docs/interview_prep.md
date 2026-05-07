# Interview Prep (Deep Dive)

## 1) Why ORC JIT v2 over MCJIT?

- ORC v2 is LLVM's modern JIT API and actively maintained.
- Better symbol/materialization model and cleaner embedding in long-running systems.
- MCJIT is legacy and not where LLVM performance/runtime work is going.

## 2) DSL string -> native execution path

1. Read `.sig` source (`src/main.cpp`).
2. Tokenize with lexer (`src/lexer.cpp`).
3. Parse into AST (`src/parser.cpp`).
4. Parse multi-signal program and inline dependencies (`src/signal_program.cpp`).
5. Build symbol table once (ticker -> integer ID).
6. Compile with backend:
   - LLVM available: AST -> LLVM IR -> O2 pipeline -> function pointer.
   - LLVM unavailable: fallback to interpreter.
7. On each market event, update `MarketState` and evaluate function with `SignalContext`.

## 3) Why rolling windows are runtime calls

- Stateful operators (`ema`, `sma`, `rolling_std`, min/max) have mutable state across ticks.
- Keeping state updates in C++ runtime:
  - simplifies correctness testing,
  - avoids duplicating complex mutable logic in codegen,
  - keeps JIT focused on arithmetic/control flow.

## 4) EMA cold start behavior

- First sample initializes EMA state directly: `ema_0 = x_0`.
- Subsequent samples use:
  - `alpha = 2 / (period + 1)`
  - `ema_t = alpha * x_t + (1 - alpha) * ema_{t-1}`.

## 5) Why Welford/running-stat approach over naive variance

- Naive formula `E[x^2] - E[x]^2` is numerically unstable due to cancellation.
- Running-stat/ring-stat approach is more stable for similar-magnitude price series.

## 6) MarketState layout rationale

- Fixed-size array: `instruments[id]` in [`runtime.h`](/C:/Users/bhask/Documents/PROJECTS/JIT/jit-signal-engine/src/runtime.h).
- Integer ID lookup happens once at compile/parse stage, not in hot path.
- `alignas(64)` on `InstrumentState` improves cache-line behavior and avoids false sharing risks in future multithreading.

## 7) Key LLVM passes

- Current pipeline uses LLVM `O2` default module pipeline.
- Practical wins for this workload usually come from:
  - constant folding,
  - inlining,
  - common subexpression elimination,
  - dead code elimination.

## 8) p99 vs p999 latency interpretation

- p50 tracks steady-state hot-path cost.
- p99 captures occasional cache misses/branch variance.
- p999 adds rarer disruptions: allocator noise, OS scheduling, thermal/turbo drift.

## 9) Remaining bottlenecks in this codebase today

- LLVM backend availability is environment-dependent on this Windows setup.
- Stateful runtime calls still dominate some expressions.
- No cross-signal subexpression cache yet.

## 10) What to optimize next

1. Subexpression memoization within one tick (especially shared EMA inputs).
2. Better arena allocation/ID mapping for state slots.
3. Linux perf-guided tuning on realistic event traces.

## 11) Multi-leg signals extension path

- Keep current dependency graph (`signal_program`) and evaluate signals in topological order.
- Introduce per-tick cache map keyed by AST node or lowered expression ID.
- Expose outputs as named references for downstream signal nodes.

## 12) Compilation latency vs runtime speed tradeoff

- Higher optimization increases compile time but lowers per-tick cost.
- In production, choose pass level by use case:
  - research iteration: fast compile / lower opt,
  - deployed strategy: slower compile / higher opt.
