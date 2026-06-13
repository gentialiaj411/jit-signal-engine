#include "dsl_formatter.h"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace jitse {
namespace {

// Precedence levels used during pretty-printing. Higher binds
// tighter. These match the parser's recursive-descent levels so the
// printed output reparses to the same tree.
//
//   PREC_COND       0   if-then-else
//   PREC_OR         1   ||
//   PREC_AND        2   &&
//   PREC_EQ         3   == !=
//   PREC_REL        4   < > <= >=
//   PREC_ADD        5   + - (binary)
//   PREC_MUL        6   * /
//   PREC_UNARY      7   - + (unary)
//   PREC_PRIMARY    8   literals, identifiers, function calls, parens
constexpr int kPrecCond = 0;
constexpr int kPrecOr = 1;
constexpr int kPrecAnd = 2;
constexpr int kPrecEq = 3;
constexpr int kPrecRel = 4;
constexpr int kPrecAdd = 5;
constexpr int kPrecMul = 6;
constexpr int kPrecUnary = 7;
constexpr int kPrecPrimary = 8;

int BinaryPrec(BinaryOpKind k) {
  switch (k) {
    case BinaryOpKind::Or:       return kPrecOr;
    case BinaryOpKind::And:      return kPrecAnd;
    case BinaryOpKind::Eq:
    case BinaryOpKind::NotEq:    return kPrecEq;
    case BinaryOpKind::Gt:
    case BinaryOpKind::Lt:
    case BinaryOpKind::Gte:
    case BinaryOpKind::Lte:      return kPrecRel;
    case BinaryOpKind::Add:
    case BinaryOpKind::Sub:      return kPrecAdd;
    case BinaryOpKind::Mul:
    case BinaryOpKind::Div:      return kPrecMul;
  }
  return kPrecPrimary;
}

const char* BinarySpelling(BinaryOpKind k) {
  switch (k) {
    case BinaryOpKind::Or:    return "||";
    case BinaryOpKind::And:   return "&&";
    case BinaryOpKind::Eq:    return "==";
    case BinaryOpKind::NotEq: return "!=";
    case BinaryOpKind::Gt:    return ">";
    case BinaryOpKind::Lt:    return "<";
    case BinaryOpKind::Gte:   return ">=";
    case BinaryOpKind::Lte:   return "<=";
    case BinaryOpKind::Add:   return "+";
    case BinaryOpKind::Sub:   return "-";
    case BinaryOpKind::Mul:   return "*";
    case BinaryOpKind::Div:   return "/";
  }
  return "?";
}

const char* UnarySpelling(UnaryOpKind k) {
  switch (k) {
    case UnaryOpKind::Plus:  return "+";
    case UnaryOpKind::Minus: return "-";
  }
  return "?";
}

// Print a double in a form the lexer accepts and that round-trips
// bit-exact via std::strtod. For integer-valued doubles we print the
// plain integer form (e.g. `10` not `10.0`) because that's what the
// lexer's literal token would have produced; for non-integer values
// we use `%g` with 17 significant digits.
std::string FormatNumber(double v) {
  if (std::isnan(v)) return "(0.0/0.0)";  // there is no NaN literal token in the DSL;
                                          // emit a divide-by-zero expression instead.
  if (std::isinf(v)) return std::signbit(v) ? "(-1.0/0.0)" : "(1.0/0.0)";
  if (v == 0.0) return "0";
  // Integer-valued check.
  const double trunc = std::trunc(v);
  if (trunc == v && std::fabs(v) < 1e15) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(trunc));
    return buf;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

// Forward decl.
void FormatExprInto(const Expr& expr, int outer_prec, std::ostringstream& out);

void FormatBinary(const BinaryOp& b, int outer_prec, std::ostringstream& out) {
  const int my_prec = BinaryPrec(b.kind);
  const bool need_paren = my_prec < outer_prec;
  if (need_paren) out << "(";
  // Left-associative for all current operators. Right child needs a
  // strictly-higher precedence to avoid reparse-as-left-child; left
  // child needs the same precedence.
  FormatExprInto(*b.left, my_prec, out);
  out << " " << BinarySpelling(b.kind) << " ";
  FormatExprInto(*b.right, my_prec + 1, out);
  if (need_paren) out << ")";
}

void FormatConditional(const Conditional& c, int outer_prec, std::ostringstream& out) {
  const bool need_paren = kPrecCond < outer_prec;
  if (need_paren) out << "(";
  out << "if ";
  FormatExprInto(*c.condition, kPrecCond + 1, out);
  out << " then ";
  FormatExprInto(*c.then_branch, kPrecCond + 1, out);
  out << " else ";
  // The else-branch is right-associative for chained `if ... then ...
  // else if ...`, so it takes the same precedence to allow nesting.
  FormatExprInto(*c.else_branch, kPrecCond, out);
  if (need_paren) out << ")";
}

void FormatUnary(const UnaryOp& u, int outer_prec, std::ostringstream& out) {
  const bool need_paren = kPrecUnary < outer_prec;
  if (need_paren) out << "(";
  out << UnarySpelling(u.kind);
  FormatExprInto(*u.operand, kPrecUnary, out);
  if (need_paren) out << ")";
}

void FormatFunctionCall(const FunctionCall& fn, std::ostringstream& out) {
  out << fn.name << "(";
  for (std::size_t i = 0; i < fn.args.size(); ++i) {
    if (i > 0) out << ", ";
    FormatExprInto(*fn.args[i], kPrecCond, out);
  }
  out << ")";
}

void FormatExprInto(const Expr& expr, int outer_prec, std::ostringstream& out) {
  if (const auto* n = dynamic_cast<const NumberLiteral*>(&expr)) {
    out << FormatNumber(n->value);
    return;
  }
  if (const auto* id = dynamic_cast<const IdentifierExpr*>(&expr)) {
    out << id->name;
    return;
  }
  if (const auto* p = dynamic_cast<const ParameterExpr*>(&expr)) {
    out << p->name;
    return;
  }
  if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
    FormatUnary(*u, outer_prec, out);
    return;
  }
  if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
    FormatBinary(*b, outer_prec, out);
    return;
  }
  if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
    FormatConditional(*c, outer_prec, out);
    return;
  }
  if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
    FormatFunctionCall(*fn, out);
    return;
  }
  throw std::runtime_error("FormatExpr: unknown AST node");
}

}  // namespace

std::string FormatExpr(const Expr& expr, int outer_prec) {
  std::ostringstream out;
  FormatExprInto(expr, outer_prec, out);
  return out.str();
}

std::string FormatSignalDef(const SignalDef& s) {
  std::ostringstream out;
  out << "signal " << s.name << " = ";
  if (s.body) FormatExprInto(*s.body, kPrecCond, out);
  return out.str();
}

std::string FormatParamDef(const ParamDef& p) {
  std::ostringstream out;
  out << "param " << p.name << " = " << FormatNumber(p.default_value);
  return out.str();
}

std::string FormatProgram(const std::vector<SignalDef>& signals) {
  std::ostringstream out;
  for (const auto& s : signals) {
    out << FormatSignalDef(s) << "\n";
  }
  return out.str();
}

std::string FormatProgram(const ProgramDef& program) {
  std::ostringstream out;
  for (const auto& p : program.params) {
    out << FormatParamDef(p) << "\n";
  }
  for (const auto& s : program.signals) {
    out << FormatSignalDef(s) << "\n";
  }
  return out.str();
}

}  // namespace jitse
