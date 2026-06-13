#include "autodiff.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "ast_clone.h"
#include "constant_fold.h"

namespace jitse {
namespace {

std::unique_ptr<Expr> Num(double v, SourceLoc loc = {}) {
  auto out = std::make_unique<NumberLiteral>(v);
  out->loc = loc;
  return out;
}

std::unique_ptr<Expr> Unary(UnaryOpKind kind, std::unique_ptr<Expr> operand, SourceLoc loc) {
  auto out = std::make_unique<UnaryOp>(kind, std::move(operand));
  out->loc = loc;
  return out;
}

std::unique_ptr<Expr> Binary(
    BinaryOpKind kind, std::unique_ptr<Expr> left, std::unique_ptr<Expr> right, SourceLoc loc) {
  auto out = std::make_unique<BinaryOp>(kind, std::move(left), std::move(right));
  out->loc = loc;
  return out;
}

std::unique_ptr<Expr> Cond(
    std::unique_ptr<Expr> cond,
    std::unique_ptr<Expr> then_branch,
    std::unique_ptr<Expr> else_branch,
    SourceLoc loc) {
  auto out = std::make_unique<Conditional>(
      std::move(cond), std::move(then_branch), std::move(else_branch));
  out->loc = loc;
  return out;
}

std::unique_ptr<Expr> Fn(
    std::string name, std::vector<std::unique_ptr<Expr>> args, SourceLoc loc) {
  auto out = std::make_unique<FunctionCall>(std::move(name), std::move(args));
  out->loc = loc;
  return out;
}

bool IsStatelessBuiltin(const std::string& name) {
  return name == "mid" || name == "bid" || name == "ask" || name == "spread" ||
         name == "abs" || name == "log" || name == "sqrt";
}

void RequirePhase1Supported(const Expr& expr) {
  if (dynamic_cast<const NumberLiteral*>(&expr) || dynamic_cast<const IdentifierExpr*>(&expr) ||
      dynamic_cast<const ParameterExpr*>(&expr)) {
    return;
  }
  if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
    RequirePhase1Supported(*u->operand);
    return;
  }
  if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
    RequirePhase1Supported(*b->left);
    RequirePhase1Supported(*b->right);
    return;
  }
  if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
    RequirePhase1Supported(*c->condition);
    RequirePhase1Supported(*c->then_branch);
    RequirePhase1Supported(*c->else_branch);
    return;
  }
  if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
    if (!IsStatelessBuiltin(fn->name)) {
      throw std::runtime_error(
          "Phase 1 autodiff does not support stateful or unknown function: " + fn->name);
    }
    for (const auto& arg : fn->args) {
      RequirePhase1Supported(*arg);
    }
    return;
  }
  throw std::runtime_error("Phase 1 autodiff hit unknown AST node");
}

