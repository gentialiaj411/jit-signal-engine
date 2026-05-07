#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jitse {

struct NumberLiteral;
struct IdentifierExpr;
struct UnaryOp;
struct BinaryOp;
struct FunctionCall;
struct Conditional;

struct ExprVisitor {
  virtual ~ExprVisitor() = default;
  virtual void Visit(const NumberLiteral&) = 0;
  virtual void Visit(const IdentifierExpr&) = 0;
  virtual void Visit(const UnaryOp&) = 0;
  virtual void Visit(const BinaryOp&) = 0;
  virtual void Visit(const FunctionCall&) = 0;
  virtual void Visit(const Conditional&) = 0;
};

enum class BinaryOpKind {
  Add,
  Sub,
  Mul,
  Div,
  Gt,
  Lt,
  Gte,
  Lte,
  Eq,
  NotEq,
  And,
  Or,
};

enum class UnaryOpKind {
  Plus,
  Minus,
};

struct Expr {
  virtual ~Expr() = default;
  virtual void Accept(ExprVisitor& v) const = 0;
};

struct NumberLiteral final : Expr {
  explicit NumberLiteral(double v) : value(v) {}
  double value;
  void Accept(ExprVisitor& v) const override { v.Visit(*this); }
};

// In MVP, ticker references appear as arguments in mid(AAPL).
struct IdentifierExpr final : Expr {
  explicit IdentifierExpr(std::string n) : name(std::move(n)) {}
  std::string name;
  void Accept(ExprVisitor& v) const override { v.Visit(*this); }
};

struct UnaryOp final : Expr {
  UnaryOp(UnaryOpKind k, std::unique_ptr<Expr> e)
      : kind(k), operand(std::move(e)) {}
  UnaryOpKind kind;
  std::unique_ptr<Expr> operand;
  void Accept(ExprVisitor& v) const override { v.Visit(*this); }
};

struct BinaryOp final : Expr {
  BinaryOp(BinaryOpKind k, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r)
      : kind(k), left(std::move(l)), right(std::move(r)) {}
  BinaryOpKind kind;
  std::unique_ptr<Expr> left;
  std::unique_ptr<Expr> right;
  void Accept(ExprVisitor& v) const override { v.Visit(*this); }
};

struct FunctionCall final : Expr {
  FunctionCall(std::string fn, std::vector<std::unique_ptr<Expr>> a)
      : name(std::move(fn)), args(std::move(a)) {}
  std::string name;
  std::vector<std::unique_ptr<Expr>> args;
  mutable std::int64_t node_id = -1;
  void Accept(ExprVisitor& v) const override { v.Visit(*this); }
};

struct Conditional final : Expr {
  Conditional(std::unique_ptr<Expr> c, std::unique_ptr<Expr> t, std::unique_ptr<Expr> e)
      : condition(std::move(c)), then_branch(std::move(t)), else_branch(std::move(e)) {}
  std::unique_ptr<Expr> condition;
  std::unique_ptr<Expr> then_branch;
  std::unique_ptr<Expr> else_branch;
  void Accept(ExprVisitor& v) const override { v.Visit(*this); }
};

struct SignalDef {
  std::string name;
  std::unique_ptr<Expr> body;
};

}  // namespace jitse
