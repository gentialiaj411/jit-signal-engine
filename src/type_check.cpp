#include "type_check.h"

#include <string>

namespace jitse {

namespace {

const char* TypeName(StaticType t) {
  return t == StaticType::Number ? "number" : "bool";
}

bool IsTickerFirstArgFn(const std::string& name) {
  return name == "mid" || name == "bid" || name == "ask" ||
         name == "spread" || name == "vwap";
}

void RequireType(const Expr& expr, StaticType actual, StaticType want,
                 const char* context) {
  if (actual == want) return;
  std::string msg = "type error: ";
  msg += context;
  msg += " requires `";
  msg += TypeName(want);
  msg += "`, got `";
  msg += TypeName(actual);
  msg += "`";
  throw ParseError(msg, expr.loc);
}

}  // namespace

StaticType TypeCheckExpr(const Expr& expr) {
  if (dynamic_cast<const NumberLiteral*>(&expr)) {
    return StaticType::Number;
  }
  if (dynamic_cast<const IdentifierExpr*>(&expr)) {
    // Bare identifiers are signal references (inlined before
    // codegen) or, illegally, anywhere else. Either way they're
    // Number-typed -- signals always store doubles.
    return StaticType::Number;
  }
  if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
    const StaticType inner = TypeCheckExpr(*u->operand);
    RequireType(*u->operand, inner, StaticType::Number, "unary +/-");
    return StaticType::Number;
  }
  if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
    const StaticType lt = TypeCheckExpr(*b->left);
    const StaticType rt = TypeCheckExpr(*b->right);
    switch (b->kind) {
      case BinaryOpKind::Add:
      case BinaryOpKind::Sub:
      case BinaryOpKind::Mul:
      case BinaryOpKind::Div:
        RequireType(*b->left, lt, StaticType::Number, "arithmetic operator");
        RequireType(*b->right, rt, StaticType::Number, "arithmetic operator");
        return StaticType::Number;
      case BinaryOpKind::Gt:
      case BinaryOpKind::Lt:
      case BinaryOpKind::Gte:
      case BinaryOpKind::Lte:
      case BinaryOpKind::Eq:
      case BinaryOpKind::NotEq:
        RequireType(*b->left, lt, StaticType::Number, "comparison operator");
        RequireType(*b->right, rt, StaticType::Number, "comparison operator");
        return StaticType::Bool;
      case BinaryOpKind::And:
      case BinaryOpKind::Or:
        RequireType(*b->left, lt, StaticType::Bool, "logical && / ||");
        RequireType(*b->right, rt, StaticType::Bool, "logical && / ||");
        return StaticType::Bool;
    }
    return StaticType::Number;  // unreachable
  }
  if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
    const StaticType ct = TypeCheckExpr(*c->condition);
    RequireType(*c->condition, ct, StaticType::Bool, "if-condition");
    const StaticType tt = TypeCheckExpr(*c->then_branch);
    const StaticType et = TypeCheckExpr(*c->else_branch);
    // Both branches must produce a Number; the expression value flows
    // into the signal's stored double.
    RequireType(*c->then_branch, tt, StaticType::Number, "then-branch");
    RequireType(*c->else_branch, et, StaticType::Number, "else-branch");
    return StaticType::Number;
  }
  if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
    const bool ticker_first = IsTickerFirstArgFn(fn->name);
    for (std::size_t i = 0; i < fn->args.size(); ++i) {
      if (ticker_first && i == 0) {
        // Ticker placeholder; must be a bare identifier (validated
        // elsewhere). Don't enforce a "number" type rule here.
        if (!dynamic_cast<const IdentifierExpr*>(fn->args[i].get())) {
          throw ParseError(
              fn->name + "() first argument must be a ticker identifier",
              fn->args[i]->loc);
        }
        continue;
      }
      const StaticType at = TypeCheckExpr(*fn->args[i]);
      RequireType(*fn->args[i], at, StaticType::Number,
                  "function-call argument");
    }
    // All currently-defined function calls return a Number.
    return StaticType::Number;
  }
  // Unknown node: treat as Number so the rest of the pipeline (which
  // will fail on it for real reasons) gives the actual error.
  return StaticType::Number;
}

void TypeCheckSignal(const SignalDef& signal) {
  const StaticType bt = TypeCheckExpr(*signal.body);
  if (bt != StaticType::Number) {
    throw ParseError(
        "type error: signal `" + signal.name +
            "` body must be `number`, but evaluates to `bool` "
            "(wrap it in an if/then/else or use 1.0/0.0)",
        signal.body->loc);
  }
}

}  // namespace jitse
