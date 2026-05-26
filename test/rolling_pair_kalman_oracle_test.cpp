// P7: differential oracle test for rolling_corr, rolling_beta, kalman1d.
//
// The parity test (rolling_pair_kalman_parity_test) verifies that
// interpreter and JIT agree. This test goes further: it computes a
// reference value with a completely independent algorithm and checks
// that the engine's output is within an acceptable tolerance.
//
// The reference algorithms here are deliberately *different* from the
// engine's implementation so the test catches systematic bugs that
// would not show up if interp + JIT shared a faulty helper:
//
//   rolling_corr / rolling_beta : naive two-pass O(window) recomputation
//     from the buffered samples. Slower but immune to running-sum
//     cancellation; this is the "definitionally correct" answer.
//
//   kalman1d : a hand-written textbook scalar Kalman update that uses
//     local double variables instead of the runtime state struct.
//
// Tolerance: 1e-9 absolute for non-zero values, 1e-9 absolute around
// zero. The running-sum form is well-conditioned for the synthetic
// stream here (5000 samples, prices in [50, 200]) so this is well
// inside the regime where it agrees with the two-pass form.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "interpreter.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

int failures = 0;
#define EXPECT(cond)                                                                  \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " : " << #cond << "\n"; \
      ++failures;                                                                     \
    }                                                                                 \
  } while (0)

constexpr std::size_t kTicks = 5000;
constexpr double kTol = 1e-9;

// Deterministic, well-conditioned (x, y) stream. The two series are
// correlated (a linear combination of a common factor plus independent
// noise) so rolling_corr should be far from {0, +1, -1} most of the
// time. This stresses the off-diagonal terms in the running sums.
struct Stream {
  std::vector<double> xs;
  std::vector<double> ys;
};

Stream MakeStream() {
  std::mt19937 rng(7);
  std::normal_distribution<double> noise(0.0, 1.0);
  Stream s;
  s.xs.reserve(kTicks);
  s.ys.reserve(kTicks);
  double level = 100.0;
  for (std::size_t i = 0; i < kTicks; ++i) {
    const double f = noise(rng);         // common factor
    const double n1 = 0.7 * noise(rng);  // independent noise
    const double n2 = 0.7 * noise(rng);
    level += 0.05 * f;                   // slow drift
    s.xs.push_back(level + f + n1);
    s.ys.push_back(level + 0.6 * f + n2);
  }
  return s;
}

// Two-pass reference: compute mean over the trailing window then sum
// (x-mean_x)(y-mean_y). O(window) per call, immune to running-sum
// cancellation.
double ReferenceCorr(const Stream& s, std::size_t i, std::size_t period) {
  if (i + 1 < period) return std::numeric_limits<double>::quiet_NaN();
  const std::size_t start = i + 1 - period;
  double sx = 0.0, sy = 0.0;
  for (std::size_t k = start; k <= i; ++k) {
    sx += s.xs[k];
    sy += s.ys[k];
  }
  const double mx = sx / static_cast<double>(period);
  const double my = sy / static_cast<double>(period);
  double sxx = 0.0, syy = 0.0, sxy = 0.0;
  for (std::size_t k = start; k <= i; ++k) {
    const double dx = s.xs[k] - mx;
    const double dy = s.ys[k] - my;
    sxx += dx * dx;
    syy += dy * dy;
    sxy += dx * dy;
  }
  if (sxx <= 0.0 || syy <= 0.0) return std::numeric_limits<double>::quiet_NaN();
  double r = sxy / std::sqrt(sxx * syy);
  if (r > 1.0) r = 1.0;
  if (r < -1.0) r = -1.0;
  return r;
}

double ReferenceBeta(const Stream& s, std::size_t i, std::size_t period) {
  if (i + 1 < period) return std::numeric_limits<double>::quiet_NaN();
  const std::size_t start = i + 1 - period;
  double sx = 0.0, sy = 0.0;
  for (std::size_t k = start; k <= i; ++k) {
    sx += s.xs[k];
    sy += s.ys[k];
  }
  const double mx = sx / static_cast<double>(period);
  const double my = sy / static_cast<double>(period);
  double sxx = 0.0, sxy = 0.0;
  for (std::size_t k = start; k <= i; ++k) {
    const double dx = s.xs[k] - mx;
    const double dy = s.ys[k] - my;
    sxx += dx * dx;
    sxy += dx * dy;
  }
  if (sxx <= 0.0) return std::numeric_limits<double>::quiet_NaN();
  return sxy / sxx;
}

// Textbook scalar Kalman. Uses local state variables, not Kalman1dState.
struct KalmanRef {
  double x_hat = 0.0;
  double p = 0.0;
  bool init = false;
  double Step(double z, double q, double r) {
    if (!init) {
      x_hat = z;
      p = r;
      init = true;
      return x_hat;
    }
    const double p_pred = p + q;
    const double K = p_pred / (p_pred + r);
    x_hat = x_hat + K * (z - x_hat);
    p = (1.0 - K) * p_pred;
    return x_hat;
  }
};

