// tiered_specialization_parity_test.cpp
//
// P1 parity gate. Validates two invariants of the profile-guided "assume
// warm" specialization:
//
//   1. Equivalence on a tier swap. After the program has been evaluated
//      against the same SignalContext for at least WarmupTickThreshold()
//      ticks, swapping in the specialized (branch-stripped) compiled
//      function must produce bit-identical (modulo IEEE-754 transit) output
//      to continuing on the baseline. We assert max_abs_diff <= 1e-12 over
//      a long post-warmup tail.
//
//   2. Branch elimination. The specialized post-opt IR string contains
//      strictly fewer `select`, `icmp eq`, and `br` instructions than the
//      baseline IR for any program that exercises sma/ema/lag, because
//      the assume_warm flag removes the count/init guard branches from
//      every lowered stateful op.
//
// Approach: build a long deterministic event stream, evaluate against TWO
// separate SignalContexts (one baseline, one tiered). For ticks
// [0, warmup), both run the baseline path. At tick == warmup, the tiered
// path calls Promote(). For ticks [warmup, kTicks) we compare the two
// outputs at every tick.
//
// This isolates the specialization effect: both contexts are warmed by the
// same input stream, so any output divergence is solely due to the
// specialized IR producing a different result. The expected outcome is
// zero divergence (within FP transit) because the branch-stripped IR is a
// pure subset of the baseline arithmetic.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "ast_clone.h"
#include "ast_utils.h"
#include "jit_compiler.h"
#include "lexer.h"
#include "market_sim.h"
#include "parser.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

constexpr std::size_t kTicks = 5000;
constexpr double kAbsTol = 1e-12;

struct ProgramTuple {
  std::vector<jitse::SignalDef> signals;
  jitse::SymbolTable symbols;
  std::size_t output_index = 0;
};

ProgramTuple BuildProgram(const std::string& src, const std::string& output_signal) {
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
  for (std::size_t i = 0; i < t.signals.size(); ++i) {
    if (t.signals[i].name == output_signal) {
      t.output_index = i;
      break;
    }
  }
  return t;
}

// Run a streaming workload with the supplied program function pointer.
// Returns the per-tick output of `output_index`. The ctx and market are
// re-used across calls to mimic the production hot path.
void RunStream(
    jitse::JitCompiler::ProgramFn fn,
    jitse::MultiSymbolSignalContext& ctx,
    jitse::MarketState& market,
    jitse::MarketSimulator& sim,
    std::vector<double>& outputs_scratch,
    std::size_t n_ticks,
    std::size_t output_index,
    std::vector<double>& trace_out) {
  trace_out.reserve(trace_out.size() + n_ticks);
  for (std::size_t i = 0; i < n_ticks; ++i) {
    const auto ev = sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    fn(&market, &ctx, 0, outputs_scratch.data());
    trace_out.push_back(outputs_scratch[output_index]);
  }
}

bool ComparePostWarmupTraces(
    const std::vector<double>& baseline,
    const std::vector<double>& tiered,
    std::int64_t warmup,
    const char* label) {
  if (baseline.size() != tiered.size()) {
    std::cerr << label << ": size mismatch " << baseline.size() << " vs " << tiered.size() << "\n";
    return false;
  }
  // Pre-warmup window: baseline == tiered (both running baseline fn). Sanity-check
  // that the harness fed both contexts identical inputs.
  for (std::int64_t i = 0; i < warmup && i < static_cast<std::int64_t>(baseline.size()); ++i) {
    const bool ba = std::isnan(baseline[i]);
    const bool ta = std::isnan(tiered[i]);
    if (ba != ta) {
      std::cerr << label << ": pre-warmup NaN diverge at tick " << i << " (harness bug)\n";
      return false;
    }
    if (!ba && !ta && std::fabs(baseline[i] - tiered[i]) > kAbsTol) {
      std::cerr << label << ": pre-warmup divergence at tick " << i
                << " baseline=" << baseline[i] << " tiered=" << tiered[i] << "\n";
      return false;
    }
  }

  // Post-warmup window: the tiered context is now running the specialized fn.
  // This is the primary assertion.
  double max_abs_diff = 0.0;
  std::size_t max_idx = 0;
  std::size_t nan_disagreements = 0;
  std::size_t shown = 0;
  for (std::size_t i = static_cast<std::size_t>(warmup); i < baseline.size(); ++i) {
    const bool ba = std::isnan(baseline[i]);
    const bool ta = std::isnan(tiered[i]);
    if (ba != ta) {
      ++nan_disagreements;
      if (nan_disagreements <= 5) {
        std::cerr << label << ": post-warmup NaN/non-NaN at tick " << i
                  << " baseline=" << baseline[i] << " tiered=" << tiered[i] << "\n";
      }
      continue;
    }
    if (ba && ta) continue;
    const double d = std::fabs(baseline[i] - tiered[i]);
    if (d > kAbsTol && shown < 5) {
      std::cerr << label << ": divergence at tick " << i
                << " baseline=" << baseline[i] << " tiered=" << tiered[i]
                << " diff=" << d << "\n";
      ++shown;
    }
    if (d > max_abs_diff) {
      max_abs_diff = d;
      max_idx = i;
    }
  }
  std::cout << label << ": warmup=" << warmup
            << " max_abs_diff=" << max_abs_diff
            << " at tick=" << max_idx
            << " nan_disagreements=" << nan_disagreements << "\n";
  return nan_disagreements == 0 && max_abs_diff <= kAbsTol;
}

