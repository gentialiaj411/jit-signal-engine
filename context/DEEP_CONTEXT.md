# DEEP_CONTEXT
Optional deep reference for agents. Do not read by default unless the task requires implementation-level detail.

## Architecture Notes
- Front end: line-based signal program parsing, keyword/operator lexing, AST construction, and dependency inlining.
- Stateful execution: dense `node_id` assignment keeps `SignalContext` storage vector-backed and hot-path friendly.
- Runtime helpers: rolling stats, VWAP, lag, and cross-over helpers are shared by interpreter and JIT paths.
- JIT path: LLVM ORC `LLJIT` is used when available; otherwise execution falls back to the interpreter.
- Whole-program mode: `CompileProgram` emits all signals into one native function to reduce dispatch overhead and expose more optimization opportunity.
- SIMD path: AVX2-gated lowering targets eligible `sma` codegen with scalar fallback; cross-symbol vectorization (P2/P10/P4) widens stateless IR to `<K x double>`.
- Stateful lowering: production default `kAll` inlines all 14 stateful ops; bases via `lowered_bases` GEP; fused JIT÷hw **1.42×** (`lowering_gap_phase3/`); P4 per-lane lowered+vec parity green.
- Multi-symbol path: `MultiSymbolSignalContext` reuses one compiled program across symbol slots.
- Backtest path: mdh canonical journals drive deterministic signal output plus IC reporting.

## Module Map
- `src/lexer.*`: tokens for identifiers, numbers, operators, and punctuation.
- `src/parser.*`: precedence-based parsing of conditionals, arithmetic, comparisons, and logical operators.
- `src/signal_program.*`: program parsing, dependency closure, cycle handling, and stateful node allocation.
- `src/runtime.*`: symbol table, market state, ring-buffer style state machines, JIT runtime hooks.
- `src/interpreter.*`: tree walk semantics used as correctness reference.
- `src/jit_compiler.*`: LLVM lowering, runtime symbol registration, pre/post IR dumping, SIMD lowering.
- `src/signal_backend.*`: backend abstraction used by CLI and benchmark runners.
- `src/market_sim.*`: deterministic market replay inputs for repeatable tests and benchmarks.
- `src/main.cpp`: CLI flow, replay loop, and benchmark timing.

## Test Meaning Map
- `test/lexer_test.cpp`: tokenization and EOF handling.
- `test/parser_smoke_test.cpp`: precedence, subtraction, and conditional parsing.
- `test/interpreter_test.cpp`: arithmetic, conditionals, EMA/SMA, rolling std, zscore, VWAP, lag, and cross operators.
- `test/runtime_test.cpp`: ring stats math and sliding-window behavior.
- `test/market_sim_test.cpp`: deterministic replay invariants and timestamp monotonicity.
- `test/signal_program_test.cpp`: multi-signal parsing, dependency inlining, cycle detection, and duplicate rejection.
- `test/jit_test.cpp`: JIT availability handling, single-signal compilation, parity on stateful operators, and `CompileProgram` parity.
- `test/fuzz_parity_test.cpp`: randomized JIT/interpreter parity on bounded-depth signals.
- `test/fuzz_parity_simd_test.cpp`: scalar-vs-AVX2 parity on SIMD-enabled rolling-window codegen.
- `test/fuzz_parity_multisymbol_test.cpp`: single-symbol reference versus multi-symbol execution parity.
- `test/hot_path_allocation_test.cpp`: warmed evaluation loop allocation checks.
- `test/backtest_determinism_test.cpp`: identical recorded inputs produce identical IC sections.
- `test/node_state_layout_test.cpp`: stable node IDs and node-indexed runtime layout.
- `test/stateful_lowering_parity_test.cpp`: lowered IR vs `jit_rt_*` reference per op.
- `test/vectorized_stateful_parity_test.cpp`: scalar K-runs vs vec fan-out (incl. `kAll` P4 cases).
- `test/runtime_call_profile_test.cpp`: gates `jit_rt_*` sample-share drop under `kAll`.

## Commands and Benchmarks
- Build: `cmake -B build -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`.
- Test: `ctest --test-dir build --output-on-failure`.
- CLI demo: `./build/[Release/]jit_signal_engine examples/filtered_momentum.sig`.
- IR inspection: `./build/[Release/]jit_signal_engine --print-ast --dump-ir --all-signals examples/filtered_momentum.sig`.
- Benchmarks: `bash bench/run_benchmarks.sh ...`, `bash bench/run_pinned_speedup.sh build-wsl`, `bash bench/run_lowering_gap_phase3.sh build-wsl`, `bash bench/run_stateful_vec_lowering_phase4.sh build-wsl`.

## Known Limits / TODOs
- JIT availability is environment dependent.
- Fused JIT market load dedup is verified (emitter memoization; IR **22→2** on `filtered_momentum.sig`). LLVM CSE alone was insufficient.
- Canonical JIT speedups and multi-thread scaling use the pinned WSL2 host (`bench/PINNED_HOST.md`).
- Benchmark results are environment specific and should be treated as artifacts, not assumptions.
- Any claim not tied to a test or benchmark artifact should remain `TODO/VERIFY`.

## Current Resume Claims
- C++20 DSL engine for trading-signal evaluation with interpreter and LLVM ORC JIT execution paths.
- JIT/interpreter parity validated with fuzz and deterministic benchmark coverage.
- Whole-program JIT fusion reduces dispatch overhead for multi-signal evaluation.
- SIMD, multi-symbol, and recorded-backtest extensions are each backed by dedicated tests and artifacts.
