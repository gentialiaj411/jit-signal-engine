#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "lexer.h"

using jitse::Lexer;
using jitse::Token;
using jitse::TokenKind;

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-12; }

int main() {
  {
    const std::string src = "signal x = 1 + 2.5 * mid(AAPL)";
    Lexer lexer(src);
    std::vector<Token> tokens = lexer.Tokenize();
    if (tokens.size() != 12) {
      std::cerr << "token count mismatch: " << tokens.size() << "\n";
      return 1;
    }
    if (tokens[0].kind != TokenKind::Signal) return 2;
    if (!(tokens[1].kind == TokenKind::Identifier && tokens[1].lexeme == "x")) return 3;
    if (tokens[2].kind != TokenKind::Assign) return 4;
    if (!(tokens[3].kind == TokenKind::Number && Near(tokens[3].number_value, 1.0))) return 5;
    if (tokens[4].kind != TokenKind::Plus) return 6;
    if (!(tokens[5].kind == TokenKind::Number && Near(tokens[5].number_value, 2.5))) return 7;
    if (tokens[6].kind != TokenKind::Star) return 8;
    if (!(tokens[7].kind == TokenKind::Identifier && tokens[7].lexeme == "mid")) return 9;
    if (tokens[8].kind != TokenKind::LParen) return 10;
    if (!(tokens[9].kind == TokenKind::Identifier && tokens[9].lexeme == "AAPL")) return 11;
    if (tokens[10].kind != TokenKind::RParen) return 12;
    if (tokens[11].kind != TokenKind::EndOfFile) return 13;
  }

  {
    const std::string src = "signal y = -.5";
    Lexer lexer(src);
    std::vector<Token> tokens = lexer.Tokenize();
    if (tokens.size() < 6) return 14;
    if (tokens[3].kind != TokenKind::Minus) return 15;
    if (tokens[4].kind != TokenKind::Number) return 16;
    if (!Near(tokens[4].number_value, 0.5)) return 17;
  }

  {
    const std::string src = "signal z = if 3 >= 2 then 1 else 0";
    Lexer lexer(src);
    std::vector<Token> tokens = lexer.Tokenize();
    bool saw_if = false;
    bool saw_then = false;
    bool saw_else = false;
    bool saw_gte = false;
    for (const auto& t : tokens) {
      if (t.kind == TokenKind::If) saw_if = true;
      if (t.kind == TokenKind::Then) saw_then = true;
      if (t.kind == TokenKind::Else) saw_else = true;
      if (t.kind == TokenKind::Gte) saw_gte = true;
    }
    if (!(saw_if && saw_then && saw_else && saw_gte)) return 18;
  }
  {
    const std::string src = "signal q = a && b || c != d";
    Lexer lexer(src);
    std::vector<Token> tokens = lexer.Tokenize();
    bool saw_and = false;
    bool saw_or = false;
    bool saw_neq = false;
    for (const auto& t : tokens) {
      if (t.kind == TokenKind::And) saw_and = true;
      if (t.kind == TokenKind::Or) saw_or = true;
      if (t.kind == TokenKind::NotEq) saw_neq = true;
    }
    if (!(saw_and && saw_or && saw_neq)) return 19;
  }

  return 0;
}
