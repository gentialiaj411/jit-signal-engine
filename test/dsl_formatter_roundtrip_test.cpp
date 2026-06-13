// P15: round-trip and idempotency gate for the DSL formatter.
//
// For every .sig under `examples/` we assert three properties:
//
//   1. PARSE.  The source parses cleanly (every example in the
//      repo must be syntactically valid; if a new example breaks
//      this, the failing message points at the offending file).
//   2. ROUND-TRIP.  `parse(format(parse(src)))` produces a program
//      structurally-equal to `parse(src)`. We use AstEquals on
//      every signal's body (also implicitly checks signal name and
//      order match).
//   3. IDEMPOTENCY.  `format(parse(format(parse(src)))) ==
//      format(parse(src))`. The formatter's output is its own
//      fixed point -- the gofmt invariant.
//
// We also exercise the precedence path on a handful of hand-crafted
// expressions (parens-around-low-prec-inside-high-prec, conditional
// nested in arithmetic, etc.) to gate that the formatter doesn't
// silently reassociate.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ast_clone.h"
#include "dsl_formatter.h"
#include "parser.h"
#include "signal_program.h"

namespace {

int failures = 0;
#define EXPECT(cond)                                                                  \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " : " << #cond << "\n"; \
      ++failures;                                                                     \
    }                                                                                 \
  } while (0)

std::string ReadFile(const std::filesystem::path& p) {
  std::ifstream f(p);
  std::ostringstream out;
  out << f.rdbuf();
  return out.str();
}

bool ProgramsStructurallyEqual(const jitse::ProgramDef& a,
                               const jitse::ProgramDef& b) {
  if (a.params.size() != b.params.size()) return false;
  for (std::size_t i = 0; i < a.params.size(); ++i) {
    if (a.params[i].name != b.params[i].name) return false;
    if (std::fabs(a.params[i].default_value - b.params[i].default_value) > 1e-12) return false;
  }
  if (a.signals.size() != b.signals.size()) return false;
  for (std::size_t i = 0; i < a.signals.size(); ++i) {
    if (a.signals[i].name != b.signals[i].name) return false;
    if (a.signals[i].body && b.signals[i].body) {
      if (!jitse::AstEquals(*a.signals[i].body, *b.signals[i].body)) return false;
    } else if (a.signals[i].body != nullptr || b.signals[i].body != nullptr) {
      return false;
    }
  }
  return true;
}

void CheckRoundTrip(const std::string& name, const std::string& src) {
  jitse::ProgramDef parsed1;
  try {
    parsed1 = jitse::ParseProgram(src);
  } catch (const std::exception& e) {
    std::cerr << "  parse(src) failed for " << name << ": " << e.what() << "\n";
    ++failures;
    return;
  }
  const std::string fmt1 = jitse::FormatProgram(parsed1);

  jitse::ProgramDef parsed2;
  try {
    parsed2 = jitse::ParseProgram(fmt1);
  } catch (const std::exception& e) {
    std::cerr << "  parse(format(parse(src))) failed for " << name << ": " << e.what() << "\n";
    std::cerr << "  formatted output was:\n" << fmt1 << "\n";
    ++failures;
    return;
  }
  if (!ProgramsStructurallyEqual(parsed1, parsed2)) {
    std::cerr << "  round-trip mismatch for " << name << "\n";
    std::cerr << "  original src:\n" << src << "\n";
    std::cerr << "  formatted:\n" << fmt1 << "\n";
    ++failures;
    return;
  }

  // Idempotency: fmt(fmt(x)) == fmt(x).
  const std::string fmt2 = jitse::FormatProgram(parsed2);
  if (fmt2 != fmt1) {
    std::cerr << "  formatter is NOT idempotent for " << name << "\n";
    std::cerr << "  fmt1:\n" << fmt1 << "\n";
    std::cerr << "  fmt2:\n" << fmt2 << "\n";
    ++failures;
  }
}

}  // namespace

int main() {
  // Find the examples directory relative to the build's working dir.
  // ctest runs from `build-wsl`; the source tree is one level up
  // under `examples/`. Honor an env var override for non-default
  // layouts.
  const char* override_dir = std::getenv("JITSE_EXAMPLES_DIR");
  std::filesystem::path dir;
  if (override_dir != nullptr) {
    dir = override_dir;
  } else {
    const std::filesystem::path candidates[] = {
        "../examples",
        "examples",
        "../../examples",
    };
    for (const auto& c : candidates) {
      if (std::filesystem::is_directory(c)) {
        dir = c;
        break;
      }
    }
  }
  if (dir.empty() || !std::filesystem::is_directory(dir)) {
    std::cerr << "could not locate examples/ directory; set JITSE_EXAMPLES_DIR\n";
    return 1;
  }
  std::cout << "examples dir: " << dir << "\n";

  std::vector<std::filesystem::path> sigs;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".sig") sigs.push_back(entry.path());
  }
  std::sort(sigs.begin(), sigs.end());
  EXPECT(!sigs.empty());

  for (const auto& p : sigs) {
    std::cout << "checking " << p.filename().string() << "\n";
    CheckRoundTrip(p.filename().string(), ReadFile(p));
  }

  // Hand-crafted programs that exercise the precedence-paren logic.
  // If any of these formats wrong, downstream the reparse would
  // misassociate and `ProgramsStructurallyEqual` would catch it.
  CheckRoundTrip("precedence_arith",
                 "signal s = 1 + 2 * 3 - 4 / 5\n");
  CheckRoundTrip("precedence_relational",
                 "signal s = if mid(AAPL) > 100 then 1 else 0\n");
  CheckRoundTrip("precedence_logical",
                 "signal a = mid(AAPL) - mid(MSFT)\n"
                 "signal b = mid(AAPL) + mid(MSFT)\n"
                 "signal s = if a > 0 && b > 0 || a < -1 then a / b else 0\n");
  CheckRoundTrip("nested_conditional",
                 "signal s = if mid(AAPL) > 0 then if mid(MSFT) > 0 then 1 else 2 else 3\n");
  CheckRoundTrip("unary_chain",
                 "signal s = -(-(-mid(AAPL)))\n");
  CheckRoundTrip("stateful_inside_arith",
                 "signal s = ema(mid(AAPL), 10) - sma(mid(AAPL), 30) + 1.5\n");
  CheckRoundTrip("constant_fold_inputs",
                 "signal s = sma(mid(AAPL), 5 + 5)\n");

  if (failures > 0) {
    std::cerr << "dsl_formatter_roundtrip_test: " << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "dsl_formatter_roundtrip_test: PASSED\n";
  return 0;
}
