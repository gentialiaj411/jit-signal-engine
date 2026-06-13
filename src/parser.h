#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "ast.h"
#include "lexer.h"

namespace jitse {

// P6.1: every parse / type-check error carries a SourceLoc plus the
// originating line of source. RenderSourceError() turns the pair into a
// compiler-style diagnostic with a caret underline pointing at the
// offending token.
//
// The line of source is optional (defaults to ""); when present, the
// renderer emits the source line plus a "^^^" underline beneath it. When
// absent (line==0 or source empty), the renderer falls back to a plain
// "error: msg at line N, col C" string.
class ParseError : public std::runtime_error {
 public:
  ParseError(std::string msg, SourceLoc loc, std::string source_line = "")
      : std::runtime_error(BuildWhat(msg, loc, source_line)),
        msg_(std::move(msg)),
        loc_(loc),
        source_line_(std::move(source_line)) {}

  const std::string& Message() const noexcept { return msg_; }
  SourceLoc Loc() const noexcept { return loc_; }
  const std::string& SourceLine() const noexcept { return source_line_; }

  // Returns the same string what() returns; provided for explicit calls.
  static std::string Render(const std::string& msg, SourceLoc loc,
                            const std::string& source_line) {
    return BuildWhat(msg, loc, source_line);
  }

 private:
  static std::string BuildWhat(const std::string& msg, SourceLoc loc,
                               const std::string& source_line);
  std::string msg_;
  SourceLoc loc_;
  std::string source_line_;
};

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens);
  SignalDef ParseSignalDef();
  ParamDef ParseParamDef();

 private:
  const Token& Peek() const;
  const Token& Previous() const;
  bool IsAtEnd() const;
  bool Match(TokenKind kind);
  const Token& Consume(TokenKind kind, const std::string& message);
  [[noreturn]] void Fail(const std::string& msg, SourceLoc loc) const;

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
