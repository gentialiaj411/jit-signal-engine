#pragma once

#include <cstdint>
#include <unordered_map>

#include "ast.h"
#include "runtime.h"

namespace jitse {

class Interpreter : public ExprVisitor {
 public:
  explicit Interpreter(const SymbolTable& symbols);
  double Evaluate(
      const SignalDef& signal,
      const MarketState& market,
      MultiSymbolSignalContext& arena,
      std::uint32_t symbol_id);
  double Evaluate(const SignalDef& signal, const MarketState& market, SignalContext& ctx);
  void Visit(const NumberLiteral&) override;
  void Visit(const IdentifierExpr&) override;
  void Visit(const UnaryOp&) override;
  void Visit(const BinaryOp&) override;
  void Visit(const FunctionCall&) override;
  void Visit(const Conditional&) override;

 private:
  double EvalChild(const Expr& expr);
  double EvalMid(const FunctionCall& fn, const MarketState& market) const;
  double EvalBid(const FunctionCall& fn, const MarketState& market) const;
  double EvalAsk(const FunctionCall& fn, const MarketState& market) const;
  double EvalSpread(const FunctionCall& fn, const MarketState& market) const;
  double EvalEma(const FunctionCall& fn, const MarketState& market, SignalContext& ctx);
  double EvalSma(const FunctionCall& fn, const MarketState& market, SignalContext& ctx);
  double EvalRollingStd(const FunctionCall& fn, const MarketState& market, SignalContext& ctx);
  double EvalZscore(const FunctionCall& fn, const MarketState& market, SignalContext& ctx);
  double EvalVwap(const FunctionCall& fn, const MarketState& market, SignalContext& ctx);
  double EvalLag(const FunctionCall& fn, const MarketState& market, SignalContext& ctx);
  double EvalCrossAbove(const FunctionCall& fn, const MarketState& market, SignalContext& ctx);
  double EvalCrossBelow(const FunctionCall& fn, const MarketState& market, SignalContext& ctx);
  double EvalRollingMin(const FunctionCall& fn, const MarketState& market, SignalContext& ctx);
  double EvalRollingMax(const FunctionCall& fn, const MarketState& market, SignalContext& ctx);
  // P7: paired-series and Kalman ops.
  double EvalRollingCorr(const FunctionCall& fn, SignalContext& ctx);
  double EvalRollingBeta(const FunctionCall& fn, SignalContext& ctx);
  double EvalKalman1d(const FunctionCall& fn, SignalContext& ctx);
  static int ParsePositiveIntegerPeriod(const Expr& expr, const char* fn_name);

  const SymbolTable& symbols_;
  const MarketState* market_ = nullptr;
  SignalContext* ctx_ = nullptr;
  double result_ = 0.0;
  // P11: per-Evaluate cache of stateful FunctionCall results,
  // keyed by FunctionCall::node_id (assigned by
  // AllocateNodeIds/AllocateProgramNodeIds). When a stateful
  // subtree appears multiple times in one signal body (e.g.,
  // referenced both in an if-condition AND inside a then-branch
  // after `InlineSignalDependencies`), `AllocateNodeIds` aliases
  // their node_ids so they share state; this cache makes the
  // interpreter honor that sharing by evaluating the FIRST
  // occurrence (always at an unconditional position, by the dedup
  // rule) and returning the cached value for any LATER occurrence
  // -- the same one-call-per-tick contract the JIT enforces via
  // its `stateful_emit_cache`. Cleared at the top of each
  // Evaluate so successive signals see fresh state.
  std::unordered_map<std::int64_t, double> stateful_eval_cache_;
};

}  // namespace jitse
