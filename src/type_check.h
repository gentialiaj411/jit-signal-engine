#pragma once

#include <string>

#include "ast.h"
#include "parser.h"  // for ParseError

namespace jitse {

// P6.3: minimal type system. The DSL has exactly two static types:
//
//   Number  -- doubles. The result of arithmetic, FunctionCall, and the
//             conditional expression. Everything ultimately flows into a
//             Number because that's what the runtime stores per signal.
//   Bool    -- the result of comparisons (a > b) and logical ops
//             (cond && cond). Required for `if` conditions and as
//             operands to `&&`/`||`.
//
// We intentionally do *not* implicitly coerce. A program that writes
// `if 1 then x else y` was previously accepted (since everything was
// double, and "1" was truthy); under P6.3 it is now a typed-language
// error reported with a source caret.
//
// Note on tickers: function calls like `mid(AAPL)` take a ticker
// identifier as their first argument. The type checker special-cases
// ticker-first-arg market-data builtins and treats the first arg as
// opaque (skipping the type rule for it). Other arguments are checked
// as Number.
enum class StaticType {
  Number,
  Bool,
};

// Walks the AST, type-checks, and returns the type of the root.
// Throws ParseError (location-bearing) on the first type violation.
StaticType TypeCheckExpr(const Expr& expr);

// Convenience: type-checks a signal definition; the signal's body must
// have type Number (since that's what the runtime stores).
void TypeCheckSignal(const SignalDef& signal);

}  // namespace jitse
