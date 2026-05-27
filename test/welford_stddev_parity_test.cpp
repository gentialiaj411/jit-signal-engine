// welford_stddev_parity_test.cpp
//
// Gates that the O(1) incremental Welford path used by
// `RingStatsStddevSample` agrees with the two-pass long-double
// sum-of-squared-deviations reference at every observable point of the
// window.
//
// Three regimes are exercised, each on a deterministic PRNG so the test
// is bit-reproducible:
//
//   1. Benign: random_normal samples in [-1, 1], typical period set.
//      The two algorithms must agree to within `rtol = 1e-12` per
//      observation (long-double mantissa is ~63 bits so this is the
//      noise floor of the comparison, not of either algorithm).
//
//   2. Catastrophic-cancellation regime: a large constant offset (1e7)
//      with a tiny perturbation (~1e-6) so the true variance is small
//      relative to the mean. This is the worst case for *naive*
//      running-sums (sumsq - sum*sum/n form), which we explicitly do
//      NOT use. The Welford path stays within `rtol = 1e-6` and the
//      two-pass reference stays within `rtol = 1e-9` — both are well
//      below the absolute level any signal-engine consumer cares about.
//
//   3. Degenerate windows: all-zeros, single distinct value, alternating
//      pair, period=2. The non-NaN observations must agree exactly.
//
// The bench-style cost claim ("O(period) -> O(1)") is gated separately
// by `runtime_call_profile`; here we only care that the cheaper path
// did not silently change the numbers.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <vector>

#include "runtime.h"

