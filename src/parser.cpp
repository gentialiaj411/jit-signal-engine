#include "parser.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace jitse {

// P6.1: Render compiler-style diagnostics. With a non-empty source_line
// we produce:
//
//   error: <msg> at line N, col C
//     | <source line>
//     |       ^^^^
//
// Without a source line (line==0 or empty source), we fall back to a
// single-line "error: <msg>" or "error: <msg> at line N, col C".
std::string ParseError::BuildWhat(const std::string& msg, SourceLoc loc,
                                  const std::string& source_line) {
  std::ostringstream os;
  os << "error: " << msg;
  if (loc.line != 0) {
    os << " at line " << loc.line << ", col " << loc.col;
  }
  if (loc.line != 0 && !source_line.empty()) {
    os << "\n  | " << source_line << "\n  | ";
    const std::uint32_t col = loc.col > 0 ? loc.col - 1 : 0;
    for (std::uint32_t i = 0; i < col; ++i) os << ' ';
    const std::uint32_t len = loc.length > 0 ? loc.length : 1;
    for (std::uint32_t i = 0; i < len; ++i) os << '^';
  }
  return os.str();
}

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
  // P6.1: Carry source location through into ParseError so the
  // line-aware orchestrator (ParseSignalProgram) can paint a caret.
  Fail(message + " (got: '" + Peek().lexeme + "')", Peek().loc);
}

void Parser::Fail(const std::string& msg, SourceLoc loc) const {
  throw ParseError(msg, loc);
}

std::unique_ptr<Expr> Parser::ParseExpr() {
  return ParseConditional();
}

std::unique_ptr<Expr> Parser::ParseConditional() {
  if (Peek().kind == TokenKind::If) {
    const SourceLoc start = Peek().loc;
    ++pos_;  // consume 'if'
    std::unique_ptr<Expr> cond = ParseExpr();
    Consume(TokenKind::Then, "Expected 'then' in conditional expression");
    std::unique_ptr<Expr> then_expr = ParseExpr();
    Consume(TokenKind::Else, "Expected 'else' in conditional expression");
    std::unique_ptr<Expr> else_expr = ParseExpr();
    auto out = std::make_unique<Conditional>(std::move(cond), std::move(then_expr), std::move(else_expr));
    out->loc = start;
    return out;
  }
  return ParseLogical();
}

std::unique_ptr<Expr> Parser::ParseLogical() {
  std::unique_ptr<Expr> expr = ParseComparison();
  while (true) {
    if (Peek().kind == TokenKind::And || Peek().kind == TokenKind::Or) {
      const SourceLoc op_loc = Peek().loc;
      const BinaryOpKind kind = Peek().kind == TokenKind::And ? BinaryOpKind::And : BinaryOpKind::Or;
      ++pos_;
      auto rhs = ParseComparison();
      auto next = std::make_unique<BinaryOp>(kind, std::move(expr), std::move(rhs));
      next->loc = op_loc;
      expr = std::move(next);
    } else {
      break;
    }
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseComparison() {
  std::unique_ptr<Expr> expr = ParseTerm();
  while (true) {
    BinaryOpKind kind;
    switch (Peek().kind) {
      case TokenKind::Gt: kind = BinaryOpKind::Gt; break;
      case TokenKind::Lt: kind = BinaryOpKind::Lt; break;
      case TokenKind::Gte: kind = BinaryOpKind::Gte; break;
      case TokenKind::Lte: kind = BinaryOpKind::Lte; break;
      case TokenKind::Eq: kind = BinaryOpKind::Eq; break;
      case TokenKind::NotEq: kind = BinaryOpKind::NotEq; break;
      default: return expr;
    }
    const SourceLoc op_loc = Peek().loc;
    ++pos_;
    auto rhs = ParseTerm();
    auto next = std::make_unique<BinaryOp>(kind, std::move(expr), std::move(rhs));
    next->loc = op_loc;
    expr = std::move(next);
  }
}

std::unique_ptr<Expr> Parser::ParseTerm() {
  std::unique_ptr<Expr> expr = ParseFactor();
  while (true) {
    BinaryOpKind kind;
    if (Peek().kind == TokenKind::Plus) kind = BinaryOpKind::Add;
    else if (Peek().kind == TokenKind::Minus) kind = BinaryOpKind::Sub;
    else return expr;
    const SourceLoc op_loc = Peek().loc;
    ++pos_;
    auto rhs = ParseFactor();
    auto next = std::make_unique<BinaryOp>(kind, std::move(expr), std::move(rhs));
    next->loc = op_loc;
    expr = std::move(next);
  }
}

std::unique_ptr<Expr> Parser::ParseFactor() {
  std::unique_ptr<Expr> expr = ParseUnary();
  while (true) {
    BinaryOpKind kind;
    if (Peek().kind == TokenKind::Star) kind = BinaryOpKind::Mul;
    else if (Peek().kind == TokenKind::Slash) kind = BinaryOpKind::Div;
    else return expr;
    const SourceLoc op_loc = Peek().loc;
    ++pos_;
    auto rhs = ParseUnary();
    auto next = std::make_unique<BinaryOp>(kind, std::move(expr), std::move(rhs));
    next->loc = op_loc;
    expr = std::move(next);
  }
}

std::unique_ptr<Expr> Parser::ParseUnary() {
  if (Peek().kind == TokenKind::Plus || Peek().kind == TokenKind::Minus) {
    const SourceLoc op_loc = Peek().loc;
    const UnaryOpKind kind = Peek().kind == TokenKind::Plus ? UnaryOpKind::Plus : UnaryOpKind::Minus;
    ++pos_;
    auto inner = ParseUnary();
    auto out = std::make_unique<UnaryOp>(kind, std::move(inner));
    out->loc = op_loc;
    return out;
  }
  return ParsePrimary();
}

std::unique_ptr<Expr> Parser::ParsePrimary() {
  if (Peek().kind == TokenKind::Number) {
    const Token tok = Peek();
    ++pos_;
    auto out = std::make_unique<NumberLiteral>(tok.number_value);
    out->loc = tok.loc;
    return out;
  }

  if (Peek().kind == TokenKind::Identifier) {
    const Token identifier = Peek();
    ++pos_;
    if (Match(TokenKind::LParen)) {
      std::vector<std::unique_ptr<Expr>> args = ParseArgList();
      Consume(TokenKind::RParen, "Expected ')' after function args");
      auto out = std::make_unique<FunctionCall>(identifier.lexeme, std::move(args));
      out->loc = identifier.loc;
      return out;
    }
    auto out = std::make_unique<IdentifierExpr>(identifier.lexeme);
    out->loc = identifier.loc;
    return out;
  }

  if (Peek().kind == TokenKind::LParen) {
    const SourceLoc lparen_loc = Peek().loc;
    ++pos_;
    std::unique_ptr<Expr> expr = ParseExpr();
    Consume(TokenKind::RParen, "Expected ')' after expression");
    // Keep the inner expr's loc -- the parens are syntactic noise. But
    // if the inner expression somehow has no loc (e.g. malformed), fall
    // back to the '(' location so diagnostics still point at something.
    if (expr->loc.line == 0 && expr->loc.col == 0) expr->loc = lparen_loc;
    return expr;
  }

  Fail("Expected expression", Peek().loc);
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
