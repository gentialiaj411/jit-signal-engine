#include "lexer.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace jitse {

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

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

    Advance();
    switch (c) {
      case '=':
        if (!IsAtEnd() && Peek() == '=') {
          Advance();
          out.push_back({TokenKind::Eq, "==", 0.0});
        } else {
          out.push_back({TokenKind::Assign, "=", 0.0});
        }
        break;
      case '!':
        if (!IsAtEnd() && Peek() == '=') {
          Advance();
          out.push_back({TokenKind::NotEq, "!=", 0.0});
        } else {
          throw std::runtime_error("Unexpected '!' - did you mean '!='?");
        }
        break;
      case '>':
        if (!IsAtEnd() && Peek() == '=') {
          Advance();
          out.push_back({TokenKind::Gte, ">=", 0.0});
        } else {
          out.push_back({TokenKind::Gt, ">", 0.0});
        }
        break;
      case '&':
        if (!IsAtEnd() && Peek() == '&') {
          Advance();
          out.push_back({TokenKind::And, "&&", 0.0});
        } else {
          throw std::runtime_error("Unexpected '&' - did you mean '&&'?");
        }
        break;
      case '|':
        if (!IsAtEnd() && Peek() == '|') {
          Advance();
          out.push_back({TokenKind::Or, "||", 0.0});
        } else {
          throw std::runtime_error("Unexpected '|' - did you mean '||'?");
        }
        break;
      case '<':
        if (!IsAtEnd() && Peek() == '=') {
          Advance();
          out.push_back({TokenKind::Lte, "<=", 0.0});
        } else {
          out.push_back({TokenKind::Lt, "<", 0.0});
        }
        break;
      case '+':
        out.push_back({TokenKind::Plus, "+", 0.0});
        break;
      case '-':
        out.push_back({TokenKind::Minus, "-", 0.0});
        break;
      case '*':
        out.push_back({TokenKind::Star, "*", 0.0});
        break;
      case '/':
        out.push_back({TokenKind::Slash, "/", 0.0});
        break;
      case '(':
        out.push_back({TokenKind::LParen, "(", 0.0});
        break;
      case ')':
        out.push_back({TokenKind::RParen, ")", 0.0});
        break;
      case ',':
        out.push_back({TokenKind::Comma, ",", 0.0});
        break;
      default:
        throw std::runtime_error("Unexpected character in lexer");
    }
  }

  out.push_back({TokenKind::EndOfFile, "", 0.0});
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
  return {TokenKind::Number, lexeme, std::strtod(lexeme.c_str(), nullptr)};
}

Token Lexer::ReadIdentifierOrKeyword() {
  const std::size_t start = pos_;
  while (!IsAtEnd()) {
    const char c = Peek();
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      Advance();
      continue;
    }
    break;
  }

  const std::string lexeme = source_.substr(start, pos_ - start);
  if (lexeme == "signal") {
    return {TokenKind::Signal, lexeme, 0.0};
  }
  if (lexeme == "if") {
    return {TokenKind::If, lexeme, 0.0};
  }
  if (lexeme == "then") {
    return {TokenKind::Then, lexeme, 0.0};
  }
  if (lexeme == "else") {
    return {TokenKind::Else, lexeme, 0.0};
  }
  return {TokenKind::Identifier, lexeme, 0.0};
}

}  // namespace jitse
