#pragma once

#include <memory>

#include "ast.h"

namespace jitse {

std::unique_ptr<Expr> CloneExpr(const Expr& expr);

// P11: structural deep equality for AST subtrees. Returns true iff
// `a` and `b` represent the same expression up to:
//   - identical node kind
//   - identical operator kind / identifier name / function name /
//     literal value
//   - structurally-equal children in order
//
// Source locations (`Expr::loc`) and bookkeeping fields like
// `FunctionCall::node_id` and `FunctionCall::symbol_id` are NOT
// considered -- they are output of later passes, not part of the
// AST's identity.
//
// Used by the post-inline node-id dedup pass so that two structurally-
// equal stateful subtrees (which `InlineSignalDependencies` produces
// when the same signal is referenced twice in one body) share a node-
// id, and thus share runtime state and a single emitted IR call.
bool AstEquals(const Expr& a, const Expr& b);

// P13: deterministic canonical-string serialization of an AST. The
// output is whitespace-insensitive, depends only on the structural
// shape + operator kinds + literals + identifier/function names, and
// is stable across runs of the same binary (and across hosts running
// the same source). Two ASTs serialise to the same string iff
// `AstEquals` would return true for them; this is what the JIT module
// cache hashes to key its bitcode files. Source locations,
// `FunctionCall::node_id`, and `FunctionCall::symbol_id` are NOT
// included -- they are downstream pass output, not part of the
// program's identity.
std::string AstCanonicalString(const Expr& expr);

}  // namespace jitse

