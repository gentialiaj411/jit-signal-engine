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
