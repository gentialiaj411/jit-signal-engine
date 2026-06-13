#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "autodiff.h"
#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

constexpr double kRelTol = 1e-4;
constexpr double kAbsTol = 1e-8;
constexpr double kCompiledRelTol = 1e-12;
constexpr double kCompiledAbsTol = 1e-12;

struct PreparedProgram {
  jitse::ProgramDef program;
  jitse::SymbolTable symbols;
  std::size_t output_index = 0;
};

PreparedProgram PrepareProgram(const std::string& src, const std::string& output_signal) {
  PreparedProgram out;
  out.program = jitse::InlineSignalDependencies(jitse::ParseProgram(src));
  jitse::AllocateProgramNodeIds(out.program.signals);
  for (const auto& s : out.program.signals) {
    for (const auto& ticker : jitse::CollectTickerSymbols(s)) {
      out.symbols.RegisterOrGetId(ticker);
    }
  }
  for (auto& s : out.program.signals) {
    jitse::BindSymbolIds(s, out.symbols);
  }
  for (std::size_t i = 0; i < out.program.signals.size(); ++i) {
    if (out.program.signals[i].name == output_signal) {
      out.output_index = i;
      return out;
    }
  }
  throw std::runtime_error("Missing output signal: " + output_signal);
}

jitse::MarketState MakeMarket(double aapl_bid = 100.0, double aapl_ask = 100.2,
                              double msft_bid = 99.5, double msft_ask = 99.8) {
  jitse::MarketState market;
  market.instruments[0].bid = aapl_bid;
  market.instruments[0].ask = aapl_ask;
  market.instruments[0].volume = 50.0;
  market.instruments[1].bid = msft_bid;
  market.instruments[1].ask = msft_ask;
  market.instruments[1].volume = 25.0;
  market.current_time_ns = 123456789;
  return market;
}

std::vector<double> DefaultParams(const PreparedProgram& program) {
  std::vector<double> params(program.program.params.size(), 0.0);
  for (const auto& p : program.program.params) {
    params[static_cast<std::size_t>(p.param_id)] = p.default_value;
  }
  return params;
}

double EvalInterpSequence(
    const PreparedProgram& program,
    const jitse::SymbolTable& symbols,
    const std::vector<jitse::MarketState>& markets,
    const std::vector<double>& params) {
  jitse::Interpreter interp(symbols);
  jitse::SignalContext ctx;
  jitse::SetStandaloneParameters(ctx, params);
  for (const auto& signal : program.program.signals) {
    jitse::PrewarmSignalContext(ctx, signal);
  }
  double out = std::numeric_limits<double>::quiet_NaN();
  for (const auto& market : markets) {
    out = interp.Evaluate(program.program.signals[program.output_index], market, ctx);
  }
  return out;
}

double EvalJitSequence(
    const PreparedProgram& program,
    const std::vector<jitse::MarketState>& markets,
    const std::vector<double>& params,
    bool& ran) {
  ran = false;
  jitse::JitCompiler jit;
  if (!jit.IsAvailable()) return 0.0;
  if (!jit.CompileProgram(program.program.signals, program.symbols)) {
    throw std::runtime_error("CompileProgram failed: " + jit.LastError());
  }
  auto* fn = jit.GetProgramFunction();
  if (fn == nullptr) {
    throw std::runtime_error("CompileProgram returned null function");
  }
  jitse::MultiSymbolSignalContext arena(1);
  arena.SetParameters(params);
  for (const auto& signal : program.program.signals) {
    jitse::PrewarmSignalContext(arena, 0, signal);
  }
  std::vector<double> outputs(program.program.signals.size(), 0.0);
  for (const auto& market : markets) {
    fn(&market, &arena, 0, outputs.data());
  }
  ran = true;
  return outputs[program.output_index];
}

bool WithinTol(double got, double want) {
  if (std::isnan(got) || std::isnan(want)) {
    return std::isnan(got) && std::isnan(want);
  }
  const double abs_err = std::fabs(got - want);
  const double scale = std::max(1.0, std::fabs(want));
  return abs_err <= kAbsTol || (abs_err / scale) <= kRelTol;
}

