#pragma once

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
  static int ParsePositiveIntegerPeriod(const Expr& expr, const char* fn_name);

  const SymbolTable& symbols_;
  const MarketState* market_ = nullptr;
  SignalContext* ctx_ = nullptr;
  double result_ = 0.0;
};

}  // namespace jitse
