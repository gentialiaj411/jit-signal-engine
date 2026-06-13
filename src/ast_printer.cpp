#include "ast_printer.h"

namespace jitse {

AstPrinter::AstPrinter(std::ostream& out) : out_(out) {}

void AstPrinter::Print(const Expr& expr) { expr.Accept(*this); }

void AstPrinter::Visit(const NumberLiteral& n) {
  Indent();
  out_ << "NumberLiteral(" << n.value << ")\n";
}

void AstPrinter::Visit(const IdentifierExpr& id) {
  Indent();
  out_ << "Identifier(" << id.name << ")\n";
}

void AstPrinter::Visit(const ParameterExpr& p) {
  Indent();
  out_ << "Parameter(" << p.name << ", id=" << p.param_id << ")\n";
}

void AstPrinter::Visit(const UnaryOp& u) {
  Indent();
  out_ << "UnaryOp(" << UnaryOpName(u.kind) << ")\n";
  ++indent_;
  u.operand->Accept(*this);
  --indent_;
}

void AstPrinter::Visit(const BinaryOp& b) {
  Indent();
  out_ << "BinaryOp(" << BinaryOpName(b.kind) << ")\n";
  ++indent_;
  b.left->Accept(*this);
  b.right->Accept(*this);
  --indent_;
}

void AstPrinter::Visit(const FunctionCall& fn) {
  Indent();
  out_ << "FunctionCall(" << fn.name << ")\n";
  ++indent_;
  for (const auto& arg : fn.args) {
    arg->Accept(*this);
  }
  --indent_;
}

void AstPrinter::Visit(const Conditional& c) {
  Indent();
  out_ << "Conditional\n";
  ++indent_;
  c.condition->Accept(*this);
  c.then_branch->Accept(*this);
  c.else_branch->Accept(*this);
  --indent_;
}

void AstPrinter::Indent() const {
  for (int i = 0; i < indent_; ++i) {
    out_ << "  ";
  }
}

std::string AstPrinter::BinaryOpName(BinaryOpKind kind) {
  switch (kind) {
    case BinaryOpKind::Add: return "Add";
    case BinaryOpKind::Sub: return "Sub";
    case BinaryOpKind::Mul: return "Mul";
    case BinaryOpKind::Div: return "Div";
    case BinaryOpKind::Gt: return "Gt";
    case BinaryOpKind::Lt: return "Lt";
    case BinaryOpKind::Gte: return "Gte";
    case BinaryOpKind::Lte: return "Lte";
    case BinaryOpKind::Eq: return "Eq";
    case BinaryOpKind::NotEq: return "NotEq";
    case BinaryOpKind::And: return "And";
    case BinaryOpKind::Or: return "Or";
  }
  return "Unknown";
}

std::string AstPrinter::UnaryOpName(UnaryOpKind kind) {
  switch (kind) {
    case UnaryOpKind::Plus: return "Plus";
    case UnaryOpKind::Minus: return "Minus";
  }
  return "Unknown";
}

}  // namespace jitse
