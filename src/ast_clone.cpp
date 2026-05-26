#include "ast_clone.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace jitse {

std::unique_ptr<Expr> CloneExpr(const Expr& expr) {
  // P6.1: preserve SourceLoc through clone -- after inlining a subtree the
  // copies still point at the original tokens, which is the right answer
  // for "error: this `n` is invalid" diagnostics after the dependency
  // inliner has expanded a referencing signal.
  std::unique_ptr<Expr> out;
  if (const auto* n = dynamic_cast<const NumberLiteral*>(&expr)) {
    out = std::make_unique<NumberLiteral>(n->value);
  } else if (const auto* id = dynamic_cast<const IdentifierExpr*>(&expr)) {
    out = std::make_unique<IdentifierExpr>(id->name);
  } else if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
    out = std::make_unique<UnaryOp>(u->kind, CloneExpr(*u->operand));
  } else if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
    out = std::make_unique<BinaryOp>(b->kind, CloneExpr(*b->left), CloneExpr(*b->right));
  } else if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
    out = std::make_unique<Conditional>(CloneExpr(*c->condition), CloneExpr(*c->then_branch), CloneExpr(*c->else_branch));
  } else if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
    std::vector<std::unique_ptr<Expr>> args;
    args.reserve(fn->args.size());
    for (const auto& arg : fn->args) {
      args.push_back(CloneExpr(*arg));
    }
    out = std::make_unique<FunctionCall>(fn->name, std::move(args));
  } else {
    throw std::runtime_error("CloneExpr: unknown AST node");
  }
  out->loc = expr.loc;
  return out;
}

bool AstEquals(const Expr& a, const Expr& b) {
  // Discriminate on dynamic type via dynamic_cast chains. We do NOT
  // typeid-compare because the dynamic_cast chain is the same shape
  // the rest of the codebase uses, so the equality predicate stays
  // aligned with how the rest of the engine inspects AST nodes.
  if (const auto* an = dynamic_cast<const NumberLiteral*>(&a)) {
    const auto* bn = dynamic_cast<const NumberLiteral*>(&b);
    if (bn == nullptr) return false;
    // Bitwise-equal doubles. We deliberately treat +0 and -0 as
    // unequal here, mirroring how the JIT and interpreter would
    // emit/evaluate them. NaN == NaN is true under this rule (any
    // two NaN-bit-patterns produced by the parser are the same
    // literal token).
    std::uint64_t ai, bi;
    static_assert(sizeof(ai) == sizeof(an->value));
    std::memcpy(&ai, &an->value, sizeof(ai));
    std::memcpy(&bi, &bn->value, sizeof(bi));
    return ai == bi;
  }
  if (const auto* ai = dynamic_cast<const IdentifierExpr*>(&a)) {
    const auto* bi = dynamic_cast<const IdentifierExpr*>(&b);
    return bi != nullptr && ai->name == bi->name;
  }
  if (const auto* au = dynamic_cast<const UnaryOp*>(&a)) {
    const auto* bu = dynamic_cast<const UnaryOp*>(&b);
    return bu != nullptr && au->kind == bu->kind && AstEquals(*au->operand, *bu->operand);
  }
  if (const auto* ab = dynamic_cast<const BinaryOp*>(&a)) {
    const auto* bb = dynamic_cast<const BinaryOp*>(&b);
    return bb != nullptr && ab->kind == bb->kind &&
           AstEquals(*ab->left, *bb->left) && AstEquals(*ab->right, *bb->right);
  }
  if (const auto* ac = dynamic_cast<const Conditional*>(&a)) {
    const auto* bc = dynamic_cast<const Conditional*>(&b);
    return bc != nullptr && AstEquals(*ac->condition, *bc->condition) &&
           AstEquals(*ac->then_branch, *bc->then_branch) &&
           AstEquals(*ac->else_branch, *bc->else_branch);
  }
  if (const auto* af = dynamic_cast<const FunctionCall*>(&a)) {
    const auto* bf = dynamic_cast<const FunctionCall*>(&b);
    if (bf == nullptr) return false;
    if (af->name != bf->name) return false;
    if (af->args.size() != bf->args.size()) return false;
    for (std::size_t i = 0; i < af->args.size(); ++i) {
      if (!AstEquals(*af->args[i], *bf->args[i])) return false;
    }
    return true;
  }
  return false;
}

namespace {
// Stringify a double in a form that round-trips bit-exact via
// std::strtod. We use %.17g which is the IEEE 754 round-trip
// guarantee for doubles. NaN literals serialise as their bit
// pattern so the cache treats two different NaNs distinctly (the
// parser cannot emit a NaN literal, so this only matters for
// constant-folded arithmetic that produces NaN -- which the cache
// must NOT collapse onto a different NaN-producing program).
std::string CanonicalDouble(double v) {
  std::uint64_t bits;
  std::memcpy(&bits, &v, sizeof(double));
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g[%llx]", v, static_cast<unsigned long long>(bits));
  return buf;
}

void CanonicalAppend(const Expr& expr, std::string& out) {
  if (const auto* n = dynamic_cast<const NumberLiteral*>(&expr)) {
    out.append("n(");
    out.append(CanonicalDouble(n->value));
    out.append(")");
    return;
  }
  if (const auto* id = dynamic_cast<const IdentifierExpr*>(&expr)) {
    out.append("id(");
    out.append(id->name);
    out.append(")");
    return;
  }
  if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
    out.append("u(");
    out.append(std::to_string(static_cast<int>(u->kind)));
    out.append(",");
    CanonicalAppend(*u->operand, out);
    out.append(")");
    return;
  }
  if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
    out.append("b(");
    out.append(std::to_string(static_cast<int>(b->kind)));
    out.append(",");
    CanonicalAppend(*b->left, out);
    out.append(",");
    CanonicalAppend(*b->right, out);
    out.append(")");
    return;
  }
  if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
    out.append("if(");
    CanonicalAppend(*c->condition, out);
    out.append(",");
    CanonicalAppend(*c->then_branch, out);
    out.append(",");
    CanonicalAppend(*c->else_branch, out);
    out.append(")");
    return;
  }
  if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
    out.append("f(");
    out.append(fn->name);
    out.append(",");
    out.append(std::to_string(fn->args.size()));
    for (const auto& a : fn->args) {
      out.append(",");
      CanonicalAppend(*a, out);
    }
    out.append(")");
    return;
  }
}
}  // namespace

std::string AstCanonicalString(const Expr& expr) {
  std::string out;
  out.reserve(64);
  CanonicalAppend(expr, out);
  return out;
}

}  // namespace jitse

