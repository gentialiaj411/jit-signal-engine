#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jitse {

// P6.1: source location attached to every Token (and propagated to every
// AST Expr by the parser). `line` is 1-based; `col` is 1-based within the
// line; `length` is the byte length of the token's source lexeme.
// `line == 0` is the sentinel "unknown source" used by synthetic AST nodes
// (constant-folded results, fuzz-generated programs, etc.) -- error
// renderers check for line==0 and fall back to "<unknown>".
struct SourceLoc {
  std::uint32_t line = 0;
  std::uint32_t col = 0;
  std::uint32_t length = 0;
};

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
  SourceLoc loc{};  // filled by Lexer (col + length); line is set by the orchestrator
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
  std::uint32_t Col() const {
    // 1-based column within the line. pos_ is 0-based.
    return static_cast<std::uint32_t>(pos_ + 1);
  }

  std::string source_;
  std::size_t pos_ = 0;
};

}  // namespace jitse
