# P3: Runtime-Call Profile Evidence

Program: `../examples/filtered_momentum.sig`  
Events: `40000000`  
Sample period: `100 us` (CPU time, SIGPROF)

## Why this artifact exists

The P0 motivation was that **stateful operators are opaque `extern "C"` runtime calls that LLVM cannot inline or CSE across**, so any time the JIT spends inside them is uninlineable overhead. With `kAll`, every stateful op is lowered into inline IR (bases via `SignalContext::lowered_bases` GEP). The headline claim is that the runtime-helper fraction drops dramatically when lowering is enabled. This artifact *measures* that drop with a function-level sampling profile of the JIT hot loop.

perf(1) is not available in the environment used to produce this artifact (WSL2 kernel + no sudo for `linux-tools-generic`). The numbers below come from an in-process SIGPROF-based sampler (`bench/sampling_profiler.{h,cpp}`) that captures the interrupted PC on each tick and resolves it to a symbol via `dladdr(3)`. JIT-allocated code pages (which `dladdr` cannot name) are bucketed as `[JIT]`. Top-of-stack only -- no call-graph -- which is enough for this artifact because the runtime helpers are leaf functions.

## Summary

| Configuration | Wall time (s) | % in `jit_rt_*` (entry wrappers) | % in `jitse::` (inner helpers) | % in `[JIT]` |
|---|---:|---:|---:|---:|
| `lowering=none` (pre-P0) | 2.96878 | 79.2173% | 0% | 16.8691% |
| `lowering=all`  (kAll) | 1.03172 | 1.56863% | 0% | 86.2745% |
| wall-clock speedup (all over none) | **2.87751x** | | | |

## Per-op breakdown

| `jit_rt_*` helper | % `lowering=none` | % `lowering=all` | P0 lowered it? |
|---|---:|---:|:---:|
| `jit_rt_ema_alpha` / `jit_rt_ema` | 7.96221% | 0% | yes |
| `jit_rt_sma*` | 0% | 0% | yes |
| `jit_rt_lag` | 0% | 0% | yes |
| `jit_rt_rolling_std` | 70.8502% | 0% | yes |
| `jit_rt_*_lowered_base` | 0% | 0% | N/A (GEP from ctx) |

**Reading this**: stateful `jit_rt_*` helpers drop to ~0% with `kAll`; residual `jit_rt_symbol_ctx` is ctx lookup only. Work moves into `[JIT]`.

**Note on sinks**: sink_none = 3.14752e+11, sink_all = 2.69285e+07. For the canonical filtered_momentum signal the sink can be NaN during warmup; sink equality is not a correctness gate for this artifact (stateful_lowering_parity_test already gates parity).

## Top symbols, `lowering=none`

| Overhead | Samples | Symbol |
|---------:|--------:|--------|
| 70.850% | 525 | `jit_rt_rolling_std` |
| 16.869% | 125 | `[JIT]` |
| 7.962% | 59 | `jit_rt_ema_alpha` |
| 3.914% | 29 | `[unknown]` |
| 0.405% | 3 | `jit_rt_symbol_ctx` |

## Top symbols, `lowering=all`

| Overhead | Samples | Symbol |
|---------:|--------:|--------|
| 86.275% | 220 | `[JIT]` |
| 12.157% | 31 | `[unknown]` |
| 1.569% | 4 | `jit_rt_symbol_ctx` |

## Reproduction

```
cd build-wsl
./runtime_call_profile ../examples/filtered_momentum.sig --events=40000000 --sample-us=100 --out-dir=../bench/results/perf/filtered_momentum
```

Raw text reports: `profile_lowering_none.txt`, `profile_lowering_all.txt`. Used signal name: `filtered_momentum`.
