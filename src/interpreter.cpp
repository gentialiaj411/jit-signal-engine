#include "interpreter.h"

#include <cmath>
#include <limits>
#include <stdexcept>

#include "signal_program.h"

namespace jitse {
namespace {
std::size_t ResolveSymbolSlot(const FunctionCall& fn, const SymbolTable& symbols, const char* fn_name) {
  if (fn.symbol_id >= 0) {
    return static_cast<std::size_t>(fn.symbol_id);
  }
  if (fn.args.size() != 1 && std::string(fn_name) != "vwap") {
    throw std::runtime_error(std::string(fn_name) + "() expects exactly one argument");
  }
  if (fn.args.empty()) {
    throw std::runtime_error(std::string(fn_name) + "() requires ticker identifier");
  }
  const auto* id_expr = dynamic_cast<const IdentifierExpr*>(fn.args[0].get());
  if (id_expr == nullptr) {
    throw std::runtime_error(std::string(fn_name) + "() argument must be ticker identifier");
  }
  return symbols.LookupId(id_expr->name);
}
}  // namespace

Interpreter::Interpreter(const SymbolTable& symbols) : symbols_(symbols) {}

double Interpreter::Evaluate(
    const SignalDef& signal,
    const MarketState& market,
    MultiSymbolSignalContext& arena,
    std::uint32_t symbol_id) {
  market_ = &market;
  ctx_ = &arena.PerSymbol(symbol_id);
  stateful_eval_cache_.clear();
  signal.body->Accept(*this);
  return result_;
}

double Interpreter::Evaluate(const SignalDef& signal, const MarketState& market, SignalContext& ctx) {
  market_ = &market;
  ctx_ = &ctx;
  stateful_eval_cache_.clear();
  signal.body->Accept(*this);
  return result_;
}

double Interpreter::EvalChild(const Expr& expr) {
  expr.Accept(*this);
  return result_;
}

void Interpreter::Visit(const NumberLiteral& n) { result_ = n.value; }

void Interpreter::Visit(const IdentifierExpr&) {
  throw std::runtime_error("Bare identifiers are not valid expressions in MVP");
}

void Interpreter::Visit(const UnaryOp& u) {
  const double v = EvalChild(*u.operand);
  result_ = (u.kind == UnaryOpKind::Plus) ? v : -v;
}

void Interpreter::Visit(const BinaryOp& b) {
  const double l = EvalChild(*b.left);
  const double r = EvalChild(*b.right);
  switch (b.kind) {
    case BinaryOpKind::Add:
      result_ = l + r;
      return;
    case BinaryOpKind::Sub:
      result_ = l - r;
      return;
    case BinaryOpKind::Mul:
      result_ = l * r;
      return;
    case BinaryOpKind::Div:
      result_ = l / r;
      return;
    case BinaryOpKind::Gt:
      result_ = l > r ? 1.0 : 0.0;
      return;
    case BinaryOpKind::Lt:
      result_ = l < r ? 1.0 : 0.0;
      return;
    case BinaryOpKind::Gte:
      result_ = l >= r ? 1.0 : 0.0;
      return;
    case BinaryOpKind::Lte:
      result_ = l <= r ? 1.0 : 0.0;
      return;
    case BinaryOpKind::Eq:
      result_ = std::fabs(l - r) < 1e-12 ? 1.0 : 0.0;
      return;
    case BinaryOpKind::NotEq:
      result_ = std::fabs(l - r) >= 1e-12 ? 1.0 : 0.0;
      return;
    case BinaryOpKind::And:
      result_ = (l != 0.0 && r != 0.0) ? 1.0 : 0.0;
      return;
    case BinaryOpKind::Or:
      result_ = (l != 0.0 || r != 0.0) ? 1.0 : 0.0;
      return;
  }
}

void Interpreter::Visit(const FunctionCall& fn) {
  // P11: stateful-dedup cache. If this FunctionCall has a positive
  // node_id (assigned by AllocateNodeIds for stateful ops) and an
  // earlier structurally-equal occurrence has already been
  // evaluated in this Evaluate call, return that cached value
  // rather than re-evaluating. Re-evaluation would push the
  // operator's state TWICE per tick and produce the
  // conditional/then-branch divergence we set out to fix. Non-
  // stateful ops have node_id == -1 (the AST default) so the cache
  // check is a fast miss.
  if (fn.node_id > 0) {
    auto it = stateful_eval_cache_.find(fn.node_id);
    if (it != stateful_eval_cache_.end()) {
      result_ = it->second;
      return;
    }
  }
  auto cache_and_return = [&](double v) {
    if (fn.node_id > 0) stateful_eval_cache_[fn.node_id] = v;
    result_ = v;
  };

  if (fn.name == "mid") {
    result_ = EvalMid(fn, *market_);
    return;
  }
  if (fn.name == "bid") {
    result_ = EvalBid(fn, *market_);
    return;
  }
  if (fn.name == "ask") {
    result_ = EvalAsk(fn, *market_);
    return;
  }
  if (fn.name == "spread") {
    result_ = EvalSpread(fn, *market_);
    return;
  }
  if (fn.name == "ema") {
    cache_and_return(EvalEma(fn, *market_, *ctx_));
    return;
  }
  if (fn.name == "sma") {
    cache_and_return(EvalSma(fn, *market_, *ctx_));
    return;
  }
  if (fn.name == "rolling_std") {
    cache_and_return(EvalRollingStd(fn, *market_, *ctx_));
    return;
  }
  if (fn.name == "zscore") {
    cache_and_return(EvalZscore(fn, *market_, *ctx_));
    return;
  }
  if (fn.name == "vwap") {
    cache_and_return(EvalVwap(fn, *market_, *ctx_));
    return;
  }
  if (fn.name == "lag") {
    cache_and_return(EvalLag(fn, *market_, *ctx_));
    return;
  }
  if (fn.name == "cross_above") {
    cache_and_return(EvalCrossAbove(fn, *market_, *ctx_));
    return;
  }
  if (fn.name == "cross_below") {
    cache_and_return(EvalCrossBelow(fn, *market_, *ctx_));
    return;
  }
  if (fn.name == "rolling_min") {
    cache_and_return(EvalRollingMin(fn, *market_, *ctx_));
    return;
  }
  if (fn.name == "rolling_max") {
    cache_and_return(EvalRollingMax(fn, *market_, *ctx_));
    return;
  }
  if (fn.name == "rolling_corr") {
    cache_and_return(EvalRollingCorr(fn, *ctx_));
    return;
  }
  if (fn.name == "rolling_beta") {
    cache_and_return(EvalRollingBeta(fn, *ctx_));
    return;
  }
  if (fn.name == "kalman1d") {
    cache_and_return(EvalKalman1d(fn, *ctx_));
    return;
  }
  if (fn.name == "abs") {
    if (fn.args.size() != 1) {
      throw std::runtime_error("abs() expects one argument");
    }
    result_ = std::fabs(EvalChild(*fn.args[0]));
    return;
  }
  if (fn.name == "log") {
    if (fn.args.size() != 1) {
      throw std::runtime_error("log() expects one argument");
    }
    result_ = std::log(EvalChild(*fn.args[0]));
    return;
  }
  if (fn.name == "sqrt") {
    if (fn.args.size() != 1) {
      throw std::runtime_error("sqrt() expects one argument");
    }
    result_ = std::sqrt(EvalChild(*fn.args[0]));
    return;
  }
  throw std::runtime_error("Unsupported function in pre-LLVM stage: " + fn.name);
}

void Interpreter::Visit(const Conditional& c) {
  const double cond = EvalChild(*c.condition);
  if (cond != 0.0) {
    c.then_branch->Accept(*this);
  } else {
    c.else_branch->Accept(*this);
  }
}

double Interpreter::EvalMid(const FunctionCall& fn, const MarketState& market) const {
  if (fn.args.size() != 1) {
    throw std::runtime_error("mid() expects exactly one argument");
  }
  const std::size_t id = ResolveSymbolSlot(fn, symbols_, "mid");
  const InstrumentState& ins = market.instruments[id];
  return (ins.bid + ins.ask) * 0.5;
}

double Interpreter::EvalBid(const FunctionCall& fn, const MarketState& market) const {
  if (fn.args.size() != 1) {
    throw std::runtime_error("bid() expects exactly one argument");
  }
  const std::size_t id = ResolveSymbolSlot(fn, symbols_, "bid");
  return market.instruments[id].bid;
}

double Interpreter::EvalAsk(const FunctionCall& fn, const MarketState& market) const {
  if (fn.args.size() != 1) {
    throw std::runtime_error("ask() expects exactly one argument");
  }
  const std::size_t id = ResolveSymbolSlot(fn, symbols_, "ask");
  return market.instruments[id].ask;
}

double Interpreter::EvalSpread(const FunctionCall& fn, const MarketState& market) const {
  if (fn.args.size() != 1) {
    throw std::runtime_error("spread() expects exactly one argument");
  }
  const std::size_t id = ResolveSymbolSlot(fn, symbols_, "spread");
  const InstrumentState& ins = market.instruments[id];
  return ins.ask - ins.bid;
}

int Interpreter::ParsePositiveIntegerPeriod(const Expr& expr, const char* fn_name) {
  const auto* period_node = dynamic_cast<const NumberLiteral*>(&expr);
  if (period_node == nullptr) {
    throw std::runtime_error(std::string(fn_name) + " period must be a numeric literal in pre-LLVM stage");
  }
  const int period = static_cast<int>(period_node->value);
  if (period <= 0 || std::fabs(period_node->value - static_cast<double>(period)) > 1e-12) {
    throw std::runtime_error(std::string(fn_name) + " period must be a positive integer");
  }
  return period;
}

double Interpreter::EvalEma(const FunctionCall& fn, const MarketState& market, SignalContext& ctx) {
  if (fn.args.size() != 2) {
    throw std::runtime_error("ema() expects two arguments: ema(expr, period)");
  }
  const int period = ParsePositiveIntegerPeriod(*fn.args[1], "ema()");
  const double x = EvalChild(*fn.args[0]);

  if (fn.node_id < 0) {
    throw std::runtime_error("ema() node_id not allocated");
  }
  const std::size_t node_id = static_cast<std::size_t>(fn.node_id);
  EMAState& st = ctx.ema_states[node_id];
  if (!st.initialized) {
    st.value = x;
    st.initialized = true;
    return st.value;
  }

  const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
  st.value = alpha * x + (1.0 - alpha) * st.value;
  return st.value;
}

double Interpreter::EvalSma(const FunctionCall& fn, const MarketState& market, SignalContext& ctx) {
  if (fn.args.size() != 2) {
    throw std::runtime_error("sma() expects two arguments: sma(expr, period)");
  }
  const int period = ParsePositiveIntegerPeriod(*fn.args[1], "sma()");
  const double x = EvalChild(*fn.args[0]);

  if (fn.node_id < 0) {
    throw std::runtime_error("sma() node_id not allocated");
  }
  const std::size_t node_id = static_cast<std::size_t>(fn.node_id);
  RingStatsState& st = ctx.sma_states[node_id];
  RingStatsPush(st, static_cast<std::size_t>(period), x);
  if (!RingStatsFull(st)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return RingStatsMean(st);
}

double Interpreter::EvalRollingStd(const FunctionCall& fn, const MarketState& market, SignalContext& ctx) {
  if (fn.args.size() != 2) {
    throw std::runtime_error("rolling_std() expects two arguments: rolling_std(expr, period)");
  }
  const int period = ParsePositiveIntegerPeriod(*fn.args[1], "rolling_std()");
  const double x = EvalChild(*fn.args[0]);

  if (fn.node_id < 0) {
    throw std::runtime_error("rolling_std() node_id not allocated");
  }
  const std::size_t node_id = static_cast<std::size_t>(fn.node_id);
  RingStatsState& st = ctx.rolling_std_states[node_id];
  RingStatsPush(st, static_cast<std::size_t>(period), x);
  if (!RingStatsFull(st)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return RingStatsStddevSample(st);
}

double Interpreter::EvalZscore(const FunctionCall& fn, const MarketState& market, SignalContext& ctx) {
  if (fn.args.size() != 2) {
    throw std::runtime_error("zscore() expects two arguments: zscore(expr, period)");
  }
  const int period = ParsePositiveIntegerPeriod(*fn.args[1], "zscore()");
  const double x = EvalChild(*fn.args[0]);

  if (fn.node_id < 0) {
    throw std::runtime_error("zscore() node_id not allocated");
  }
  const std::size_t node_id = static_cast<std::size_t>(fn.node_id);
  RingStatsState& st = ctx.zscore_states[node_id];
  RingStatsPush(st, static_cast<std::size_t>(period), x);
  if (!RingStatsFull(st)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double mean = RingStatsMean(st);
  const double stddev = RingStatsStddevSample(st);
  if (std::isnan(stddev) || std::fabs(stddev) < 1e-18) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return (x - mean) / stddev;
}

double Interpreter::EvalVwap(const FunctionCall& fn, const MarketState& market, SignalContext& ctx) {
  if (fn.args.size() != 2) {
    throw std::runtime_error("vwap() expects two arguments: vwap(symbol, period)");
  }
  const int period = ParsePositiveIntegerPeriod(*fn.args[1], "vwap()");
  const std::size_t sym_id = ResolveSymbolSlot(fn, symbols_, "vwap");
  const InstrumentState& ins = market.instruments[sym_id];
  const double price = (ins.bid + ins.ask) * 0.5;
  const double volume = (ins.volume > 0.0) ? ins.volume : 1.0;

  if (fn.node_id < 0) {
    throw std::runtime_error("vwap() node_id not allocated");
  }
  const std::size_t node_id = static_cast<std::size_t>(fn.node_id);
  VwapState& st = ctx.vwap_states[node_id];
  VwapPush(st, static_cast<std::size_t>(period), price, volume);
  if (!VwapFull(st)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return VwapValue(st);
}

double Interpreter::EvalRollingMin(const FunctionCall& fn, const MarketState& market, SignalContext& ctx) {
  if (fn.args.size() != 2) {
    throw std::runtime_error("rolling_min() expects two arguments: rolling_min(expr, period)");
  }
  const int period = ParsePositiveIntegerPeriod(*fn.args[1], "rolling_min()");
  const double x = EvalChild(*fn.args[0]);

  if (fn.node_id < 0) {
    throw std::runtime_error("rolling_min() node_id not allocated");
  }
  const std::size_t node_id = static_cast<std::size_t>(fn.node_id);
  auto& dq = ctx.rolling_min_deques[node_id];
  std::size_t& idx = ctx.rolling_min_indices[node_id];
  const double result = UpdateRollingMin(dq, idx, static_cast<std::size_t>(period), x);
  if (idx < static_cast<std::size_t>(period)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return result;
}

double Interpreter::EvalRollingMax(const FunctionCall& fn, const MarketState& market, SignalContext& ctx) {
  if (fn.args.size() != 2) {
    throw std::runtime_error("rolling_max() expects two arguments: rolling_max(expr, period)");
  }
  const int period = ParsePositiveIntegerPeriod(*fn.args[1], "rolling_max()");
  const double x = EvalChild(*fn.args[0]);

  if (fn.node_id < 0) {
    throw std::runtime_error("rolling_max() node_id not allocated");
  }
  const std::size_t node_id = static_cast<std::size_t>(fn.node_id);
  auto& dq = ctx.rolling_max_deques[node_id];
  std::size_t& idx = ctx.rolling_max_indices[node_id];
  const double result = UpdateRollingMax(dq, idx, static_cast<std::size_t>(period), x);
  if (idx < static_cast<std::size_t>(period)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return result;
}

double Interpreter::EvalLag(const FunctionCall& fn, const MarketState& market, SignalContext& ctx) {
  if (fn.args.size() != 2) {
    throw std::runtime_error("lag() expects two arguments: lag(expr, period)");
  }
  const int period = ParsePositiveIntegerPeriod(*fn.args[1], "lag()");
  const double x = EvalChild(*fn.args[0]);
  if (fn.node_id < 0) throw std::runtime_error("lag() node_id not allocated");
  const std::size_t node_id = static_cast<std::size_t>(fn.node_id);
  LagState& st = ctx.lag_states[node_id];
  const double lagged = LagValue(st);
  LagPush(st, static_cast<std::size_t>(period), x);
  return lagged;
}

double Interpreter::EvalCrossAbove(const FunctionCall& fn, const MarketState& market, SignalContext& ctx) {
  if (fn.args.size() != 2) {
    throw std::runtime_error("cross_above() expects two arguments");
  }
  const double a = EvalChild(*fn.args[0]);
  const double b = EvalChild(*fn.args[1]);
  if (fn.node_id < 0) throw std::runtime_error("cross_above() node_id not allocated");
  CrossState& st = ctx.cross_states[static_cast<std::size_t>(fn.node_id)];
  if (!st.initialized) {
    st.prev_a = a;
    st.prev_b = b;
    st.initialized = true;
    return 0.0;
  }
  const bool crossed = (st.prev_a <= st.prev_b) && (a > b);
  st.prev_a = a;
  st.prev_b = b;
  return crossed ? 1.0 : 0.0;
}

double Interpreter::EvalCrossBelow(const FunctionCall& fn, const MarketState& market, SignalContext& ctx) {
  if (fn.args.size() != 2) {
    throw std::runtime_error("cross_below() expects two arguments");
  }
  const double a = EvalChild(*fn.args[0]);
  const double b = EvalChild(*fn.args[1]);
  if (fn.node_id < 0) throw std::runtime_error("cross_below() node_id not allocated");
  CrossState& st = ctx.cross_states[static_cast<std::size_t>(fn.node_id)];
  if (!st.initialized) {
    st.prev_a = a;
    st.prev_b = b;
    st.initialized = true;
    return 0.0;
  }
  const bool crossed = (st.prev_a >= st.prev_b) && (a < b);
  st.prev_a = a;
  st.prev_b = b;
  return crossed ? 1.0 : 0.0;
}

// P7: paired-series rolling correlation. Calls RollingPairPush + Correlation
// on a per-node state slot; this MUST match the runtime path bit-for-bit
// since the JIT helper (jit_rt_rolling_corr) routes through exactly the
// same helpers. The parity test gates that.
double Interpreter::EvalRollingCorr(const FunctionCall& fn, SignalContext& ctx) {
  if (fn.args.size() != 3) {
    throw std::runtime_error("rolling_corr() expects three arguments: rolling_corr(x, y, period)");
  }
  const int period = ParsePositiveIntegerPeriod(*fn.args[2], "rolling_corr()");
  const double x = EvalChild(*fn.args[0]);
  const double y = EvalChild(*fn.args[1]);
  if (fn.node_id < 0) throw std::runtime_error("rolling_corr() node_id not allocated");
  RollingPairState& st = ctx.rolling_corr_states[static_cast<std::size_t>(fn.node_id)];
  RollingPairPush(st, static_cast<std::size_t>(period), x, y);
  return RollingPairCorrelation(st);
}

double Interpreter::EvalRollingBeta(const FunctionCall& fn, SignalContext& ctx) {
  if (fn.args.size() != 3) {
    throw std::runtime_error("rolling_beta() expects three arguments: rolling_beta(x, y, period)");
  }
  const int period = ParsePositiveIntegerPeriod(*fn.args[2], "rolling_beta()");
  const double x = EvalChild(*fn.args[0]);
  const double y = EvalChild(*fn.args[1]);
  if (fn.node_id < 0) throw std::runtime_error("rolling_beta() node_id not allocated");
  RollingPairState& st = ctx.rolling_beta_states[static_cast<std::size_t>(fn.node_id)];
  RollingPairPush(st, static_cast<std::size_t>(period), x, y);
  return RollingPairBeta(st);
}

double Interpreter::EvalKalman1d(const FunctionCall& fn, SignalContext& ctx) {
  if (fn.args.size() != 3) {
    throw std::runtime_error("kalman1d() expects three arguments: kalman1d(x, q, r)");
  }
  // q and r are doubles -- not necessarily integers, so use a literal check.
  const auto* q_lit = dynamic_cast<const NumberLiteral*>(fn.args[1].get());
  const auto* r_lit = dynamic_cast<const NumberLiteral*>(fn.args[2].get());
  if (!q_lit || !r_lit) {
    throw std::runtime_error("kalman1d() q and r must be numeric literals");
  }
  const double q = q_lit->value;
  const double r = r_lit->value;
  if (q < 0.0 || r <= 0.0) {
    throw std::runtime_error("kalman1d() requires q >= 0 and r > 0");
  }
  const double x = EvalChild(*fn.args[0]);
  if (fn.node_id < 0) throw std::runtime_error("kalman1d() node_id not allocated");
  Kalman1dState& st = ctx.kalman1d_states[static_cast<std::size_t>(fn.node_id)];
  return Kalman1dStep(st, x, q, r);
}

}  // namespace jitse
