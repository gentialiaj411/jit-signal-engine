# Agent Architecture Brief

Purpose: give an LLM enough architecture context to make safe, focused changes without reading the whole repo. Keep this file compact; use deeper docs only when a task needs evidence or claim wording.

## What This Project Does

`jit-signal-engine` is a C++20 trading-signal DSL engine. It reads `.sig` files, builds an AST, assigns stable runtime state IDs, and evaluates each market tick through either:

- `Interpreter`: correctness reference.
- `JitCompiler`: optional LLVM ORC JIT backend when LLVM is available.

The core invariant is interpreter/JIT parity. Any parser, runtime, or JIT change should preserve matching outputs unless the intended semantic change is explicit and tested.

## End-To-End Flow

```text
.sig source
  -> Lexer
  -> Parser
  -> SignalProgram transforms
       - parse multi-signal files
       - inline dependencies where needed
       - assign stable node_id values to stateful ops
       - bind ticker names to integer symbol_id slots
  -> execution backend
       - Interpreter::Evaluate(...)
       - JitCompiler::Compile(...) for one signal
       - JitCompiler::CompileProgram(...) for fused multi-signal eval
  -> double signal output(s) per tick
```

Runtime inputs are `MarketState` plus per-signal/per-symbol mutable state in `SignalContext` or `MultiSymbolSignalContext`.

## Key Modules

| Area | Files | Responsibility |
|---|---|---|
| AST | `src/ast.h`, `src/ast_clone.*`, `src/ast_utils.*` | Expression tree types and traversal helpers. |
| DSL front-end | `src/lexer.*`, `src/parser.*` | Tokenization and precedence-based parsing. |
| Program assembly | `src/signal_program.*` | Multi-signal parsing, dependency handling, cycle checks, node ID allocation, symbol binding. |
| Runtime state | `src/runtime.*` | Market state, rolling-window state, EMA/VWAP/lag/cross helpers, JIT runtime C ABI hooks. |
| Interpreter | `src/interpreter.*` | Reference semantics for all operators. |
| JIT | `src/jit_compiler.*` | LLVM IR emission, ORC JIT setup, O2 optimization, runtime symbol registration, SIMD lowering. |
| Backend facade | `src/signal_backend.*` | Wrapper used by CLI/benchmarks for compiled execution. |
| CLI | `src/main.cpp` | Reads signals, builds symbols/state, runs eval, optional IR dumps. |
| Simulation | `src/market_sim.*` | Deterministic synthetic events for tests and benches. |
| Backtest | `examples/backtest_runner.cpp` | Replays mdh journals or CSV fixtures, writes `signals.csv` and `ic_report.json`. |

## Runtime State Model

The hot path is node-indexed and symbol-indexed, not map-based.

- `MarketState` stores fixed instrument slots in `instruments[symbol_id]`.
- `SignalContext` stores state vectors such as `ema_states`, `sma_states`, `rolling_std_states`, `vwap_states`, and `lag_states`.
- `SignalContext::lowered_bases` (offset-0 POD) lets the JIT load lowered state array bases via GEP without `jit_rt_*_lowered_base` extern calls (Phase 3).
- Stateful AST nodes carry `FunctionCall::node_id`.
- Market-data calls carry `FunctionCall::symbol_id` after `BindSymbolIds`.
- `MultiSymbolSignalContext` owns one `SignalContext` per output symbol so one compiled program can run across many symbols.
- `PrewarmSignalContext` prepares buffers/state so warmed evaluation can avoid heap allocation.

Do not change node ID allocation, symbol binding, or runtime helper contracts without matching parity/allocation tests.

## Operators

Core DSL operators:

- Market reads: `mid`, `bid`, `ask`, `spread`.
- Rolling/stateful: `ema`, `sma`, `rolling_std`, `rolling_min`, `rolling_max`, `zscore`, `vwap`, `lag`.
- Cross-state: `cross_above`, `cross_below`.
- Math/control: arithmetic, comparisons, `&&`, `||`, `if then else`, plus `abs`, `log`, `sqrt` where supported by the backend path.

Interpreter semantics are the source of truth. If adding or changing an operator, update parser/interpreter/JIT/runtime as needed and add parity tests.

## Execution Paths

### Interpreter

`Interpreter::Evaluate` tree-walks the AST and calls runtime helpers for stateful operators. This is the correctness oracle used by tests.

### Single-Signal JIT

`JitCompiler::Compile` lowers one `SignalDef` to LLVM IR and exposes:

```cpp
double (*)(const MarketState*, MultiSymbolSignalContext*, uint32_t)
```

Production default (`StatefulLoweringFlags::kAll`): stateful operators emit inline lowered IR. Set `JITSE_LOWER_STATEFUL=none` or `SetStatefulLowering(kNone)` for opaque `jit_rt_*` calls (parity reference).