bool WithinCompiledTol(double got, double want) {
  if (std::isnan(got) || std::isnan(want)) {
    return std::isnan(got) && std::isnan(want);
  }
  const double abs_err = std::fabs(got - want);
  const double scale = std::max(1.0, std::fabs(want));
  return abs_err <= kCompiledAbsTol || (abs_err / scale) <= kCompiledRelTol;
}

double CentralDifference(
    const PreparedProgram& program,
    const std::vector<jitse::MarketState>& markets,
    std::size_t param_index,
    const std::vector<double>& params) {
  std::vector<double> plus = params;
  std::vector<double> minus = params;
  const double h = 1e-6 * std::max(1.0, std::fabs(params[param_index]));
  plus[param_index] += h;
  minus[param_index] -= h;
  const double f_plus = EvalInterpSequence(program, program.symbols, markets, plus);
  const double f_minus = EvalInterpSequence(program, program.symbols, markets, minus);
  return (f_plus - f_minus) / (2.0 * h);
}

double EvalGradientSequence(
    const PreparedProgram& program,
    const std::vector<jitse::MarketState>& markets,
    const std::vector<double>& params,
    std::size_t param_index) {
  jitse::SignalContext ctx;
  jitse::SetStandaloneParameters(ctx, params);
  for (const auto& signal : program.program.signals) {
    jitse::PrewarmSignalContext(ctx, signal);
  }
  jitse::ValueGradient out;
  for (const auto& market : markets) {
    out = jitse::EvaluateSignalGradient(
        program.program.signals[program.output_index],
        program.symbols,
        market,
        ctx,
        static_cast<std::int64_t>(param_index));
  }
  return out.gradient;
}

struct CompiledGradientResult {
  double value = std::numeric_limits<double>::quiet_NaN();
  double gradient = std::numeric_limits<double>::quiet_NaN();
  bool ran = false;
  bool available = false;
};

CompiledGradientResult EvalCompiledGradientSequence(
    const PreparedProgram& program,
    const std::vector<jitse::MarketState>& markets,
    const std::vector<double>& params,
    std::size_t param_index) {
  CompiledGradientResult out;
  jitse::JitCompiler jit;
  out.available = jit.IsAvailable();
  if (!out.available) return out;
  if (!jit.CompileProgramGradient(program.program.signals, program.symbols)) {
    throw std::runtime_error("CompileProgramGradient failed: " + jit.LastError());
  }
  auto* fn = jit.GetProgramGradientFunction();
  if (fn == nullptr) {
    throw std::runtime_error("CompileProgramGradient returned null function");
  }
  jitse::MultiSymbolSignalContext arena(1);
  arena.SetParameters(params);
  for (const auto& signal : program.program.signals) {
    jitse::PrewarmSignalContext(arena, 0, signal);
  }
  std::vector<double> outputs(program.program.signals.size(), 0.0);
  std::vector<double> gradients(program.program.signals.size(), 0.0);
  for (const auto& market : markets) {
    fn(&market, &arena, 0, static_cast<std::int64_t>(param_index), outputs.data(), gradients.data());
  }
  out.ran = true;
  out.value = outputs[program.output_index];
  out.gradient = gradients[program.output_index];
  return out;
}

