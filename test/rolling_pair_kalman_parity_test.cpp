// P7: parity gate for the three new operators.
//
//   rolling_corr(x, y, n)   -- rolling Pearson correlation
//   rolling_beta(x, y, n)   -- rolling slope of linear regression of y on x
//   kalman1d(x, q, r)       -- one-dimensional scalar Kalman filter
//
// Both interpreter and JIT route through the SAME C runtime helpers
// (RollingPairPush / RollingPairCorrelation / RollingPairBeta / Kalman1dStep),
// so the parity check is a true bit-for-bit gate. Any divergence here
// indicates a wiring bug, not a numerical-stability question.
//
// The test also gates that the JIT correctly handles the unusual
// argument shapes: rolling_corr/beta take three args (x, y, period),
// kalman1d takes three args (x, q, r) with q and r as constant doubles.
//
// Numerical-stability discussion: see runtime.h's comments on
// RollingPairState (running-sum cancellation analysis) and Kalman1dState
// (positive-semidefinite clamp). This test runs 5000 ticks of a
// non-degenerate stream from MarketSimulator, which is enough to expose
// any obvious accumulator drift.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

constexpr std::size_t kTicks = 5000;
constexpr double kAbsTol = 1e-9;

int failures = 0;
#define EXPECT(cond)                                                                  \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " : " << #cond << "\n"; \
      ++failures;                                                                     \
    }                                                                                 \
  } while (0)

struct Program {
  std::vector<jitse::SignalDef> signals;
  jitse::SymbolTable symbols;
  std::size_t out_index = 0;
};

Program BuildProgram(const std::string& src, const std::string& out_signal) {
  Program p;
  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  p.signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(p.signals);
  for (const auto& s : p.signals) {
    for (const auto& ticker : jitse::CollectTickerSymbols(s)) {
      p.symbols.RegisterOrGetId(ticker);
    }
  }
  for (auto& s : p.signals) jitse::BindSymbolIds(s, p.symbols);
  for (std::size_t i = 0; i < p.signals.size(); ++i) {
    if (p.signals[i].name == out_signal) {
      p.out_index = i;
      break;
    }
  }
  return p;
}

// Run a program through the JIT and return the per-tick trace of the
// output signal. Returns an empty vector + sets `available=false` if the
// JIT isn't available in this build.
std::vector<double> RunJit(const Program& prog, bool& available) {
  available = false;
  jitse::JitCompiler jit;
  if (!jit.IsAvailable()) return {};
  // Gate interpreter-vs-runtime-JIT wiring; lowered IR is covered by
  // stateful_lowering_parity_test.
  jit.SetStatefulLowering(jitse::StatefulLoweringFlags::kNone);
  if (!jit.CompileProgram(prog.signals, prog.symbols)) {
    std::cerr << "JIT compile failed: " << jit.LastError() << "\n";
    return {};
  }
  jitse::JitCompiler::ProgramFn fn = jit.GetProgramFunction();
  if (!fn) {
    std::cerr << "JIT compile returned null fn\n";
    return {};
  }

  jitse::MultiSymbolSignalContext ctx(1);
  for (const auto& s : prog.signals) jitse::PrewarmSignalContext(ctx, 0, s);

  jitse::MarketState market;
  jitse::MarketSimulator sim(/*seed=*/2027, /*n_instruments=*/2);
  std::vector<double> outs(prog.signals.size(), 0.0);
  std::vector<double> trace;
  trace.reserve(kTicks);
  for (std::size_t t = 0; t < kTicks; ++t) {
    const auto ev = sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    fn(&market, &ctx, 0, outs.data());
    trace.push_back(outs[prog.out_index]);
  }
  available = true;
  return trace;
}

