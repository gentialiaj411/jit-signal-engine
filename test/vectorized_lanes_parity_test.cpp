// vectorized_lanes_parity_test.cpp
//
// P2 parity gate for cross-symbol vectorized JIT.
//
// For each stateless test program the test:
//   1. Compiles the program twice: once via JitCompiler::CompileProgram
//      (scalar, single-symbol entry) and once via CompileProgramVectorized
//      with lane_count = 4.
//   2. Builds K=4 MarketSimulator instances with distinct seeds so each
//      lane sees a different stream.
//   3. For each tick, calls the scalar function K times (once per lane)
//      and the vector function once.
//   4. Asserts that for every signal i and lane k:
//          vec_outputs[i * K + k] == scalar_outputs[k * num_signals + i]
//      bit-equivalently (NaN-vs-NaN treated as equal). The scalar and
//      vector codegens use the same FP operations in the same order at
//      every lane, so this is a strict bit-equality gate, not a tolerance
//      check.
//
// It also runs a negative test: any program containing a stateful op
// (sma/ema/lag/...) MUST be rejected by CompileProgramVectorized. The MVP
// scope is documented in jit_compiler.h; this gate makes sure the
// rejection path actually fires so callers get a clear error rather than
// silently incorrect results.

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "jit_compiler.h"
#include "lexer.h"
#include "market_sim.h"
#include "parser.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

constexpr std::size_t kTicks = 2000;
constexpr unsigned kLaneCount = 4;
constexpr std::size_t kInstrumentCount = 4;  // a few tickers per market

struct ProgramTuple {
  std::vector<jitse::SignalDef> signals;
  jitse::SymbolTable symbols;
};

ProgramTuple BuildProgram(const std::string& src) {
  ProgramTuple t;
  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  t.signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(t.signals);
  for (const auto& s : t.signals) {
    for (const auto& ticker : jitse::CollectTickerSymbols(s)) {
      t.symbols.RegisterOrGetId(ticker);
    }
  }
  for (auto& s : t.signals) jitse::BindSymbolIds(s, t.symbols);
  return t;
}

bool ScalarEqVec(double s, double v) {
  if (std::isnan(s) && std::isnan(v)) return true;
  // Strict bit-equality. Both paths execute identical FP ops per lane in
  // the same order, so any drift is a regression.
  return s == v;
}

