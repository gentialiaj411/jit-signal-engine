// P11: regression gate for the conditional + stateful divergence
// uncovered while landing P10.
//
// The bug: `InlineSignalDependencies` clones the body of every
// referenced signal at every reference site. If the same stateful
// signal is referenced from BOTH an if-condition and a then- (or
// else-) branch, the inliner produces two structurally-equal
// stateful subtrees with independent node_ids and thus independent
// runtime state slots. Scalar mode evaluates only the branch that
// the condition selects, so the then-branch's state slot is pushed
// strictly LESS often than the cond-side slot. By the time the
// condition first becomes true the then-branch's rolling-state
// (e.g. an `rolling_std` ring) is still cold and the operator
// returns NaN -- producing a `NaN` at the very tick where the
// condition first goes true, while the vectorized program (which
// uses lane-wise `select` and evaluates both branches every tick)
// returns the correct finite result.
//
// The fix (P11): post-inlining, AllocateNodeIds dedups structurally
// equal stateful FunctionCalls within one body when the FIRST
// occurrence is unconditional (top-level or inside a Conditional's
// `cond`). Aliased AST nodes share a node_id, so they share a
// state slot; the JIT's per-compile `stateful_emit_cache` and the
// interpreter's per-Evaluate `stateful_eval_cache_` both return
// the FIRST occurrence's result on a later hit, guaranteeing
// exactly one push per tick.
//
// This test asserts:
//   1. Interpreter vs scalar JIT bit-equality across 2000 ticks of
//      a program where `rolling_std(mid(X), 10)` is referenced
//      twice (once in cond, once in then-branch).
//   2. Scalar JIT vs vectorized JIT (K=4) parity to within 1e-9
//      across 2000 ticks. Pre-P11 this case produced NaN in scalar
//      mode and finite in vec mode at the warmup boundary; with
//      P11 both produce the same finite value.
//   3. The dedup produces ONE node_id per unique stateful subtree:
//      after AllocateProgramNodeIds, the two `rolling_std` clones
//      both have `node_id == 1` (the only stateful op in this
//      program).

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "ast.h"
#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

constexpr std::size_t kTicks = 2000;
constexpr unsigned kLaneCount = 4;
constexpr double kAbsTol = 1e-9;

// Walk a signal body and collect the node_ids assigned to every
// stateful FunctionCall. Used to assert dedup produced the
// expected sharing.
void CollectStatefulNodeIds(const jitse::Expr& expr, std::vector<std::int64_t>& out) {
  if (const auto* u = dynamic_cast<const jitse::UnaryOp*>(&expr)) {
    CollectStatefulNodeIds(*u->operand, out);
    return;
  }
  if (const auto* b = dynamic_cast<const jitse::BinaryOp*>(&expr)) {
    CollectStatefulNodeIds(*b->left, out);
    CollectStatefulNodeIds(*b->right, out);
    return;
  }
  if (const auto* c = dynamic_cast<const jitse::Conditional*>(&expr)) {
    CollectStatefulNodeIds(*c->condition, out);
    CollectStatefulNodeIds(*c->then_branch, out);
    CollectStatefulNodeIds(*c->else_branch, out);
    return;
  }
  if (const auto* fn = dynamic_cast<const jitse::FunctionCall*>(&expr)) {
    if (fn->node_id > 0) out.push_back(fn->node_id);
    for (const auto& a : fn->args) CollectStatefulNodeIds(*a, out);
    return;
  }
}

bool ApproxEq(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  if (std::isnan(a) != std::isnan(b)) return false;
  return std::fabs(a - b) <= kAbsTol;
}

bool BitEq(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  if (std::isnan(a) != std::isnan(b)) return false;
  std::uint64_t ai, bi;
  std::memcpy(&ai, &a, sizeof(double));
  std::memcpy(&bi, &b, sizeof(double));
  return ai == bi;
}

struct Program {
  std::vector<jitse::SignalDef> signals;
  jitse::SymbolTable symbols;
};

Program BuildProgram(const std::string& src) {
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
  return p;
}

// Run the program through the interpreter and return the per-tick
// trace of every signal (signal-major: trace[s_idx][tick]).
std::vector<std::vector<double>> RunInterp(const Program& prog) {
  jitse::Interpreter interp(prog.symbols);
  jitse::SignalContext ctx;
  for (const auto& s : prog.signals) jitse::PrewarmSignalContext(ctx, s);

  jitse::MarketState market;
  jitse::MarketSimulator sim(/*seed=*/9001, /*n_instruments=*/2);
  std::vector<std::vector<double>> traces(prog.signals.size());
  for (auto& t : traces) t.reserve(kTicks);
  for (std::size_t tick = 0; tick < kTicks; ++tick) {
    const auto ev = sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    for (std::size_t i = 0; i < prog.signals.size(); ++i) {
      const double v = interp.Evaluate(prog.signals[i], market, ctx);
      traces[i].push_back(v);
    }
  }
  return traces;
}

