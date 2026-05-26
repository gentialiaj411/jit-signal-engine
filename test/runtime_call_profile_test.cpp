// runtime_call_profile_test.cpp
//
// P3 smoke gate. Runs the same dual-configuration profile as
// runtime_call_profile.cpp but asserts the headline claim of the
// artifact: every helper that P0 lowered drops to ~0% sample share
// between `lowering=none` and `lowering=all`, while the unlowered
// `rolling_std` does not.
//
// This is a regression gate, not a benchmark. If someone breaks the IR
// lowering for sma/ema/lag (e.g. a future refactor that accidentally
// re-routes them through the runtime helpers), this test fires.
//
// The test is `ENABLE_EXPORTS`-dependent (so dladdr can resolve helper
// names). Disable on platforms where SIGPROF / dladdr aren't available.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "parser.h"
#include "runtime.h"
#include "sampling_profiler.h"
#include "signal_program.h"

namespace {

#if !defined(__linux__)
int main_unsupported() {
  std::cout << "SamplingProfiler is Linux-only; SKIPPING runtime_call_profile_test\n";
  return 0;
}
#endif

constexpr const char* kCanonicalSignal = R"(
signal short_ma = ema(mid(AAPL), 10)
signal long_ma = sma(mid(AAPL), 60)
signal lagged = lag(mid(AAPL), 5)
signal vol = rolling_std(mid(AAPL), 30)
signal score = short_ma - long_ma + (mid(AAPL) - lagged) * 0.25
signal out = if vol > 0.0 then score / vol else 0.0
)";

struct Compiled {
  std::vector<jitse::SignalDef> signals;
  jitse::SymbolTable symbols;
  std::unique_ptr<jitse::JitCompiler> jit;
  jitse::JitCompiler::ProgramFn fn = nullptr;
};

Compiled BuildAndCompile(const std::string& src,
                         jitse::StatefulLoweringFlags flags) {
  Compiled c;
  c.jit = std::make_unique<jitse::JitCompiler>();
  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  c.signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(c.signals);
  for (const auto& s : c.signals)
    for (const auto& t : jitse::CollectTickerSymbols(s)) c.symbols.RegisterOrGetId(t);
  for (auto& s : c.signals) jitse::BindSymbolIds(s, c.symbols);
  c.jit->SetStatefulLowering(flags);
  if (!c.jit->IsAvailable() || !c.jit->CompileProgram(c.signals, c.symbols)) {
    std::cerr << "compile failed: " << c.jit->LastError() << "\n";
    std::exit(2);
  }
  c.fn = c.jit->GetProgramFunction();
  return c;
}

void RunProfiled(const Compiled& cp, std::size_t events,
                 jitse::SamplingProfiler& prof) {
  jitse::MarketSimulator sim(0xC0FFEEull, 4);
  std::vector<jitse::MarketEvent> events_vec;
  events_vec.reserve(events);
  for (std::size_t i = 0; i < events; ++i) events_vec.push_back(sim.NextEvent(1000));

  jitse::MarketState market{};
  jitse::MultiSymbolSignalContext arena(1);
  for (const auto& s : cp.signals) jitse::PrewarmSignalContext(arena, 0, s);
  std::vector<double> out(cp.signals.size(), 0.0);
  volatile double sink = 0.0;

  if (!prof.Start()) {
    std::cerr << "profiler failed to start (platform without SIGPROF?); SKIP\n";
    std::exit(0);
  }
  for (std::size_t i = 0; i < events; ++i) {
    const auto& ev = events_vec[i];
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    cp.fn(&market, &arena, 0, out.data());
    sink += out.back();
  }
  prof.Stop();
  (void)sink;
}

// Returns true iff the lowered op's post-lowering sample share is
// `max_post_pct` OR LOWER. The gate is per-op because each op has a
// different baseline (jit_rt_ema_alpha and jit_rt_sma_prepare are
// chunky, jit_rt_lag is a tiny indexed-load that barely registers
// above SIGPROF noise even on a warm cache).
//
// A SECOND independent gate (`min_drop_factor`) catches the case
// where the absolute threshold passes only because the baseline
// itself was small. We require post <= pre / min_drop_factor when
// the baseline is meaningful (>= 1% in the unlowered profile); for
// baselines below 1% the absolute threshold is enough on its own,
// since "tiny went to tinier" is the spirit of the claim.
bool CheckLoweredOpDrop(const jitse::SamplingProfiler& none,
                        const jitse::SamplingProfiler& all,
                        const std::string& prefix,
                        double max_post_pct,
                        double min_drop_factor) {
  const double pre = none.PercentForPrefix(prefix);
  const double post = all.PercentForPrefix(prefix);
  const bool absolute_ok = post <= max_post_pct;
  const bool relative_ok = (pre < 1.0) || (post * min_drop_factor <= pre);
  const bool ok = absolute_ok && relative_ok;
  std::cout << "  " << prefix << ": none=" << pre << "%  all=" << post << "%  "
            << (ok ? "OK" : "FAIL") << " (gate: all<=" << max_post_pct << "%"
            << " AND (pre<1% OR all*" << min_drop_factor << "<=pre))\n";
  return ok;
}

}  // namespace

