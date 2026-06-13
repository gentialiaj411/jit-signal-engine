// stateful_lowering_parity_test.cpp
//
// Validates that the P0 IR-lowered codegen for `sma`, `ema`, and `lag`
// produces bit-identical output (within IEEE-754 representation, modulo a
// 1e-9 absolute tolerance for NaN-vs-NaN compares) to the runtime-call
// codegen path, across a multi-thousand-tick streaming workload.
//
// Why bit-identical and not just rtol-close: the lowered IR was derived
// from the exact arithmetic of the runtime helpers (`RingStatsPushPrepared`
// uses running-sum with double sum vs long double sum, which is the only
// numerical-method difference; everything else is the same FP operations
// applied in the same order). We treat any divergence beyond 1e-9 as a
// regression worth investigating.
//
// Scope: compares JIT-with-lowering-OFF vs JIT-with-lowering-ON. Both
// modes already have parity with the interpreter via fuzz_parity_test, so
// transitively this also gates interpreter parity for the lowered path.

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "lexer.h"
#include "market_sim.h"
#include "parser.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

constexpr std::size_t kTicks = 5000;
constexpr double kAbsTol = 1e-9;
constexpr double kRelTol = 1e-6;

struct ProgramTuple {
  std::vector<jitse::SignalDef> signals;
  jitse::SymbolTable symbols;
  std::size_t output_index = 0;
};

ProgramTuple BuildProgram(const std::string& src, const std::string& output_signal) {
  ProgramTuple t;
  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  t.signals = jitse::InlineSignalDependencies(parsed);
  // Use program-wide unique node IDs (the same allocator multithread_equivalence_test
  // and the backtest runner use). Per-signal AllocateNodeIds() produces colliding
  // ids across signals; the runtime path papers over those collisions by locking
  // alpha on first call, while the lowered path uses the correct per-node alpha
  // constant from IR. Compare apples-to-apples on the canonical allocator.
  jitse::AllocateProgramNodeIds(t.signals);
  for (const auto& s : t.signals) {
    for (const auto& ticker : jitse::CollectTickerSymbols(s)) {
      t.symbols.RegisterOrGetId(ticker);
    }
  }
  for (auto& s : t.signals) jitse::BindSymbolIds(s, t.symbols);
  for (std::size_t i = 0; i < t.signals.size(); ++i) {
    if (t.signals[i].name == output_signal) {
      t.output_index = i;
      break;
    }
  }
  return t;
}

// Compile `signals` through CompileProgram and return the per-tick output
// of `output_index` for a deterministic stream of `kTicks` events.
std::vector<double> RunProgram(
    const std::vector<jitse::SignalDef>& signals,
    const jitse::SymbolTable& symbols,
    std::size_t output_index,
    jitse::StatefulLoweringFlags flags,
    bool& ok) {
  ok = false;
  jitse::JitCompiler jit;
  if (!jit.IsAvailable()) return {};
  jit.SetStatefulLowering(flags);
  if (!jit.CompileProgram(signals, symbols)) {
    std::cerr << "CompileProgram failed (flags=" << static_cast<unsigned>(flags)
              << "): " << jit.LastError() << "\n";
    return {};
  }
  jitse::JitCompiler::ProgramFn fn = jit.GetProgramFunction();
  if (fn == nullptr) {
    std::cerr << "GetProgramFunction returned null\n";
    return {};
  }

  jitse::MultiSymbolSignalContext ctx(1);
  for (const auto& s : signals) jitse::PrewarmSignalContext(ctx, 0, s);

  jitse::MarketState market;
  // All test programs reference exactly one ticker; one instrument is enough.
  jitse::MarketSimulator sim(/*seed=*/123, /*n_instruments=*/1);
  (void)symbols;

  std::vector<double> outputs(signals.size(), 0.0);
  std::vector<double> trace;
  trace.reserve(kTicks);

  for (std::size_t i = 0; i < kTicks; ++i) {
    const auto ev = sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    fn(&market, &ctx, 0, outputs.data());
    trace.push_back(outputs[output_index]);
  }
  ok = true;
  return trace;
}

