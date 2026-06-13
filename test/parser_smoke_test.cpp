#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "signal_program.h"

using jitse::BinaryOp;
using jitse::BinaryOpKind;
using jitse::Conditional;
using jitse::Expr;
using jitse::FunctionCall;
using jitse::IdentifierExpr;
using jitse::Lexer;
using jitse::NumberLiteral;
using jitse::Parser;

template <typename T>
const T* As(const std::unique_ptr<Expr>& expr) {
  return dynamic_cast<const T*>(expr.get());
}

int main() {
  {
    const std::string src = "signal spread = mid(AAPL) - mid(MSFT)";
    Lexer lexer(src);
    Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    assert(def.name == "spread");

    const BinaryOp* top = As<BinaryOp>(def.body);
    assert(top != nullptr);
    assert(top->kind == BinaryOpKind::Sub);

    const FunctionCall* left = As<FunctionCall>(top->left);
    const FunctionCall* right = As<FunctionCall>(top->right);
    assert(left != nullptr && right != nullptr);
    assert(left->name == "mid");
    assert(right->name == "mid");
    assert(left->args.size() == 1);
    assert(right->args.size() == 1);

    const IdentifierExpr* aapl = As<IdentifierExpr>(left->args[0]);
    const IdentifierExpr* msft = As<IdentifierExpr>(right->args[0]);
    assert(aapl != nullptr && msft != nullptr);
    assert(aapl->name == "AAPL");
    assert(msft->name == "MSFT");
  }

  {
    const std::string src = "signal x = 1 + 2 * 3";
    Lexer lexer(src);
    Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();

    const BinaryOp* add = As<BinaryOp>(def.body);
    assert(add != nullptr);
    assert(add->kind == BinaryOpKind::Add);

    const NumberLiteral* one = As<NumberLiteral>(add->left);
    const BinaryOp* mul = As<BinaryOp>(add->right);
    assert(one != nullptr && std::fabs(one->value - 1.0) < 1e-12);
    assert(mul != nullptr && mul->kind == BinaryOpKind::Mul);
  }
  {
    const std::string src = "signal c = if 1 + 2 * 3 > 6 then 10 else 20";
    Lexer lexer(src);
    Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    const Conditional* cond = As<Conditional>(def.body);
    assert(cond != nullptr);

    const BinaryOp* gt = As<BinaryOp>(cond->condition);
    assert(gt != nullptr && gt->kind == BinaryOpKind::Gt);
    const BinaryOp* add = As<BinaryOp>(gt->left);
    assert(add != nullptr && add->kind == BinaryOpKind::Add);
  }
  {
    const std::string src =
        "param alpha = 1 + 2\n"
        "signal s = alpha * 3\n";
    jitse::ProgramDef program = jitse::ParseProgram(src);
    assert(program.params.size() == 1);
    assert(program.params[0].name == "alpha");
    assert(std::fabs(program.params[0].default_value - 3.0) < 1e-12);
    assert(program.signals.size() == 1);
    const BinaryOp* mul = As<BinaryOp>(program.signals[0].body);
    assert(mul != nullptr && mul->kind == BinaryOpKind::Mul);
    const jitse::ParameterExpr* param = As<jitse::ParameterExpr>(mul->left);
    assert(param != nullptr);
    assert(param->name == "alpha");
    assert(param->param_id == 0);
  }

  std::cout << "parser_smoke_test passed\n";
  return 0;
}