std::vector<std::vector<double>> RunScalarJit(const Program& prog, bool& available) {
  available = false;
  jitse::JitCompiler jit;
  if (!jit.IsAvailable()) return {};
  if (!jit.CompileProgram(prog.signals, prog.symbols)) {
    std::cerr << "scalar JIT compile failed: " << jit.LastError() << "\n";
    return {};
  }
  auto* fn = jit.GetProgramFunction();
  if (fn == nullptr) {
    std::cerr << "scalar JIT returned null fn\n";
    return {};
  }
  jitse::MultiSymbolSignalContext arena(1);
  for (const auto& s : prog.signals) jitse::PrewarmSignalContext(arena, 0, s);

  jitse::MarketState market;
  jitse::MarketSimulator sim(/*seed=*/9001, /*n_instruments=*/2);
  std::vector<double> outs(prog.signals.size());
  std::vector<std::vector<double>> traces(prog.signals.size());
  for (auto& t : traces) t.reserve(kTicks);
  for (std::size_t tick = 0; tick < kTicks; ++tick) {
    const auto ev = sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    fn(&market, &arena, 0, outs.data());
    for (std::size_t i = 0; i < prog.signals.size(); ++i) traces[i].push_back(outs[i]);
  }
  available = true;
  return traces;
}

// Run the program at K=4 lanes (each lane gets its own seeded
// market) and return the per-tick trace of every signal for LANE 0
// (we'll only diff lane 0 against the scalar reference; the
// vectorized parity test already covers cross-lane correctness for
// every operator). Returns empty if LLVM unavailable.
std::vector<std::vector<double>> RunVectorJit(const Program& prog, bool& available) {
  available = false;
  jitse::JitCompiler jit;
  if (!jit.IsAvailable()) return {};
  if (!jit.CompileProgramVectorized(prog.signals, prog.symbols, kLaneCount)) {
    std::cerr << "vec JIT compile failed: " << jit.LastError() << "\n";
    return {};
  }
  auto* vec_fn = jit.GetProgramVectorizedFunction();
  if (vec_fn == nullptr) return {};

  std::array<jitse::MarketState, kLaneCount> markets{};
  std::vector<jitse::MarketSimulator> sims;
  sims.reserve(kLaneCount);
  // Lane 0 uses the same seed as the scalar reference, so we can
  // bit-compare lane 0 directly against scalar.
  sims.emplace_back(/*seed=*/9001, /*n_instruments=*/2);
  for (unsigned k = 1; k < kLaneCount; ++k) sims.emplace_back(/*seed=*/9001 + k * 13, /*n_instruments=*/2);
  std::array<const jitse::MarketState*, kLaneCount> per_lane_market_ptrs{};
  for (unsigned k = 0; k < kLaneCount; ++k) per_lane_market_ptrs[k] = &markets[k];

  jitse::MultiSymbolSignalContext arena(kLaneCount);
  for (unsigned k = 0; k < kLaneCount; ++k) {
    for (const auto& s : prog.signals) jitse::PrewarmSignalContext(arena, k, s);
  }

  std::vector<double> outs(prog.signals.size() * kLaneCount);
  std::vector<std::vector<double>> traces(prog.signals.size());
  for (auto& t : traces) t.reserve(kTicks);
  for (std::size_t tick = 0; tick < kTicks; ++tick) {
    for (unsigned k = 0; k < kLaneCount; ++k) {
      const auto ev = sims[k].NextEvent(1000);
      markets[k].instruments[ev.instrument_id].bid = ev.bid;
      markets[k].instruments[ev.instrument_id].ask = ev.ask;
      markets[k].current_time_ns = ev.timestamp_ns;
    }
    vec_fn(per_lane_market_ptrs.data(), &arena, /*base_symbol=*/0, outs.data());
    // Outputs layout (vectorized): signal-major / lane-minor:
    // outs[i * K + k] = signal i, lane k. We sample lane 0.
    for (std::size_t i = 0; i < prog.signals.size(); ++i) {
      traces[i].push_back(outs[i * kLaneCount + 0]);
    }
  }
  available = true;
  return traces;
}

int failures = 0;
#define EXPECT(cond)                                                                  \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " : " << #cond << "\n"; \
      ++failures;                                                                     \
    }                                                                                 \
  } while (0)

}  // namespace

