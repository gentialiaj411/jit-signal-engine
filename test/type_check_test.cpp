// P6.3: minimal type system.
//
// The DSL has two static types: Number and Bool. This test verifies:
//   - Legal programs that mixed types correctly still parse and type-check.
//   - Illegal programs (if 1 then ..., (a > b) + c, vol && 0) are
//     rejected at parse time with a ParseError that carries a source
//     location.
//
// The test runs entirely through ParseSignalProgram, which is the only
// entry point that invokes the type checker (fuzz tests that build AST
// directly are intentionally exempt; the type checker is for source
// programs).

#include <cassert>
#include <iostream>
#include <string>

#include "parser.h"
#include "signal_program.h"
#include "type_check.h"

using jitse::ParseError;

namespace {

int failures = 0;
#define EXPECT(cond)                                                                  \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " : " << #cond << "\n"; \
      ++failures;                                                                     \
    }                                                                                 \
  } while (0)

void ExpectOk(const std::string& src) {
  try {
    (void)jitse::ParseSignalProgram(src);
  } catch (const std::exception& e) {
    std::cerr << "FAIL: expected OK but got: " << e.what() << "\n";
    std::cerr << "    src=" << src << "\n";
    ++failures;
  }
}

void ExpectTypeError(const std::string& src, const std::string& expected_substr) {
  try {
    (void)jitse::ParseSignalProgram(src);
    std::cerr << "FAIL: expected type error for: " << src << "\n";
    ++failures;
  } catch (const ParseError& e) {
    const std::string what = e.what();
    if (what.find("type error") == std::string::npos) {
      std::cerr << "FAIL: error wasn't a type error: " << what << "\n";
      ++failures;
      return;
    }
    if (what.find(expected_substr) == std::string::npos) {
      std::cerr << "FAIL: error didn't mention `" << expected_substr << "`: " << what << "\n";
      ++failures;
      return;
    }
  }
}

}  // namespace

int main() {
  // Legal: well-typed conditional.
  ExpectOk("signal s = if mid(AAPL) > 0 then 1 else 0\n");
  // Legal: nested comparisons inside &&.
  ExpectOk("signal s = if (mid(AAPL) > 0) && (mid(MSFT) < 100) then 1 else 0\n");
  // Legal: arithmetic.
  ExpectOk("signal s = mid(AAPL) + mid(MSFT) * 0.5\n");
  // Legal: a function-call result is a Number used in arithmetic.
  ExpectOk("signal s = sma(mid(AAPL), 10) - mid(AAPL)\n");

  // Illegal: bare number as if-condition.
  ExpectTypeError("signal s = if 1 then 2 else 3\n", "if-condition");
  // Illegal: arithmetic on a bool result.
  ExpectTypeError("signal s = (mid(AAPL) > 0) + 1\n", "arithmetic operator");
  // Illegal: logical operator on numbers.
  ExpectTypeError("signal s = if mid(AAPL) && 0 then 1 else 0\n", "logical");
  // Illegal: comparing a comparison's result.
  ExpectTypeError("signal s = if (mid(AAPL) > 0) == 1 then 1 else 0\n",
                  "comparison operator");
  // Illegal: signal whose body has type Bool. The whole-body rule
  // requires Number, so `signal s = mid(A) > 0` is rejected.
  ExpectTypeError("signal s = mid(AAPL) > 0\n", "must be `number`");

  if (failures == 0) {
    std::cout << "type_check_test passed\n";
    return 0;
  }
  std::cerr << failures << " assertion(s) failed\n";
  return 1;
}
