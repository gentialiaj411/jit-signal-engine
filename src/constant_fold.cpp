#include "constant_fold.h"

#include <cmath>
#include <utility>

namespace jitse {

namespace {

const NumberLiteral* AsNumber(const Expr* e) {
  return dynamic_cast<const NumberLiteral*>(e);
}

// Build a literal that takes the SourceLoc of the original (fused) node.
std::unique_ptr<NumberLiteral> MakeLiteral(double v, SourceLoc loc) {
  auto out = std::make_unique<NumberLiteral>(v);
  out->loc = loc;
  return out;
}

}  // namespace

std::unique_ptr<Expr> FoldConstants(std::unique_ptr<Expr> expr) {
  if (!expr) return expr;

  if (auto* u = dynamic_cast<UnaryOp*>(expr.get())) {
    u->operand = FoldConstants(std::move(u->operand));
    if (const auto* n = AsNumber(u->operand.get())) {
      const double v = (u->kind == UnaryOpKind::Minus) ? -n->value : n->value;
      return MakeLiteral(v, expr->loc);
    }
    return expr;
  }

  if (auto* b = dynamic_cast<BinaryOp*>(expr.get())) {
    b->left = FoldConstants(std::move(b->left));
    b->right = FoldConstants(std::move(b->right));
    const auto* lhs = AsNumber(b->left.get());
    const auto* rhs = AsNumber(b->right.get());
    if (lhs && rhs) {
      const double l = lhs->value;
      const double r = rhs->value;
      double out = 0.0;
      switch (b->kind) {
        case BinaryOpKind::Add: out = l + r; break;
        case BinaryOpKind::Sub: out = l - r; break;
        case BinaryOpKind::Mul: out = l * r; break;
        case BinaryOpKind::Div:
          // Preserve IEEE-754 behaviour at compile time: x/0 -> inf, 0/0 -> nan.
          // Both interp and JIT do the same divide at runtime, so folding
          // here changes only WHEN the result is computed, not WHAT.
          out = l / r;
          break;
        case BinaryOpKind::Gt: out = (l > r) ? 1.0 : 0.0; break;
        case BinaryOpKind::Lt: out = (l < r) ? 1.0 : 0.0; break;
        case BinaryOpKind::Gte: out = (l >= r) ? 1.0 : 0.0; break;
        case BinaryOpKind::Lte: out = (l <= r) ? 1.0 : 0.0; break;
        case BinaryOpKind::Eq: out = (l == r) ? 1.0 : 0.0; break;
        case BinaryOpKind::NotEq: out = (l != r) ? 1.0 : 0.0; break;
        case BinaryOpKind::And:
          // Match runtime semantics: 0 is false, anything else (incl. NaN) is true.
          out = ((l != 0.0) && (r != 0.0)) ? 1.0 : 0.0;
          break;
        case BinaryOpKind::Or:
          out = ((l != 0.0) || (r != 0.0)) ? 1.0 : 0.0;
          break;
      }
      return MakeLiteral(out, expr->loc);
    }
    return expr;
  }

  if (auto* c = dynamic_cast<Conditional*>(expr.get())) {
    c->condition = FoldConstants(std::move(c->condition));
    c->then_branch = FoldConstants(std::move(c->then_branch));
    c->else_branch = FoldConstants(std::move(c->else_branch));
    if (const auto* cn = AsNumber(c->condition.get())) {
      // Match runtime: nonzero -> then branch, zero -> else.
      // NaN is "truthy" because comparison with 0 is false (matches what
      // the JIT does when an expression evaluates to NaN; in practice
      // type-checking already forbids non-bool conditions but we keep
      // this rule for legacy unchecked AST input from fuzz tests).
      std::unique_ptr<Expr> picked =
          (cn->value != 0.0) ? std::move(c->then_branch) : std::move(c->else_branch);
      picked->loc = expr->loc;
      return picked;
    }
    return expr;
  }

  if (auto* fn = dynamic_cast<FunctionCall*>(expr.get())) {
    // Fold args (e.g. lookback period `5 + 5` -> 10) but never the call
    // itself: every callable in the DSL either reads market state or
    // accumulates rolling state, neither of which is statically known.
    for (auto& a : fn->args) {
      a = FoldConstants(std::move(a));
    }
    return expr;
  }

  // NumberLiteral, IdentifierExpr, ParameterExpr: nothing to fold.
  return expr;
}

void FoldConstantsInPlace(SignalDef& signal) {
  signal.body = FoldConstants(std::move(signal.body));
}

}  // namespace jitse