std::unique_ptr<Expr> Differentiate(const Expr& expr, std::int64_t target_param_id) {
  if (const auto* n = dynamic_cast<const NumberLiteral*>(&expr)) {
    return Num(0.0, n->loc);
  }
  if (const auto* id = dynamic_cast<const IdentifierExpr*>(&expr)) {
    (void)id;
    return Num(0.0, expr.loc);
  }
  if (const auto* p = dynamic_cast<const ParameterExpr*>(&expr)) {
    return Num(p->param_id == target_param_id ? 1.0 : 0.0, p->loc);
  }
  if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
    std::unique_ptr<Expr> inner = Differentiate(*u->operand, target_param_id);
    if (u->kind == UnaryOpKind::Plus) return inner;
    return Unary(UnaryOpKind::Minus, std::move(inner), u->loc);
  }
  if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
    std::unique_ptr<Expr> dl = Differentiate(*b->left, target_param_id);
    std::unique_ptr<Expr> dr = Differentiate(*b->right, target_param_id);
    switch (b->kind) {
      case BinaryOpKind::Add:
        return Binary(BinaryOpKind::Add, std::move(dl), std::move(dr), b->loc);
      case BinaryOpKind::Sub:
        return Binary(BinaryOpKind::Sub, std::move(dl), std::move(dr), b->loc);
      case BinaryOpKind::Mul:
        return Binary(
            BinaryOpKind::Add,
            Binary(BinaryOpKind::Mul, std::move(dl), CloneExpr(*b->right), b->loc),
            Binary(BinaryOpKind::Mul, CloneExpr(*b->left), std::move(dr), b->loc),
            b->loc);
      case BinaryOpKind::Div:
        return Binary(
            BinaryOpKind::Div,
            Binary(
                BinaryOpKind::Sub,
                Binary(BinaryOpKind::Mul, std::move(dl), CloneExpr(*b->right), b->loc),
                Binary(BinaryOpKind::Mul, CloneExpr(*b->left), std::move(dr), b->loc),
                b->loc),
            Binary(BinaryOpKind::Mul, CloneExpr(*b->right), CloneExpr(*b->right), b->loc),
            b->loc);
      case BinaryOpKind::Gt:
      case BinaryOpKind::Lt:
      case BinaryOpKind::Gte:
      case BinaryOpKind::Lte:
      case BinaryOpKind::Eq:
      case BinaryOpKind::NotEq:
      case BinaryOpKind::And:
      case BinaryOpKind::Or:
        return Num(0.0, b->loc);
    }
  }
  if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
    return Cond(
        CloneExpr(*c->condition),
        Differentiate(*c->then_branch, target_param_id),
        Differentiate(*c->else_branch, target_param_id),
        c->loc);
  }
  if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
    if (fn->name == "mid" || fn->name == "bid" || fn->name == "ask" || fn->name == "spread") {
      return Num(0.0, fn->loc);
    }
    if (fn->name == "abs") {
      std::unique_ptr<Expr> x_prime = Differentiate(*fn->args[0], target_param_id);
      std::unique_ptr<Expr> positive_grad = CloneExpr(*x_prime);
      std::unique_ptr<Expr> negative_grad =
          Unary(UnaryOpKind::Minus, std::move(x_prime), fn->loc);
      std::unique_ptr<Expr> positive_cond =
          Binary(BinaryOpKind::Gt, CloneExpr(*fn->args[0]), Num(0.0, fn->loc), fn->loc);
      std::unique_ptr<Expr> negative_cond =
          Binary(BinaryOpKind::Lt, CloneExpr(*fn->args[0]), Num(0.0, fn->loc), fn->loc);
      std::unique_ptr<Expr> zero_grad = Num(0.0, fn->loc);
      std::unique_ptr<Expr> negative_branch =
          Cond(std::move(negative_cond), std::move(negative_grad), std::move(zero_grad), fn->loc);
      return Cond(
          std::move(positive_cond),
          std::move(positive_grad),
          std::move(negative_branch),
          fn->loc);
    }
    if (fn->name == "log") {
      return Binary(
          BinaryOpKind::Div,
          Differentiate(*fn->args[0], target_param_id),
          CloneExpr(*fn->args[0]),
          fn->loc);
    }
    if (fn->name == "sqrt") {
      std::vector<std::unique_ptr<Expr>> sqrt_args;
      sqrt_args.push_back(CloneExpr(*fn->args[0]));
      return Binary(
          BinaryOpKind::Div,
          Differentiate(*fn->args[0], target_param_id),
          Binary(
              BinaryOpKind::Mul,
              Num(2.0, fn->loc),
              Fn("sqrt", std::move(sqrt_args), fn->loc),
              fn->loc),
          fn->loc);
    }
    throw std::runtime_error("Phase 1 autodiff does not support function: " + fn->name);
  }
  throw std::runtime_error("Phase 1 autodiff hit unknown AST node");
}

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

int ParsePositiveIntegerPeriod(const Expr& expr, const char* fn_name) {
  const auto* period_node = dynamic_cast<const NumberLiteral*>(&expr);
  if (period_node == nullptr) {
    throw std::runtime_error(std::string(fn_name) + " period must be a numeric literal");
  }
  const int period = static_cast<int>(period_node->value);
  if (period <= 0 || std::fabs(period_node->value - static_cast<double>(period)) > 1e-12) {
    throw std::runtime_error(std::string(fn_name) + " period must be a positive integer");
  }
  return period;
}

std::size_t GradientSlot(const SignalContext& ctx, std::size_t node_id, std::size_t param_id) {
  if (ctx.gradient_param_count == 0) {
    throw std::runtime_error("Gradient slot access without initialized parameter storage");
  }
  return node_id * ctx.gradient_param_count + param_id;
}

double LagValueAsDouble(const LagState& st) {
  return LagValue(st);
}

