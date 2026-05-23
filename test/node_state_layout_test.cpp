#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

void CollectNodeIds(const jitse::Expr& expr, std::vector<std::int64_t>& out) {
  if (const auto* u = dynamic_cast<const jitse::UnaryOp*>(&expr)) {
    CollectNodeIds(*u->operand, out);
    return;
  }
  if (const auto* b = dynamic_cast<const jitse::BinaryOp*>(&expr)) {
    CollectNodeIds(*b->left, out);
    CollectNodeIds(*b->right, out);
    return;
  }
  if (const auto* c = dynamic_cast<const jitse::Conditional*>(&expr)) {
    CollectNodeIds(*c->condition, out);
    CollectNodeIds(*c->then_branch, out);
    CollectNodeIds(*c->else_branch, out);
    return;
  }
  if (const auto* fn = dynamic_cast<const jitse::FunctionCall*>(&expr)) {
    if (fn->node_id >= 0) out.push_back(fn->node_id);
    for (const auto& a : fn->args) CollectNodeIds(*a, out);
    return;
  }
}

}  // namespace

int main() {
  const std::string src =
      "signal short_ma = ema(mid(AAPL), 10)\n"
      "signal long_ma = ema(mid(AAPL), 60)\n"
      "signal vol = rolling_std(mid(AAPL), 30)\n"
      "signal raw = short_ma - long_ma\n"
      "signal filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0\n";

  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);

  std::vector<std::int64_t> ids_before;
  std::int64_t max_node_id = 0;
  for (auto& s : signals) {
    max_node_id = std::max(max_node_id, jitse::AllocateNodeIds(s));
    CollectNodeIds(*s.body, ids_before);
  }
  std::vector<std::int64_t> ids_after;
  for (auto& s : signals) {
    const std::int64_t second = jitse::AllocateNodeIds(s);
    max_node_id = std::max(max_node_id, second);
    CollectNodeIds(*s.body, ids_after);
  }
  assert(ids_before == ids_after && "node_id assignment must be stable after initial allocation");

  std::unordered_set<std::int64_t> unique_ids(ids_before.begin(), ids_before.end());
  assert(unique_ids.size() == ids_before.size() && "stateful node IDs must be unique");

  jitse::SymbolTable symbols;
  for (const auto& s : signals) {
    for (const auto& t : jitse::CollectTickerSymbols(s)) symbols.RegisterOrGetId(t);
  }
  for (auto& s : signals) jitse::BindSymbolIds(s, symbols);

  jitse::SignalContext ctx;
  for (const auto& s : signals) jitse::PrewarmSignalContext(ctx, s);
  const std::size_t needed = static_cast<std::size_t>(max_node_id + 1);
  assert(ctx.ema_states.size() >= needed);
  assert(ctx.sma_states.size() >= needed);
  assert(ctx.rolling_std_states.size() >= needed);
  assert(ctx.zscore_states.size() >= needed);
  assert(ctx.vwap_states.size() >= needed);
  assert(ctx.lag_states.size() >= needed);
  assert(ctx.cross_states.size() >= needed);
  assert(ctx.rolling_min_deques.size() >= needed);
  assert(ctx.rolling_max_deques.size() >= needed);
  assert(ctx.rolling_min_indices.size() >= needed);
  assert(ctx.rolling_max_indices.size() >= needed);

  jitse::Interpreter interp(symbols);
  jitse::SignalContext interp_ctx;
  for (const auto& s : signals) jitse::PrewarmSignalContext(interp_ctx, s);
  jitse::MarketState market;
  jitse::MarketSimulator sim(2026, 1);

  jitse::JitCompiler jit;
  const bool jit_ok = jit.IsAvailable() && jit.CompileProgram(signals, symbols) && (jit.GetProgramFunction() != nullptr);
  jitse::MultiSymbolSignalContext jit_ctx(1);
  for (const auto& s : signals) jitse::PrewarmSignalContext(jit_ctx, 0, s);
  std::vector<double> outputs(signals.size(), 0.0);

  for (int i = 0; i < 500; ++i) {
    const auto ev = sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    if (jit_ok) {
      jit.GetProgramFunction()(&market, &jit_ctx, 0, outputs.data());
      for (std::size_t s = 0; s < signals.size(); ++s) {
        const double iv = interp.Evaluate(signals[s], market, interp_ctx);
        const double jv = outputs[s];
        if (std::isnan(iv) || std::isnan(jv)) {
          assert(std::isnan(iv) && std::isnan(jv));
        } else {
          assert(std::fabs(iv - jv) < 1e-8);
        }
      }
    } else {
      for (const auto& s : signals) (void)interp.Evaluate(s, market, interp_ctx);
    }
  }

  return 0;
}
