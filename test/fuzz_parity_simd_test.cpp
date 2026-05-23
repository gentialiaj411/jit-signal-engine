#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "ast.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

using jitse::BinaryOp;
using jitse::BinaryOpKind;
using jitse::Conditional;
using jitse::Expr;
using jitse::FunctionCall;
using jitse::IdentifierExpr;
using jitse::NumberLiteral;

void SetForceDisableAvx2(bool disable) {
#ifdef _WIN32
  _putenv_s("JITSE_FORCE_DISABLE_AVX2", disable ? "1" : "");
#else
  if (disable) {
    setenv("JITSE_FORCE_DISABLE_AVX2", "1", 1);
  } else {
    unsetenv("JITSE_FORCE_DISABLE_AVX2");
  }
#endif
}

std::unique_ptr<Expr> MidExpr(const std::string& ticker) {
  std::vector<std::unique_ptr<Expr>> args;
  args.push_back(std::make_unique<IdentifierExpr>(ticker));
  return std::make_unique<FunctionCall>("mid", std::move(args));
}

std::unique_ptr<Expr> GenExpr(std::mt19937& rng, int depth, const std::string& ticker) {
  std::uniform_real_distribution<double> prob(0.0, 1.0);
  const bool choose_terminal = (depth == 0) || (prob(rng) < 0.40);

  std::unique_ptr<Expr> expr;
  if (choose_terminal) {
    std::uniform_int_distribution<int> terminal_pick(0, 6);
    const int pick = terminal_pick(rng);
    switch (pick) {
      case 0: {
        static const double vals[] = {0.1, 0.5, 1.0, 2.0, 5.0, 10.0};
        std::uniform_int_distribution<int> d(0, 5);
        expr = std::make_unique<NumberLiteral>(vals[d(rng)]);
        break;
      }
      case 1:
        expr = MidExpr(ticker);
        break;
      case 2:
      case 3:
      case 4:
      case 5:
      case 6: {
        static const int periods[] = {3, 5, 8, 10, 16};
        std::uniform_int_distribution<int> p(0, 4);
        const double period = static_cast<double>(periods[p(rng)]);
        std::vector<std::unique_ptr<Expr>> args;
        args.push_back(MidExpr(ticker));
        args.push_back(std::make_unique<NumberLiteral>(period));
        const char* name = "sma";
        if (pick == 3) name = "ema";
        if (pick == 4) name = "rolling_std";
        if (pick == 5) name = "zscore";
        if (pick == 6) name = "lag";
        expr = std::make_unique<FunctionCall>(name, std::move(args));
        break;
      }
    }
  } else {
    static const BinaryOpKind kinds[] = {
        BinaryOpKind::Add, BinaryOpKind::Sub, BinaryOpKind::Mul, BinaryOpKind::Div,
        BinaryOpKind::Gt,  BinaryOpKind::Lt,  BinaryOpKind::And, BinaryOpKind::Or,
    };
    std::uniform_int_distribution<int> k(0, 7);
    expr = std::make_unique<BinaryOp>(
        kinds[k(rng)], GenExpr(rng, depth - 1, ticker), GenExpr(rng, depth - 1, ticker));
  }

  if (depth > 0 && prob(rng) < 0.20) {
    expr =
        std::make_unique<Conditional>(GenExpr(rng, 0, ticker), GenExpr(rng, depth - 1, ticker), GenExpr(rng, depth - 1, ticker));
  }

  return expr;
}

}  // namespace

int main() {
  const int kNumSignals = 200;
  const int kTicksPerSignal = 50;
  constexpr double kTol = 1e-8;
  std::mt19937 rng(424242);

  for (int s = 0; s < kNumSignals; ++s) {
    auto body = GenExpr(rng, 3, "AAPL");
    jitse::SignalDef signal{"simd_fuzz_" + std::to_string(s), std::move(body)};
    jitse::AllocateNodeIds(signal);

    jitse::SymbolTable symbols;
    symbols.RegisterOrGetId("AAPL");

    SetForceDisableAvx2(true);
    jitse::JitCompiler scalar_jit;
    const bool scalar_ok = scalar_jit.IsAvailable() && scalar_jit.Compile(signal, symbols);
    auto scalar_fn = scalar_ok ? scalar_jit.GetFunction() : nullptr;
    if (!scalar_fn) {
      std::fprintf(stderr, "SCALAR JIT unavailable\n");
      return 1;
    }

    SetForceDisableAvx2(false);
    jitse::JitCompiler simd_jit;
    if (!simd_jit.IsAvailable()) {
      std::printf("simd parity skipped: jit unavailable\n");
      return 0;
    }
    if (!simd_jit.HasAVX2()) {
      std::printf("simd parity skipped: avx2 unavailable\n");
      return 0;
    }
    const bool simd_ok = simd_jit.Compile(signal, symbols);
    auto simd_fn = simd_ok ? simd_jit.GetFunction() : nullptr;
    if (!simd_fn) {
      std::fprintf(stderr, "SIMD JIT compile failed\n");
      return 1;
    }

    jitse::SignalContext scalar_ctx;
    jitse::SignalContext simd_ctx;
    jitse::PrewarmSignalContext(scalar_ctx, signal);
    jitse::PrewarmSignalContext(simd_ctx, signal);

    jitse::MarketState market;
    jitse::MarketSimulator sim(static_cast<std::uint64_t>(s), 1);
    for (int t = 0; t < kTicksPerSignal; ++t) {
      const auto ev = sim.NextEvent(1000);
      market.instruments[0].bid = ev.bid;
      market.instruments[0].ask = ev.ask;
      market.current_time_ns = ev.timestamp_ns;

      const double scalar_out = scalar_fn(&market, &scalar_ctx);
      const double simd_out = simd_fn(&market, &simd_ctx);
      const bool both_nan = std::isnan(scalar_out) && std::isnan(simd_out);
      const bool both_inf = std::isinf(scalar_out) && std::isinf(simd_out) && ((scalar_out > 0) == (simd_out > 0));
      const bool equal = std::fabs(scalar_out - simd_out) < kTol;
      if (!both_nan && !both_inf && !equal) {
        std::fprintf(
            stderr,
            "SIMD PARITY FAIL: signal=%d tick=%d scalar=%.15g simd=%.15g\n",
            s,
            t,
            scalar_out,
            simd_out);
        return 1;
      }
    }
  }

  std::printf("fuzz_parity_simd: %d signals x %d ticks OK\n", kNumSignals, kTicksPerSignal);
  return 0;
}
