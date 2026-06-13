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

## 3) Stateful operators: opaque runtime vs inline lowering

- Production default (`StatefulLoweringFlags::kAll`): all 14 stateful ops emit **inline lowered IR**; state bases via `SignalContext::lowered_bases` GEP.
- Legacy opaque path (`kNone` / `JITSE_LOWER_STATEFUL=none`): `jit_rt_*` C++ helpers — kept for differential oracle and parity reference.
- Rationale for starting with runtime calls: correctness isolation (e.g. `rolling_std` cancellation bug caught in C++). Lowering closed fused JIT÷hw gap to **1.42×**; profile `jit_rt_*` **80% → ~3%** on `filtered_momentum`.

## 4) EMA cold start behavior

- First sample initializes EMA state directly: `ema_0 = x_0`.
- Subsequent samples use:
  - `alpha = 2 / (period + 1)`
  - `ema_t = alpha * x_t + (1 - alpha) * ema_{t-1}`.

## 5) Why Welford/running-stat approach over naive variance

- Naive formula `E[x^2] - E[x]^2` is numerically unstable due to cancellation.
- Running-stat/ring-stat approach is more stable for similar-magnitude price series.

## 6) MarketState layout rationale

- Fixed-size array: `instruments[id]` in [`runtime.h`](/C:/Users/bhask/Documents/PROJECTS/jit-signal-engine/src/runtime.h).
- Integer ID lookup happens once at compile/parse stage, not in hot path.
- `alignas(64)` on `InstrumentState` improves cache-line behavior and reduces false sharing in the sharded multi-thread path (`multithread_eval`, one `MultiSymbolSignalContext` per thread).

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

- LLVM backend availability is environment-dependent on some hosts.
- Residual `jit_rt_symbol_ctx` (~3% on fused `filtered_momentum` profile) and other non-stateful externs.
- Cross-signal **structural** stateful CSE blocked on fused `Evaluate` parity (attempted, reverted).
- P4 vec+lowered does not beat scalar-lowered on stateful programs without K-wide SIMD ring state.

## 10) What to optimize next

1. K-wide SoA lowered ring buffers for vectorized stateful ops (P4 perf stretch).
2. Profile-guided optimization beyond `assume_warm` tiering.
3. Bare-metal SPSC/latency rerun for tighter p99 claims (WSL scheduler noise).

## 11) Multi-leg signals extension path

- Keep current dependency graph (`signal_program`) and evaluate signals in topological order.
- Introduce per-tick cache map keyed by AST node or lowered expression ID.
- Expose outputs as named references for downstream signal nodes.

## 12) Compilation latency vs runtime speed tradeoff

- Higher optimization increases compile time but lowers per-tick cost.
- In production, choose pass level by use case:
  - research iteration: fast compile / lower opt,
  - deployed strategy: slower compile / higher opt.