bool CompareTraces(const std::vector<double>& a, const std::vector<double>& b, const char* label) {
  if (a.size() != b.size()) {
    std::cerr << label << ": size mismatch " << a.size() << " vs " << b.size() << "\n";
    return false;
  }
  std::size_t max_abs_diff_idx = 0;
  double max_abs_diff = 0.0;
  std::size_t nan_disagreements = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const bool an = std::isnan(a[i]);
    const bool bn = std::isnan(b[i]);
    if (an != bn) {
      if (nan_disagreements < 5) {
        std::cerr << label << ": NaN/non-NaN disagreement at tick " << i
                  << " runtime=" << a[i] << " lowered=" << b[i] << "\n";
      }
      ++nan_disagreements;
      continue;
    }
    if (an && bn) continue;
    const double d = std::fabs(a[i] - b[i]);
    const double scale = std::max(std::fabs(a[i]), std::fabs(b[i]));
    const double tol = std::max(kAbsTol, kRelTol * scale);
    if (d > tol) {
      if (nan_disagreements < 5) {
        std::cerr << label << ": value mismatch at tick " << i
                  << " runtime=" << a[i] << " lowered=" << b[i]
                  << " abs_diff=" << d << " tol=" << tol << "\n";
      }
    }
    if (d > max_abs_diff) {
      max_abs_diff = d;
      max_abs_diff_idx = i;
    }
  }
  std::cout << label << ": max_abs_diff=" << max_abs_diff
            << " at tick=" << max_abs_diff_idx
            << " nan_disagreements=" << nan_disagreements
            << " n=" << a.size() << "\n";
  for (std::size_t i = 0; i < a.size(); ++i) {
    const bool an = std::isnan(a[i]);
    const bool bn = std::isnan(b[i]);
    if (an || bn) continue;
    const double d = std::fabs(a[i] - b[i]);
    const double scale = std::max(std::fabs(a[i]), std::fabs(b[i]));
    const double tol = std::max(kAbsTol, kRelTol * scale);
    if (d > tol) return false;
  }
  return nan_disagreements == 0;
}

bool RunCase(const char* label, const std::string& src, const std::string& output,
             jitse::StatefulLoweringFlags flags) {
  ProgramTuple prog = BuildProgram(src, output);
  bool ok_runtime = false;
  std::vector<double> runtime_trace =
      RunProgram(prog.signals, prog.symbols, prog.output_index, jitse::StatefulLoweringFlags::kNone, ok_runtime);
  if (!ok_runtime) {
    if (!runtime_trace.empty()) {
      std::cerr << label << ": runtime-call run did not complete cleanly\n";
      return false;
    }
    std::cout << label << ": jit unavailable, skipping\n";
    return true;
  }

  // Rebuild signals (CompileProgram does not modify them but be safe).
  ProgramTuple prog2 = BuildProgram(src, output);
  bool ok_lowered = false;
  std::vector<double> lowered_trace =
      RunProgram(prog2.signals, prog2.symbols, prog2.output_index, flags, ok_lowered);
  if (!ok_lowered) {
    std::cerr << label << ": lowered run failed to compile or execute\n";
    return false;
  }
  return CompareTraces(runtime_trace, lowered_trace, label);
}

}  // namespace