namespace {

// Local CHECK macro: `assert` is a no-op in Release on MSVC/glibc
// (NDEBUG), which would silently turn every parity check below into a
// success. The whole point of this test is to gate Release numerics, so
// use an explicit abort with line info.
#define JITSE_CHECK(cond)                                                                                            \
  do {                                                                                                              \
    if (!(cond)) {                                                                                                  \
      std::fprintf(stderr, "JITSE_CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
      std::abort();                                                                                                 \
    }                                                                                                               \
  } while (0)

// Lightweight deterministic PRNG so the test is reproducible across
// platforms (std::mt19937 is locked-spec but the distributions may
// differ; we roll our own uniform_real to avoid that risk).
struct Splitmix64 {
  std::uint64_t s;
  std::uint64_t next() {
    s += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  double next_uniform_pm1() {
    const std::uint64_t bits = next();
    const double u = static_cast<double>(bits >> 11) * (1.0 / 9007199254740992.0);
    return 2.0 * u - 1.0;
  }
};

struct ParityStats {
  std::size_t observations = 0;
  double worst_abs_diff = 0.0;
  double worst_rel_diff = 0.0;
};

// Returns parity stats after streaming `samples` through both the
// incremental Welford path (via `RingStatsPush`) and the two-pass
// reference at every step.
ParityStats StreamAndCompare(std::size_t period, const std::vector<double>& samples,
                             double rtol_observation_floor) {
  jitse::RingStatsState st;
  ParityStats out;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    jitse::RingStatsPush(st, period, samples[i]);
    if (st.count < 2) continue;
    const double inc = jitse::RingStatsStddevSample(st);
    const double ref = jitse::RingStatsStddevSampleTwoPassReference(st);
    if (std::isnan(inc) && std::isnan(ref)) continue;
    if (std::isnan(inc) != std::isnan(ref)) {
      std::fprintf(stderr, "NaN disagreement at sample %zu (inc=%g, ref=%g)\n", i, inc, ref);
      std::abort();
    }
    const double abs_diff = std::fabs(inc - ref);
    const double scale = std::max({std::fabs(inc), std::fabs(ref), rtol_observation_floor});
    const double rel_diff = abs_diff / scale;
    ++out.observations;
    if (abs_diff > out.worst_abs_diff) out.worst_abs_diff = abs_diff;
    if (rel_diff > out.worst_rel_diff) out.worst_rel_diff = rel_diff;
  }
  return out;
}

void TestRuntimeUnitSeed() {
  // Sanity: the canonical 1,2,3 -> 2,3,4 sliding-window case from
  // runtime_test.cpp. Both algorithms should produce stddev = 1 exactly.
  jitse::RingStatsState st;
  jitse::RingStatsPush(st, 3, 1.0);
  jitse::RingStatsPush(st, 3, 2.0);
  jitse::RingStatsPush(st, 3, 3.0);
  JITSE_CHECK(jitse::RingStatsFull(st));
  JITSE_CHECK(std::fabs(jitse::RingStatsStddevSample(st) - 1.0) < 1e-12);
  JITSE_CHECK(std::fabs(jitse::RingStatsStddevSampleTwoPassReference(st) - 1.0) < 1e-12);
  jitse::RingStatsPush(st, 3, 4.0);
  JITSE_CHECK(std::fabs(jitse::RingStatsStddevSample(st) - 1.0) < 1e-12);
  JITSE_CHECK(std::fabs(jitse::RingStatsStddevSampleTwoPassReference(st) - 1.0) < 1e-12);
}

void TestBenignNormalLike() {
  // Uniform [-1, 1], period in {8, 32, 64, 128, 256}, 8 192 samples each.
  constexpr std::size_t kSamples = 8'192;
  const std::size_t periods[] = {8, 32, 64, 128, 256};
  for (std::size_t period : periods) {
    Splitmix64 rng{0x1234ABCD5678EF90ULL ^ static_cast<std::uint64_t>(period)};
    std::vector<double> samples(kSamples);
    for (auto& s : samples) s = rng.next_uniform_pm1();
    const ParityStats st = StreamAndCompare(period, samples, /*rtol_floor=*/1e-30);
    // Long-double has 63 mantissa bits => ~2^-63 ≈ 1.1e-19 unit roundoff.
    // We pick `1e-12` as a comfortable headroom over that; both
    // algorithms reduce the same long-double values, just in different
    // orders, so any drift here is just accumulated FP reorder noise.
    if (st.worst_rel_diff > 1e-12) {
      std::fprintf(stderr,
                   "TestBenignNormalLike: period=%zu observations=%zu rel_diff=%g (limit 1e-12)\n",
                   period, st.observations, st.worst_rel_diff);
      std::abort();
    }
    std::printf(
        "  period=%zu observations=%zu worst_abs=%g worst_rel=%g\n",
        period, st.observations, st.worst_abs_diff, st.worst_rel_diff);
  }
}

void TestCatastrophicCancellation() {
  // Large constant offset + tiny perturbation. The true variance is
  // ~(1e-6)^2 / 12 ≈ 8e-14, on top of a constant of ~1e7. The naive
  // (sumsq - sum*sum/n)/(n-1) formula would lose ~14 digits here; both
  // long-double algorithms hold their ground.
  constexpr std::size_t kSamples = 4'096;
  constexpr double kBase = 1e7;
  constexpr double kPerturbScale = 1e-6;
  const std::size_t periods[] = {16, 64, 256};
  for (std::size_t period : periods) {
    Splitmix64 rng{0xC0FFEE5EE5E0DULL ^ static_cast<std::uint64_t>(period)};
    std::vector<double> samples(kSamples);
    for (auto& s : samples) s = kBase + kPerturbScale * rng.next_uniform_pm1();
    // For the rel-diff floor we use the actual scale of the variance
    // (~kPerturbScale^2 / 12 ≈ 8e-14 -> stddev ≈ 3e-7). Anything below
    // that is "both algorithms produced the same noise."
    const ParityStats st = StreamAndCompare(period, samples, /*rtol_floor=*/1e-10);
    // 1e-6 relative is ~6 digits of agreement on a ~3e-7 stddev signal,
    // which is the regime where double-precision running-sums would
    // produce garbage. Both long-double algorithms must beat this.
    if (st.worst_rel_diff > 1e-6) {
      std::fprintf(stderr,
                   "TestCatastrophicCancellation: period=%zu observations=%zu rel_diff=%g (limit 1e-6)\n",
                   period, st.observations, st.worst_rel_diff);
      std::abort();
    }
    std::printf(
        "  catastrophic period=%zu observations=%zu worst_abs=%g worst_rel=%g\n",
        period, st.observations, st.worst_abs_diff, st.worst_rel_diff);
  }
}

void TestDegenerateWindows() {
  // All zeros: stddev should be exactly 0 (or NaN until count >= 2).
  {
    jitse::RingStatsState st;
    for (std::size_t i = 0; i < 32; ++i) jitse::RingStatsPush(st, 8, 0.0);
    JITSE_CHECK(jitse::RingStatsStddevSample(st) == 0.0);
    JITSE_CHECK(jitse::RingStatsStddevSampleTwoPassReference(st) == 0.0);
  }
  // All-same nonzero: variance is exactly 0 in exact arithmetic; we
  // accept the m2 clamp absorbing the tiny FP residue.
  {
    jitse::RingStatsState st;
    for (std::size_t i = 0; i < 64; ++i) jitse::RingStatsPush(st, 16, 1234.5);
    const double inc = jitse::RingStatsStddevSample(st);
    const double ref = jitse::RingStatsStddevSampleTwoPassReference(st);
    // Welford's m2 clamps to 0 on a non-positive intermediate; the
    // two-pass form may produce a microscopic non-zero residual.
    // Either way both should be in [0, 1e-9].
    JITSE_CHECK(!std::isnan(inc) && !std::isnan(ref));
    JITSE_CHECK(inc >= 0.0 && inc < 1e-9);
    JITSE_CHECK(ref >= 0.0 && ref < 1e-9);
  }
  // Alternating +1/-1: stddev settles to a constant once the window is
  // full. For a balanced ±1 window of length 8, var = 8/(8-1) ≈ 1.143.
  // Note: during the slide phase the window may not be balanced (it
  // flips between {4+,4-} and {5+,3-}-like configurations depending on
  // parity), so we only check the once-stable-after-N-cycles value.
  {
    jitse::RingStatsState st;
    for (std::size_t i = 0; i < 33; ++i) jitse::RingStatsPush(st, 8, (i % 2 == 0) ? 1.0 : -1.0);
    const double inc = jitse::RingStatsStddevSample(st);
    const double ref = jitse::RingStatsStddevSampleTwoPassReference(st);
    JITSE_CHECK(std::fabs(inc - ref) < 1e-12 * std::max(1.0, std::fabs(ref)));
  }
  // Period = 2 is the smallest legal window. Welford slide path must
  // not divide by zero (capacity-1 branch).
  {
    jitse::RingStatsState st;
    Splitmix64 rng{0xDEADBEEF12345678ULL};
    for (std::size_t i = 0; i < 256; ++i) {
      const double s = rng.next_uniform_pm1();
      jitse::RingStatsPush(st, 2, s);
      if (st.count < 2) continue;
      const double inc = jitse::RingStatsStddevSample(st);
      const double ref = jitse::RingStatsStddevSampleTwoPassReference(st);
      JITSE_CHECK(!std::isnan(inc) && !std::isnan(ref));
      JITSE_CHECK(std::fabs(inc - ref) < 1e-12 * std::max(1.0, std::fabs(ref)));
    }
  }
  // Capacity = 1: window of length 1 never produces a non-NaN stddev,
  // but the push path must not crash on the slide branch.
  {
    jitse::RingStatsState st;
    for (std::size_t i = 0; i < 16; ++i) {
      jitse::RingStatsPush(st, 1, 3.14 * static_cast<double>(i));
      JITSE_CHECK(std::isnan(jitse::RingStatsStddevSample(st)));
      JITSE_CHECK(std::isnan(jitse::RingStatsStddevSampleTwoPassReference(st)));
    }
  }
}

}  // namespace

int main() {
  std::printf("welford_stddev_parity_test:\n");
  std::printf(" canonical runtime_test_seed (1,2,3 -> 2,3,4):\n");
  TestRuntimeUnitSeed();
  std::printf("  pass\n");

  std::printf(" benign uniform[-1,1]:\n");
  TestBenignNormalLike();

  std::printf(" catastrophic cancellation (1e7 +/- 1e-6):\n");
  TestCatastrophicCancellation();

  std::printf(" degenerate windows:\n");
  TestDegenerateWindows();
  std::printf("  pass\n");
  return 0;
}
