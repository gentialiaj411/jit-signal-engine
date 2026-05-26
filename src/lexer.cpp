#include "lexer.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace jitse {

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

// P6.1: every token returned by Tokenize() carries (col, length) in
// `loc`; `line` is left at 0 here and filled by the caller in
// ParseSignalProgram, which is the only place that knows which DSL line
// we're on. `length` is the byte length of the lexeme.
std::vector<Token> Lexer::Tokenize() {
  std::vector<Token> out;
  while (!IsAtEnd()) {
    SkipWhitespace();
    if (IsAtEnd()) {
      break;
    }

    const char c = Peek();
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
      out.push_back(ReadNumber());
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      out.push_back(ReadIdentifierOrKeyword());
      continue;
    }

    const std::uint32_t start_col = Col();
    Advance();
    auto emit = [&](TokenKind kind, const char* lex, std::uint32_t len) {
      Token t;
      t.kind = kind;
      t.lexeme = lex;
      t.number_value = 0.0;
      t.loc.line = 0;
      t.loc.col = start_col;
      t.loc.length = len;
      out.push_back(std::move(t));
    };
    switch (c) {
      case '=':
        if (!IsAtEnd() && Peek() == '=') {
          Advance();
          emit(TokenKind::Eq, "==", 2);
        } else {
          emit(TokenKind::Assign, "=", 1);
        }
        break;
      case '!':
        if (!IsAtEnd() && Peek() == '=') {
          Advance();
          emit(TokenKind::NotEq, "!=", 2);
        } else {
          throw std::runtime_error("Unexpected '!' - did you mean '!='?");
        }
        break;
      case '>':
        if (!IsAtEnd() && Peek() == '=') {
          Advance();
          emit(TokenKind::Gte, ">=", 2);
        } else {
          emit(TokenKind::Gt, ">", 1);
        }
        break;
      case '&':
        if (!IsAtEnd() && Peek() == '&') {
          Advance();
          emit(TokenKind::And, "&&", 2);
        } else {
          throw std::runtime_error("Unexpected '&' - did you mean '&&'?");
        }
        break;
      case '|':
        if (!IsAtEnd() && Peek() == '|') {
          Advance();
          emit(TokenKind::Or, "||", 2);
        } else {
          throw std::runtime_error("Unexpected '|' - did you mean '||'?");
        }
        break;
      case '<':
        if (!IsAtEnd() && Peek() == '=') {
          Advance();
          emit(TokenKind::Lte, "<=", 2);
        } else {
          emit(TokenKind::Lt, "<", 1);
        }
        break;
      case '+': emit(TokenKind::Plus, "+", 1); break;
      case '-': emit(TokenKind::Minus, "-", 1); break;
      case '*': emit(TokenKind::Star, "*", 1); break;
      case '/': emit(TokenKind::Slash, "/", 1); break;
      case '(': emit(TokenKind::LParen, "(", 1); break;
      case ')': emit(TokenKind::RParen, ")", 1); break;
      case ',': emit(TokenKind::Comma, ",", 1); break;
      default:
        throw std::runtime_error("Unexpected character in lexer");
    }
  }

  Token eof;
  eof.kind = TokenKind::EndOfFile;
  eof.loc.col = Col();
  eof.loc.length = 0;
  out.push_back(std::move(eof));
  return out;
}

bool Lexer::IsAtEnd() const { return pos_ >= source_.size(); }

char Lexer::Peek() const { return source_[pos_]; }

char Lexer::Advance() { return source_[pos_++]; }

void Lexer::SkipWhitespace() {
  while (!IsAtEnd() && std::isspace(static_cast<unsigned char>(Peek()))) {
    Advance();
  }
}

Token Lexer::ReadNumber() {
  const std::size_t start = pos_;
  const std::uint32_t start_col = Col();
  bool seen_dot = false;

  while (!IsAtEnd()) {
    const char c = Peek();
    if (std::isdigit(static_cast<unsigned char>(c))) {
      Advance();
      continue;
    }
    if (c == '.') {
      if (seen_dot) {
        break;
      }
      seen_dot = true;
      Advance();
      continue;
    }
    break;
  }

  const std::string lexeme = source_.substr(start, pos_ - start);
  if (lexeme == ".") {
    throw std::runtime_error("Invalid numeric literal");
  }
  Token t;
  t.kind = TokenKind::Number;
  t.lexeme = lexeme;
  t.number_value = std::strtod(lexeme.c_str(), nullptr);
  t.loc.line = 0;
  t.loc.col = start_col;
  t.loc.length = static_cast<std::uint32_t>(lexeme.size());
  return t;
}

Token Lexer::ReadIdentifierOrKeyword() {
  const std::size_t start = pos_;
  const std::uint32_t start_col = Col();
  while (!IsAtEnd()) {
    const char c = Peek();
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      Advance();
      continue;
    }
    break;
  }

  const std::string lexeme = source_.substr(start, pos_ - start);
  Token t;
  t.lexeme = lexeme;
  t.number_value = 0.0;
  t.loc.line = 0;
  t.loc.col = start_col;
  t.loc.length = static_cast<std::uint32_t>(lexeme.size());
  if (lexeme == "signal") t.kind = TokenKind::Signal;
  else if (lexeme == "if") t.kind = TokenKind::If;
  else if (lexeme == "then") t.kind = TokenKind::Then;
  else if (lexeme == "else") t.kind = TokenKind::Else;
  else t.kind = TokenKind::Identifier;
  return t;
}

}  // namespace jitse