### Fused Program JIT

`JitCompiler::CompileProgram` lowers multiple signals into one native function:

```cpp
void (*)(const MarketState*, MultiSymbolSignalContext*, uint32_t, double* outputs)
```

This is the important performance path: one native call evaluates all signals for a tick. It also resolves signal references to previously emitted values in the same program. Per-tick market bid/ask loads are preloaded once per symbol at function entry and reused (verified **22→2** on `filtered_momentum.sig`; see `cse_diff_verified.md`). Multi-threaded sharded evaluation reuses one compiled program per thread (`multithread_eval`, `multithread_equivalence_test`).

### SIMD and cross-symbol vectorization

- Per-operator AVX2 lowering for some rolling-window paths (`fuzz_parity_simd_test`; historical `bench/results_simd.csv`).
- **Cross-symbol vectorization:** `CompileProgramVectorized` widens stateless ops to `<K x double>`; stateful ops use per-lane fan-out (P10). **P4:** default `kAll` lowering composes via `LaneEmitScope` per-lane inline IR. Stateless: **~2.6×** vs scalar JIT on `stateless_compute_heavy.sig` at K=4 (`bench/results/avx2_speedup/`). Stateful `filtered_momentum`: vec+lowered **~2.1×** vs vec+opaque, **~0.55×** vs scalar-lowered (`bench/results/stateful_vec_lowering_speedup.md`). Parity: `vectorized_lanes_parity_test`, `vectorized_stateful_parity_test`.

### Runtime and ingest

- `rolling_std` uses O(1) Welford + periodic buffer refresh (`welford_stddev_parity_test`).
- `src/spsc_ring.h` + `spsc_jit_pipeline_bench` for producer/consumer ingest latency (`spsc_ring_test`).

### Tooling

- `jitse fmt` / `jitse lint` — canonical formatter and frontend lint (`dsl_formatter_roundtrip_test`).
- `ARCHITECTURE.md` — full pipeline guide (P0–P15 + operator lowering Phases 0–4).

## Backtest Integration

`examples/backtest_runner.cpp` consumes market data, updates `MarketState`, evaluates compiled signals, and writes:

- `signals.csv`
- `ic_report.json`

The mdh integration reads canonical `mf::core::BookEvent` journals when `JITSE_MDH_ROOT` is available. CSV fixture mode exists for deterministic oracle tests.

Backtest IC is methodology/determinism evidence, not a claim of live-trading alpha.

## Test Map

Use the narrowest relevant test first.

| Change area | Tests to run first |
|---|---|
| Lexer/parser grammar | `lexer_test`, `parser_smoke_test` |
| Interpreter/operator semantics | `interpreter_test`, `runtime_test`, `welford_stddev_parity_test` |
| Node IDs / program assembly | `signal_program_test`, `node_state_layout_test`, `stateful_subtree_dedup_test` |
| JIT lowering | `jit_test`, `fuzz_parity_test`, `jit_module_cache_test` |
| SIMD / vec path | `fuzz_parity_simd_test`, `vectorized_lanes_parity_test`, `vectorized_stateful_parity_test` |
| Multi-symbol path | `fuzz_parity_multisymbol_test`, `hot_path_allocation_test` |
| SPSC ring / pipeline | `spsc_ring_test` |
| Formatter | `dsl_formatter_roundtrip_test` |
| Backtest/oracle | `backtest_determinism_test`, `differential_oracle_test` |

Full check:

```bash
ctest --test-dir build --output-on-failure
```

## Build Notes

Normal build:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

LLVM/JIT availability is environment-dependent. If LLVM is missing, interpreter tests can still pass while JIT-specific branches may skip or report unavailable. On Windows native, LLVM runtime DLLs may need to be on `PATH`.

## Safe Change Rules

- Preserve interpreter/JIT parity.
- Keep edits scoped to the smallest relevant module.
- Do not rewrite benchmark or claim docs unless behavior/evidence changed.
- Treat benchmark numbers as artifact-specific, never universal.
- Update `CLAIMS_MATRIX.md`, `PROJECT_STATE.md`, and context docs only when evidence or public claims change.
- Quote fused load-dedup and speedup numbers from `cse_diff_verified.md` and pinned-host artifacts only; label historical CSV speedups explicitly.

## Fast Debug Path

1. Reproduce with the smallest test.
2. Decide whether the bug is front-end, program assembly, runtime state, interpreter, or JIT lowering.
3. Compare interpreter vs JIT output before changing semantics.
4. Check node IDs and prewarm behavior before optimizing stateful operators.
5. Patch minimally.
6. Run the targeted test, then the nearest parity test.
