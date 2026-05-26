// latency_histogram.h
//
// HdrHistogram-style log-linear bucketed histogram for nanosecond latency
// measurements. The point of using this layout instead of a sorted
// std::vector is twofold:
//
//   1. O(1) Add() with no allocation in the hot path. The existing
//      bench/signal_benchmark.cpp pushes one sample per batch of 64 into a
//      vector and then sorts at the end. That's fine for p50/p99 with
//      ~1e4 batched-mean samples but it hides per-call tail latency and
//      caps the practical sample count.
//   2. Per-call accuracy at high quantiles (p999, p9999) requires
//      ~1e4..1e6 samples. Sorting that vector takes ~10 ms by itself; the
//      bucketed histogram percentile is a single linear scan over the
//      ~480 buckets and is essentially free.
//
// Bucket layout: standard HdrHistogram with `kPrecisionBits` sub-buckets
// per power of two. For `kPrecisionBits = 4` the relative bucket width is
// ~6%, plenty for reporting tail percentiles to two significant digits.
//
// Total range: [0, 2^kMaxMagnitude) ns. For kMaxMagnitude = 30 that's
// ~1.07 s -- well past anything a JIT signal call could plausibly take.
// Anything beyond goes into the last bucket so percentiles can't be
// silently wrong.
//
// Memory: kMaxMagnitude * (1 << kPrecisionBits) * sizeof(uint64_t)
//       = 30 * 16 * 8 = 3840 bytes per histogram. Fits in L1.

#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace jitse {

class LatencyHistogram {
 public:
  static constexpr int kPrecisionBits = 4;
  static constexpr int kSubBuckets = 1 << kPrecisionBits;  // 16
  static constexpr int kMaxMagnitude = 30;                  // covers ~1 s
  static constexpr int kBucketCount = kMaxMagnitude * kSubBuckets;

  LatencyHistogram() { Reset(); }

  void Reset() {
    buckets_.fill(0);
    total_ = 0;
    overflow_ = 0;
    max_observed_ = 0;
    min_observed_ = UINT64_MAX;
  }

  // Records one ns sample. Async-signal-safe (no allocation, no locking).
  inline void Add(std::uint64_t ns) {
    if (ns < min_observed_) min_observed_ = ns;
    if (ns > max_observed_) max_observed_ = ns;
    ++total_;
    if (ns == 0) {
      ++buckets_[0];
      return;
    }
#if defined(__GNUC__) || defined(__clang__)
    const int msb = 63 - __builtin_clzll(ns);
#else
    int msb = 0;
    for (std::uint64_t v = ns; v >>= 1;) ++msb;
#endif
    if (msb >= kMaxMagnitude) {
      ++overflow_;
      ++buckets_[kBucketCount - 1];
      return;
    }
    // Sub-bucket index: keep the top (kPrecisionBits + 1) bits, drop the
    // implicit leading 1 to land in [0, kSubBuckets).
    int shift = msb - kPrecisionBits;
    if (shift < 0) shift = 0;
    const std::uint64_t sub = (ns >> shift) & (kSubBuckets - 1);
    const int idx = msb * kSubBuckets + static_cast<int>(sub);
    ++buckets_[idx];
  }

  // Returns the ns value at the given percentile (0..1). Linear scan of
  // ~480 buckets; ~100 ns. Result is bucket-midpoint -- accuracy is
  // ~3% relative for kPrecisionBits=4.
  std::uint64_t Percentile(double p) const;

  std::uint64_t Total() const { return total_; }
  std::uint64_t Min() const { return total_ == 0 ? 0 : min_observed_; }
  std::uint64_t Max() const { return max_observed_; }
  std::uint64_t Overflow() const { return overflow_; }

  // For SVG and CSV rendering: bucket index -> {lo_ns, hi_ns, count}.
  struct BucketView {
    std::uint64_t lo_ns;
    std::uint64_t hi_ns;
    std::uint64_t count;
  };
  BucketView At(int idx) const;

  // CSV columns: bucket_index,lo_ns,hi_ns,count,cum_count,cum_fraction.
  // Only emits non-empty buckets.
  void WriteCsv(std::ostream& out, const std::string& label_col = "") const;

  // Markdown summary table with named percentiles, plus min/max/total.
  void WriteMarkdownSummary(std::ostream& out, const std::string& label) const;

  // Self-contained SVG CDF plot, X = log10(ns), Y = cumulative fraction.
  // `series_labels[i]` paired with `series[i]` -> draws N overlaid CDFs.
  static void WriteSvgCdf(std::ostream& out,
                          const std::vector<const LatencyHistogram*>& series,
                          const std::vector<std::string>& series_labels,
                          int width_px = 800, int height_px = 480);

 private:
  std::array<std::uint64_t, kBucketCount> buckets_{};
  std::uint64_t total_ = 0;
  std::uint64_t overflow_ = 0;
  std::uint64_t max_observed_ = 0;
  std::uint64_t min_observed_ = UINT64_MAX;
};

}  // namespace jitse
