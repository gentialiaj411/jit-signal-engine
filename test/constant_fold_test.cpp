// P6.2: AST-level constant folding.
//
// Two-layer test:
//   1. Pure AST test: FoldConstants(2 + 3 * 4) collapses to a single
//      NumberLiteral(14). Also verifies comparison + logical folding,
//      conditional folding on a literal condition, and that FunctionCall
//      itself is never folded (only its args).
//   2. IR-emission test: ParseSignalProgram("signal y = sma(mid(AAPL), 5+5)")
//      and ParseSignalProgram("signal y = sma(mid(AAPL), 10)") produce
//      IDENTICAL pre-opt IR.
//
// The IR-equality check is the "real compiler" claim: a user-facing
// folding pass must guarantee the JIT cannot tell the difference.

#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include "ast_utils.h"
#include "constant_fold.h"
#include "jit_compiler.h"
#include "lexer.h"
#include "parser.h"
#include "runtime.h"
#include "signal_program.h"

using jitse::BinaryOp;
using jitse::BinaryOpKind;
using jitse::Conditional;
using jitse::Expr;
using jitse::FoldConstants;
using jitse::FunctionCall;
using jitse::NumberLiteral;
using jitse::UnaryOp;
using jitse::UnaryOpKind;

namespace {

int failures = 0;
#define EXPECT(cond)                                                                  \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " : " << #cond << "\n"; \
      ++failures;                                                                     \
    }                                                                                 \
  } while (0)

#define EXPECT_NEAR(a, b)                                                                            \
  do {                                                                                               \
    const double aa = (a);                                                                           \
    const double bb = (b);                                                                           \
    if (std::fabs(aa - bb) > 1e-12) {                                                                \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " : " << aa << " != " << bb << "\n";   \
      ++failures;                                                                                    \
    }                                                                                                \
  } while (0)

std::unique_ptr<Expr> Num(double v) { return std::make_unique<NumberLiteral>(v); }

void TestArithmeticFolds() {
  // 2 + 3 * 4 = 14.
  auto e = std::make_unique<BinaryOp>(
      BinaryOpKind::Add, Num(2),
      std::make_unique<BinaryOp>(BinaryOpKind::Mul, Num(3), Num(4)));
  auto folded = FoldConstants(std::move(e));
  const auto* n = dynamic_cast<const NumberLiteral*>(folded.get());
  EXPECT(n != nullptr);
  if (n) EXPECT_NEAR(n->value, 14.0);
}

void TestUnaryFolds() {
  // -(5) = -5.
  auto e = std::make_unique<UnaryOp>(UnaryOpKind::Minus, Num(5));
  auto folded = FoldConstants(std::move(e));
  const auto* n = dynamic_cast<const NumberLiteral*>(folded.get());
  EXPECT(n != nullptr);
  if (n) EXPECT_NEAR(n->value, -5.0);
}

void TestComparisonFolds() {
  // (3 > 2) -> 1.0.
  auto e = std::make_unique<BinaryOp>(BinaryOpKind::Gt, Num(3), Num(2));
  auto folded = FoldConstants(std::move(e));
  const auto* n = dynamic_cast<const NumberLiteral*>(folded.get());
  EXPECT(n != nullptr);
  if (n) EXPECT_NEAR(n->value, 1.0);

  // (3 < 2) -> 0.0.
  auto e2 = std::make_unique<BinaryOp>(BinaryOpKind::Lt, Num(3), Num(2));
  auto folded2 = FoldConstants(std::move(e2));
  const auto* n2 = dynamic_cast<const NumberLiteral*>(folded2.get());
  EXPECT(n2 != nullptr);
  if (n2) EXPECT_NEAR(n2->value, 0.0);
}

void TestConditionalFoldsOnLiteralCondition() {
  // if 1.0 then 7 else 99 -> 7
  auto e = std::make_unique<Conditional>(Num(1), Num(7), Num(99));
  auto folded = FoldConstants(std::move(e));
  const auto* n = dynamic_cast<const NumberLiteral*>(folded.get());
  EXPECT(n != nullptr);
  if (n) EXPECT_NEAR(n->value, 7.0);
  // if 0.0 then 7 else 99 -> 99
  auto e2 = std::make_unique<Conditional>(Num(0), Num(7), Num(99));
  auto folded2 = FoldConstants(std::move(e2));
  const auto* n2 = dynamic_cast<const NumberLiteral*>(folded2.get());
  EXPECT(n2 != nullptr);
  if (n2) EXPECT_NEAR(n2->value, 99.0);
}

