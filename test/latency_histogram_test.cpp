// latency_histogram_test.cpp -- correctness tests for the histogram type
// used by the P4 latency artifact. The harness output is only as good as
// the histogram, so we test percentile accuracy on inputs where the right
// answer is known a priori.

#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

#include "latency_histogram.h"

using jitse::LatencyHistogram;

namespace {

bool ApproxEqual(std::uint64_t observed, std::uint64_t expected, double rel_tol) {
  if (observed == expected) return true;
  const double diff = static_cast<double>(observed) - static_cast<double>(expected);
  const double tol = static_cast<double>(expected) * rel_tol + 1.0;
  return std::abs(diff) <= tol;
}

bool CheckPercentile(const LatencyHistogram& h, double p, std::uint64_t expected,
                     double rel_tol, const char* tag) {
  const std::uint64_t observed = h.Percentile(p);
  const bool ok = ApproxEqual(observed, expected, rel_tol);
  std::cout << "  [" << tag << "] p=" << p << " observed=" << observed
            << " expected~" << expected << "  "
            << (ok ? "OK" : "FAIL") << "\n";
  return ok;
}

bool TestUniform() {
  std::cout << "test: uniform [1, 10000] ns\n";
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 10000; ++v) h.Add(v);
  bool ok = true;
  // Histogram is bucketed so percentiles are bucket-midpoints; allow ~7%
  // relative tolerance (one HdrHistogram sub-bucket width).
  ok &= CheckPercentile(h, 0.50, 5000, 0.07, "uniform p50");
  ok &= CheckPercentile(h, 0.90, 9000, 0.07, "uniform p90");
  ok &= CheckPercentile(h, 0.99, 9900, 0.07, "uniform p99");
  ok &= (h.Total() == 10000);
  ok &= (h.Min() == 1);
  ok &= (h.Max() == 10000);
  return ok;
}

bool TestSpikeAtTail() {
  std::cout << "test: 99% at 100ns + 1% at 100000ns (tail spike)\n";
  LatencyHistogram h;
  for (int i = 0; i < 99000; ++i) h.Add(100);
  for (int i = 0; i < 1000; ++i) h.Add(100000);
  bool ok = true;
  ok &= CheckPercentile(h, 0.50, 100, 0.07, "spike p50");
  ok &= CheckPercentile(h, 0.989, 100, 0.07, "spike p98.9");
  // p99.5 is well inside the tail spike so it should land near 100000.
  ok &= CheckPercentile(h, 0.995, 100000, 0.07, "spike p99.5");
  ok &= CheckPercentile(h, 0.999, 100000, 0.07, "spike p99.9");
  ok &= (h.Total() == 100000);
  ok &= (h.Max() == 100000);
  return ok;
}

bool TestMonotonicity() {
  std::cout << "test: percentile monotonicity on random sample\n";
  std::mt19937_64 rng(0xBADC0DEull);
  std::lognormal_distribution<double> dist(std::log(500.0), 1.0);
  LatencyHistogram h;
  for (int i = 0; i < 200000; ++i) {
    const auto v = static_cast<std::uint64_t>(dist(rng));
    h.Add(v == 0 ? 1 : v);
  }
  const auto p50 = h.Percentile(0.50);
  const auto p90 = h.Percentile(0.90);
  const auto p99 = h.Percentile(0.99);
  const auto p999 = h.Percentile(0.999);
  const auto p9999 = h.Percentile(0.9999);
  const bool ok = p50 <= p90 && p90 <= p99 && p99 <= p999 && p999 <= p9999;
  std::cout << "  p50=" << p50 << " p90=" << p90 << " p99=" << p99
            << " p999=" << p999 << " p9999=" << p9999 << "  "
            << (ok ? "OK" : "FAIL (not monotonic)") << "\n";
  return ok;
}

bool TestOverflow() {
  std::cout << "test: overflow accounting\n";
  LatencyHistogram h;
  // Values beyond 2^30 ns should land in the overflow counter, not crash.
  h.Add(1);
  h.Add(static_cast<std::uint64_t>(1ULL << 30) + 1ULL);
  h.Add(static_cast<std::uint64_t>(1ULL << 40));
  const bool ok = h.Overflow() == 2 && h.Total() == 3;
  std::cout << "  overflow=" << h.Overflow() << " total=" << h.Total() << "  "
            << (ok ? "OK" : "FAIL") << "\n";
  return ok;
}

bool TestCsvNonempty() {
  std::cout << "test: CSV emission only includes non-empty buckets\n";
  LatencyHistogram h;
  for (int i = 0; i < 1000; ++i) h.Add(100);
  std::ostringstream oss;
  h.WriteCsv(oss);
  const std::string s = oss.str();
  const bool has_header = s.find("bucket_index") != std::string::npos;
  // Single populated bucket -> exactly two newlines (header + 1 row).
  std::size_t nl = 0;
  for (char c : s) if (c == '\n') ++nl;
  const bool one_row = nl == 2;
  std::cout << "  header=" << has_header << " rows=" << (nl - 1) << "  "
            << (has_header && one_row ? "OK" : "FAIL") << "\n";
  return has_header && one_row;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= TestUniform();
  ok &= TestSpikeAtTail();
  ok &= TestMonotonicity();
  ok &= TestOverflow();
  ok &= TestCsvNonempty();
  std::cout << (ok ? "PASS" : "FAIL") << "\n";
  return ok ? 0 : 1;
}
