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

For public speedup and scaling claims, use the pinned host drivers first:

```bash
bash bench/run_pinned_speedup.sh build-wsl
bash bench/run_pinned_multithread_scaling.sh build-wsl
```

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
- `allocations_interp` and `allocations_jit` after warmup (target: `0`)
- single-signal vs all-signals deltas

## Output Artifacts

- `bench/results.csv`: per-signal latency/throughput rows.
- `bench/throughput.png`: interpreter throughput per signal.
- `bench/latency_p99.png`: interpreter p99 latency per signal.
- `bench/throughput_compare.png`: interpreter vs handwritten throughput.

When LLVM is unavailable, JIT columns are emitted as unavailable/NaN. This is expected and keeps scripts stable across environments.

## Canonical pinned-host speedups (resume-safe)

**Host:** `wsl2-ultra9-275hx-2026-05` — see `bench/PINNED_HOST.md`.  
**Command:** `bash bench/run_pinned_speedup.sh build-wsl` (30×1M-event repeats, core 2, bootstrap 95% CI).

| Path | Median speedup | 95% CI |
|---|---|---|
| momentum (single signal) | **7.32×** | 7.29× – 7.48× |
| all-signals fused (`CompileProgram`) | **15.56×** | 15.52× – 15.58× |

Artifacts: `bench/results/pinned_host_speedup.{json,md}`.

Multi-thread scaling (10k symbols, 60s/run): P-cores **5.45×** / **90.8%** at 6 threads (`multithread_scaling_pcores.md`); hybrid **5.69×** / **35.6%** at 16T (`multithread_scaling.md`). Launcher: `bash bench/run_pinned_multithread_scaling.sh build-wsl`.

### Cross-symbol vectorization (vec JIT vs scalar JIT)

```bash
taskset -c 2 ./build-wsl/cross_symbol_benchmark ../examples/stateless_compute_heavy.sig 2000000 --lanes=4 --runs=15 \
  --md=bench/results/avx2_speedup/stateless_compute_heavy.md
```

Canonical write-up: `bench/results/avx2_speedup/README.md`. Headline: **~2.6×** on FP-heavy `stateless_compute_heavy.sig`, **~1.4×** on memory-bound `stateless_heavy.sig` (K=4, pinned).

### Latency distribution

```bash
./build-wsl/latency_bench ../examples/spread_signal.sig --events=1000000 --jit-only --out-dir=bench/results/latency
```

CO-aware runs: `--rate-hz=R`. Index: `bench/results/latency/index.md`.

### SPSC live-ingest pipeline

```bash
JITSE_BENCH_PRODUCER_CPU=2 JITSE_BENCH_CONSUMER_CPU=4 \
  ./build-wsl/spsc_jit_pipeline_bench ../examples/spread_signal.sig \
  --events=2000000 --warmup=200000 --rate-hz=2000000 \
  --out-md=bench/results/spsc_pipeline/spread_signal_2mhz.md
```

See `bench/results/spsc_pipeline/README.md` for p50 decomposition vs standalone JIT.

### Operator lowering gap (JIT vs hand-written C++)

```bash
bash bench/run_lowering_gap_phase3.sh build-wsl
```

Artifacts: `bench/results/lowering_gap_phase3/` — fused JIT÷hw **1.42×** on pinned host.

### P4: lowered stateful + cross-symbol vectorization

```bash
bash bench/run_stateful_vec_lowering_phase4.sh build-wsl
```

Artifact: `bench/results/stateful_vec_lowering_speedup.md` — three-way table (scalar+`kAll`, vec+`kAll`, vec+`kNone` on `filtered_momentum`, K=4).

---

## Historical reproduced results (different host — not canonical)

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
- These speedups are **historical** (different host). Do not quote them in place of pinned-host numbers.
- The `all-signals` path (CompileProgram) fuses dispatch; redundant `mid(AAPL)` bid/ask loads are deduplicated in codegen (IR: 22→2 loads — see `bench/results/cse_evidence/cse_diff_verified.md`).
- JIT p99 is 7–21 ns across all signals. Interpreter p99 is 28–531 ns.
- **Historical note:** this run predates production `kAll` lowering. Current default inlines all stateful ops; fused JIT÷hw **1.42×** with `jit_rt_*` profile **~3%** (`lowering_gap_phase3/`, `runtime_call_profile` on `filtered_momentum`).

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