int main() {
#if !defined(__linux__)
  return main_unsupported();
#endif
  // Workload sizing. SIGPROF runs at 1/kSampleUs Hz (~10kHz at 100us)
  // and we need enough samples per profile to make a 1-2% gate
  // statistically meaningful. At 20M events this historically
  // delivered ~400 samples/profile, which is too few for the
  // smallest op (`jit_rt_lag`, baseline ~1-3% pre-lowering) -- a
  // single stray sample could push post past 0.5% on a 400-sample
  // run. 35M events brings us up to ~600-700 samples/profile, which
  // halves the per-op noise floor.
  constexpr std::size_t kEvents = 35'000'000;
  constexpr unsigned kSampleUs = 100;

  Compiled cp_none = BuildAndCompile(kCanonicalSignal, jitse::StatefulLoweringFlags::kNone);
  jitse::SamplingProfiler prof_none(kSampleUs);
  RunProfiled(cp_none, kEvents, prof_none);

  Compiled cp_all = BuildAndCompile(kCanonicalSignal, jitse::StatefulLoweringFlags::kAll);
  jitse::SamplingProfiler prof_all(kSampleUs);
  RunProfiled(cp_all, kEvents, prof_all);

  std::cout << "samples_none=" << prof_none.TotalSamples()
            << "  samples_all=" << prof_all.TotalSamples() << "\n";

  // Sample count sanity. The gate logic below depends on each profile
  // accumulating at least a few hundred samples; below that the
  // percentages are too noisy to assert on at any sensible threshold.
  // We raise the floor from 100 -> 300 to match the tightened gate.
  if (prof_none.TotalSamples() < 300 || prof_all.TotalSamples() < 300) {
    std::cout << "SKIP: too few samples to assert (likely a virtualization "
                 "environment where SIGPROF granularity is coarse). Did "
                 "not regress; not enough signal.\n";
    return 0;
  }

  // Per-op thresholds.
  //
  // The absolute ceiling (`max_post_pct`) reflects the SIGPROF noise
  // floor for each op's typical function-body size: bigger ops have
  // tighter ceilings because a stray sample is less likely to land
  // inside them, smaller ops have looser ones. The relative gate
  // (`min_drop_factor`) is the real claim of P0 IR lowering: the
  // helper went from "real work" to "essentially gone" -- it
  // should be at least 2x smaller in the all profile vs none.
  //
  // We could in principle ask for a much larger drop (the typical
  // post for ema_alpha/sma_prepare is 0%, an infinite drop) but
  // jit_rt_lag pins the lower bound: it's a single ring-buffer
  // indexed load whose unlowered baseline is already small (1-3%
  // in `none`). With ~700-900 samples per profile, the SIGPROF
  // noise floor sits at ~0.3-0.5%, so demanding e.g. a 4x drop on
  // an op whose baseline is 1.5% forces post below 0.4%, which
  // statistical drift exceeds about one run in five. The 2x bar
  // is well above that drift while still catching the actual
  // regression mode (a broken lowering leaves post ~= pre, not
  // pre/2).
  std::cout << "P0 lowered ops: each must drop substantially in lowering=all\n";
  bool ok = true;
  ok &= CheckLoweredOpDrop(prof_none, prof_all, "jit_rt_ema_alpha",
                           /*max_post_pct=*/1.0, /*min_drop_factor=*/2.0);
  ok &= CheckLoweredOpDrop(prof_none, prof_all, "jit_rt_sma_prepare",
                           /*max_post_pct=*/1.0, /*min_drop_factor=*/2.0);
  ok &= CheckLoweredOpDrop(prof_none, prof_all, "jit_rt_lag",
                           /*max_post_pct=*/2.5, /*min_drop_factor=*/2.0);

  // Control: an op P0 did NOT lower. Its share should NOT drop dramatically
  // (sanity check that the methodology measures actual entry-point time,
  // not e.g. cache-warmth artifacts of the second run).
  const double rstd_none = prof_none.PercentForPrefix("jit_rt_rolling_std");
  const double rstd_all  = prof_all.PercentForPrefix("jit_rt_rolling_std");
  const bool ctrl_ok = rstd_all > 0.5;  // must still appear in the post-profile
  std::cout << "  control: jit_rt_rolling_std (NOT lowered): none="
            << rstd_none << "%  all=" << rstd_all << "%  "
            << (ctrl_ok ? "OK" : "FAIL")
            << " (gate: all>0.5%, i.e. still measurable)\n";
  ok &= ctrl_ok;

  if (!ok) {
    std::cout << "FAIL\n";
    return 1;
  }
  std::cout << "PASS\n";
  return 0;
}
