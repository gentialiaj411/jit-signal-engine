# Benchmarks

## What We Measure

- Single-signal eval throughput and latency (`bench/signal_benchmark.cpp`)
- Scaling behavior across instrument counts (`bench/scaling_benchmark.cpp`)
- Interpreter, JIT (when available), and handwritten baseline (where applicable)

## How To Run

```bash
cmake -S . -B build
cmake --build build

./build/Debug/signal_benchmark ./examples/spread_signal.sig 200000 ./bench/results.csv spread
./build/Debug/scaling_benchmark ./examples/spread_signal.sig 200000 spread
python ./bench/plot_results.py ./bench/results.csv ./bench
```

## Resume-Grade Runbook (Single vs All-Signals)

Use `Release` builds for any number you plan to publish.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Single-signal benchmark (final signal only):

```bash
./build/Release/signal_benchmark ./examples/filtered_momentum.sig 1000000 ./bench/results.csv filtered
./build/Release/scaling_benchmark ./examples/filtered_momentum.sig 500000 filtered
```

All-signals benchmark (`eval_all` / `CompileProgram` path):

```bash
./build/Release/signal_benchmark --all-signals ./examples/filtered_momentum.sig 1000000 ./bench/results.csv
./build/Release/scaling_benchmark --all-signals ./examples/filtered_momentum.sig 500000
```

On Windows (MSVC), use the `Debug`/`Release` subdirectory binaries:

```powershell
.\build\Release\signal_benchmark.exe --all-signals .\examples\filtered_momentum.sig 1000000 .\bench\results.csv
.\build\Release\scaling_benchmark.exe --all-signals .\examples\filtered_momentum.sig 500000
```

Suggested reporting:
- `jit_mode` (must be `enabled`)
- interpreter vs JIT throughput
- p50 / p99 / p999 latency for each mode
- single-signal vs all-signals deltas

## Output Artifacts

- `bench/results.csv`: per-signal latency/throughput rows.
- `bench/throughput.png`: interpreter throughput per signal.
- `bench/latency_p99.png`: interpreter p99 latency per signal.
- `bench/throughput_compare.png`: interpreter vs handwritten throughput.

When LLVM is unavailable, JIT columns are emitted as unavailable/NaN. This is expected and keeps scripts stable across environments.

## Reproduced Results

**Environment:** Windows 11, MSVC, LLVM 18.1.6 (vcpkg x64-windows), pinned to core 0, Release build, 1M events per signal.
**Command:** `.\bench\run_benchmarks.ps1 -BuildDir .\build\Release -OutCsv .\bench\results.csv -PinCore 0 -Events 1000000`

| Signal | Interp ev/s | JIT ev/s | Speedup | Interp p99 ns | JIT p99 ns |
|---|---|---|---|---|---|
| spread | 35.2M | 211M | 6.0x | 28 | 7 |
| momentum | 15.2M | 157.8M | **10.4x** | 54 | 9 |
| spread_z | 12.2M | 120.8M | 9.9x | 87 | 9 |
| zscore (z) | 14.3M | 113.4M | 7.9x | 73 | 10 |
| vwap (dev) | 14.2M | 107.1M | 7.5x | 73 | 12 |
| filtered_momentum (all-signals) | 4.0M | 53.4M | **13.2x** | 531 | 21 |

**Notes:**
- `jit_mode=enabled` confirmed on all rows.
- The `momentum` single-signal result reproduces the resume claim of ~10.3x.
- The `all-signals` path (CompileProgram) shows 13.2x due to single-function dispatch and LLVM CSE on repeated `mid(AAPL)` loads.
- JIT p99 is 7–21 ns across all signals. Interpreter p99 is 28–531 ns.
- Stateful operators (ema, rolling_std, etc.) call back into C++ runtime — JIT advantage is dispatch elimination and market-data load CSE, not stateful-op inlining.

## Linux perf Runbook

Use this on a Linux machine for microarchitectural evidence:

```bash
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  ./build/Release/signal_benchmark ./examples/spread_signal.sig 200000 ./bench/results.csv spread
```

For hotspots:

```bash
perf record -F 999 -g -- ./build/Release/signal_benchmark ./examples/momentum_signal.sig 200000 ./bench/results.csv momentum
perf report
```

If FlameGraph scripts are installed:

```bash
perf script > out.perf
stackcollapse-perf.pl out.perf > out.folded
flamegraph.pl out.folded > flame.svg
```
