# EVIDENCE_MAP.md

## Supported Claims
- Custom DSL parser exists.
- Interpreter execution path works.
- LLVM ORC JIT path exists and emits optimized native code.
- Built-in function set is documented and covered by runtime/interpreter/JIT evidence.
- JIT/interpreter parity is covered by fuzz and deterministic tests.
- `CompileProgram` emits one native function for all signals.
- Runtime state is preallocated by dense node IDs instead of hot-path maps.
- Benchmark runs are reproducible with fixed seeds and batched timing.
- SIMD lowering is present for eligible rolling-window codegen with scalar fallback.
- Multi-symbol SoA execution is present and parity-covered.
- Recorded-data backtest output is deterministic for identical inputs.

## Risky / Unsupported Claims
- Market-data CSE inside the fused JIT path is architectural, not independently measured.
- JIT availability is environment dependent.
- Do not claim general-purpose compiler behavior or universal market-data support.
- Do not claim `abs()` JIT parity unless a direct test or artifact proves it.

## Benchmarks and Artifacts
- `bench/results.csv`: published throughput / p99 claims.
- `bench/results_windows.csv`: Windows-native throughput / p99 claims.
- `bench/run_benchmarks.sh` and `bench/run_benchmarks.ps1`: reproducible benchmark drivers.
- Fixed seed, warmup, and batch timing are the repeatability story.
- Treat all performance numbers as artifact-backed, not prose-backed.
- Current p99 reality:
  - WSL `bench/results.csv`: JIT p99 spans 14-78 ns, with `<all_signals>` at 78 ns.
  - Windows `bench/results_windows.csv`: JIT p99 spans 10-48 ns, with `<all_signals>` at 48 ns.

## Tests and What They Prove
- Lexer/parser tests: DSL tokenization and precedence rules.
- Interpreter tests: arithmetic and stateful operator semantics.
- Runtime tests: ring state math and window maintenance.
- Signal-program tests: dependency inlining, cycle detection, and stateful node allocation.
- JIT tests: JIT availability handling, parity, and whole-program execution.
- Fuzz parity tests: randomized interpreter/JIT agreement under bounded depth.
- SIMD parity tests: scalar-vs-AVX2 agreement for rolling-window lowering.
- Multi-symbol parity tests: symbol-reused execution remains equivalent to the single-symbol reference.
- Backtest determinism test: recorded-data output remains identical across repeated runs.

## Validation Commands
- `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
- `bash bench/run_benchmarks.sh [build_dir] [out_csv] [pin_core] [events]`
- `.\bench\run_benchmarks.ps1 [-BuildDir .\build\Release] [-OutCsv .\bench\results.csv] [-PinCore 0] [-Events 1000000]`

## Resume-Safe Wording
- Built a C++20 DSL engine for trading-signal evaluation with interpreter and LLVM ORC JIT execution paths.
- Validated JIT correctness against an interpreter reference using parity and fuzz tests.
- Implemented whole-program JIT fusion so multiple signals compile into one native call.
- Designed dense runtime state indexing to avoid hot-path hash lookups.
- Added reproducible benchmark drivers with fixed seeds and batched timing.
- Added SIMD, multi-symbol, and recorded-backtest paths with dedicated artifacts.

## Claims To Avoid
- Full compiler platform or general-purpose query language claims.
- Unqualified claims about JIT speedup on every machine.
- Any statement implying the code is a live trading platform.
- Claims that market-data CSE was separately benchmarked.
- Claims that `abs()` has proven JIT parity unless a test proves it.

## Best Target Roles
- Compiler/Runtime
- Systems SWE
- Quant Dev
- Low-latency Trading Infrastructure

## Highest-ROI Improvements
1. Add a dedicated IR-diff or perf artifact that proves market-data load deduplication in the whole-program JIT path.
2. Add a README scope note that clearly separates verified claims from environment-dependent ones.
3. Lock a smaller benchmark artifact with explicit provenance metadata for resume-safe quoting.
4. Add a focused parity test for `abs()` if JIT support is intended.
5. Add CI coverage for the existing benchmark driver invocation and parity tests.
6. Convert the strongest signals into resume bullets with artifact references.
7. Add a short architecture diagram that explains interpreter, JIT, and program fusion.
8. Keep the claims matrix synchronized with any benchmark or JIT behavior changes.
