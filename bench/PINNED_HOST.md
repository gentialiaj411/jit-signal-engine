# Pinned benchmark host (canonical)

This file defines the **only** host configuration used for public JIT-vs-interpreter speedup claims after the P0 pinned-host pass. Re-run `bench/run_pinned_speedup.sh` on a different machine only after updating this document and `bench/pinned_host_spec.json` together.

## Identity

| Field | Value |
|---|---|
| Canonical ID | `wsl2-ultra9-275hx-2026-05` |
| OS | **WSL2** — Ubuntu userspace on Linux `6.6.x` `microsoft-standard-WSL2` kernel |
| Windows host | Windows 11 (build 26200), power plan **Ultimate Performance** |
| CPU | Intel(R) Core(TM) Ultra 9 275HX, 24 logical CPUs exposed to WSL |
| Virtualization | Full WSL2 hypervisor (Microsoft); cpufreq sysfs **not** exposed |
| NUMA | Single node (`NUMA node0`: CPUs 0–23) |
| Hugepages | Default (none configured) |
| Scheduler | Linux CFS default inside WSL2 |

## Toolchain (pinned build)

| Tool | Version / path |
|---|---|
| CMake | 4.2.3 (WSL) |
| C++ compiler | `g++` (Ubuntu) **15.2.0** |
| LLVM / ORC JIT | **18** (`LLVM_DIR=/usr/lib/llvm-18/cmake`) |
| Build directory | `build-wsl` (`Release`, `JITSE_ENABLE_LLVM=ON`) |
| Benchmark binary | `build-wsl/signal_benchmark` |

Configure once:

```bash
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release -DJITSE_ENABLE_LLVM=ON
cmake --build build-wsl -j
```

## Core pinning and isolation

| Setting | Value |
|---|---|
| Pinned logical CPU | **2** (`taskset -c 2` + `signal_benchmark --pin-core 2`) |
| HT sibling set for CPU 2 | `{2}` only (WSL exposes one thread per core; no sibling contention) |
| Benchmark events per run | **1,000,000** |
| Warmup | 10,000 interpreter/JIT iterations (inside `signal_benchmark`) |
| Timing batch | 64 evaluations per sample (amortizes timer overhead) |
| Statistical repeats | **30** measured runs per case (`--measure-runs 30`), one JIT compile per process |

The harness **refuses** to start when:

- `pinned_host_spec.json` fingerprint checks fail (wrong CPU model, kernel, compiler, LLVM major, CPU count).
- cpufreq governor exists and is not `performance` (bare Linux only).
- A hyperthread sibling of the pinned core has measurable non-idle utilization (see `bench/run_pinned_speedup.sh`).
- `jit_mode` is not `enabled` after the benchmark run.

## Workloads (P0 speedup cases)

| Case | Command shape | Resume mapping |
|---|---|---|
| Momentum (single signal) | `momentum_signal.sig`, signal name `momentum` | Single-signal JIT vs interpreter throughput |
| All-signals fused | `filtered_momentum.sig`, `--all-signals` | Whole-program `CompileProgram` path |

## Artifacts

| File | Purpose |
|---|---|
| `bench/results/pinned_host_speedup.json` | Raw per-run throughputs + bootstrap 95% CIs |
| `bench/results/pinned_host_speedup.md` | Headline speedup ratios (median, p99, CI) |
| `bench/run_pinned_speedup.sh` | Validates host, runs both cases, writes artifacts |

## Historical numbers (different host / protocol)

Earlier artifacts (`bench/results.csv` on a prior WSL refresh, and older Windows-native runs) reported higher speedups (e.g. momentum ~10.4x, fused ~13.2x). Those remain in the matrix as **historical, different host** rows — they are **not** the canonical pinned-host claim.

## Hybrid core mapping (host-detected)

Detected on canonical host `wsl2-ultra9-275hx-2026-05` inside WSL2 (2026-05-24).

### `lscpu` (MAXMHZ column)

Command:

```bash
lscpu --extended=CPU,CORE,SOCKET,MAXMHZ,MINMHZ
```

On this WSL2 kernel, every logical CPU reports `MAXMHZ` and `MINMHZ` as **`-`** (cpufreq sysfs is unavailable under WSL; see cpufreq policy in `bench/pinned_host_spec.json`). Nominal Intel Ark limits for the 275HX are ~**5400 MHz** (Lion Cove P-cores) vs ~**4000 MHz** (Skymont E-cores), but those values are **not** exposed to guests via `lscpu` here.

### Empirical cluster verification (WSL fallback)

Because `MAXMHZ` is blank, core class was verified by sustained pinned throughput:

```bash
bash bench/verify_core_clusters.sh
```

Result on this host: CPUs **0–7** as a group outran CPUs **8–23** (`cluster_0_7` > `cluster_8_23`), matching the Linux enumeration for Ultra 9 275HX (8 P + 16 E, no SMT).

| Class | Logical CPU indices | Notes |
|---|---|---|
| **P-cores** | **1, 2, 3, 4, 5, 6, 7** | Linux IDs **0–7** are the P-cluster; **CPU 0 reserved** for OS |
| **E-cores** | **8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23** | Full Skymont cluster |

Homogeneous scaling benches use consecutive `taskset` ranges: P-only `1..7`, E-only `8..23`. The legacy hybrid run keeps **2–17** (6 P + 10 E).

## Multi-threaded scaling (P2)

| Regime | Thread counts | Pin cores | Artifacts |
|---|---|---|---|
| Hybrid (P+E mixed) | 1, 2, 4, 8, 16 | **2–17** | `bench/results/multithread_scaling.{json,md}` |
| P-cores only | 1, 2, 4, 6 | **1–7** | `bench/results/multithread_scaling_pcores.{json,md}` |
| E-cores only | 1, 2, 4, 8, 16 | **8–23** | `bench/results/multithread_scaling_ecores.{json,md}` |

| Setting | Value |
|---|---|
| Symbols | **10,000** |
| Sustained duration | **60s** per thread count |
| NUMA | Single node — `numactl` not used (documented choice) |

Reproduce (defaults = hybrid):

```bash
bash bench/run_pinned_multithread_scaling.sh build-wsl
```

P-cores only:

```bash
bash bench/run_pinned_multithread_scaling.sh build-wsl \
  --cores 1,2,3,4,5,6,7 --thread-counts 1,2,4,6 \
  --out-json bench/results/multithread_scaling_pcores.json \
  --out-md bench/results/multithread_scaling_pcores.md --label pcores-only
```

E-cores only:

```bash
bash bench/run_pinned_multithread_scaling.sh build-wsl \
  --cores 8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23 --thread-counts 1,2,4,8,16 \
  --out-json bench/results/multithread_scaling_ecores.json \
  --out-md bench/results/multithread_scaling_ecores.md --label ecores-only
```

## Reproducing

From the repo root inside WSL:

```bash
bash bench/run_pinned_speedup.sh build-wsl
```

On Windows, set the host power plan to **Ultimate Performance** before running (WSL cannot read cpufreq).
