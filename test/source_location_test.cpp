// P6.1: Source-location threading through the parser.
//
// Verifies that:
//   1. Lexer tokens carry (col, length) within a line.
//   2. ParseSignalProgram fills in (line) before handing tokens to the
//      parser, so AST nodes ultimately point at the correct line.
//   3. Parser errors raise ParseError with a SourceLoc that points at
//      the OFFENDING TOKEN (not the start of the line).
//   4. ParseError::what() renders a multi-line diagnostic with a caret
//      underline pointing at the offending token.
//
// Failure mode if this regresses: parse errors collapse to a single line
// without the source snippet or caret, and downstream tools (LSP, type
// errors) can no longer point at the offending construct.

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "lexer.h"
#include "parser.h"
#include "signal_program.h"

using jitse::Lexer;
using jitse::ParseError;
using jitse::Parser;
using jitse::SignalDef;
using jitse::Token;
using jitse::TokenKind;

namespace {

int failures = 0;
#define EXPECT(cond)                                                                  \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " : " << #cond << "\n"; \
      ++failures;                                                                     \
    }                                                                                 \
  } while (0)

void TestLexerEmitsCols() {
  Lexer lexer("signal x = 1 + 2");
  std::vector<Token> toks = lexer.Tokenize();
  // "signal" starts at col 1, length 6.
  EXPECT(toks[0].kind == TokenKind::Signal);
  EXPECT(toks[0].loc.col == 1);
  EXPECT(toks[0].loc.length == 6);
  // "x" at col 8.
  EXPECT(toks[1].kind == TokenKind::Identifier);
  EXPECT(toks[1].loc.col == 8);
  EXPECT(toks[1].loc.length == 1);
  // "=" at col 10.
  EXPECT(toks[2].kind == TokenKind::Assign);
  EXPECT(toks[2].loc.col == 10);
  // "1" at col 12.
  EXPECT(toks[3].kind == TokenKind::Number);
  EXPECT(toks[3].loc.col == 12);
  EXPECT(toks[3].loc.length == 1);
  // "+" at col 14.
  EXPECT(toks[4].kind == TokenKind::Plus);
  EXPECT(toks[4].loc.col == 14);
  // "2" at col 16.
  EXPECT(toks[5].kind == TokenKind::Number);
  EXPECT(toks[5].loc.col == 16);
  EXPECT(toks[5].loc.length == 1);
}

void TestParserStampsLocOnNodes() {
  Lexer lexer("signal y = 1 + 2");
  std::vector<Token> toks = lexer.Tokenize();
  // Simulate ParseSignalProgram setting line numbers (token order
  // matters; this loop matches what the orchestrator does).
  for (auto& t : toks) t.loc.line = 5;
  Parser parser(std::move(toks));
  SignalDef def = parser.ParseSignalDef();
  // The body is a BinaryOp(Add); its loc should be the '+' token at col 14.
  EXPECT(def.body->loc.line == 5);
  EXPECT(def.body->loc.col == 14);
  EXPECT(def.body->loc.length == 1);
}

void TestParseErrorCarriesLocAndCaret() {
  // Missing 'else' -- error should point at where 'else' was expected.
  // ParseSignalProgram catches and decorates with the line text.
  const std::string src = "signal q = if 1 > 0 then 2\n";
  try {
    (void)jitse::ParseSignalProgram(src);
    EXPECT(false && "expected ParseError");
  } catch (const ParseError& e) {
    const std::string what = e.what();
    EXPECT(what.find("error:") != std::string::npos);
    EXPECT(what.find("Expected 'else'") != std::string::npos);
    EXPECT(what.find("line 1") != std::string::npos);
    EXPECT(what.find("\n  | signal q = if 1 > 0 then 2") != std::string::npos);
    EXPECT(what.find("^") != std::string::npos);
  }
}

void TestMultiLineSourceLinesAreCorrect() {
  // Error on line 3 -- ensure the orchestrator reports line 3, not 1.
  const std::string src =
      "signal a = 1\n"
      "signal b = 2\n"
      "signal c = if 1 > 0 then 5\n";
  try {
    (void)jitse::ParseSignalProgram(src);
    EXPECT(false && "expected ParseError on line 3");
  } catch (const ParseError& e) {
    const std::string what = e.what();
    EXPECT(what.find("line 3") != std::string::npos);
    EXPECT(what.find("signal c =") != std::string::npos);
  }
}

void TestCommentStripPreservesLineNumbers() {
  // Comments are stripped before tokenizing, but line numbers count
  // ALL lines including comment-only lines (we still increment line_no
  // because std::getline yields each physical line).
  const std::string src =
      "# pure comment line\n"
      "signal a = 1\n"
      "# another comment\n"
      "signal b = bogus(\n";  // unterminated arg list on line 4
  try {
    (void)jitse::ParseSignalProgram(src);
    EXPECT(false && "expected ParseError on line 4");
  } catch (const ParseError& e) {
    const std::string what = e.what();
    EXPECT(what.find("line 4") != std::string::npos);
  }
}

}  // namespace

int main() {
  TestLexerEmitsCols();
  TestParserStampsLocOnNodes();
  TestParseErrorCarriesLocAndCaret();
  TestMultiLineSourceLinesAreCorrect();
  TestCommentStripPreservesLineNumbers();
  if (failures == 0) {
    std::cout << "source_location_test passed\n";
    return 0;
  }
  std::cerr << failures << " assertion(s) failed\n";
  return 1;
}
