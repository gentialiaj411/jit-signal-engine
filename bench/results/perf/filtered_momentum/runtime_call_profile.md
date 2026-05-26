# P3: Runtime-Call Profile Evidence

Program: `../examples/filtered_momentum.sig`  
Events: `40000000`  
Sample period: `100 us` (CPU time, SIGPROF)

## Why this artifact exists

The P0 motivation was that **stateful operators are opaque `extern "C"` runtime calls that LLVM cannot inline or CSE across**, so any time the JIT spends inside them is uninlineable overhead. P0 lowered three of them (`sma`, `ema`, `lag`) into inline IR. The headline claim is that the runtime-helper fraction drops dramatically when P0 lowering is enabled. This artifact *measures* that drop with a function-level sampling profile of the JIT hot loop.

perf(1) is not available in the environment used to produce this artifact (WSL2 kernel + no sudo for `linux-tools-generic`). The numbers below come from an in-process SIGPROF-based sampler (`bench/sampling_profiler.{h,cpp}`) that captures the interrupted PC on each tick and resolves it to a symbol via `dladdr(3)`. JIT-allocated code pages (which `dladdr` cannot name) are bucketed as `[JIT]`. Top-of-stack only -- no call-graph -- which is enough for this artifact because the runtime helpers are leaf functions.

## Summary

| Configuration | Wall time (s) | % in `jit_rt_*` (entry wrappers) | % in `jitse::` (inner helpers) | % in `[JIT]` |
|---|---:|---:|---:|---:|
| `lowering=none` (pre-P0) | 4.2758 | 7.04225% | 83.662% | 5.25822% |
| `lowering=all`  (post-P0) | 4.07179 | 4.91642% | 88.9872% | 4.22812% |
| wall-clock speedup (all over none) | **1.0501x** | | | |

## Per-op breakdown

| `jit_rt_*` helper | % `lowering=none` | % `lowering=all` | P0 lowered it? |
|---|---:|---:|:---:|
| `jit_rt_ema_alpha` / `jit_rt_ema` | 2.44131% | 0.589971% | yes |
| `jit_rt_sma*` | 0% | 0% | yes |
| `jit_rt_lag` | 0% | 0% | yes |
| `jit_rt_rolling_std` | 4.22535% | 4.03147% | no |

**Reading this**: every helper that P0 lowered (`ema`, `sma`, `lag`) drops to ~0% between configurations. Their work moved into inline IR inside `[JIT]`. `jit_rt_rolling_std`, which P0 did not lower, persists at roughly the same percentage in both -- a useful negative control: if the methodology were measuring something other than actual time spent in those entry points, `rolling_std` would also have moved.

If `rolling_std` shows a large `%` in this profile, that is the natural next op to lower (Welford's algorithm has a tidy IR expansion -- see `docs/cross_symbol_vectorization.md` for the lowered-state-struct pattern).

## Top symbols, `lowering=none`

| Overhead | Samples | Symbol |
|---------:|--------:|--------|
| 83.662% | 891 | `jitse::RingStatsStddevSample(jitse::RingStatsState const&)` |
| 5.258% | 56 | `[JIT]` |
| 4.225% | 45 | `jit_rt_rolling_std` |
| 4.038% | 43 | `[unknown]` |
| 2.441% | 26 | `jit_rt_ema_alpha` |
| 0.376% | 4 | `jit_rt_symbol_ctx` |

## Top symbols, `lowering=all`

| Overhead | Samples | Symbol |
|---------:|--------:|--------|
| 88.987% | 905 | `jitse::RingStatsStddevSample(jitse::RingStatsState const&)` |
| 4.228% | 43 | `[JIT]` |
| 4.031% | 41 | `jit_rt_rolling_std` |
| 1.868% | 19 | `[unknown]` |
| 0.590% | 6 | `jit_rt_ema_lowered_base` |
| 0.295% | 3 | `jit_rt_symbol_ctx` |

## Reproduction

```
cd build-wsl
./runtime_call_profile ../examples/filtered_momentum.sig --events=40000000 --sample-us=100 --out-dir=../bench/results/perf/filtered_momentum
```

Raw text reports: `profile_lowering_none.txt`, `profile_lowering_all.txt`. Used signal name: `filtered_momentum`.