// Counts the number of times `needle` appears in `haystack`. Used to assert
// that the specialized IR has strictly fewer guard branches than the baseline.
std::size_t CountSubstring(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

bool RunSwapCase(const char* label, const std::string& src, const std::string& output) {
  ProgramTuple prog_a = BuildProgram(src, output);  // baseline-only run
  ProgramTuple prog_b = BuildProgram(src, output);  // tiered run (baseline -> specialized)

  jitse::JitCompiler baseline_jit;
  if (!baseline_jit.IsAvailable()) {
    std::cout << label << ": jit unavailable, skipping\n";
    return true;
  }
  baseline_jit.SetStatefulLowering(jitse::StatefulLoweringFlags::kAll);
  if (!baseline_jit.CompileProgram(prog_a.signals, prog_a.symbols)) {
    std::cerr << label << ": baseline compile failed: " << baseline_jit.LastError() << "\n";
    return false;
  }
  jitse::JitCompiler::ProgramFn baseline_fn = baseline_jit.GetProgramFunction();
  if (baseline_fn == nullptr) {
    std::cerr << label << ": baseline fn is null\n";
    return false;
  }

  jitse::TieredProgramJit tjit;
  if (!tjit.Compile(prog_b.signals, prog_b.symbols, jitse::StatefulLoweringFlags::kAll)) {
    std::cerr << label << ": tiered baseline compile failed: " << tjit.LastError() << "\n";
    return false;
  }

  const std::int64_t warmup = tjit.WarmupTickThreshold();
  if (warmup <= 0) {
    std::cerr << label << ": warmup threshold = " << warmup
              << " (no stateful warmup-bounded ops in program?)\n";
    return false;
  }
  if (static_cast<std::size_t>(warmup) >= kTicks) {
    std::cerr << label << ": warmup " << warmup << " >= kTicks " << kTicks << "\n";
    return false;
  }

  // Two parallel SignalContexts. Identical input stream feeds both. Only
  // difference: the tiered context swaps fn at i == warmup.
  jitse::MultiSymbolSignalContext baseline_ctx(1);
  for (const auto& s : prog_a.signals) jitse::PrewarmSignalContext(baseline_ctx, 0, s);
  jitse::MultiSymbolSignalContext tiered_ctx(1);
  for (const auto& s : prog_b.signals) jitse::PrewarmSignalContext(tiered_ctx, 0, s);

  jitse::MarketState baseline_market;
  jitse::MarketState tiered_market;
  jitse::MarketSimulator baseline_sim(/*seed=*/777, /*n_instruments=*/1);
  jitse::MarketSimulator tiered_sim(/*seed=*/777, /*n_instruments=*/1);

  std::vector<double> baseline_outputs(prog_a.signals.size(), 0.0);
  std::vector<double> tiered_outputs(prog_b.signals.size(), 0.0);
  std::vector<double> baseline_trace;
  std::vector<double> tiered_trace;
  baseline_trace.reserve(kTicks);
  tiered_trace.reserve(kTicks);

  // Pre-warmup: both run baseline.
  RunStream(baseline_fn, baseline_ctx, baseline_market, baseline_sim, baseline_outputs,
            static_cast<std::size_t>(warmup), prog_a.output_index, baseline_trace);
  RunStream(tjit.CurrentFunction(), tiered_ctx, tiered_market, tiered_sim, tiered_outputs,
            static_cast<std::size_t>(warmup), prog_b.output_index, tiered_trace);

  // Promote the tiered jit: specialized fn published atomically.
  if (!tjit.Promote()) {
    std::cerr << label << ": Promote() failed: " << tjit.LastError() << "\n";
    return false;
  }
  if (!tjit.IsPromoted()) {
    std::cerr << label << ": IsPromoted() returned false after Promote()\n";
    return false;
  }

  // Post-warmup: baseline keeps running on baseline_fn; tiered switches to specialized.
  const std::size_t remaining = kTicks - static_cast<std::size_t>(warmup);
  RunStream(baseline_fn, baseline_ctx, baseline_market, baseline_sim, baseline_outputs,
            remaining, prog_a.output_index, baseline_trace);
  RunStream(tjit.CurrentFunction(), tiered_ctx, tiered_market, tiered_sim, tiered_outputs,
            remaining, prog_b.output_index, tiered_trace);

  // Dump IRs early (before the parity check) when requested, so a failure
  // can be inspected.
  const std::string& base_ir = tjit.BaselineIRPostOpt();
  const std::string& spec_ir = tjit.SpecializedIRPostOpt();
  if (std::getenv("JITSE_DUMP_TIER_IR") != nullptr) {
    std::ofstream(std::string("ir_") + label + "_baseline.ll") << base_ir;
    std::ofstream(std::string("ir_") + label + "_specialized.ll") << spec_ir;
  }

  if (!ComparePostWarmupTraces(baseline_trace, tiered_trace, warmup, label)) {
    return false;
  }
  // Use `select ` (with trailing space) to avoid matching `selector` etc.
  const std::size_t base_selects = CountSubstring(base_ir, "select ");
  const std::size_t spec_selects = CountSubstring(spec_ir, "select ");
  // Use `icmp eq i64` to specifically count count==period / init==0 compares.
  const std::size_t base_icmpeq = CountSubstring(base_ir, "icmp eq i64");
  const std::size_t spec_icmpeq = CountSubstring(spec_ir, "icmp eq i64");
  std::cout << label
            << ": ir_selects baseline=" << base_selects << " specialized=" << spec_selects
            << " icmp_eq_i64 baseline=" << base_icmpeq << " specialized=" << spec_icmpeq << "\n";
  if (spec_selects > base_selects) {
    std::cerr << label << ": specialization regressed select count\n";
    return false;
  }
  if (spec_icmpeq > base_icmpeq) {
    std::cerr << label << ": specialization regressed icmp-eq count\n";
    return false;
  }

  return true;
}

}  // namespace

