#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "lexer.h"

namespace jitse {

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens);
  SignalDef ParseSignalDef();

 private:
  const Token& Peek() const;
  const Token& Previous() const;
  bool IsAtEnd() const;
  bool Match(TokenKind kind);
  const Token& Consume(TokenKind kind, const std::string& message);

  std::unique_ptr<Expr> ParseExpr();
  std::unique_ptr<Expr> ParseConditional();
  std::unique_ptr<Expr> ParseLogical();
  std::unique_ptr<Expr> ParseComparison();
  std::unique_ptr<Expr> ParseTerm();
  std::unique_ptr<Expr> ParseFactor();
  std::unique_ptr<Expr> ParseUnary();
  std::unique_ptr<Expr> ParsePrimary();
  std::vector<std::unique_ptr<Expr>> ParseArgList();

  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
};

}  // namespace jitse