long double RollingStdRefreshSensitivity(RollingStdSensitivityState& st, const RingStatsState& primal) {
  const long double n = static_cast<long double>(primal.count);
  long double mean_prime = 0.0L;
  for (std::size_t i = 0; i < primal.count; ++i) {
    mean_prime += st.buffer[i];
  }
  mean_prime /= n;
  long double m2_prime = 0.0L;
  for (std::size_t i = 0; i < primal.count; ++i) {
    const long double d = static_cast<long double>(primal.buffer[i]) - primal.mean;
    const long double d_prime = st.buffer[i] - mean_prime;
    m2_prime += 2.0L * d * d_prime;
  }
  st.mean = mean_prime;
  st.m2 = m2_prime;
  st.slides_since_refresh = 0;
  return mean_prime;
}

struct ForwardGradientInterpreter {
  const SymbolTable& symbols;
  const MarketState* market = nullptr;
  SignalContext* ctx = nullptr;
  std::size_t target_param = 0;
  std::unordered_map<std::int64_t, ValueGradient> stateful_cache;

  ValueGradient EvalSignal(const SignalDef& signal, const MarketState& in_market, SignalContext& in_ctx) {
    market = &in_market;
    ctx = &in_ctx;
    stateful_cache.clear();
    return EvalExpr(*signal.body);
  }

