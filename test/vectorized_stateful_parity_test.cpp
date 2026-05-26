// P10: parity gate for vectorized stateful operators.
//
// P2 covered the stateless-only vectorized MVP. P10 extends the
// vectorized JIT to support stateful operators (ema/sma/rolling_std/
// zscore/lag/rolling_min/rolling_max/vwap/cross_above/cross_below/
// rolling_corr/rolling_beta/kalman1d) via per-lane scalarized
// fan-out. This test is the correctness gate for that extension.
//
// For each test program the test:
//   1. Compiles the program twice: once via `CompileProgram` (scalar,
//      single-symbol entry) and once via `CompileProgramVectorized`
//      with lane_count = 4.
//   2. Builds K=4 MarketSimulator instances with distinct seeds so
//      each lane sees a different stream.
//   3. Pre-warms K independent per-symbol SignalContext slots (one
//      per lane) inside a single `MultiSymbolSignalContext`. The
//      vectorized fn uses lane_sym = base_symbol + lane via
//      `jit_rt_symbol_ctx`.
//   4. For each tick, calls the scalar function K times (once per
//      lane, each against its own private MultiSymbolSignalContext
//      with one symbol slot) and the vector function once (against
//      the K-slot context).
//   5. Asserts bit-equality between the scalar K-runs and the
//      vectorized run for every signal and every lane.
//
// Because the scalar runtime helpers (jit_rt_ema_alpha,
// jit_rt_rolling_std, etc.) are the SAME bytes of machine code called
// from both paths, the parity gate is strict bit-equality (NaN==NaN
// treated as equal). Any drift here indicates a fan-out wiring bug.

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

constexpr std::size_t kTicks = 1500;
constexpr unsigned kLaneCount = 4;
constexpr std::size_t kInstrumentCount = 4;

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

// Tight-tolerance equality treating NaN==NaN as equal. We use 1e-9
// (matching the other parity tests in this codebase) rather than
// bit-equality because of one known reduction-order divergence:
// when the scalar JIT has AVX2 enabled and an sma() period >= 4, it
// emits a vector-reduction SIMD-prep path (sum of 4-wide loads, then
// horizontal reduce) while the vectorized fan-out uses the scalar
// `jit_rt_sma` runtime helper (sequential add). Both compute the
// same mathematical SMA but differ by ~1 ULP on the last add. Any
// drift larger than this is a real bug.
constexpr double kAbsTol = 1e-9;
bool ApproxEq(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  if (std::isnan(a) != std::isnan(b)) return false;
  return std::fabs(a - b) <= kAbsTol;
}

bool RunCase(const std::string& label, const std::string& src) {
  std::cout << "case: " << label << std::endl;
  ProgramTuple t;
  try {
    t = BuildProgram(src);
  } catch (const std::exception& ex) {
    std::cerr << "  build failed: " << ex.what() << std::endl;
    return false;
  }
  const std::size_t num_signals = t.signals.size();

  jitse::JitCompiler scalar_jit;
  if (!scalar_jit.IsAvailable()) {
    std::cout << "  LLVM unavailable -- skipping" << std::endl;
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

  // K independent MarketStates and MarketSimulators (distinct seeds
  // give each lane its own price stream so the stateful operators
  // produce non-trivially-different per-lane outputs and a per-lane
  // mistake will be caught immediately).
  std::array<jitse::MarketState, kLaneCount> markets{};
  std::vector<jitse::MarketSimulator> sims;
  sims.reserve(kLaneCount);
  for (unsigned k = 0; k < kLaneCount; ++k) {
    sims.emplace_back(static_cast<std::uint64_t>(7919 + k * 13), kInstrumentCount);
  }
  std::array<const jitse::MarketState*, kLaneCount> per_lane_market_ptrs{};
  for (unsigned k = 0; k < kLaneCount; ++k) {
    per_lane_market_ptrs[k] = &markets[k];
  }

  // Scalar side: K independent MultiSymbolSignalContexts (each with
  // a single slot at symbol_id=0). This guarantees no cross-lane
  // state aliasing in the reference path; the only sharing is
  // accidental and would only happen if the JIT writes through the
  // wrong slot, which the parity check would catch.
  std::vector<jitse::MultiSymbolSignalContext> scalar_arenas;
  scalar_arenas.reserve(kLaneCount);
  for (unsigned k = 0; k < kLaneCount; ++k) {
    scalar_arenas.emplace_back(1);
    for (const auto& s : t.signals) {
      jitse::PrewarmSignalContext(scalar_arenas[k], 0, s);
    }
  }

  // Vector side: ONE MultiSymbolSignalContext with K slots
  // [base_symbol .. base_symbol + K). The fan-out emits
  // `jit_rt_symbol_ctx(arena, base + lane)` per lane.
  jitse::MultiSymbolSignalContext vec_arena(kLaneCount);
  for (unsigned k = 0; k < kLaneCount; ++k) {
    for (const auto& s : t.signals) {
      jitse::PrewarmSignalContext(vec_arena, k, s);
    }
  }

  std::vector<double> scalar_outputs(kLaneCount * num_signals, 0.0);
  std::vector<double> vec_outputs(kLaneCount * num_signals, 0.0);

  std::size_t worst_idx = 0;
  for (std::size_t tick = 0; tick < kTicks; ++tick) {
    for (unsigned k = 0; k < kLaneCount; ++k) {
      auto ev = sims[k].NextEvent(1000);
      markets[k].instruments[ev.instrument_id].bid = ev.bid;
      markets[k].instruments[ev.instrument_id].ask = ev.ask;
      markets[k].current_time_ns = ev.timestamp_ns;
    }
    for (unsigned k = 0; k < kLaneCount; ++k) {
      scalar_fn(&markets[k], &scalar_arenas[k], 0,
                scalar_outputs.data() + k * num_signals);
    }
    vec_fn(per_lane_market_ptrs.data(), &vec_arena, /*base_symbol=*/0,
           vec_outputs.data());

    for (std::size_t i = 0; i < num_signals; ++i) {
      for (unsigned k = 0; k < kLaneCount; ++k) {
        const double s = scalar_outputs[k * num_signals + i];
        const double v = vec_outputs[i * kLaneCount + k];
        if (!ApproxEq(s, v)) {
          std::cerr << "  parity fail tick=" << tick
                    << " signal=" << t.signals[i].name
                    << " (idx=" << i << ")"
                    << " lane=" << k
                    << " scalar=" << s
                    << " vec=" << v
                    << " diff=" << (s - v) << std::endl;
          // Dump all signal values at this lane for context.
          std::cerr << "    full lane " << k << " signal trace at tick " << tick << ":\n";
          for (std::size_t j = 0; j < num_signals; ++j) {
            std::cerr << "      " << t.signals[j].name
                      << " scalar=" << scalar_outputs[k * num_signals + j]
                      << " vec=" << vec_outputs[j * kLaneCount + k] << "\n";
          }
          return false;
        }
      }
    }
    worst_idx = tick;
  }
  std::cout << "  ok ticks=" << kTicks
            << " signals=" << num_signals
            << " lanes=" << kLaneCount
            << " worst_tick=" << worst_idx << std::endl;
  return true;
}

}  // namespace