bool RunCase(
    const char* label,
    const std::string& src,
    const std::string& output_signal,
    const std::vector<jitse::MarketState>& markets) {
  PreparedProgram program = PrepareProgram(src, output_signal);
  bool ok = true;
  std::vector<double> params = DefaultParams(program);

  const double interp_value = EvalInterpSequence(program, program.symbols, markets, params);
  bool jit_ran = false;
  const double jit_value = EvalJitSequence(program, markets, params, jit_ran);
  if (jit_ran && !WithinTol(jit_value, interp_value)) {
    std::cerr << label << ": JIT/interpreter value mismatch interp=" << interp_value
              << " jit=" << jit_value << "\n";
    ok = false;
  }

  for (std::size_t i = 0; i < program.program.params.size(); ++i) {
    const double ad = EvalGradientSequence(program, markets, params, i);
    const double fd = CentralDifference(program, markets, i, params);
    if (!WithinTol(ad, fd)) {
      std::cerr << label << ": gradient mismatch for param "
                << program.program.params[i].name << " ad=" << ad
                << " fd=" << fd << " rel_tol=" << kRelTol
                << " abs_tol=" << kAbsTol << "\n";
      ok = false;
    }
    const CompiledGradientResult compiled = EvalCompiledGradientSequence(program, markets, params, i);
    if (compiled.ran) {
      if (!WithinTol(compiled.value, interp_value)) {
        std::cerr << label << ": compiled gradient value mismatch for param "
                  << program.program.params[i].name << " interp=" << interp_value
                  << " compiled=" << compiled.value << "\n";
        ok = false;
      }
      if (!WithinCompiledTol(compiled.gradient, ad)) {
        std::cerr << label << ": compiled gradient mismatch for param "
                  << program.program.params[i].name << " compiled=" << compiled.gradient
                  << " interpreted=" << ad << " rel_tol=" << kCompiledRelTol
                  << " abs_tol=" << kCompiledAbsTol << "\n";
        ok = false;
      }
    } else if (!compiled.available) {
      std::cout << label << ": LLVM unavailable -- skipping compiled gradient checks\n";
    }
  }

  return ok;
}

bool RunRejectCase(
    const char* label,
    const std::string& src,
    const std::string& output_signal,
    const std::vector<jitse::MarketState>& markets,
    const char* expected_fragment) {
  PreparedProgram program = PrepareProgram(src, output_signal);
  std::vector<double> params = DefaultParams(program);
  bool ok = true;

  try {
    (void)EvalInterpSequence(program, program.symbols, markets, params);
    std::cerr << label << ": interpreter unexpectedly accepted invalid program\n";
    ok = false;
  } catch (const std::runtime_error& ex) {
    if (std::string(ex.what()).find(expected_fragment) == std::string::npos) {
      std::cerr << label << ": interpreter error mismatch: " << ex.what() << "\n";
      ok = false;
    }
  }

  try {
    bool jit_ran = false;
    (void)EvalJitSequence(program, markets, params, jit_ran);
    if (jit_ran) {
      std::cerr << label << ": JIT unexpectedly accepted invalid program\n";
      ok = false;
    } else {
      std::cout << label << ": LLVM unavailable -- skipping JIT rejection check\n";
    }
  } catch (const std::runtime_error& ex) {
    if (std::string(ex.what()).find(expected_fragment) == std::string::npos) {
      std::cerr << label << ": JIT error mismatch: " << ex.what() << "\n";
      ok = false;
    }
  }

  try {
    const CompiledGradientResult compiled = EvalCompiledGradientSequence(program, markets, params, 0);
    if (compiled.available) {
      std::cerr << label << ": compiled gradient unexpectedly accepted invalid program\n";
      ok = false;
    } else {
      std::cout << label << ": LLVM unavailable -- skipping compiled gradient rejection check\n";
    }
  } catch (const std::runtime_error& ex) {
    if (std::string(ex.what()).find(expected_fragment) == std::string::npos) {
      std::cerr << label << ": compiled gradient error mismatch: " << ex.what() << "\n";
      ok = false;
    }
  }

  return ok;
}

}  // namespace