  ValueGradient EvalExpr(const Expr& expr) {
    if (const auto* n = dynamic_cast<const NumberLiteral*>(&expr)) {
      return {n->value, 0.0};
    }
    if (dynamic_cast<const IdentifierExpr*>(&expr)) {
      throw std::runtime_error("Bare identifiers are not valid expressions");
    }
    if (const auto* p = dynamic_cast<const ParameterExpr*>(&expr)) {
      return {ctx->params[static_cast<std::size_t>(p->param_id)],
              p->param_id == static_cast<std::int64_t>(target_param) ? 1.0 : 0.0};
    }
    if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
      ValueGradient inner = EvalExpr(*u->operand);
      if (u->kind == UnaryOpKind::Plus) return inner;
      return {-inner.value, -inner.gradient};
    }
    if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
      const ValueGradient l = EvalExpr(*b->left);
      const ValueGradient r = EvalExpr(*b->right);
      switch (b->kind) {
        case BinaryOpKind::Add: return {l.value + r.value, l.gradient + r.gradient};
        case BinaryOpKind::Sub: return {l.value - r.value, l.gradient - r.gradient};
        case BinaryOpKind::Mul:
          return {l.value * r.value, l.gradient * r.value + l.value * r.gradient};
        case BinaryOpKind::Div:
          return {l.value / r.value,
                  (l.gradient * r.value - l.value * r.gradient) / (r.value * r.value)};
        case BinaryOpKind::Gt: return {l.value > r.value ? 1.0 : 0.0, 0.0};
        case BinaryOpKind::Lt: return {l.value < r.value ? 1.0 : 0.0, 0.0};
        case BinaryOpKind::Gte: return {l.value >= r.value ? 1.0 : 0.0, 0.0};
        case BinaryOpKind::Lte: return {l.value <= r.value ? 1.0 : 0.0, 0.0};
        case BinaryOpKind::Eq: return {std::fabs(l.value - r.value) < 1e-12 ? 1.0 : 0.0, 0.0};
        case BinaryOpKind::NotEq: return {std::fabs(l.value - r.value) >= 1e-12 ? 1.0 : 0.0, 0.0};
        case BinaryOpKind::And: return {(l.value != 0.0 && r.value != 0.0) ? 1.0 : 0.0, 0.0};
        case BinaryOpKind::Or: return {(l.value != 0.0 || r.value != 0.0) ? 1.0 : 0.0, 0.0};
      }
    }
    if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
      const ValueGradient cond = EvalExpr(*c->condition);
      return (cond.value != 0.0) ? EvalExpr(*c->then_branch) : EvalExpr(*c->else_branch);
    }
    if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
      if (fn->node_id > 0) {
        auto it = stateful_cache.find(fn->node_id);
        if (it != stateful_cache.end()) return it->second;
      }
      const ValueGradient out = EvalFunction(*fn);
      if (fn->node_id > 0) stateful_cache[fn->node_id] = out;
      return out;
    }
    throw std::runtime_error("Unknown AST node in EvaluateSignalGradient");
  }

  ValueGradient EvalMarketRead(const FunctionCall& fn, const char* fn_name) const {
    const std::size_t id = ResolveSymbolSlot(fn, symbols, fn_name);
    const InstrumentState& ins = market->instruments[id];
    if (fn.name == "mid") return {(ins.bid + ins.ask) * 0.5, 0.0};
    if (fn.name == "bid") return {ins.bid, 0.0};
    if (fn.name == "ask") return {ins.ask, 0.0};
    return {ins.ask - ins.bid, 0.0};
  }

  ValueGradient EvalFunction(const FunctionCall& fn) {
    if (fn.name == "mid" || fn.name == "bid" || fn.name == "ask" || fn.name == "spread") {
      return EvalMarketRead(fn, fn.name.c_str());
    }
    if (fn.name == "abs") {
      const ValueGradient x = EvalExpr(*fn.args[0]);
      const double sign = (x.value > 0.0) ? 1.0 : ((x.value < 0.0) ? -1.0 : 0.0);
      return {std::fabs(x.value), sign * x.gradient};
    }
    if (fn.name == "log") {
      const ValueGradient x = EvalExpr(*fn.args[0]);
      return {std::log(x.value), x.gradient / x.value};
    }
    if (fn.name == "sqrt") {
      const ValueGradient x = EvalExpr(*fn.args[0]);
      const double root = std::sqrt(x.value);
      return {root, x.gradient / (2.0 * root)};
    }
    if (fn.node_id < 0) {
      throw std::runtime_error("Stateful function missing node_id: " + fn.name);
    }
    const std::size_t node_id = static_cast<std::size_t>(fn.node_id);
    const std::size_t grad_slot = GradientSlot(*ctx, node_id, target_param);

    if (fn.name == "lag") {
      const int period = ParsePositiveIntegerPeriod(*fn.args[1], "lag()");
      const ValueGradient x = EvalExpr(*fn.args[0]);
      LagState& primal = ctx->lag_states[node_id];
      LagState& grad = ctx->lag_sensitivity_states[grad_slot];
      const double lagged = LagValue(primal);
      const double lagged_grad = LagValueAsDouble(grad);
      LagPush(primal, static_cast<std::size_t>(period), x.value);
      LagPush(grad, static_cast<std::size_t>(period), x.gradient);
      return {lagged, lagged_grad};
    }

    if (fn.name == "sma") {
      const int period = ParsePositiveIntegerPeriod(*fn.args[1], "sma()");
      const ValueGradient x = EvalExpr(*fn.args[0]);
      RingStatsState& primal = ctx->sma_states[node_id];
      RingStatsState& grad = ctx->sma_sensitivity_states[grad_slot];
      RingStatsPush(primal, static_cast<std::size_t>(period), x.value);
      RingStatsPush(grad, static_cast<std::size_t>(period), x.gradient);
      if (!RingStatsFull(primal)) {
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
      }
      return {RingStatsMean(primal), RingStatsMean(grad)};
    }

    if (fn.name == "ema" || fn.name == "ema_alpha") {
      const ValueGradient x = EvalExpr(*fn.args[0]);
      ValueGradient alpha_vg{};
      double alpha = 0.0;
      if (fn.name == "ema") {
        const int period = ParsePositiveIntegerPeriod(*fn.args[1], "ema()");
        alpha = 2.0 / (static_cast<double>(period) + 1.0);
        alpha_vg = {alpha, 0.0};
      } else {
        alpha_vg = EvalExpr(*fn.args[1]);
        alpha = alpha_vg.value;
      }
      EMAState& primal = ctx->ema_states[node_id];
      EmaSensitivityState& grad = ctx->ema_sensitivity_states[grad_slot];
      if (!primal.initialized) {
        primal.value = x.value;
        primal.alpha = alpha;
        primal.initialized = true;
        grad.value = x.gradient;
        return {primal.value, grad.value};
      }
      const double prev = primal.value;
      const double prev_grad = grad.value;
      primal.alpha = alpha;
      primal.value = alpha * x.value + (1.0 - alpha) * primal.value;
      grad.value =
          alpha * x.gradient + (1.0 - alpha) * prev_grad + alpha_vg.gradient * (x.value - prev);
      return {primal.value, grad.value};
    }

    if (fn.name == "rolling_std" || fn.name == "zscore") {
      const int period = ParsePositiveIntegerPeriod(
          *fn.args[1], fn.name == "rolling_std" ? "rolling_std()" : "zscore()");
      if (period < 2) {
        throw std::runtime_error(
            fn.name + "() requires period >= 2 (sample standard deviation of one sample is mathematically undefined)");
      }
      const ValueGradient x = EvalExpr(*fn.args[0]);
      RingStatsState& primal =
          (fn.name == "rolling_std") ? ctx->rolling_std_states[node_id] : ctx->zscore_states[node_id];
      RollingStdSensitivityState& grad =
          (fn.name == "rolling_std")
              ? ctx->rolling_std_sensitivity_states[grad_slot]
              : ctx->zscore_sensitivity_states[grad_slot];
      if (primal.capacity != static_cast<std::size_t>(period)) {
        primal.buffer.assign(static_cast<std::size_t>(period), 0.0);
        primal.capacity = static_cast<std::size_t>(period);
        primal.head = 0;
        primal.count = 0;
        primal.sum = 0.0L;
        primal.mean = 0.0L;
        primal.m2 = 0.0L;
        primal.slides_since_refresh = 0;
      }
      if (grad.capacity != primal.capacity) {
        grad.buffer.assign(primal.capacity, 0.0L);
        grad.capacity = primal.capacity;
        grad.head = 0;
        grad.count = 0;
        grad.mean = 0.0L;
        grad.m2 = 0.0L;
        grad.slides_since_refresh = 0;
      }

      bool slid = false;
      if (primal.count == primal.capacity) {
        const double old = primal.buffer[primal.head];
        const long double old_prime = grad.buffer[grad.head];
        primal.sum -= old;
        if (primal.capacity <= 1) {
          primal.mean = static_cast<long double>(x.value);
          primal.m2 = 0.0L;
          grad.mean = static_cast<long double>(x.gradient);
          grad.m2 = 0.0L;
        } else {
          const long double n = static_cast<long double>(primal.count);
          const long double delta_r = static_cast<long double>(old) - primal.mean;
          const long double delta_r_prime = old_prime - grad.mean;
          const long double mean_r = primal.mean - delta_r / (n - 1.0L);
          const long double mean_r_prime = grad.mean - delta_r_prime / (n - 1.0L);
          const long double m2_r =
              primal.m2 - delta_r * (static_cast<long double>(old) - mean_r);
          const long double m2_r_prime =
              grad.m2 - delta_r_prime * (static_cast<long double>(old) - mean_r) -
              delta_r * (old_prime - mean_r_prime);

          const long double delta_a = static_cast<long double>(x.value) - mean_r;
          const long double delta_a_prime = static_cast<long double>(x.gradient) - mean_r_prime;
          primal.mean = mean_r + delta_a / n;
          grad.mean = mean_r_prime + delta_a_prime / n;
          primal.m2 = m2_r + delta_a * (static_cast<long double>(x.value) - primal.mean);
          grad.m2 = m2_r_prime +
                    delta_a_prime * (static_cast<long double>(x.value) - primal.mean) +
                    delta_a * (static_cast<long double>(x.gradient) - grad.mean);
          if (grad.m2 < 0.0L) grad.m2 = 0.0L;
          if (primal.m2 < 0.0L) primal.m2 = 0.0L;
        }
        slid = true;
      } else {
        ++primal.count;
        ++grad.count;
        const long double n = static_cast<long double>(primal.count);
        const long double delta = static_cast<long double>(x.value) - primal.mean;
        const long double delta_prime = static_cast<long double>(x.gradient) - grad.mean;
        primal.mean += delta / n;
        grad.mean += delta_prime / n;
        primal.m2 += delta * (static_cast<long double>(x.value) - primal.mean);
        grad.m2 += delta_prime * (static_cast<long double>(x.value) - primal.mean) +
                   delta * (static_cast<long double>(x.gradient) - grad.mean);
      }

      if (!grad.buffer.empty()) {
        primal.buffer[primal.head] = x.value;
        primal.sum += x.value;
        grad.buffer[grad.head] = static_cast<long double>(x.gradient);
        grad.head = (grad.head + 1) % grad.capacity;
        primal.head = (primal.head + 1) % primal.capacity;
      }
      if (slid) {
        ++primal.slides_since_refresh;
        ++grad.slides_since_refresh;
        if (grad.slides_since_refresh >= grad.capacity && grad.capacity > 1) {
          const long double n = static_cast<long double>(primal.count);
          long double sum = 0.0L;
          for (std::size_t i = 0; i < primal.count; ++i) {
            sum += static_cast<long double>(primal.buffer[i]);
          }
          primal.mean = sum / n;
          long double ss = 0.0L;
          for (std::size_t i = 0; i < primal.count; ++i) {
            const long double d = static_cast<long double>(primal.buffer[i]) - primal.mean;
            ss += d * d;
          }
          primal.m2 = ss < 0.0L ? 0.0L : ss;
          primal.slides_since_refresh = 0;
          RollingStdRefreshSensitivity(grad, primal);
        }
      }

      if (!RingStatsFull(primal)) {
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
      }
      const double stddev = RingStatsStddevSample(primal);
      if (fn.name == "rolling_std") {
        if (primal.count < 2) {
          return {std::numeric_limits<double>::quiet_NaN(),
                  std::numeric_limits<double>::quiet_NaN()};
        }
        const long double n = static_cast<long double>(primal.count);
        const long double var_prime = grad.m2 / (n - 1.0L);
        const double out_grad =
            std::isnan(stddev) ? std::numeric_limits<double>::quiet_NaN()
                               : static_cast<double>(var_prime / (2.0L * static_cast<long double>(stddev)));
        return {stddev, out_grad};
      }
      const double mean = RingStatsMean(primal);
      if (std::isnan(stddev) || std::fabs(stddev) < 1e-18) {
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
      }
      const long double n = static_cast<long double>(primal.count);
      const long double var_prime = grad.m2 / (n - 1.0L);
      const long double std_prime = var_prime / (2.0L * static_cast<long double>(stddev));
      const double z = (x.value - mean) / stddev;
      const double z_prime =
          (x.gradient - static_cast<double>(grad.mean)) / stddev -
          (x.value - mean) * static_cast<double>(std_prime) / (stddev * stddev);
      return {z, z_prime};
    }

    if (fn.name == "rolling_corr" || fn.name == "rolling_beta") {
      const int period = ParsePositiveIntegerPeriod(
          *fn.args[2], fn.name == "rolling_corr" ? "rolling_corr()" : "rolling_beta()");
      const ValueGradient x = EvalExpr(*fn.args[0]);
      const ValueGradient y = EvalExpr(*fn.args[1]);
      RollingPairState& primal =
          (fn.name == "rolling_corr") ? ctx->rolling_corr_states[node_id] : ctx->rolling_beta_states[node_id];
      RollingPairSensitivityState& grad =
          (fn.name == "rolling_corr")
              ? ctx->rolling_corr_sensitivity_states[grad_slot]
              : ctx->rolling_beta_sensitivity_states[grad_slot];
      if (primal.capacity != static_cast<std::size_t>(period)) {
        primal.x_buf.assign(static_cast<std::size_t>(period), 0.0);
        primal.y_buf.assign(static_cast<std::size_t>(period), 0.0);
        primal.capacity = static_cast<std::size_t>(period);
        primal.head = 0;
        primal.count = 0;
        primal.sum_x = primal.sum_y = primal.sum_xy = primal.sum_xx = primal.sum_yy = 0.0L;
      }
      if (grad.capacity != primal.capacity) {
        grad.x_buf.assign(primal.capacity, 0.0L);
        grad.y_buf.assign(primal.capacity, 0.0L);
        grad.capacity = primal.capacity;
        grad.head = 0;
        grad.count = 0;
        grad.sum_x = grad.sum_y = grad.sum_xy = grad.sum_xx = grad.sum_yy = 0.0L;
      }

      if (primal.count == primal.capacity) {
        const long double old_xp = grad.x_buf[primal.head];
        const long double old_yp = grad.y_buf[primal.head];
        const double old_x = primal.x_buf[primal.head];
        const double old_y = primal.y_buf[primal.head];
        primal.sum_x -= old_x;
        primal.sum_y -= old_y;
        primal.sum_xy -= static_cast<long double>(old_x) * static_cast<long double>(old_y);
        primal.sum_xx -= static_cast<long double>(old_x) * static_cast<long double>(old_x);
        primal.sum_yy -= static_cast<long double>(old_y) * static_cast<long double>(old_y);
        grad.sum_x -= old_xp;
        grad.sum_y -= old_yp;
        grad.sum_xy -= old_xp * old_y + old_x * old_yp;
        grad.sum_xx -= 2.0L * static_cast<long double>(old_x) * old_xp;
        grad.sum_yy -= 2.0L * static_cast<long double>(old_y) * old_yp;
      } else {
        ++primal.count;
        ++grad.count;
      }
      primal.x_buf[primal.head] = x.value;
      primal.y_buf[primal.head] = y.value;
      primal.sum_x += x.value;
      primal.sum_y += y.value;
      primal.sum_xy += static_cast<long double>(x.value) * static_cast<long double>(y.value);
      primal.sum_xx += static_cast<long double>(x.value) * static_cast<long double>(x.value);
      primal.sum_yy += static_cast<long double>(y.value) * static_cast<long double>(y.value);
      grad.x_buf[primal.head] = static_cast<long double>(x.gradient);
      grad.y_buf[primal.head] = static_cast<long double>(y.gradient);
      grad.sum_x += x.gradient;
      grad.sum_y += y.gradient;
      grad.sum_xy += x.gradient * y.value + x.value * y.gradient;
      grad.sum_xx += 2.0L * x.value * x.gradient;
      grad.sum_yy += 2.0L * y.value * y.gradient;
      primal.head = (primal.head + 1) % primal.capacity;
      grad.head = primal.head;

      if (!RollingPairFull(primal)) {
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
      }

      const long double n = static_cast<long double>(primal.count);
      const long double cov =
          primal.sum_xy - (primal.sum_x * primal.sum_y) / n;
      const long double cov_prime =
          grad.sum_xy - (grad.sum_x * primal.sum_y + primal.sum_x * grad.sum_y) / n;
      const long double var_x =
          primal.sum_xx - (primal.sum_x * primal.sum_x) / n;
      const long double var_x_prime =
          grad.sum_xx - 2.0L * primal.sum_x * grad.sum_x / n;

      if (fn.name == "rolling_beta") {
        const double beta = RollingPairBeta(primal);
        const long double beta_prime =
            (cov_prime * var_x - cov * var_x_prime) / (var_x * var_x);
        return {beta, static_cast<double>(beta_prime)};
      }

      const long double var_y =
          primal.sum_yy - (primal.sum_y * primal.sum_y) / n;
      const long double var_y_prime =
          grad.sum_yy - 2.0L * primal.sum_y * grad.sum_y / n;
      const double corr = RollingPairCorrelation(primal);
      const long double denom = std::sqrt(static_cast<double>(var_x * var_y));
      const long double denom_prime =
          (var_x_prime * var_y + var_x * var_y_prime) / (2.0L * denom);
      const long double corr_prime = cov_prime / denom - cov * denom_prime / (denom * denom);
      return {corr, static_cast<double>(corr_prime)};
    }

    if (fn.name == "kalman1d") {
      const ValueGradient q = EvalExpr(*fn.args[1]);
      const ValueGradient r = EvalExpr(*fn.args[2]);
      if (q.value < 0.0 || r.value <= 0.0) {
        throw std::runtime_error("kalman1d() requires q >= 0 and r > 0");
      }
      const ValueGradient x = EvalExpr(*fn.args[0]);
      Kalman1dState& primal = ctx->kalman1d_states[node_id];
      Kalman1dSensitivityState& grad = ctx->kalman1d_sensitivity_states[grad_slot];
      if (!primal.initialized) {
        primal.x_hat = x.value;
        primal.p = r.value;
        primal.q = q.value;
        primal.r = r.value;
        primal.initialized = true;
        grad.x_hat = x.gradient;
        grad.p = r.gradient;
        return {primal.x_hat, grad.x_hat};
      }
      const double p_pred = primal.p + q.value;
      const double p_pred_grad = grad.p + q.gradient;
      const double denom = p_pred + r.value;
      const double denom_grad = p_pred_grad + r.gradient;
      if (denom <= 1e-18) {
        return {primal.x_hat, grad.x_hat};
      }
      const double k = p_pred / denom;
      const double k_grad = (p_pred_grad * denom - p_pred * denom_grad) / (denom * denom);
      const double innov = x.value - primal.x_hat;
      const double innov_grad = x.gradient - grad.x_hat;
      const double old_x_hat_grad = grad.x_hat;
      primal.x_hat = primal.x_hat + k * innov;
      grad.x_hat = old_x_hat_grad + k_grad * innov + k * innov_grad;
      const double p_new = (1.0 - k) * p_pred;
      const double p_new_grad = -k_grad * p_pred + (1.0 - k) * p_pred_grad;
      if (p_new < 0.0) {
        primal.p = 0.0;
        grad.p = 0.0;
      } else {
        primal.p = p_new;
        grad.p = p_new_grad;
      }
      primal.q = q.value;
      primal.r = r.value;
      return {primal.x_hat, grad.x_hat};
    }

    if (fn.name == "rolling_min" || fn.name == "rolling_max") {
      const int period = ParsePositiveIntegerPeriod(
          *fn.args[1], fn.name == "rolling_min" ? "rolling_min()" : "rolling_max()");
      const ValueGradient x = EvalExpr(*fn.args[0]);
      auto& dq =
          (fn.name == "rolling_min") ? ctx->rolling_min_deques[node_id] : ctx->rolling_max_deques[node_id];
      std::size_t& idx =
          (fn.name == "rolling_min") ? ctx->rolling_min_indices[node_id] : ctx->rolling_max_indices[node_id];
      LagState& grad_ring =
          (fn.name == "rolling_min")
              ? ctx->rolling_min_sensitivity_states[grad_slot]
              : ctx->rolling_max_sensitivity_states[grad_slot];
      const std::size_t before_idx = idx;
      const double out =
          (fn.name == "rolling_min")
              ? UpdateRollingMin(dq, idx, static_cast<std::size_t>(period), x.value)
              : UpdateRollingMax(dq, idx, static_cast<std::size_t>(period), x.value);
      if (grad_ring.capacity != static_cast<std::size_t>(period)) {
        grad_ring.buffer.assign(static_cast<std::size_t>(period), 0.0);
        grad_ring.capacity = static_cast<std::size_t>(period);
        grad_ring.head = 0;
        grad_ring.count = 0;
      }
      if (grad_ring.capacity > 0) {
        if (grad_ring.count < grad_ring.capacity) {
          ++grad_ring.count;
        }
        grad_ring.buffer[before_idx % grad_ring.capacity] = x.gradient;
        grad_ring.head = idx % grad_ring.capacity;
      }
      if (idx < static_cast<std::size_t>(period)) {
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
      }
      const std::size_t active_tick = dq.buf[dq.head].first;
      const double grad_out =
          grad_ring.buffer[active_tick % static_cast<std::size_t>(period)];
      return {out, grad_out};
    }

    if (fn.name == "cross_above" || fn.name == "cross_below") {
      const ValueGradient a = EvalExpr(*fn.args[0]);
      const ValueGradient b = EvalExpr(*fn.args[1]);
      CrossState& st = ctx->cross_states[node_id];
      if (!st.initialized) {
        st.prev_a = a.value;
        st.prev_b = b.value;
        st.initialized = true;
        return {0.0, 0.0};
      }
      const bool crossed =
          (fn.name == "cross_above")
              ? ((st.prev_a <= st.prev_b) && (a.value > b.value))
              : ((st.prev_a >= st.prev_b) && (a.value < b.value));
      st.prev_a = a.value;
      st.prev_b = b.value;
      return {crossed ? 1.0 : 0.0, 0.0};
    }

    throw std::runtime_error("Unsupported function in EvaluateSignalGradient: " + fn.name);
  }
};

}  // namespace