void TestFunctionCallNotFoldedButArgsAre() {
  // We can't construct a real FunctionCall with state here, but we can
  // build one whose 2nd arg is `5 + 5` and verify only the arg folds.
  std::vector<std::unique_ptr<Expr>> args;
  args.push_back(std::make_unique<jitse::IdentifierExpr>("AAPL"));
  args.push_back(std::make_unique<BinaryOp>(BinaryOpKind::Add, Num(5), Num(5)));
  auto e = std::make_unique<FunctionCall>("sma", std::move(args));
  auto folded = FoldConstants(std::move(e));
  const auto* fn = dynamic_cast<const FunctionCall*>(folded.get());
  EXPECT(fn != nullptr);
  if (!fn) return;
  EXPECT(fn->name == "sma");
  EXPECT(fn->args.size() == 2);
  const auto* arg2 = dynamic_cast<const NumberLiteral*>(fn->args[1].get());
  EXPECT(arg2 != nullptr);
  if (arg2) EXPECT_NEAR(arg2->value, 10.0);
}

// The IR-equality test. Compiles two programs that differ only in
// whether the lookback was written `5 + 5` or `10`. The folded version
// is what ParseSignalProgram returns (it folds inside the parser). The
// non-folded baseline is constructed by writing `10` directly in source.
// They must produce identical pre-opt IR -- if they don't, folding
// either isn't running, isn't reaching this site, or is leaving residual
// nodes.
bool TestIREqualityOfFoldedExpression() {
  jitse::JitCompiler jit_fold, jit_baseline;
  if (!jit_fold.IsAvailable() || !jit_baseline.IsAvailable()) {
    std::cout << "  [skip] IR-equality: LLVM unavailable\n";
    return true;
  }
  jit_fold.SetStatefulLowering(jitse::StatefulLoweringFlags::kAll);
  jit_baseline.SetStatefulLowering(jitse::StatefulLoweringFlags::kAll);

  jitse::SymbolTable symbols;
  symbols.RegisterOrGetId("AAPL");

  // Folded path: parse "5 + 5" through ParseSignalProgram (which folds).
  auto signals_fold = jitse::ParseSignalProgram("signal y = sma(mid(AAPL), 5 + 5)\n");
  auto signals_base = jitse::ParseSignalProgram("signal y = sma(mid(AAPL), 10)\n");
  EXPECT(signals_fold.size() == 1);
  EXPECT(signals_base.size() == 1);
  for (auto& s : signals_fold) {
    jitse::BindSymbolIds(s, symbols);
  }
  for (auto& s : signals_base) {
    jitse::BindSymbolIds(s, symbols);
  }
  jitse::AllocateProgramNodeIds(signals_fold);
  jitse::AllocateProgramNodeIds(signals_base);

  if (!jit_fold.CompileProgram(signals_fold, symbols)) {
    std::cerr << "  [fail] compile folded: " << jit_fold.LastError() << "\n";
    return false;
  }
  if (!jit_baseline.CompileProgram(signals_base, symbols)) {
    std::cerr << "  [fail] compile baseline: " << jit_baseline.LastError() << "\n";
    return false;
  }

  const std::string& ir_fold = jit_fold.LastIRPreOpt();
  const std::string& ir_base = jit_baseline.LastIRPreOpt();
  if (ir_fold == ir_base) {
    std::cout << "  IR-equality: PASS (both produce identical pre-opt IR)\n";
    return true;
  }
  std::cerr << "  IR-equality: FAIL -- folded and baseline IR differ\n";
  std::cerr << "  -- folded IR --\n" << ir_fold << "\n";
  std::cerr << "  -- baseline IR --\n" << ir_base << "\n";
  return false;
}

}  // namespace

int main() {
  TestArithmeticFolds();
  TestUnaryFolds();
  TestComparisonFolds();
  TestConditionalFoldsOnLiteralCondition();
  TestFunctionCallNotFoldedButArgsAre();
  const bool ir_ok = TestIREqualityOfFoldedExpression();
  if (!ir_ok) ++failures;

  if (failures == 0) {
    std::cout << "constant_fold_test passed\n";
    return 0;
  }
  std::cerr << failures << " assertion(s) failed\n";
  return 1;
}
