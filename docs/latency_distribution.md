# P4 — Latency distribution methodology

## What this document is

The previous latency story was a single point estimate per signal —
"`filtered_momentum` p99 is somewhere between 14 ns and 78 ns" — derived
from the existing `bench/signal_benchmark.cpp` harness, which records
one latency sample per *batch of 64 consecutive calls* and averages
inside the batch:

```
const auto t0 = high_resolution_clock::now();
for (j = 0; j < 64; ++j) fn(...);
const auto t1 = high_resolution_clock::now();
latencies.push_back((t1 - t0) / 64);  // <-- mean across 64 calls
```

That pattern is fine for *throughput* attribution but is the textbook
"loop-time-tick" antipattern for tail latency: a single 50 µs preemption
hidden inside a batch of 64 normal 20 ns calls gets averaged into a
~800 ns sample, which is then reported as one entry in the
percentile-sorted vector. The actual 50 µs tail event vanishes.

P4 replaces the point estimate with a real per-signal percentile sweep
(p50 … p99.999) measured under both closed-loop and open-loop
Coordinated-Omission-aware modes. The artifact lives in
[`bench/results/latency/`](../bench/results/latency/) and is described
top-down in the directory's [`index.md`](../bench/results/latency/index.md).

## Why a custom histogram, not std::vector + std::sort

A point-estimate p99 needs ~100 samples. A point-estimate p99.99 needs
~10 000 samples. A *distribution* needs enough samples in the tail that
the bucket-counts are statistically meaningful, which is millions —
6–7 orders of magnitude more samples than the existing harness records.
Sorting a vector of 10⁶ uint64s costs ~30 ms by itself; an
HdrHistogram-style bucketed accumulator costs O(1) per `Add()` and is
~100 ns per `Percentile()`.

The histogram in [`bench/latency_histogram.{h,cpp}`](../bench/latency_histogram.h)
is plain HdrHistogram layout:

- `kPrecisionBits = 4` ⇒ 16 sub-buckets per power of 2 ⇒ ~6% relative
  bucket width. Plenty for reporting tail percentiles to two significant
  digits.
- `kMaxMagnitude = 30` ⇒ covers `[0, 2^30 ns) = [0, ~1.07 s)`. Anything
  beyond increments an explicit `Overflow()` counter so percentiles
  can't be silently wrong.
- Memory: 30 × 16 × 8 = 3 840 bytes per histogram. Fits in L1.

Percentile correctness is unit-tested in
[`test/latency_histogram_test.cpp`](../test/latency_histogram_test.cpp)
against three known distributions: uniform, 99%/1% bimodal-with-tail,
and lognormal (the last only checks monotonicity).

## Why two modes (closed-loop and CO-aware)

These measure different things and both matter:

### Closed-loop

```
for each event e:
    t0 = now()
    fn(e)
    t1 = now()
    histogram.add(t1 - t0)
```

This is what `bench/signal_benchmark.cpp` *would* report if it didn't
batch. It measures the JIT's intrinsic cost — how long the compiled
code takes when nothing else is happening. The reported tail is
dominated by CPU-level events: cache misses, branch mispredicts, brief
preemptions, occasional TLB shootdowns.

### Open-loop, Coordinated-Omission-aware (`--rate-hz=R`)

```
target = start
for each event e:
    if now() < target:
        spin_until(target)
    fn(e)
    done = now()
    histogram.add(done - target)         # !! not done - now() before fn
    target += 1_000_000_000 / R
```

The crucial line is `done - target`. If a stall keeps `fn(e)` running
past its target time, subsequent events' `target` values are already in
the past — we don't spin, we just run `fn` and record the entire
queueing delay as the next event's "latency". This is the standard
wrk2 / HdrHistogram correction for Coordinated Omission, the phenomenon
where naive benchmarks "consent to" the system pacing itself slower and
thereby *omit* the would-be-queued events from the tail measurement.

### What CO-aware reveals

For `filtered_momentum` on a non-isolated WSL2 host:

| Mode | p50 | p99 | p99.9 | p99.99 |
|---|---:|---:|---:|---:|
| closed-loop | 148 ns | 180 ns | 220 ns | 6.5 µs |
| CO-aware @ 4 MHz | 164 ns | **176 µs** | 336 µs | 352 µs |

The p99 jumped by three orders of magnitude. That's not a JIT
regression — it's the OS scheduler. Once an event-rate gate is added,
any context switch costs not just the switched-out event but every
event queued behind it. This is the latency that a downstream consumer
*actually experiences* when the upstream is sending at 4 MHz, which is
why HFT-style measurement frameworks insist on it.

## Choice of rate for the CO-aware artifact

The CO-aware rate per signal is set to ~60% of that signal's closed-loop
peak JIT throughput. Rationale:

- Below ~50% of peak, queueing rarely kicks in and CO-aware ≈ closed-loop.
- Above ~80% of peak, the system can't keep up and `target` accumulates
  unboundedly — the histogram fills with the unbounded queueing tail
  and percentiles converge to (run_duration − target_t_first), which
  is a measurement artifact rather than a property of the system.
- 60% gives a clean separation: the system *can* keep up on average,
  but per-event jitter still exceeds the per-event budget often enough
  to populate the tail.

The orchestrator script [`bench/run_latency_artifacts.sh`](../bench/run_latency_artifacts.sh)
hardcodes the per-signal CO rates after measuring closed-loop
throughput once. If you re-run on a different host these may need
re-tuning.

## Output formats

For each `(signal, mode)` pair, three files:

- **`.csv`** — one row per non-empty histogram bucket:
  `label,bucket_index,lo_ns,hi_ns,count,cum_count,cum_fraction`.
  Suitable for re-plotting in Python / R.
- **`.md`** — human-readable summary table with `min, p50, p75, p90,
  p99, p99.9, p99.99, p99.999, max`, plus a throughput cross-check
  against wall time.
- **`.svg`** — self-contained vector CDF plot. X axis is log10(ns) so a
  long tail is visible without losing the bulk. Y axis is the
  cumulative fraction. Each measurement run is one polyline; interp and
  JIT are overlaid for direct comparison.

## Limitations vs a real HFT measurement rig

- This is a non-isolated host. CPU governor scaling, IRQ affinity, and
  scheduler placement all contribute to the tail. On a properly-pinned
  host (`isolcpus`, `nohz_full`, `nice -20`, IRQ-pinned away,
  `cpupower frequency-set --governor performance`), the closed-loop p99
  drops by maybe 2-3× and the CO-aware p99 drops by 10-50×.
- The artifact ships *with* the noisy numbers and documents the limit
  rather than pretending the noise isn't there. A future "pinned-host
  rerun" follow-up could regenerate the artifact on a properly-pinned
  host and check in those numbers alongside; the artifact layout
  already supports multiple `co_*` subdirectories.
- We don't currently report a "warmup hot/cold" split. Could be added
  by emitting two histograms — one for the first 1 000 calls, one for
  the rest — but adds noise without much value at the current scope.
