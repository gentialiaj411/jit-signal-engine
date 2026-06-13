// jitse_lint -- run the full frontend on a DSL source file and report
// any issues with caret-style diagnostics (P15).
//
//   jitse_lint <file>          # exit 0 on clean, 1 on findings
//   jitse_lint -               # read from stdin
//   jitse_lint --quiet <file>  # only set exit status, suppress output
//
// What this checks (in order, fail-fast):
//   1. Lex + parse  -- syntax errors with caret span
//   2. Type-check   -- enforces the P6.3 Number/Bool discipline
//   3. Constant fold + AllocateNodeIds + Inline -- catches
//      cycle-in-signal-graph, duplicate signal names, and undefined
//      references (any reference that isn't another signal in the
//      program OR a known runtime function).
//   4. Symbol resolution -- catches `mid(NotATicker)` style refs.
//
// Successful exit means the program would compile cleanly through to
// the JIT. This is the same chain `jit_signal_engine` runs.

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "parser.h"
#include "runtime.h"  // SymbolTable
#include "signal_program.h"

namespace {

struct Args {
  std::string path;
  bool stdin_input = false;
  bool quiet = false;
};

Args ParseArgs(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    if (s == "--quiet") a.quiet = true;
    else if (s == "-h" || s == "--help") {
      std::cout << "Usage: jitse_lint [--quiet] <file|->\n";
      std::exit(0);
    }
    else if (s == "-") { a.stdin_input = true; a.path = "<stdin>"; }
    else if (a.path.empty()) a.path = s;
    else throw std::runtime_error("unexpected extra argument: " + s);
  }
  if (a.path.empty()) {
    throw std::runtime_error("usage: jitse_lint [--quiet] <file|->");
  }
  return a;
}

std::string ReadAll(const Args& args) {
  std::ostringstream out;
  if (args.stdin_input) {
    out << std::cin.rdbuf();
  } else {
    std::ifstream f(args.path);
    if (!f) throw std::runtime_error("cannot open: " + args.path);
    out << f.rdbuf();
  }
  return out.str();
}

}  // namespace

int main(int argc, char** argv) try {
  const Args args = ParseArgs(argc, argv);
  const std::string src = ReadAll(args);

  // Stage 1+2: parse, type-check, constant-fold. The parser raises
  // a ParseError with a SourceLoc + the original line text for
  // type errors too (TypeCheckSignal throws ParseError under P6.3
  // -- see `signal_program.cpp` and `docs/dsl_real_language.md`).
  // The error text already includes the "error: ... at line N,
  // col C" caret line so we just print `what()` and bail.
  jitse::ProgramDef parsed;
  try {
    parsed = jitse::ParseProgram(src);
  } catch (const jitse::ParseError& e) {
    if (!args.quiet) std::cerr << args.path << ":" << e.what() << "\n";
    return 1;
  }

  // Stage 3: inline. Catches signal-graph cycles and duplicate
  // signal names. (Inlining itself produces structurally larger
  // trees; for lint we only need the side-effects, namely the
  // dependency validation. We throw away the result.)
  jitse::ProgramDef inlined;
  try {
    inlined = jitse::InlineSignalDependencies(parsed);
  } catch (const std::exception& e) {
    if (!args.quiet) std::cerr << args.path << ": " << e.what() << "\n";
    return 1;
  }

  // Stage 4: node-id + symbol-id allocation. Allocate node-ids for
  // stateful ops AND bind symbol-ids for market-data ticker
  // arguments. The latter catches `mid(NotATicker)` even when
  // NotATicker has never been mentioned anywhere else.
  try {
    jitse::AllocateProgramNodeIds(inlined.signals);
    jitse::SymbolTable symbols;
    for (const auto& s : inlined.signals) {
      for (const auto& t : jitse::CollectTickerSymbols(s)) {
        symbols.RegisterOrGetId(t);
      }
    }
    for (auto& s : inlined.signals) jitse::BindSymbolIds(s, symbols);
  } catch (const std::exception& e) {
    if (!args.quiet) std::cerr << args.path << ": " << e.what() << "\n";
    return 1;
  }

  if (!args.quiet) {
    std::cout << args.path << ": clean (" << parsed.signals.size() << " signal"
              << (parsed.signals.size() == 1 ? "" : "s") << ", "
              << parsed.params.size() << " param"
              << (parsed.params.size() == 1 ? "" : "s") << ")\n";
  }
  return 0;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << "\n";
  return 2;
}