std::vector<SignalDef> BuildStatelessGradientSignals(
    const SignalDef& signal,
    const std::vector<ParamDef>& params) {
  RequirePhase1Supported(*signal.body);
  std::vector<SignalDef> out;
  out.reserve(params.size());
  for (const auto& param : params) {
    SignalDef grad;
    grad.name = signal.name + "__grad__" + param.name;
    grad.body = Differentiate(*signal.body, param.param_id);
    FoldConstantsInPlace(grad);
    out.push_back(std::move(grad));
  }
  return out;
}

ValueGradient EvaluateSignalGradient(
    const SignalDef& signal,
    const SymbolTable& symbols,
    const MarketState& market,
    SignalContext& ctx,
    std::int64_t param_id) {
  if (ctx.params == nullptr || ctx.num_params == 0) {
    throw std::runtime_error("EvaluateSignalGradient requires initialized parameters");
  }
  if (param_id < 0 || static_cast<std::size_t>(param_id) >= ctx.num_params) {
    throw std::runtime_error("EvaluateSignalGradient param_id out of range");
  }
  ForwardGradientInterpreter evaluator{symbols};
  evaluator.target_param = static_cast<std::size_t>(param_id);
  return evaluator.EvalSignal(signal, market, ctx);
}

}  // namespace jitse