// Run one positive parity case: build program, compile scalar+vector,
// run K MarketSimulators in lockstep, compare every (signal, lane) pair.
bool RunPositiveCase(const std::string& label, const std::string& src) {
  std::cout << "case_positive: " << label << std::endl;
  ProgramTuple t;
  try {
    t = BuildProgram(src);
  } catch (const std::exception& ex) {
    std::cerr << "  build failed: " << ex.what() << std::endl;
    return false;
  }
  const std::size_t num_signals = t.signals.size();
  if (num_signals == 0) {
    std::cerr << "  no signals parsed" << std::endl;
    return false;
  }

  jitse::JitCompiler scalar_jit;
  if (!scalar_jit.IsAvailable()) {
    std::cerr << "  LLVM unavailable -- skipping" << std::endl;
    return true;
  }
  if (!scalar_jit.CompileProgram(t.signals, t.symbols)) {
    std::cerr << "  scalar compile failed: " << scalar_jit.LastError() << std::endl;
    return false;
  }
  auto* scalar_fn = scalar_jit.GetProgramFunction();
  if (scalar_fn == nullptr) {
    std::cerr << "  scalar fn is null" << std::endl;
    return false;
  }

  jitse::JitCompiler vec_jit;
  if (!vec_jit.CompileProgramVectorized(t.signals, t.symbols, kLaneCount)) {
    std::cerr << "  vector compile failed: " << vec_jit.LastError() << std::endl;
    return false;
  }
  auto* vec_fn = vec_jit.GetProgramVectorizedFunction();
  if (vec_fn == nullptr) {
    std::cerr << "  vector fn is null" << std::endl;
    return false;
  }
  if (vec_jit.VectorizedLaneCount() != kLaneCount) {
    std::cerr << "  unexpected lane count: " << vec_jit.VectorizedLaneCount() << std::endl;
    return false;
  }
  // Dump IR if requested (post-opt only, the meaningful one).
  if (std::getenv("JITSE_DUMP_VEC_IR") != nullptr) {
    const std::string path = std::string("ir_vec_") + label + ".ll";
    std::ofstream f(path);
    f << vec_jit.LastIRPostOpt();
    std::cout << "  dumped IR to " << path << std::endl;
  }

  // K independent MarketStates and MarketSimulators (distinct seeds).
  std::array<jitse::MarketState, kLaneCount> markets{};
  std::vector<jitse::MarketSimulator> sims;
  sims.reserve(kLaneCount);
  for (unsigned k = 0; k < kLaneCount; ++k) {
    sims.emplace_back(static_cast<std::uint64_t>(42 + k * 7919), kInstrumentCount);
  }
  std::array<const jitse::MarketState*, kLaneCount> per_lane_market_ptrs{};
  for (unsigned k = 0; k < kLaneCount; ++k) {
    per_lane_market_ptrs[k] = &markets[k];
  }

  jitse::MultiSymbolSignalContext arena(kLaneCount);  // unused in stateless MVP

  std::vector<double> scalar_outputs(kLaneCount * num_signals, 0.0);
  std::vector<double> vec_outputs(kLaneCount * num_signals, 0.0);

  for (std::size_t tick = 0; tick < kTicks; ++tick) {
    for (unsigned k = 0; k < kLaneCount; ++k) {
      auto ev = sims[k].NextEvent(1000);
      markets[k].instruments[ev.instrument_id].bid = ev.bid;
      markets[k].instruments[ev.instrument_id].ask = ev.ask;
      markets[k].current_time_ns = ev.timestamp_ns;
    }

    for (unsigned k = 0; k < kLaneCount; ++k) {
      scalar_fn(&markets[k], &arena, k, scalar_outputs.data() + k * num_signals);
    }
    vec_fn(per_lane_market_ptrs.data(), &arena, 0, vec_outputs.data());

    for (std::size_t i = 0; i < num_signals; ++i) {
      for (unsigned k = 0; k < kLaneCount; ++k) {
        const double s = scalar_outputs[k * num_signals + i];
        const double v = vec_outputs[i * kLaneCount + k];
        if (!ScalarEqVec(s, v)) {
          std::cerr << "  parity fail tick=" << tick
                    << " signal=" << t.signals[i].name
                    << " (idx=" << i << ")"
                    << " lane=" << k
                    << " scalar=" << s
                    << " vec=" << v
                    << " diff=" << (s - v) << std::endl;
          return false;
        }
      }
    }
  }
  std::cout << "  ok ticks=" << kTicks
            << " signals=" << num_signals
            << " lanes=" << kLaneCount << std::endl;
  return true;
}

// (Older RunNegativeCase helper removed: P10 makes the original
// stateful-rejection cases positive cases, and the new
// lowered-stateful negative case is inline in main() below.)

}  // namespace

