#include "parser.h"

#include <stdexcept>
#include <utility>

namespace jitse {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

SignalDef Parser::ParseSignalDef() {
  Consume(TokenKind::Signal, "Expected 'signal' at start of definition");
  const Token& name = Consume(TokenKind::Identifier, "Expected signal name");
  Consume(TokenKind::Assign, "Expected '=' after signal name");
  std::unique_ptr<Expr> body = ParseExpr();
  Consume(TokenKind::EndOfFile, "Expected end of input");
  return SignalDef{name.lexeme, std::move(body)};
}

const Token& Parser::Peek() const { return tokens_[pos_]; }

const Token& Parser::Previous() const { return tokens_[pos_ - 1]; }

bool Parser::IsAtEnd() const { return Peek().kind == TokenKind::EndOfFile; }

bool Parser::Match(TokenKind kind) {
  if (Peek().kind != kind) {
    return false;
  }
  ++pos_;
  return true;
}

const Token& Parser::Consume(TokenKind kind, const std::string& message) {
  if (Peek().kind == kind) {
    ++pos_;
    return Previous();
  }
  throw std::runtime_error(message + " (got: '" + Peek().lexeme + "')");
}

std::unique_ptr<Expr> Parser::ParseExpr() {
  return ParseConditional();
}

std::unique_ptr<Expr> Parser::ParseConditional() {
  if (Match(TokenKind::If)) {
    std::unique_ptr<Expr> cond = ParseExpr();
    Consume(TokenKind::Then, "Expected 'then' in conditional expression");
    std::unique_ptr<Expr> then_expr = ParseExpr();
    Consume(TokenKind::Else, "Expected 'else' in conditional expression");
    std::unique_ptr<Expr> else_expr = ParseExpr();
    return std::make_unique<Conditional>(std::move(cond), std::move(then_expr), std::move(else_expr));
  }
  return ParseLogical();
}

std::unique_ptr<Expr> Parser::ParseLogical() {
  std::unique_ptr<Expr> expr = ParseComparison();
  while (true) {
    if (Match(TokenKind::And)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::And, std::move(expr), ParseComparison());
    } else if (Match(TokenKind::Or)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::Or, std::move(expr), ParseComparison());
    } else {
      break;
    }
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseComparison() {
  std::unique_ptr<Expr> expr = ParseTerm();
  while (true) {
    if (Match(TokenKind::Gt)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::Gt, std::move(expr), ParseTerm());
    } else if (Match(TokenKind::Lt)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::Lt, std::move(expr), ParseTerm());
    } else if (Match(TokenKind::Gte)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::Gte, std::move(expr), ParseTerm());
    } else if (Match(TokenKind::Lte)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::Lte, std::move(expr), ParseTerm());
    } else if (Match(TokenKind::Eq)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::Eq, std::move(expr), ParseTerm());
    } else if (Match(TokenKind::NotEq)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::NotEq, std::move(expr), ParseTerm());
    } else {
      break;
    }
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseTerm() {
  std::unique_ptr<Expr> expr = ParseFactor();
  while (true) {
    if (Match(TokenKind::Plus)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::Add, std::move(expr), ParseFactor());
    } else if (Match(TokenKind::Minus)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::Sub, std::move(expr), ParseFactor());
    } else {
      break;
    }
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseFactor() {
  std::unique_ptr<Expr> expr = ParseUnary();
  while (true) {
    if (Match(TokenKind::Star)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::Mul, std::move(expr), ParseUnary());
    } else if (Match(TokenKind::Slash)) {
      expr = std::make_unique<BinaryOp>(BinaryOpKind::Div, std::move(expr), ParseUnary());
    } else {
      break;
    }
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseUnary() {
  if (Match(TokenKind::Plus)) {
    return std::make_unique<UnaryOp>(UnaryOpKind::Plus, ParseUnary());
  }
  if (Match(TokenKind::Minus)) {
    return std::make_unique<UnaryOp>(UnaryOpKind::Minus, ParseUnary());
  }
  return ParsePrimary();
}

std::unique_ptr<Expr> Parser::ParsePrimary() {
  if (Match(TokenKind::Number)) {
    return std::make_unique<NumberLiteral>(Previous().number_value);
  }

  if (Match(TokenKind::Identifier)) {
    const Token identifier = Previous();
    if (Match(TokenKind::LParen)) {
      std::vector<std::unique_ptr<Expr>> args = ParseArgList();
      Consume(TokenKind::RParen, "Expected ')' after function args");
      return std::make_unique<FunctionCall>(identifier.lexeme, std::move(args));
    }
    return std::make_unique<IdentifierExpr>(identifier.lexeme);
  }

  if (Match(TokenKind::LParen)) {
    std::unique_ptr<Expr> expr = ParseExpr();
    Consume(TokenKind::RParen, "Expected ')' after expression");
    return expr;
  }

  throw std::runtime_error("Expected expression");
}

std::vector<std::unique_ptr<Expr>> Parser::ParseArgList() {
  std::vector<std::unique_ptr<Expr>> args;
  if (Peek().kind == TokenKind::RParen) {
    return args;
  }

  args.push_back(ParseExpr());
  while (Match(TokenKind::Comma)) {
    args.push_back(ParseExpr());
  }
  return args;
}

}  // namespace jitse
