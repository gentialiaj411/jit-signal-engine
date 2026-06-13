# P3 — Runtime-call profile evidence

## What this document is

P0 was sold on a one-line claim: **"stateful operators are opaque
`extern "C"` calls into the runtime, and LLVM cannot inline or CSE
across them; lowering them into IR removes that ceiling."** Production
default is now **`kAll`** (all stateful ops lowered). Phase 3 profile
on fused `filtered_momentum` (40M events): `jit_rt_*` **80.2% → 2.8%**
(`bench/results/perf/filtered_momentum/runtime_call_profile.md`). This
document describes the in-process sampler and the original P0/P3
evidence methodology.

The artifact lives at
[`bench/results/perf/runtime_call_profile.md`](../bench/results/perf/runtime_call_profile.md)
(canonical signal exercising `sma`+`ema`+`lag`+`rolling_std`) and
[`bench/results/perf/filtered_momentum/runtime_call_profile.md`](../bench/results/perf/filtered_momentum/runtime_call_profile.md)
(the originally-named program from the P0 motivation: only `ema` +
`rolling_std`). Each artifact contains a `perf report`-style top-N
table for `lowering=none` (pre-P0) and `lowering=all` (post-P0), plus a
per-op breakdown that shows the lowered helpers collapsing to ~0%
between configurations.

## Why we built an in-process sampling profiler

The original P3 plan was "use `perf record` + flamegraph.pl". That isn't
available in the build environment used here:

- WSL2 kernel does not expose hardware PMU events.
- `linux-tools-generic` (which ships `perf`) requires sudo to install and
  this account does not have one.
- Even when `perf stat -e task-clock` works under WSL2, `perf record`
  with kernel symbols typically does not.

So instead this repo ships a ~250-line in-process software sampler
(`bench/sampling_profiler.{h,cpp}`) that does the same thing perf would
have done for our specific question:

- A periodic `SIGPROF` signal driven by `setitimer(ITIMER_PROF, ...)`.
  The OS only ticks the timer while *this process* is on-CPU, so samples
  count actual CPU work, not wall time spent context-switched out.
- The signal handler walks the interrupted program counter out of the
  `ucontext_t` (`gregs[REG_RIP]` on x86-64) and resolves it to a symbol
  via `dladdr(3)`.
- JIT-allocated executable pages (which `dladdr` cannot name) are
  bucketed as `[JIT]`. Other unresolved IPs are bucketed as `[unknown]`.
- The handler aggregates into a fixed-size, lock-free, async-signal-safe
  global state. `Stop()` snapshots that global into the profiler
  instance so two profilers can be run sequentially without clobbering
  each other.

**Limitations vs. perf**:

- *No call graph*. Only the top frame is attributed. This is sufficient
  for our purposes because the runtime helpers are leaf functions; >95%
  of their CPU samples land their own IP at the top of the stack. perf
  with `--call-graph dwarf` would have produced a flamegraph; we settle
  for a flat top-N.
- *Sample granularity*. `setitimer` is bound by kernel jiffies; on this
  WSL2 kernel that's ~1 ms even though we ask for 100 µs. To accumulate
  hundreds of samples per configuration we run several seconds of
  work (40M events on the canonical signal ⇒ ~6 s on this host).
- *Single thread*. The hot loop is single-threaded by design, so this is
  fine.

The methodology is calibrated by a built-in negative control:
`jit_rt_rolling_std`, which P0 *did not* lower, must still appear at
roughly the same percentage in both configurations. If the methodology
were really measuring some artifact of the second run (cache state,
e.g.), `rolling_std` would also have moved. It doesn't.

## How to read the artifact

Each artifact has three blocks:

1. **Summary** — wall-clock time, total sample fraction in `jit_rt_*`
   entry wrappers, in `jitse::` inner helpers (like
   `RingStatsStddevSample`), and in `[JIT]` body code.
2. **Per-op breakdown** — exactly the headline:

   | helper        | none | all | lowered |
   | ------------- | ---- | --- | ------- |
   | `jit_rt_ema*` | X%   | 0%  | yes     |
   | `jit_rt_sma*` | X%   | 0%  | yes     |
   | `jit_rt_lag`  | X%   | 0%  | yes     |
   | `jit_rt_rolling_std` | Y% | Y% (control) | no |

3. **Top symbols** — a perf-report-style flat ranking for each
   configuration, including unrelated symbols (`memcpy`, `__memset`,
   `__cxx_atexit` glue, etc.) so it's clear the methodology is not
   filtering anything out.

## Reproducing

```
cd build-wsl
make -j8 runtime_call_profile
./runtime_call_profile ../examples/profile_canonical.sig \
    --events=40000000 --sample-us=100 \
    --out-dir=../bench/results/perf
```

For the originally-named `filtered_momentum.sig`:

```
./runtime_call_profile ../examples/filtered_momentum.sig \
    --events=40000000 --sample-us=100 \
    --out-dir=../bench/results/perf/filtered_momentum
```

## Smoke gate

`test/runtime_call_profile_test.cpp` runs the same dual-configuration
profile on a short workload (20M events) and asserts:

- Every helper that P0 lowered (`jit_rt_ema_alpha`,
  `jit_rt_sma_prepare`, `jit_rt_lag`) must show ≤0.5% sample share in
  `lowering=all`.
- The control helper `jit_rt_rolling_std` (not lowered) must still
  show >0.5% sample share in `lowering=all`. If it doesn't, either the
  methodology is broken or someone secretly lowered `rolling_std`
  without telling the doc.

If a future refactor accidentally re-routes a lowered op through the
runtime helper (e.g. removing the IR lowering branch in
`jit_compiler.cpp`), this test fires.

The gate is conservatively gated: if either configuration produces
fewer than 100 samples (which can happen on virtualization hosts with
coarse `SIGPROF` granularity), the test exits 0 with `SKIP` rather
than asserting on a noisy denominator.

## What the artifact shows, headline numbers

From the canonical profile (40M events, ~6 s of CPU each side):

- `jit_rt_sma_prepare`: 4.0% → 0.08% (a one-off slot accessor)
- `jit_rt_ema_alpha`: 1.0% → 0.08%
- `jit_rt_lag`: 0.85% → 0% (under sampling floor)
- `jit_rt_rolling_std` (control): 3.6% → 3.9%
- Wall-clock speedup: **1.19×**

The wall-clock speedup is *much* smaller than the helper-share drop
suggests because the dominant cost on this signal is
`RingStatsStddevSample` inside `jit_rt_rolling_std`, which P0 did not
touch. That's the natural next lowering target — see the design notes
in `docs/cross_symbol_vectorization.md` for the lowered-state-struct
pattern that would let `rolling_std` follow the same path.

## Next-op hint

The artifact answers the bigger question: **"where to invest next?"**

- `jitse::RingStatsStddevSample`: 60% (none) → 87% (all). The dominant
  CPU consumer on stateful signals. It is called from
  `jit_rt_rolling_std` and is the obvious next lowering target.
- `[JIT]` body: 28% (none) → 7% (all). Inlined work moved here from
  the lowered helpers. This drop in `[JIT]` share isn't a regression —
  total `[JIT]` *time* didn't shrink as much as the percentage
  suggests; the percentage shrank because the total denominator
  changed (the helper buckets disappeared but `RingStatsStddevSample`
  stayed constant, so its share rose).
- Entry-point `jit_rt_*` wrappers (everything but `_rolling_std`):
  6.7% (none) → 0.55% (all). The P0 win, measured.
