#include "ast_utils.h"

#include <functional>

namespace jitse {

std::unordered_set<std::string> CollectTickerSymbols(const SignalDef& signal) {
  std::unordered_set<std::string> out;

  std::function<void(const Expr&)> walk = [&](const Expr& expr) {
    if (const auto* n = dynamic_cast<const NumberLiteral*>(&expr)) {
      (void)n;
      return;
    }
    if (const auto* id = dynamic_cast<const IdentifierExpr*>(&expr)) {
      (void)id;
      return;
    }
    if (const auto* p = dynamic_cast<const ParameterExpr*>(&expr)) {
      (void)p;
      return;
    }
    if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
      walk(*u->operand);
      return;
    }
    if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
      walk(*b->left);
      walk(*b->right);
      return;
    }
    if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
      walk(*c->condition);
      walk(*c->then_branch);
      walk(*c->else_branch);
      return;
    }
    if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
      if ((fn->name == "mid" || fn->name == "bid" || fn->name == "ask" || fn->name == "spread" || fn->name == "vwap") &&
          !fn->args.empty()) {
        if (const auto* ticker = dynamic_cast<const IdentifierExpr*>(fn->args[0].get())) {
          out.insert(ticker->name);
        }
      }
      for (const auto& arg : fn->args) {
        walk(*arg);
      }
      return;
    }
  };

  walk(*signal.body);
  return out;
}

}  // namespace jitse
