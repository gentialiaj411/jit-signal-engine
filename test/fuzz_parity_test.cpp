#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "ast.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

constexpr double kAbsTol = 1e-9;
constexpr double kRelTol = 1e-5;

bool ValuesMatch(double interp_out, double jit_out) {
  const bool both_nan = std::isnan(interp_out) && std::isnan(jit_out);
  const bool both_inf = std::isinf(interp_out) && std::isinf(jit_out) &&
                        ((interp_out > 0) == (jit_out > 0));
  if (both_nan || both_inf) return true;
  const double scale = std::max(std::fabs(interp_out), std::fabs(jit_out));
  const double tol = std::max(kAbsTol, kRelTol * scale);
  return std::fabs(interp_out - jit_out) <= tol;
}

using jitse::BinaryOp;
using jitse::BinaryOpKind;
using jitse::Conditional;
using jitse::Expr;
using jitse::FunctionCall;
using jitse::IdentifierExpr;
using jitse::NumberLiteral;
using jitse::ParameterExpr;

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
    std::uniform_int_distribution<int> terminal_pick(0, 12);
    const int pick = terminal_pick(rng);
    switch (pick) {
      case 0: {
        static const double vals[] = {0.1, 0.5, 1.0, 2.0, 5.0, 10.0};
        std::uniform_int_distribution<int> d(0, 5);
        expr = std::make_unique<NumberLiteral>(vals[d(rng)]);
        break;
      }
      case 1: {
        expr = MidExpr(ticker);
        break;
      }
      case 2:
      case 3:
      case 4:
      case 5:
      case 6:
      case 7:
      case 8: {
        static const int periods[] = {3, 5, 10};
        std::uniform_int_distribution<int> p(0, 2);
        const double period = static_cast<double>(periods[p(rng)]);
        if (pick == 8) {
          std::vector<std::unique_ptr<Expr>> args;
          args.push_back(std::make_unique<IdentifierExpr>(ticker));
          args.push_back(std::make_unique<NumberLiteral>(period));
          expr = std::make_unique<FunctionCall>("vwap", std::move(args));
        } else {
          std::vector<std::unique_ptr<Expr>> args;
          args.push_back(MidExpr(ticker));
          args.push_back(std::make_unique<NumberLiteral>(period));
          const char* name = "ema";
          if (pick == 3) name = "sma";
          if (pick == 4) name = "lag";
          if (pick == 5) name = "rolling_std";
          if (pick == 6) name = "rolling_min";
          if (pick == 7) name = "rolling_max";
          expr = std::make_unique<FunctionCall>(name, std::move(args));
        }
        break;
      }
      case 9: {
        static const int periods[] = {3, 5, 10};
        std::uniform_int_distribution<int> p(0, 2);
        std::vector<std::unique_ptr<Expr>> args;
        args.push_back(MidExpr(ticker));
        args.push_back(std::make_unique<NumberLiteral>(static_cast<double>(periods[p(rng)])));
        expr = std::make_unique<FunctionCall>("zscore", std::move(args));
        break;
      }
      case 10:
      case 11: {
        std::unique_ptr<Expr> rhs = GenExpr(rng, 0, ticker);
        // Guard against unbounded recursion when depth-0 randomly re-picks cross_*.
        if (dynamic_cast<FunctionCall*>(rhs.get()) != nullptr) {
          auto* fn = static_cast<FunctionCall*>(rhs.get());
          if (fn->name == "cross_above" || fn->name == "cross_below") {
            rhs = std::make_unique<NumberLiteral>(1.0);
          }
        }
        std::vector<std::unique_ptr<Expr>> args;
        args.push_back(MidExpr(ticker));
        args.push_back(std::move(rhs));
        const char* name = (pick == 10) ? "cross_above" : "cross_below";
        expr = std::make_unique<FunctionCall>(name, std::move(args));
        break;
      }
      case 12: {
        expr = std::make_unique<ParameterExpr>("alpha", 0);
        break;
      }
    }
  } else {
    static const BinaryOpKind kinds[] = {
        BinaryOpKind::Add, BinaryOpKind::Sub, BinaryOpKind::Mul, BinaryOpKind::Div,
        BinaryOpKind::Gt,  BinaryOpKind::Lt,  BinaryOpKind::And, BinaryOpKind::Or,
    };
    std::uniform_int_distribution<int> k(0, 7);
    expr = std::make_unique<BinaryOp>(kinds[k(rng)], GenExpr(rng, depth - 1, ticker), GenExpr(rng, depth - 1, ticker));
  }

  if (depth > 0 && prob(rng) < 0.20) {
    expr = std::make_unique<Conditional>(GenExpr(rng, 0, ticker), GenExpr(rng, depth - 1, ticker), GenExpr(rng, depth - 1, ticker));
  }

  return expr;
}

}  // namespace

int main() {
  const int kNumSignals = 200;
  const int kTicksPerSignal = 50;
  std::mt19937 rng(42);

  for (int s = 0; s < kNumSignals; ++s) {
    auto body = GenExpr(rng, 3, "AAPL");
    jitse::SignalDef signal{"fuzz_" + std::to_string(s), std::move(body)};
    jitse::AllocateNodeIds(signal);

    jitse::SymbolTable symbols;
    symbols.RegisterOrGetId("AAPL");
    jitse::Interpreter interp(symbols);
    jitse::SignalContext interp_ctx;
    jitse::SetStandaloneParameters(interp_ctx, {0.35});
    jitse::PrewarmSignalContext(interp_ctx, signal);

    jitse::JitCompiler jit;
    const bool jit_ok = jit.IsAvailable() && jit.Compile(signal, symbols);
    auto jit_fn = jit_ok ? jit.GetFunction() : nullptr;
    jitse::MultiSymbolSignalContext jit_ctx(1);
    if (jit_ok) {
      jit_ctx.SetParameters({0.35});
      jitse::PrewarmSignalContext(jit_ctx, 0, signal);
    }

    jitse::MarketState market;
    jitse::MarketSimulator sim(static_cast<std::uint64_t>(s), 1);
    for (int t = 0; t < kTicksPerSignal; ++t) {
      const auto ev = sim.NextEvent(1000);
      market.instruments[0].bid = ev.bid;
      market.instruments[0].ask = ev.ask;
      market.current_time_ns = ev.timestamp_ns;

      const double interp_out = interp.Evaluate(signal, market, interp_ctx);
      if (jit_fn) {
        const double jit_out = jit_fn(&market, &jit_ctx, 0);
        const bool equal = ValuesMatch(interp_out, jit_out);
        if (!equal) {
          std::fprintf(stderr, "PARITY FAIL: signal=%d tick=%d interp=%.15g jit=%.15g\n", s, t, interp_out, jit_out);
          return 1;
        }
      }
    }
  }

  std::printf("fuzz_parity: %d signals x %d ticks OK\n", kNumSignals, kTicksPerSignal);
  return 0;
}