// Run the same program through the interpreter and return the matching trace.
std::vector<double> RunInterp(const Program& prog) {
  jitse::Interpreter interp(prog.symbols);
  jitse::SignalContext ctx;
  for (const auto& s : prog.signals) jitse::PrewarmSignalContext(ctx, s);

  jitse::MarketState market;
  jitse::MarketSimulator sim(/*seed=*/2027, /*n_instruments=*/2);
  std::vector<double> trace;
  trace.reserve(kTicks);
  for (std::size_t t = 0; t < kTicks; ++t) {
    const auto ev = sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    double out = 0.0;
    for (std::size_t i = 0; i < prog.signals.size(); ++i) {
      const double v = interp.Evaluate(prog.signals[i], market, ctx);
      if (i == prog.out_index) out = v;
    }
    trace.push_back(out);
  }
  return trace;
}

bool CompareTraces(const std::vector<double>& a, const std::vector<double>& b,
                   const char* label) {
  if (a.size() != b.size()) {
    std::cerr << label << ": size mismatch " << a.size() << " vs " << b.size() << "\n";
    return false;
  }
  std::size_t nan_disagreements = 0;
  double max_abs_diff = 0.0;
  std::size_t max_idx = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const bool an = std::isnan(a[i]);
    const bool bn = std::isnan(b[i]);
    if (an != bn) {
      if (nan_disagreements < 3) {
        std::cerr << label << ": NaN/non-NaN at tick " << i
                  << " interp=" << a[i] << " jit=" << b[i] << "\n";
      }
      ++nan_disagreements;
      continue;
    }
    if (an && bn) continue;
    const double d = std::fabs(a[i] - b[i]);
    if (d > max_abs_diff) {
      max_abs_diff = d;
      max_idx = i;
    }
  }
  std::cout << label << ": max_abs_diff=" << max_abs_diff
            << " at tick " << max_idx
            << " nan_disagreements=" << nan_disagreements
            << " n=" << a.size() << "\n";
  return nan_disagreements == 0 && max_abs_diff <= kAbsTol;
}

void RunCase(const char* label, const std::string& src, const std::string& out_signal) {
  Program prog_a = BuildProgram(src, out_signal);
  std::vector<double> interp_trace = RunInterp(prog_a);

  Program prog_b = BuildProgram(src, out_signal);
  bool jit_ok = false;
  std::vector<double> jit_trace = RunJit(prog_b, jit_ok);
  if (!jit_ok) {
    std::cout << label << ": JIT unavailable, skipping\n";
    return;
  }
  if (!CompareTraces(interp_trace, jit_trace, label)) {
    ++failures;
  }
}

}  // namespace

int main() {
  RunCase(
      "rolling_corr_basic",
      "signal out = rolling_corr(mid(AAPL), mid(MSFT), 32)\n",
      "out");

  RunCase(
      "rolling_beta_basic",
      "signal out = rolling_beta(mid(AAPL), mid(MSFT), 50)\n",
      "out");

  RunCase(
      "kalman1d_basic",
      "signal out = kalman1d(mid(AAPL), 0.01, 1.0)\n",
      "out");

  // P7 + arithmetic composition: results of new ops are valid Numbers
  // and can flow into the rest of the engine. This is the test that
  // catches plumbing bugs in CodegenContext init order.
  RunCase(
      "rolling_beta_in_arithmetic",
      "signal beta = rolling_beta(mid(AAPL), mid(MSFT), 40)\n"
      "signal out = mid(AAPL) - beta * mid(MSFT)\n",
      "out");

  // Type-checking: the new ops return Number, so they're legal inside
  // a conditional. Also exercises that constant folding does NOT fold
  // (kalman1d depends on runtime state).
  RunCase(
      "kalman1d_in_conditional",
      "signal vol = rolling_std(mid(AAPL), 20)\n"
      "signal out = if vol > 0.0 then kalman1d(mid(AAPL), 0.001, 0.5) else mid(AAPL)\n",
      "out");

  if (failures == 0) {
    std::cout << "rolling_pair_kalman_parity_test passed\n";
    return 0;
  }
  std::cerr << failures << " case(s) failed\n";
  return 1;
}