bool NearOrBothNaN(double a, double b, double tol = kTol) {
  if (std::isnan(a) && std::isnan(b)) return true;
  if (std::isnan(a) || std::isnan(b)) return false;
  return std::fabs(a - b) <= tol;
}

// Drive a SignalContext through the interpreter against the synthetic
// stream and collect the trace. The signal's expression is a single
// `rolling_corr(x_value, y_value, period)` call where `x_value` and
// `y_value` are placeholders we substitute by directly setting
// `MarketState.instruments[0].bid/ask` for two synthetic tickers.
//
// We build the signal source as `rolling_corr(mid(X), mid(Y), N)` and
// then feed the stream into the two tickers' bid==ask values.
std::vector<double> RunInterpStream(const std::string& src, const Stream& stream) {
  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(signals);
  jitse::SymbolTable symbols;
  symbols.RegisterOrGetId("X");
  symbols.RegisterOrGetId("Y");
  for (auto& s : signals) jitse::BindSymbolIds(s, symbols);

  jitse::Interpreter interp(symbols);
  jitse::SignalContext ctx;
  for (const auto& s : signals) jitse::PrewarmSignalContext(ctx, s);

  jitse::MarketState market;
  std::vector<double> trace;
  trace.reserve(stream.xs.size());
  for (std::size_t t = 0; t < stream.xs.size(); ++t) {
    market.instruments[0].bid = stream.xs[t];
    market.instruments[0].ask = stream.xs[t];
    market.instruments[1].bid = stream.ys[t];
    market.instruments[1].ask = stream.ys[t];
    double out = 0.0;
    for (std::size_t i = 0; i < signals.size(); ++i) {
      const double v = interp.Evaluate(signals[i], market, ctx);
      // The last signal in the program is the output.
      if (i + 1 == signals.size()) out = v;
    }
    trace.push_back(out);
  }
  return trace;
}

void TestRollingCorrAgainstOracle() {
  const std::size_t period = 64;
  const Stream stream = MakeStream();
  const std::string src =
      "signal out = rolling_corr(mid(X), mid(Y), 64)\n";
  const std::vector<double> got = RunInterpStream(src, stream);

  std::size_t mismatches = 0;
  double worst_abs = 0.0;
  std::size_t worst_idx = 0;
  for (std::size_t i = 0; i < stream.xs.size(); ++i) {
    const double ref = ReferenceCorr(stream, i, period);
    if (!NearOrBothNaN(got[i], ref)) {
      ++mismatches;
      const double d = std::fabs(got[i] - ref);
      if (d > worst_abs) {
        worst_abs = d;
        worst_idx = i;
      }
    }
  }
  std::cout << "rolling_corr oracle: mismatches=" << mismatches
            << " worst_abs=" << worst_abs << " at tick=" << worst_idx << "\n";
  EXPECT(mismatches == 0);
}

void TestRollingBetaAgainstOracle() {
  const std::size_t period = 50;
  const Stream stream = MakeStream();
  const std::string src =
      "signal out = rolling_beta(mid(X), mid(Y), 50)\n";
  const std::vector<double> got = RunInterpStream(src, stream);

  std::size_t mismatches = 0;
  double worst_abs = 0.0;
  std::size_t worst_idx = 0;
  for (std::size_t i = 0; i < stream.xs.size(); ++i) {
    const double ref = ReferenceBeta(stream, i, period);
    if (!NearOrBothNaN(got[i], ref)) {
      ++mismatches;
      const double d = std::fabs(got[i] - ref);
      if (d > worst_abs) {
        worst_abs = d;
        worst_idx = i;
      }
    }
  }
  std::cout << "rolling_beta oracle: mismatches=" << mismatches
            << " worst_abs=" << worst_abs << " at tick=" << worst_idx << "\n";
  EXPECT(mismatches == 0);
}

void TestKalman1dAgainstOracle() {
  const Stream stream = MakeStream();
  const double q = 0.01;
  const double r = 1.0;
  const std::string src =
      "signal out = kalman1d(mid(X), 0.01, 1.0)\n";
  const std::vector<double> got = RunInterpStream(src, stream);

  KalmanRef ref;
  std::size_t mismatches = 0;
  double worst_abs = 0.0;
  std::size_t worst_idx = 0;
  for (std::size_t i = 0; i < stream.xs.size(); ++i) {
    const double r_val = ref.Step(stream.xs[i], q, r);
    if (!NearOrBothNaN(got[i], r_val)) {
      ++mismatches;
      const double d = std::fabs(got[i] - r_val);
      if (d > worst_abs) {
        worst_abs = d;
        worst_idx = i;
      }
    }
  }
  std::cout << "kalman1d oracle: mismatches=" << mismatches
            << " worst_abs=" << worst_abs << " at tick=" << worst_idx << "\n";
  EXPECT(mismatches == 0);
}

}  // namespace

int main() {
  TestRollingCorrAgainstOracle();
  TestRollingBetaAgainstOracle();
  TestKalman1dAgainstOracle();
  if (failures == 0) {
    std::cout << "rolling_pair_kalman_oracle_test passed\n";
    return 0;
  }
  std::cerr << failures << " case(s) failed\n";
  return 1;
}