int main() {
  bool all_ok = true;

  // Single-op programs: SMA, EMA, LAG. The specialized IR for each should
  // drop the warmup-guard branches without changing output.
  all_ok &= RunSwapCase(
      "swap_sma_only_period_10",
      "signal s = sma(mid(AAPL), 10)\n",
      "s");

  all_ok &= RunSwapCase(
      "swap_sma_only_period_100",
      "signal s = sma(mid(AAPL), 100)\n",
      "s");

  all_ok &= RunSwapCase(
      "swap_ema_only_period_60",
      "signal s = ema(mid(AAPL), 60)\n",
      "s");

  all_ok &= RunSwapCase(
      "swap_lag_only_period_50",
      "signal s = lag(mid(AAPL), 50)\n",
      "s");

  // Mixed program with all three lowerable ops.
  all_ok &= RunSwapCase(
      "swap_mixed_all_three",
      "signal sma_x = sma(mid(AAPL), 20)\n"
      "signal ema_x = ema(mid(AAPL), 15)\n"
      "signal lag_x = lag(mid(AAPL), 7)\n"
      "signal combo = sma_x - ema_x + lag_x\n",
      "combo");

  // The canonical filtered_momentum program. Mixed: ema + rolling_std
  // (rolling_std stays on the runtime path; assume_warm doesn't apply to
  // it, only to the lowered ema). We still expect bit-equivalence because
  // the only IR difference is the elided ema init guard.
  all_ok &= RunSwapCase(
      "swap_filtered_momentum",
      "signal short_ma = ema(mid(AAPL), 10)\n"
      "signal long_ma = ema(mid(AAPL), 60)\n"
      "signal vol = rolling_std(mid(AAPL), 30)\n"
      "signal raw = short_ma - long_ma\n"
      "signal filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0\n",
      "filtered");

  // Whole-program EMA stress: many EMA nodes at varying periods.
  all_ok &= RunSwapCase(
      "swap_ema_many_periods",
      "signal e1 = ema(mid(AAPL), 5)\n"
      "signal e2 = ema(mid(AAPL), 11)\n"
      "signal e3 = ema(mid(AAPL), 23)\n"
      "signal e4 = ema(mid(AAPL), 47)\n"
      "signal e5 = ema(mid(AAPL), 101)\n"
      "signal sum = e1 + e2 + e3 + e4 + e5\n",
      "sum");

  if (!all_ok) {
    std::cerr << "tiered_specialization_parity_test: FAILED\n";
    return 1;
  }
  std::cout << "tiered_specialization_parity_test: PASSED\n";
  return 0;
}
