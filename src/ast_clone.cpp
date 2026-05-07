#include "ast_clone.h"

#include <stdexcept>

namespace jitse {

std::unique_ptr<Expr> CloneExpr(const Expr& expr) {
  if (const auto* n = dynamic_cast<const NumberLiteral*>(&expr)) {
    return std::make_unique<NumberLiteral>(n->value);
  }
  if (const auto* id = dynamic_cast<const IdentifierExpr*>(&expr)) {
    return std::make_unique<IdentifierExpr>(id->name);
  }
  if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
    return std::make_unique<UnaryOp>(u->kind, CloneExpr(*u->operand));
  }
  if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
    return std::make_unique<BinaryOp>(b->kind, CloneExpr(*b->left), CloneExpr(*b->right));
  }
  if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
    return std::make_unique<Conditional>(CloneExpr(*c->condition), CloneExpr(*c->then_branch), CloneExpr(*c->else_branch));
  }
  if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
    std::vector<std::unique_ptr<Expr>> args;
    args.reserve(fn->args.size());
    for (const auto& arg : fn->args) {
      args.push_back(CloneExpr(*arg));
    }
    return std::make_unique<FunctionCall>(fn->name, std::move(args));
  }
  throw std::runtime_error("CloneExpr: unknown AST node");
}

}  // namespace jitse

