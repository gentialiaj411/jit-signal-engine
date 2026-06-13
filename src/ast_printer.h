#pragma once

#include <ostream>
#include <string>

#include "ast.h"

namespace jitse {

class AstPrinter : public ExprVisitor {
 public:
  explicit AstPrinter(std::ostream& out);
  void Print(const Expr& expr);

  void Visit(const NumberLiteral& n) override;
  void Visit(const IdentifierExpr& id) override;
  void Visit(const ParameterExpr& p) override;
  void Visit(const UnaryOp& u) override;
  void Visit(const BinaryOp& b) override;
  void Visit(const FunctionCall& fn) override;
  void Visit(const Conditional& c) override;

 private:
  void Indent() const;
  static std::string BinaryOpName(BinaryOpKind kind);
  static std::string UnaryOpName(UnaryOpKind kind);

  std::ostream& out_;
  int indent_ = 0;
};

}  // namespace jitse
