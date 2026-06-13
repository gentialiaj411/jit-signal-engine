#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

struct BenchResult {
  double forward_ns_per_tick = 0.0;
  double gradient_ns_per_tick = 0.0;
  double ratio = 0.0;
  std::size_t warmup_ticks = 0;
  std::size_t measured_ticks = 0;
};

BenchResult RunBench() {
  const std::string src =
      "param alpha = 0.35\n"
      "signal base = ema_alpha(mid(AAPL), alpha)\n"
      "signal out = rolling_std(base, 3)\n";

  jitse::ProgramDef program = jitse::InlineSignalDependencies(jitse::ParseProgram(src));
  jitse::AllocateProgramNodeIds(program.signals);
  jitse::SymbolTable symbols;
  for (const auto& s : program.signals) {
    for (const auto& ticker : jitse::CollectTickerSymbols(s)) {
      symbols.RegisterOrGetId(ticker);
    }
  }
  for (auto& s : program.signals) {
    jitse::BindSymbolIds(s, symbols);
  }

  jitse::JitCompiler forward_jit;
  if (!forward_jit.IsAvailable() || !forward_jit.CompileProgram(program.signals, symbols) ||
      forward_jit.GetProgramFunction() == nullptr) {
    throw std::runtime_error("Forward CompileProgram failed: " + forward_jit.LastError());
  }

  jitse::JitCompiler grad_jit;
  if (!grad_jit.IsAvailable() || !grad_jit.CompileProgramGradient(program.signals, symbols) ||
      grad_jit.GetProgramGradientFunction() == nullptr) {
    throw std::runtime_error("Gradient CompileProgramGradient failed: " + grad_jit.LastError());
  }

  constexpr std::size_t kWarmupTicks = 5000;
  constexpr std::size_t kMeasureTicks = 200000;

  jitse::MarketState market;
  jitse::MultiSymbolSignalContext forward_ctx(1);
  forward_ctx.SetParameters({0.35});
  for (const auto& s : program.signals) {
    jitse::PrewarmSignalContext(forward_ctx, 0, s);
  }
  std::vector<double> forward_outputs(program.signals.size(), 0.0);
  jitse::MarketSimulator forward_sim(42, 1);
  for (std::size_t i = 0; i < kWarmupTicks; ++i) {
    const auto ev = forward_sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    forward_jit.GetProgramFunction()(&market, &forward_ctx, 0, forward_outputs.data());
  }

  volatile double forward_sink = 0.0;
  const auto forward_start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < kMeasureTicks; ++i) {
    const auto ev = forward_sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    forward_jit.GetProgramFunction()(&market, &forward_ctx, 0, forward_outputs.data());
    forward_sink += forward_outputs.back();
  }
  const auto forward_end = std::chrono::steady_clock::now();

  jitse::MultiSymbolSignalContext grad_ctx(1);
  grad_ctx.SetParameters({0.35});
  for (const auto& s : program.signals) {
    jitse::PrewarmSignalContext(grad_ctx, 0, s);
  }
  std::vector<double> grad_outputs(program.signals.size(), 0.0);
  std::vector<double> grad_values(program.signals.size(), 0.0);
  jitse::MarketSimulator grad_sim(42, 1);
  for (std::size_t i = 0; i < kWarmupTicks; ++i) {
    const auto ev = grad_sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    grad_jit.GetProgramGradientFunction()(
        &market, &grad_ctx, 0, 0, grad_outputs.data(), grad_values.data());
  }

  volatile double grad_sink = 0.0;
  const auto grad_start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < kMeasureTicks; ++i) {
    const auto ev = grad_sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    grad_jit.GetProgramGradientFunction()(
        &market, &grad_ctx, 0, 0, grad_outputs.data(), grad_values.data());
    grad_sink += grad_values.back();
  }
  const auto grad_end = std::chrono::steady_clock::now();

  (void)forward_sink;
  (void)grad_sink;

  const double forward_ns =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(forward_end - forward_start).count()) /
      static_cast<double>(kMeasureTicks);
  const double grad_ns =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(grad_end - grad_start).count()) /
      static_cast<double>(kMeasureTicks);

  return {
      forward_ns,
      grad_ns,
      grad_ns / forward_ns,
      kWarmupTicks,
      kMeasureTicks,
  };
}

void WriteMarkdown(const BenchResult& result, const std::string& path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("Failed to open benchmark output: " + path);
  out << "# Phase 3 Autodiff Tick Benchmark\n\n";
  out << "| Metric | Value |\n";
  out << "|---|---:|\n";
  out << std::fixed << std::setprecision(2);
  out << "| Forward-only ns/tick | " << result.forward_ns_per_tick << " |\n";
  out << "| Forward+gradient ns/tick | " << result.gradient_ns_per_tick << " |\n";
  out << "| Gradient/forward ratio | " << result.ratio << "x |\n";
  out << "| Warmup ticks | " << result.warmup_ticks << " |\n";
  out << "| Measured ticks | " << result.measured_ticks << " |\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: autodiff_benchmark <output.md>\n";
    return 2;
  }
  const BenchResult result = RunBench();
  WriteMarkdown(result, argv[1]);
  std::cout << std::fixed << std::setprecision(2)
            << "forward_ns_per_tick=" << result.forward_ns_per_tick
            << " gradient_ns_per_tick=" << result.gradient_ns_per_tick
            << " ratio=" << result.ratio << "\n";
  return 0;
}