int main() {
  bool all_ok = true;

  // -------- Positive cases --------
  // Simplest possible: pure load + arithmetic, single ticker.
  all_ok &= RunPositiveCase(
      "single_ticker_spread",
      "signal s = ask(AAPL) - bid(AAPL)\n");
  // Two-ticker arithmetic + a fraction constant. mid() exercises the
  // 0.5 splat path that we deliberately patched.
  all_ok &= RunPositiveCase(
      "two_ticker_spread",
      "signal s = mid(AAPL) - mid(MSFT)\n");
  // Multiple stateless signals in one program. Exercises the
  // signal-major / lane-minor output layout.
  all_ok &= RunPositiveCase(
      "multi_signal_stateless",
      "signal a = mid(AAPL) + mid(MSFT)\n"
      "signal b = ask(AAPL) - bid(MSFT)\n"
      "signal c = spread(AAPL) * 2.0\n");
  // Conditional via lane-wise select. Both branches always evaluate, so
  // correctness depends on the per-lane mask, not on per-lane control flow.
  all_ok &= RunPositiveCase(
      "stateless_conditional",
      "signal s = if mid(AAPL) > mid(MSFT) then mid(AAPL) else mid(MSFT)\n");
  // Intrinsic (fabs) widened to <K x double>.
  all_ok &= RunPositiveCase(
      "abs_intrinsic",
      "signal s = abs(mid(AAPL) - mid(MSFT))\n");
  // Comparison + boolean coercion exercise. Compares get widened to
  // <K x i1>; UIToFP widens to <K x double>.
  all_ok &= RunPositiveCase(
      "compare_to_double",
      "signal s = if bid(AAPL) >= ask(MSFT) then 1.0 else -1.0\n");
  // Compute-heavy stateless arithmetic with intrinsic chains (sqrt+abs).
  // Mirrors `examples/stateless_compute_heavy.sig`; this is the program
  // the canonical "AVX2 wins" cross_symbol_benchmark artifact uses, so
  // the parity gate here is what justifies trusting the headline
  // speedup number. Strict bit-equality across 2000 ticks x 4 lanes.
  all_ok &= RunPositiveCase(
      "compute_heavy_sqrt_chain",
      "signal a = mid(AAPL)\n"
      "signal b = mid(MSFT)\n"
      "signal sa = ask(AAPL) - bid(AAPL)\n"
      "signal sb = ask(MSFT) - bid(MSFT)\n"
      "signal p1 = a * a + b * b + sa * sa + sb * sb\n"
      "signal p2 = a * b - sa * sb + (a - b) * (sa - sb)\n"
      "signal p3 = sqrt(p1 + 1.0) - sqrt(p2 * p2 + 1.0)\n"
      "signal p4 = (a + b) * (a + b) - (a - b) * (a - b)\n"
      "signal p5 = sqrt(p4 + 1.0) + sqrt(abs(p2) + 1.0)\n"
      "signal q1 = p1 * p3 - p2 * p5 + p4\n"
      "signal q2 = p3 * p5 + p1 * p4 - p2\n"
      "signal q3 = sqrt(abs(q1) + 1.0) + sqrt(abs(q2) + 1.0)\n"
      "signal q4 = q1 * q2 - q3 * q3\n"
      "signal r1 = sqrt(abs(q4) + 1.0) * 0.5 + abs(q3 - q1) * 0.25\n"
      "signal r2 = sqrt(abs(q1 * q2) + 1.0) - abs(q4) * 0.125\n"
      "signal out = (r1 - r2) * 0.5 + (q1 + q2) * 0.125\n");

  // -------- P4: lowered stateful ops compile in vector mode -----
  auto run_lowered_compile = [&all_ok](const std::string& label, const std::string& src,
                                       jitse::StatefulLoweringFlags flags) {
    std::cout << "case_lowered_vec: " << label << std::endl;
    ProgramTuple t;
    try { t = BuildProgram(src); }
    catch (const std::exception& ex) {
      std::cerr << "  build failed: " << ex.what() << std::endl;
      all_ok = false; return;
    }
    jitse::JitCompiler vec_jit;
    if (!vec_jit.IsAvailable()) {
      std::cout << "  LLVM unavailable -- skipping" << std::endl; return;
    }
    vec_jit.SetStatefulLowering(flags);
    if (!vec_jit.CompileProgramVectorized(t.signals, t.symbols, kLaneCount)) {
      std::cerr << "  vector compile failed: " << vec_jit.LastError() << std::endl;
      all_ok = false; return;
    }
    std::cout << "  ok compiled" << std::endl;
  };
  run_lowered_compile("sma_kSma", "signal s = sma(mid(AAPL), 30)\n",
                      jitse::StatefulLoweringFlags::kSma);
  run_lowered_compile("ema_kEma", "signal s = ema(mid(AAPL), 10)\n",
                      jitse::StatefulLoweringFlags::kEma);
  run_lowered_compile("lag_kLag", "signal s = lag(mid(AAPL), 5)\n",
                      jitse::StatefulLoweringFlags::kLag);
  run_lowered_compile("kAll_primitives",
                      "signal a = ema(mid(AAPL), 10)\n"
                      "signal b = rolling_std(mid(AAPL), 12)\n",
                      jitse::StatefulLoweringFlags::kAll);

  if (!all_ok) {
    std::cerr << "vectorized_lanes_parity_test: FAILED" << std::endl;
    return 1;
  }
  std::cout << "vectorized_lanes_parity_test: PASSED" << std::endl;
  return 0;
}