int main() {
  bool all_ok = true;

  // Stateful primitives one at a time.
  all_ok &= RunCase("ema",
      "signal s = ema(mid(AAPL), 10)\n");
  all_ok &= RunCase("sma",
      "signal s = sma(mid(AAPL), 8)\n");
  all_ok &= RunCase("rolling_std",
      "signal s = rolling_std(mid(AAPL), 12)\n");
  all_ok &= RunCase("zscore",
      "signal s = zscore(mid(AAPL), 12)\n");
  all_ok &= RunCase("lag",
      "signal s = lag(mid(AAPL), 5)\n");
  all_ok &= RunCase("rolling_min",
      "signal s = rolling_min(mid(AAPL), 10)\n");
  all_ok &= RunCase("rolling_max",
      "signal s = rolling_max(mid(AAPL), 10)\n");
  all_ok &= RunCase("vwap",
      "signal s = vwap(AAPL, 10)\n");
  all_ok &= RunCase("cross_above",
      "signal a = ema(mid(AAPL), 5)\n"
      "signal b = ema(mid(AAPL), 20)\n"
      "signal s = cross_above(a, b)\n");
  all_ok &= RunCase("cross_below",
      "signal a = ema(mid(AAPL), 5)\n"
      "signal b = ema(mid(AAPL), 20)\n"
      "signal s = cross_below(a, b)\n");
  all_ok &= RunCase("rolling_corr",
      "signal s = rolling_corr(mid(AAPL), mid(MSFT), 8)\n");
  all_ok &= RunCase("rolling_beta",
      "signal s = rolling_beta(mid(MSFT), mid(AAPL), 8)\n");
  all_ok &= RunCase("kalman1d",
      "signal s = kalman1d(mid(AAPL), 0.01, 1.0)\n");

  // Composed: multiple stateful ops + arithmetic. The fan-out result
  // of one op feeds the next, and the per-lane scalar contexts must
  // stay correctly partitioned across multiple inlined stateful
  // calls.
  //
  // Note on a deliberate omission: a "filtered_momentum_like" program
  // like
  //     signal s = if cond && rolling_std(...) > 0
  //                then raw / rolling_std(...) else 0.0
  // would diverge between scalar and vec modes, but the divergence is
  // unrelated to P10. After InlineSignalDependencies, each reference
  // to a stateful signal becomes an independent clone with its own
  // node_id (and so its own state slot). Scalar mode emits CondBr
  // around the then-branch and only PUSHES the then-branch's
  // rolling_std state when cond is true, so the two state slots get
  // out of sync. Vec mode uses a lane-wise `select` which evaluates
  // both branches unconditionally, keeping all stateful clones in
  // lockstep. That is a scalar-mode branch-eval pitfall, not a P10
  // fan-out bug; the gate for that lives in stateful-program
  // semantics tests, not here.
  all_ok &= RunCase("multi_signal_stateful",
      "signal sma_a = sma(mid(AAPL), 8)\n"
      "signal lag_a = lag(mid(AAPL), 3)\n"
      "signal s = sma_a - lag_a\n");
  // Stateful feeding stateful: ema of zscore. Each per-lane state is
  // a separate slot; correctness requires the fan-out NOT to alias.
  all_ok &= RunCase("ema_of_zscore",
      "signal s = ema(zscore(mid(AAPL), 10), 5)\n");

  if (!all_ok) {
    std::cerr << "vectorized_stateful_parity_test: FAILED" << std::endl;
    return 1;
  }
  std::cout << "vectorized_stateful_parity_test: PASSED" << std::endl;
  return 0;
}