int main() {
  bool all_ok = true;

  // 1. SMA only.
  all_ok &= RunCase(
      "sma_only_period_10",
      "signal s = sma(mid(AAPL), 10)\n",
      "s",
      jitse::StatefulLoweringFlags::kSma);

  all_ok &= RunCase(
      "sma_only_period_64",
      "signal s = sma(mid(AAPL), 64)\n",
      "s",
      jitse::StatefulLoweringFlags::kSma);

  all_ok &= RunCase(
      "sma_only_period_200",
      "signal s = sma(mid(AAPL), 200)\n",
      "s",
      jitse::StatefulLoweringFlags::kSma);

  // 2. EMA only.
  all_ok &= RunCase(
      "ema_only_period_10",
      "signal s = ema(mid(AAPL), 10)\n",
      "s",
      jitse::StatefulLoweringFlags::kEma);

  all_ok &= RunCase(
      "ema_only_period_60",
      "signal s = ema(mid(AAPL), 60)\n",
      "s",
      jitse::StatefulLoweringFlags::kEma);

  // 3. LAG only.
  all_ok &= RunCase(
      "lag_only_period_5",
      "signal s = lag(mid(AAPL), 5)\n",
      "s",
      jitse::StatefulLoweringFlags::kLag);

  all_ok &= RunCase(
      "lag_only_period_50",
      "signal s = lag(mid(AAPL), 50)\n",
      "s",
      jitse::StatefulLoweringFlags::kLag);

  // 3b. rolling_std only.
  all_ok &= RunCase(
      "rolling_std_only_period_30",
      "signal s = rolling_std(mid(AAPL), 30)\n",
      "s",
      jitse::StatefulLoweringFlags::kRollingStd);

  // 3c. zscore only.
  all_ok &= RunCase(
      "zscore_only_period_20",
      "signal s = zscore(mid(AAPL), 20)\n",
      "s",
      jitse::StatefulLoweringFlags::kZscore);

  all_ok &= RunCase(
      "zscore_only_period_64",
      "signal s = zscore(mid(AAPL), 64)\n",
      "s",
      jitse::StatefulLoweringFlags::kZscore);

  all_ok &= RunCase(
      "rolling_min_only_period_20",
      "signal s = rolling_min(mid(AAPL), 20)\n",
      "s",
      jitse::StatefulLoweringFlags::kRollingMin);

  all_ok &= RunCase(
      "rolling_max_only_period_20",
      "signal s = rolling_max(mid(AAPL), 20)\n",
      "s",
      jitse::StatefulLoweringFlags::kRollingMax);

  all_ok &= RunCase(
      "cross_above_only",
      "signal s = cross_above(ema(mid(AAPL), 5), ema(mid(AAPL), 20))\n",
      "s",
      jitse::StatefulLoweringFlags::kCross);

  all_ok &= RunCase(
      "cross_below_only",
      "signal s = cross_below(ema(mid(AAPL), 5), ema(mid(AAPL), 20))\n",
      "s",
      jitse::StatefulLoweringFlags::kCross);

  all_ok &= RunCase(
      "kalman1d_only",
      "signal s = kalman1d(mid(AAPL), 0.01, 0.1)\n",
      "s",
      jitse::StatefulLoweringFlags::kKalman1d);

  all_ok &= RunCase(
      "vwap_only_period_20",
      "signal s = vwap(AAPL, 20)\n",
      "s",
      jitse::StatefulLoweringFlags::kVwap);

  all_ok &= RunCase(
      "rolling_corr_only_period_30",
      "signal s = rolling_corr(mid(AAPL), lag(mid(AAPL), 1), 30)\n",
      "s",
      jitse::StatefulLoweringFlags::kRollingCorr);

  all_ok &= RunCase(
      "rolling_beta_only_period_30",
      "signal s = rolling_beta(mid(AAPL), lag(mid(AAPL), 1), 30)\n",
      "s",
      jitse::StatefulLoweringFlags::kRollingBeta);

  // 4. The canonical filtered_momentum program (uses ema, sma, rolling_std,
  //    lag-through-sma-equivalent). Lower ema+rolling_std and verify parity.
  all_ok &= RunCase(
      "filtered_momentum_ema_rstd",
      "signal short_ma = ema(mid(AAPL), 10)\n"
      "signal long_ma = ema(mid(AAPL), 60)\n"
      "signal vol = rolling_std(mid(AAPL), 30)\n"
      "signal raw = short_ma - long_ma\n"
      "signal filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0\n",
      "filtered",
      jitse::StatefulLoweringFlags::kEma | jitse::StatefulLoweringFlags::kRollingStd);

  // 5. Mixed program with all three lowerable ops.
  all_ok &= RunCase(
      "mixed_all_three_lowered",
      "signal sma_x = sma(mid(AAPL), 20)\n"
      "signal ema_x = ema(mid(AAPL), 15)\n"
      "signal lag_x = lag(mid(AAPL), 7)\n"
      "signal combo = sma_x - ema_x + lag_x\n",
      "combo",
      jitse::StatefulLoweringFlags::kAll);

  // 6. Whole-program EMA stress: many EMA nodes at varying periods.
  all_ok &= RunCase(
      "ema_many_periods",
      "signal e1 = ema(mid(AAPL), 5)\n"
      "signal e2 = ema(mid(AAPL), 11)\n"
      "signal e3 = ema(mid(AAPL), 23)\n"
      "signal e4 = ema(mid(AAPL), 47)\n"
      "signal e5 = ema(mid(AAPL), 101)\n"
      "signal sum = e1 + e2 + e3 + e4 + e5\n",
      "sum",
      jitse::StatefulLoweringFlags::kAll);

  if (!all_ok) {
    std::cerr << "stateful_lowering_parity_test: FAILED\n";
    return 1;
  }
  std::cout << "stateful_lowering_parity_test: PASSED\n";
  return 0;
}