int main() {
  const std::vector<jitse::MarketState> one_tick{MakeMarket()};
  const std::vector<jitse::MarketState> lag_ticks{
      MakeMarket(100.0, 100.2),
      MakeMarket(101.0, 101.2),
      MakeMarket(102.0, 102.2),
      MakeMarket(103.0, 103.2)};
  const std::vector<jitse::MarketState> sma_ticks{
      MakeMarket(100.0, 100.2),
      MakeMarket(101.0, 101.2),
      MakeMarket(102.0, 102.2),
      MakeMarket(104.0, 104.2)};
  const std::vector<jitse::MarketState> ema_ticks{
      MakeMarket(100.0, 100.2),
      MakeMarket(101.0, 101.2),
      MakeMarket(103.0, 103.2),
      MakeMarket(106.0, 106.2)};
  const std::vector<jitse::MarketState> rolling_ticks{
      MakeMarket(100.0, 100.2, 99.0, 99.4),
      MakeMarket(102.0, 102.2, 100.0, 100.5),
      MakeMarket(101.0, 101.2, 103.0, 103.3),
      MakeMarket(105.0, 105.2, 101.0, 101.7),
      MakeMarket(104.0, 104.2, 106.0, 106.6),
      MakeMarket(107.0, 107.2, 102.0, 102.6),
      MakeMarket(103.0, 103.2, 108.0, 108.4)};
  const std::vector<jitse::MarketState> kalman_ticks{
      MakeMarket(100.0, 100.2),
      MakeMarket(101.5, 101.7),
      MakeMarket(100.8, 101.0),
      MakeMarket(103.0, 103.2),
      MakeMarket(102.0, 102.2)};
  const std::vector<jitse::MarketState> cross_up_ticks{
      MakeMarket(100.0, 100.2, 103.0, 103.2),
      MakeMarket(101.0, 101.2, 102.0, 102.2),
      MakeMarket(104.0, 104.2, 101.0, 101.2)};
  const std::vector<jitse::MarketState> cross_down_ticks{
      MakeMarket(104.0, 104.2, 101.0, 101.2),
      MakeMarket(103.0, 103.2, 102.0, 102.2),
      MakeMarket(100.0, 100.2, 103.0, 103.2)};
  bool all_ok = true;

  // Tolerance contract for autodiff parity:
  //   h = 1e-6 * max(1, |theta|)
  //   accept if rel <= 1e-4 OR abs <= 1e-8 near zero.
  //   compiled-vs-interpreted gradient gate: rel <= 1e-12 OR abs <= 1e-12 near zero.
  //   NaN convention: AD and FD both yielding NaN is treated as a pass.
  //   Boundary discipline: cases are chosen away from predicate flips,
  //   min/max ties, and Kalman/Welford guard boundaries unless the case is
  //   explicitly checking a validation rejection.
  all_ok &= RunCase(
      "fanout_alpha",
      "param alpha = 0.7\n"
      "signal s = alpha * alpha + alpha\n",
      "s",
      one_tick);

  all_ok &= RunCase(
      "log_sqrt_domain_edge",
      "param alpha = 0.0001\n"
      "signal s = sqrt(alpha + 0.0001) + log(alpha + 1.1)\n",
      "s",
      one_tick);

  all_ok &= RunCase(
      "conditional_selected_branch",
      "param alpha = 0.5\n"
      "signal s = if mid(AAPL) > 100 then alpha * mid(AAPL) else alpha * ask(AAPL)\n",
      "s",
      one_tick);

  all_ok &= RunCase(
      "whole_program_two_params",
      "param scale = 1.5\n"
      "param shift = -0.3\n"
      "signal base = (mid(AAPL) - mid(MSFT)) * scale\n"
      "signal out = abs(base) + shift * spread(AAPL) + sqrt(scale * scale + 4)\n",
      "out",
      one_tick);

  all_ok &= RunCase(
      "whole_program_log_conditional",
      "param scale = 1.25\n"
      "param shift = 0.4\n"
      "signal a = mid(AAPL) + shift\n"
      "signal b = a / (scale + 2.5)\n"
      "signal out = if a > 0 then b + log(scale + 3) else 0\n",
      "out",
      one_tick);

  all_ok &= RunCase(
      "whole_program_ema_alpha",
      "param alpha = 0.35\n"
      "signal base = ema_alpha(mid(AAPL) - 0.2 * mid(MSFT), alpha)\n"
      "signal out = base / (1.0 + abs(base))\n",
      "out",
      ema_ticks);

  all_ok &= RunCase(
      "lag_linear",
      "param scale = 1.1\n"
      "signal s = lag(scale * mid(AAPL), 2)\n",
      "s",
      lag_ticks);

  all_ok &= RunCase(
      "sma_linear",
      "param scale = 0.9\n"
      "signal s = sma(scale * mid(AAPL), 3)\n",
      "s",
      sma_ticks);

  all_ok &= RunCase(
      "ema_alpha_recurrence",
      "param alpha = 0.35\n"
      "signal s = ema_alpha(mid(AAPL), alpha)\n",
      "s",
      ema_ticks);

  all_ok &= RunCase(
      "rolling_std_refresh",
      "param scale = 1.15\n"
      "signal s = rolling_std(scale * mid(AAPL), 3)\n",
      "s",
      rolling_ticks);

  all_ok &= RunRejectCase(
      "rolling_std_period1_rejected",
      "param scale = 0.8\n"
      "signal s = rolling_std(scale * mid(AAPL), 1)\n",
      "s",
      rolling_ticks,
      "requires period >= 2");

  all_ok &= RunRejectCase(
      "zscore_period1_rejected",
      "param scale = 0.8\n"
      "signal s = zscore(scale * mid(AAPL), 1)\n",
      "s",
      rolling_ticks,
      "requires period >= 2");

  all_ok &= RunCase(
      "zscore_chain",
      "param scale = 0.6\n"
      "signal s = zscore(scale * mid(AAPL) + 0.25 * mid(MSFT), 3)\n",
      "s",
      rolling_ticks);

  all_ok &= RunCase(
      "rolling_corr_accumulators",
      "param alpha = 0.2\n"
      "signal s = rolling_corr(mid(AAPL) + alpha * mid(MSFT), mid(MSFT), 3)\n",
      "s",
      rolling_ticks);

  all_ok &= RunCase(
      "rolling_beta_accumulators",
      "param alpha = -0.15\n"
      "signal s = rolling_beta(mid(AAPL) + alpha * mid(MSFT), mid(MSFT), 3)\n",
      "s",
      rolling_ticks);

  all_ok &= RunCase(
      "kalman1d_qr",
      "param q = 0.05\n"
      "param r = 1.2\n"
      "signal s = kalman1d(mid(AAPL), q, r)\n",
      "s",
      kalman_ticks);

  all_ok &= RunCase(
      "whole_program_kalman1d",
      "param q = 0.04\n"
      "signal filt = kalman1d(mid(AAPL), q, 1.1)\n"
      "signal out = filt - lag(filt, 2)\n",
      "out",
      kalman_ticks);

  all_ok &= RunCase(
      "whole_program_rolling_mix",
      "param scale = 0.45\n"
      "signal a = sma(scale * mid(AAPL), 3)\n"
      "signal b = rolling_beta(mid(AAPL), mid(MSFT), 3)\n"
      "signal out = a + 0.1 * b\n",
      "out",
      rolling_ticks);

  all_ok &= RunCase(
      "rolling_min_subgradient",
      "param alpha = 0.3\n"
      "signal s = rolling_min(mid(AAPL) + alpha * mid(MSFT), 3)\n",
      "s",
      rolling_ticks);

  all_ok &= RunCase(
      "rolling_max_subgradient",
      "param alpha = -0.25\n"
      "signal s = rolling_max(mid(AAPL) + alpha * mid(MSFT), 3)\n",
      "s",
      rolling_ticks);

  all_ok &= RunCase(
      "cross_above_stopgrad",
      "param alpha = 0.1\n"
      "signal s = cross_above(mid(AAPL) + alpha, mid(MSFT))\n",
      "s",
      cross_up_ticks);

  all_ok &= RunCase(
      "cross_below_stopgrad",
      "param alpha = -0.1\n"
      "signal s = cross_below(mid(AAPL) + alpha, mid(MSFT))\n",
      "s",
      cross_down_ticks);

  if (!all_ok) {
    std::cerr << "gradient_parity_test: FAILED\n";
    return 1;
  }
  std::cout << "gradient_parity_test: PASSED\n";
  return 0;
}
