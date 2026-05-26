// sampling_profiler.h
//
// An in-process, software-only sampling profiler. The motivation is the
// P3 deliverable: produce a function-level time-attribution artifact for
// `filtered_momentum.sig` showing the `jit_rt_*` helpers dominating the
// pre-P0 (lowering=none) hot path and disappearing in the post-P0
// (lowering=all) path. perf(1) is the canonical tool for this, but it is
// not available in this build environment (WSL2 kernel + no sudo for
// linux-tools); this profiler fills that gap.
//
// Mechanism:
//   * `setitimer(ITIMER_PROF, ...)` fires `SIGPROF` every `sample_period_us`
//     of CPU time spent in the calling process.
//   * The signal handler walks the interrupted PC out of the ucontext and
//     resolves it to a symbol name via `dladdr(3)`. Resolution failures are
//     bucketed as "[JIT]" (typical for IPs landing in LLJIT-allocated
//     executable pages) or "[unknown]" depending on the dladdr fields.
//   * Counts are aggregated into a thread-local map keyed by symbol name.
//
// Limitations (vs perf):
//   * No stack unwinding -- only the top frame is attributed. This is
//     enough for the P3 artifact because the runtime helpers are leaf
//     functions; >95% of cycles spent in a helper land its own IP at the
//     top of the stack. perf with `--call-graph dwarf` would give a
//     flamegraph; we settle for a flat top-N.
//   * SIGPROF is CPU-time-based, not wall-clock-based, so samples are
//     attributed to the helper currently on-CPU. Good for our purposes.
//   * Single thread only. Multi-threaded measurement would need per-thread
//     sample buffers and a per-thread SIGPROF handler.
//
// Usage:
//   {
//     SamplingProfiler prof(/*sample_period_us=*/200);
//     prof.Start();
//     // ... hot loop ...
//     prof.Stop();
//     prof.WriteReport(std::cout, /*top_n=*/30);
//   }

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

namespace jitse {

class SamplingProfiler {
 public:
  explicit SamplingProfiler(unsigned sample_period_us = 200);
  ~SamplingProfiler();

  SamplingProfiler(const SamplingProfiler&) = delete;
  SamplingProfiler& operator=(const SamplingProfiler&) = delete;

  // Installs the SIGPROF handler and starts the interval timer. Idempotent
  // calls are not supported; call Stop() before re-Starting.
  bool Start();
  void Stop();

  std::uint64_t TotalSamples() const;

  // Symbol-name -> sample-count snapshot. Sorted desc by count.
  struct SymbolSamples {
    std::string name;
    std::uint64_t samples;
    double percent;  // of TotalSamples()
  };
  std::vector<SymbolSamples> Top(std::size_t n) const;

  // Writes a perf-report-style "Overhead  Samples  Symbol" listing.
  void WriteReport(std::ostream& out, std::size_t top_n,
                   const std::string& header_label) const;
  // Writes the same listing into a markdown-friendly table (no headers).
  void WriteMarkdownTable(std::ostream& out, std::size_t top_n) const;

  // Sum of percent for every symbol whose canonical name starts with
  // `name_prefix`. Used by the smoke gate to verify that `jit_rt_` fraction
  // drops to zero between lowering=none and lowering=all.
  double PercentForPrefix(const std::string& name_prefix) const;

 private:
  // The signal handler aggregates into a process-global state (because it
  // cannot reach `this` from async-signal context). When Stop() runs we
  // snapshot the global into this instance's own buckets, so multiple
  // SamplingProfiler instances measured sequentially each keep their own
  // data. Without this snapshot, the second .Start() would reset the
  // global and lose the first profiler's samples.
  struct SnapshotEntry {
    std::string name;
    std::uint64_t samples = 0;
  };
  unsigned sample_period_us_ = 200;
  bool running_ = false;
  std::uint64_t snapshot_total_ = 0;
  std::vector<SnapshotEntry> snapshot_;
};

}  // namespace jitse
