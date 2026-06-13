![CI](https://github.com/gentialiaj411/jit-signal-engine/actions/workflows/ci.yml/badge.svg)

# jit-signal-engine

A C++20 trading-signal DSL engine with an interpreter correctness oracle and optional LLVM ORC JIT backend for native signal evaluation.

The project is a focused compiler/runtime project: it parses signal formulas, assigns stable state IDs for rolling operators, evaluates market ticks through interpreter or JIT paths, and validates correctness with parity, fuzz, allocation, benchmark, and recorded-data oracle tests.

The DSL now also supports explicit scalar parameters via `param name = 1.23` declarations plus a continuous-alpha `ema_alpha(expr, alpha)` operator, which feed both fused program execution and the autodiff / calibration path.

## Architecture

```text
.sig source
  -> Lexer -> Parser -> AST
  -> SignalProgram transforms
       - dependency handling
       - stable node_id allocation for stateful ops
       - symbol_id binding for market reads
  -> Interpreter::Evaluate(...)        correctness oracle
  -> JitCompiler::Compile(...)         one native signal
  -> JitCompiler::CompileProgram(...)  fused multi-signal native eval
  -> double output(s) per market tick
```

Runtime state is intentionally dense and hot-path friendly:

- `MarketState` stores fixed instrument slots indexed by `symbol_id`.
- `SignalContext` stores rolling/EMA/VWAP/lag/cross state in `std::vector`s indexed by stable `node_id`.
- `MultiSymbolSignalContext` reuses one compiled program across many output symbols.
- Warmed evaluation is tested for zero heap allocation in the hot loop.

For more architecture detail, read [`docs/agent_architecture.md`](docs/agent_architecture.md).

## Verified Capabilities

| Capability | Evidence |
|---|---|
| Custom DSL lexer/parser | `src/lexer.*`, `src/parser.*`, `lexer_test`, `parser_smoke_test` |
| Interpreter semantics | `src/interpreter.*`, `interpreter_test`, `runtime_test` |
| LLVM ORC JIT backend | `src/jit_compiler.*`, `jit_test`, `fuzz_parity_test` |
| Whole-program JIT fusion | `JitCompiler::CompileProgram`, `jit_test`, `bench/results.csv` |
| Dense node-indexed runtime state | `src/runtime.*`, `node_state_layout_test` |
| Warmed hot path avoids heap allocation | `hot_path_allocation_test` |
| AVX2-gated SIMD + cross-symbol vectorization | `fuzz_parity_simd_test`, `vectorized_lanes_parity_test`, `bench/results/avx2_speedup/` |
| Multi-symbol execution | `fuzz_parity_multisymbol_test`, `bench/results_multisymbol.csv` |
| Pinned-host JIT vs interpreter speedup | `bench/run_pinned_speedup.sh`, `bench/results/pinned_host_speedup.md` |
| Fused JIT market load dedup | `cse_load_dedup_test`, `bench/results/cse_evidence/cse_diff_verified.md` |
| Multi-threaded sharded JIT (10k symbols) | `multithread_equivalence_test`, `bench/results/multithread_scaling_pcores.md` (P-core headline), `multithread_scaling.md` (hybrid) |
| Tiered JIT (baseline + warm specialization) | `tiered_specialization_parity_test`, `docs/tiered_specialization.md` |
| O(1) Welford `rolling_std` | `welford_stddev_parity_test`, `src/runtime.cpp` |
| Persistent JIT module cache | `jit_module_cache_test` |
| Stateful-operator IR lowering (default `kAll`) | `stateful_lowering_parity_test`, `runtime_call_profile_test`, `bench/results/lowering_gap_phase3/` |
| Lowered stateful + cross-symbol vectorization (P4 parity) | `vectorized_stateful_parity_test`, `bench/results/stateful_vec_lowering_speedup.md` |
| Latency distribution (HdrHistogram-style) | `latency_histogram_test`, `latency_bench`, `bench/results/latency/` |
| SPSC live-ingest pipeline | `spsc_ring_test`, `spsc_jit_pipeline_bench`, `bench/results/spsc_pipeline/` |
| DSL fmt/lint tooling | `dsl_formatter_roundtrip_test`, `cli/jitse_fmt.cpp`, `cli/jitse_lint.cpp` |
| Parameterized DSL programs + stateless/stateful gradient oracle checks | `parser_smoke_test`, `dsl_formatter_roundtrip_test`, `gradient_parity_test` |
| JIT-compiled gradients + fixture-backed calibration | `gradient_parity_test`, `calibration_smoke_test`, `bench/results/autodiff/` |
| Recorded-data backtest/oracle workflow | `backtest_determinism_test`, `differential_oracle_test`, `bench/results/diff_test/` |

See [`EVIDENCE.md`](EVIDENCE.md) and [`CLAIMS_MATRIX.md`](CLAIMS_MATRIX.md) for the claim-to-artifact map.

## What Is Not Claimed

- This is not a live trading platform.
- Backtest IC artifacts validate deterministic methodology, not deployable alpha.
- The current autodiff/calibration claim is fixture-backed only; the recorded-data IC/backtest calibration path is still out of scope at HEAD.
- JIT speedups and p99 latency are environment-specific; quote canonical ratios from `bench/PINNED_HOST.md` only.
- Cross-symbol vectorization wins on FP-heavy stateless programs (~2.6× vs scalar JIT at K=4); memory-bound cases are smaller (~1.4×). Per-operator `results_simd.csv` is not the canonical vec story — see `bench/results/avx2_speedup/`.
- Multi-thread: lead with **90.8%** efficiency on P-cores at 6 threads (`multithread_scaling_pcores.md`). Hybrid mixed P+E at 16 threads is **35.6%** — do not present that as the algorithmic ceiling.

## DSL Example

```sig
param alpha_fast = 0.25
param alpha_slow = 0.05
param bias = -0.10

signal fast = ema_alpha(mid(AAPL), alpha_fast)
signal slow = ema_alpha(mid(AAPL), alpha_slow)
signal out = fast - slow + bias
```

The checked-in calibration demo fits parameterized signals to a tiny deterministic CSV fixture with Adam using the compiled gradient path. See `cli/jitse_calibrate.cpp` and `bench/results/autodiff/`.

## Quick Start

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/jit_signal_engine examples/filtered_momentum.sig
ctest --test-dir build --output-on-failure
```

On Windows multi-config generators, binaries may be under `build/Release/`.

One-command demo:

```bash
bash scripts/demo_smoke.sh ./build
```

Windows PowerShell:

```powershell
.\scripts\demo_smoke.ps1 -BuildDir .\build\Release
```

## Benchmarks

Use Release builds for any number you quote.

```bash
bash bench/run_pinned_speedup.sh ./build-wsl
bash bench/run_pinned_multithread_scaling.sh ./build-wsl
bash bench/run_lowering_gap_phase3.sh ./build-wsl
bash bench/run_stateful_vec_lowering_phase4.sh ./build-wsl
bash bench/run_benchmarks.sh ./build ./bench/results.csv 0 1000000
bash bench/run_multisymbol_benchmark.sh ./build ./bench/results_multisymbol.csv 2000
bash bench/run_simd_benchmark.sh ./build ./bench/results_simd.csv 1000000
```

Current checked-in artifacts:

| Artifact | Current meaning |
|---|---|
| `bench/results/pinned_host_speedup.{json,md}` | Canonical pinned-host JIT vs interpreter speedups. |
| `bench/results/multithread_scaling.{json,md}` | Pinned 10k-symbol multi-thread throughput scaling. |
| `bench/results.csv` | WSL/Linux signal benchmark (latency; historical speedup context). |
| `bench/results_windows.csv` | Windows-native benchmark. JIT p99 spans 10-48 ns in this artifact. |
| `bench/results_multisymbol.csv` | Single-thread multi-symbol sweep through 10,000 symbols. |
| `bench/results_simd.csv` | Per-operator scalar vs AVX2 SMA benchmarks (historical). |
| `bench/results/avx2_speedup/` | Cross-symbol vec vs scalar JIT speedups (`stateless_compute_heavy.sig`, etc.). |
| `bench/results/lowering_gap_phase3/` | JIT vs hand-written C++ gap after stateful lowering (fused **1.42×**). |
| `bench/results/stateful_vec_lowering_speedup.md` | P4 three-way: scalar+`kAll` vs vec+`kAll` vs vec+`kNone`. |
| `bench/results/spsc_pipeline/` | SPSC ring + JIT pipeline latency artifacts. |
| `bench/results/latency/` | CO-aware and closed-loop latency histograms. |
| `bench/results/vec_thread_composition/` | Vector × multi-thread composition tables. |

Benchmark provenance sidecars live next to the artifacts as `*.meta.json`.

End-to-end architecture: [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Core Built-ins

- Parameters: `param name = value`
- Market reads: `mid`, `bid`, `ask`, `spread`
- Rolling/stateful: `ema`, `ema_alpha`, `sma`, `rolling_std`, `rolling_min`, `rolling_max`, `zscore`, `vwap`, `lag`
- Cross-state: `cross_above`, `cross_below`
- Math/control: arithmetic, comparisons, logical operators, conditionals, `abs`, `log`, `sqrt`

`ema(expr, period)` keeps its integer-period semantics. `ema_alpha(expr, alpha)` is the continuous-alpha form used for parameter sensitivities and calibration. `rolling_std` and `zscore` require `period >= 2`.

See [`docs/dsl-reference.md`](docs/dsl-reference.md) and [`docs/operator_taxonomy.md`](docs/operator_taxonomy.md).
