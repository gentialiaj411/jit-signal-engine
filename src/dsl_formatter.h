// P15: pretty-printer for the signal DSL.
//
// Round-trips through the parser: for any `src` accepted by
// `ParseSignalProgram`, `FormatProgram(ParseSignalProgram(src))` is
// itself accepted, parses to a structurally-equal AST, and is
// idempotent (`fmt(fmt(x)) == fmt(x)`). Comments and original
// whitespace are NOT preserved -- the formatter is the canonical
// shape of every program, like gofmt for the DSL.
//
// Precedence is reconstructed from the AST kinds; parentheses are
// inserted only where the parser would otherwise reassociate.
//
// Used by:
//   * `jitse fmt`  -- prints the formatted source to stdout (or
//     rewrites the input file with `--in-place`).
//   * `jitse lint` -- doesn't call the formatter, but shares the
//     parser/type-check/fold pipeline with it.

#pragma once

#include <string>
#include <vector>

#include "ast.h"

namespace jitse {

// Format a single Expr at the given outer-context precedence. Outer
// precedence is used to decide whether the Expr's top-level operator
// needs parentheses; callers in tests can pass 0 (the lowest
// precedence) to mean "outermost, no parens unless required by an
// inner reassociation".
std::string FormatExpr(const Expr& expr, int outer_prec = 0);

// Format one signal definition as a single line:
//
//   signal <name> = <expr>
//
// No trailing newline.
std::string FormatSignalDef(const SignalDef& s);
std::string FormatParamDef(const ParamDef& p);

// Format an entire program: one signal per line, newline-separated,
// trailing newline included. The order of signals is preserved as-is
// (the formatter does NOT run `InlineSignalDependencies` first).
std::string FormatProgram(const std::vector<SignalDef>& signals);
std::string FormatProgram(const ProgramDef& program);

}  // namespace jitse
