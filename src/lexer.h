#pragma once

#include <string>
#include <vector>

namespace jitse {

enum class TokenKind {
  EndOfFile,
  Number,
  Identifier,
  Signal,
  If,
  Then,
  Else,
  Assign,
  Plus,
  Minus,
  Star,
  Slash,
  LParen,
  RParen,
  Comma,
  Gt,
  Lt,
  Gte,
  Lte,
  Eq,
  NotEq,
  And,
  Or,
};

struct Token {
  TokenKind kind;
  std::string lexeme;
  double number_value = 0.0;
};

class Lexer {
 public:
  explicit Lexer(std::string source);
  std::vector<Token> Tokenize();

 private:
  bool IsAtEnd() const;
  char Peek() const;
  char Advance();
  void SkipWhitespace();
  Token ReadNumber();
  Token ReadIdentifierOrKeyword();

  std::string source_;
  std::size_t pos_ = 0;
};

}  // namespace jitse