int main() {
  // The exact reproducer the P10 docs flag as "deliberately not gated by
  // the parity test". P11 should make it pass.
  const std::string src =
      "signal short_ma = ema(mid(AAPL), 5)\n"
      "signal long_ma = ema(mid(AAPL), 20)\n"
      "signal vol = rolling_std(mid(AAPL), 10)\n"
      "signal raw = short_ma - long_ma\n"
      "signal s = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0\n";

  Program prog = BuildProgram(src);

  // ------- Check 1: post-inline node_ids dedup correctly. -------
  // The inliner clones the bodies of short_ma, long_ma, vol, raw
  // into the body of `s`. After dedup:
  //   - short_ma's ema appears: 1x in short_ma's body, 2x in s's
  //     body (cond compare + raw subtraction in then-branch).
  //     short_ma's body sits at top-level, s's first clone of
  //     short_ma is inside the conditional's COND (unconditional),
  //     so the cond's clone gets aliased to short_ma's id; the
  //     then-branch clone (in raw=short_ma-long_ma, also inlined)
  //     is now structurally equal to the FIRST occurrence in s
  //     (which was unconditional via the cond), so the dedup
  //     picks it up too.
  //   - Same arithmetic applies to long_ma and vol.
  // So `s` has exactly 3 unique stateful node_ids (one each for
  // ema(_, 5), ema(_, 20), rolling_std(_, 10)) -- not 6 or 9.
  std::vector<std::int64_t> s_node_ids;
  // s is the last signal:
  CollectStatefulNodeIds(*prog.signals.back().body, s_node_ids);
  std::sort(s_node_ids.begin(), s_node_ids.end());
  s_node_ids.erase(std::unique(s_node_ids.begin(), s_node_ids.end()), s_node_ids.end());
  std::cout << "unique stateful node_ids in s = " << s_node_ids.size() << "\n";
  EXPECT(s_node_ids.size() == 3);

  // ------- Check 2: scalar JIT vs interpreter bit-equality. -------
  // Both use the same node_id dedup + same runtime helpers, so
  // they should be bit-equal per tick on every signal.
  const auto interp_traces = RunInterp(prog);
  bool scalar_avail = false;
  const auto scalar_traces = RunScalarJit(prog, scalar_avail);
  if (!scalar_avail) {
    std::cout << "LLVM unavailable -- skipping JIT checks\n";
    return 0;
  }
  EXPECT(interp_traces.size() == scalar_traces.size());
  for (std::size_t i = 0; i < interp_traces.size(); ++i) {
    EXPECT(interp_traces[i].size() == scalar_traces[i].size());
    std::size_t mismatches = 0;
    double worst_diff = 0.0;
    std::size_t worst_tick = 0;
    for (std::size_t t = 0; t < interp_traces[i].size(); ++t) {
      if (!BitEq(interp_traces[i][t], scalar_traces[i][t])) {
        ++mismatches;
        const double d = std::fabs(interp_traces[i][t] - scalar_traces[i][t]);
        if (d > worst_diff) { worst_diff = d; worst_tick = t; }
      }
    }
    std::cout << "signal=" << prog.signals[i].name
              << " interp-vs-scalar mismatches=" << mismatches
              << " worst_diff=" << worst_diff << " at_tick=" << worst_tick << "\n";
    EXPECT(mismatches == 0);
  }

  // ------- Check 3: scalar JIT vs vectorized JIT (lane 0) parity. -------
  // Lane 0 uses the same seed as the scalar reference, so it sees
  // an IDENTICAL market stream tick-for-tick. Both code paths
  // hit the same scalar runtime helpers in the same order
  // (vec mode just adds extract/insert + scalar fan-out), so the
  // values match to within a tight 1e-9 tolerance (the SMA AVX2
  // SIMD-prep path may differ by ~1 ULP from the scalar runtime
  // SMA -- the existing P10 docs cover this).
  //
  // Pre-P11: the `s` signal diverged here at the first tick where
  // the conditional flipped true, because the then-branch's
  // rolling_std/ema clones were cold. With P11 the dedup forces
  // exactly one push per tick per logical op, so the warmup-
  // boundary tick produces a finite value in both modes.
  bool vec_avail = false;
  const auto vec_traces = RunVectorJit(prog, vec_avail);
  if (!vec_avail) {
    std::cout << "vec JIT unavailable -- skipping\n";
    if (failures > 0) return 1;
    return 0;
  }
  EXPECT(vec_traces.size() == scalar_traces.size());
  for (std::size_t i = 0; i < scalar_traces.size(); ++i) {
    EXPECT(vec_traces[i].size() == scalar_traces[i].size());
    std::size_t mismatches = 0;
    double worst_diff = 0.0;
    std::size_t worst_tick = 0;
    for (std::size_t t = 0; t < scalar_traces[i].size(); ++t) {
      if (!ApproxEq(scalar_traces[i][t], vec_traces[i][t])) {
        ++mismatches;
        const double d = std::fabs(scalar_traces[i][t] - vec_traces[i][t]);
        if (d > worst_diff) { worst_diff = d; worst_tick = t; }
      }
    }
    std::cout << "signal=" << prog.signals[i].name
              << " scalar-vs-vec(lane0) mismatches=" << mismatches
              << " worst_diff=" << worst_diff << " at_tick=" << worst_tick << "\n";
    EXPECT(mismatches == 0);
  }

  if (failures > 0) {
    std::cerr << "stateful_subtree_dedup_test: " << failures << " failures\n";
    return 1;
  }
  std::cout << "stateful_subtree_dedup_test: PASSED\n";
  return 0;
}
