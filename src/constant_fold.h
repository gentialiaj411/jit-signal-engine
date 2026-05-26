#pragma once

#include <memory>

#include "ast.h"

namespace jitse {

// P6.2: AST-level constant folding. Walks the expression tree bottom-up
// and replaces sub-expressions whose value is statically known with a
// `NumberLiteral`. Folding happens at the AST level *before* any IR is
// emitted, so the JIT sees `sma(mid(AAPL), 10)` whether the user wrote
// `sma(mid(AAPL), 10)` or `sma(mid(AAPL), 5 + 5)`.
//
// Folding rules (all on doubles, since the DSL is single-typed for
// numerics):
//   - UnaryOp(+x) -> x (when x is a literal)
//   - UnaryOp(-x) -> NumberLiteral(-x.value)
//   - BinaryOp(Add/Sub/Mul/Div, NumberLiteral, NumberLiteral) -> NumberLiteral
//   - BinaryOp(Gt/Lt/.../Eq/NotEq, NumberLiteral, NumberLiteral) -> NumberLiteral(1.0|0.0)
//   - BinaryOp(And/Or, NumberLiteral, NumberLiteral) -> NumberLiteral(1.0|0.0)
//   - Conditional(NumberLiteral cond, ...) -> then or else branch
//   - FunctionCall is *not* folded (operators like sma() depend on
//     runtime state; we conservatively never fold them).
//
// Folding preserves SourceLoc of the *outermost* fused node so caret
// diagnostics still point at the original source.
//
// `FoldConstants` returns a *new* expression tree; the input is consumed
// (passed by unique_ptr&&). This makes the pass purely a rewrite and
// keeps the caller free to choose where to store the result.
std::unique_ptr<Expr> FoldConstants(std::unique_ptr<Expr> expr);

// Convenience: in-place fold of a SignalDef's body.
void FoldConstantsInPlace(SignalDef& signal);

}  // namespace jitse
