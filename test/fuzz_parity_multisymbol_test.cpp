#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

int main() {
  constexpr std::size_t kSymbols = 64;
  constexpr std::size_t kTicks = 400;

  const std::string src =
      "signal short_ma = ema(mid(AAPL), 10)\n"
      "signal long_ma = ema(mid(AAPL), 60)\n"
      "signal vol = rolling_std(mid(AAPL), 30)\n"
      "signal raw = short_ma - long_ma\n"
      "signal filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0\n";

  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);
  for (auto& s : signals) jitse::AllocateNodeIds(s);

  jitse::SymbolTable symbols;
  symbols.RegisterOrGetId("AAPL");
  for (auto& s : signals) jitse::BindSymbolIds(s, symbols);

  jitse::JitCompiler jit;
  if (!jit.IsAvailable()) return 0;
  if (!jit.CompileProgram(signals, symbols) || jit.GetProgramFunction() == nullptr) return 1;
  const auto fn = jit.GetProgramFunction();

  jitse::Interpreter interp(symbols);
  jitse::MultiSymbolSignalContext multi_ctx(kSymbols);
  std::vector<jitse::SignalContext> ref_ctx(kSymbols);
  for (std::size_t sym = 0; sym < kSymbols; ++sym) {
    for (const auto& s : signals) {
      jitse::PrewarmSignalContext(multi_ctx, static_cast<std::uint32_t>(sym), s);
      jitse::PrewarmSignalContext(ref_ctx[sym], s);
    }
  }

  std::vector<jitse::MarketState> markets(kSymbols);
  std::vector<jitse::MarketSimulator> sims;
  sims.reserve(kSymbols);
  for (std::size_t sym = 0; sym < kSymbols; ++sym) {
    sims.emplace_back(static_cast<std::uint64_t>(1000 + sym), 1);
  }

  std::vector<double> multi_outputs(signals.size(), 0.0);
  for (std::size_t t = 0; t < kTicks; ++t) {
    for (std::size_t sym = 0; sym < kSymbols; ++sym) {
      const auto ev = sims[sym].NextEvent(1000);
      markets[sym].instruments[0].bid = ev.bid;
      markets[sym].instruments[0].ask = ev.ask;
      markets[sym].current_time_ns = ev.timestamp_ns;

      fn(&markets[sym], &multi_ctx, static_cast<std::uint32_t>(sym), multi_outputs.data());

      for (std::size_t s = 0; s < signals.size(); ++s) {
        const double ref = interp.Evaluate(signals[s], markets[sym], ref_ctx[sym]);
        const double got = multi_outputs[s];
        if (std::isnan(ref) || std::isnan(got)) {
          assert(std::isnan(ref) && std::isnan(got));
        } else {
          assert(std::fabs(ref - got) < 1e-8);
        }
      }
    }
  }

  return 0;
}
