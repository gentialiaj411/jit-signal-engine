#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

double Percentile(std::vector<std::uint64_t> xs, double p) {
  std::sort(xs.begin(), xs.end());
  if (xs.empty()) return 0.0;
  const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(xs.size() - 1));
  return static_cast<double>(xs[idx]);
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t passes = (argc >= 2) ? static_cast<std::size_t>(std::stoull(argv[1])) : 2000;
  const std::string out_csv = (argc >= 3) ? argv[2] : "./bench/results_multisymbol.csv";

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
  if (!jit.IsAvailable() || !jit.CompileProgram(signals, symbols) || jit.GetProgramFunction() == nullptr) {
    std::cerr << "jit unavailable\n";
    return 1;
  }
  const auto fn = jit.GetProgramFunction();

  std::ofstream out(out_csv);
  out << "n_symbols,passes,throughput_total_updates_per_sec,throughput_per_symbol_updates_per_sec,pass_lat_ns_p50,pass_lat_ns_p99\n";

  const std::vector<std::size_t> sweep = {1, 10, 100, 1000, 10000};
  for (const std::size_t n : sweep) {
    std::vector<jitse::MarketState> markets(n);
    std::vector<jitse::MarketSimulator> sims;
    sims.reserve(n);
    for (std::size_t s = 0; s < n; ++s) sims.emplace_back(static_cast<std::uint64_t>(9000 + s), 1);

    jitse::MultiSymbolSignalContext arena(n);
    for (std::size_t s = 0; s < n; ++s) {
      for (const auto& sig : signals) {
        jitse::PrewarmSignalContext(arena, static_cast<std::uint32_t>(s), sig);
      }
    }

    std::vector<double> outputs(n * signals.size(), 0.0);
    for (std::size_t w = 0; w < 200; ++w) {
      for (std::size_t s = 0; s < n; ++s) {
        const auto ev = sims[s].NextEvent(1000);
        markets[s].instruments[0].bid = ev.bid;
        markets[s].instruments[0].ask = ev.ask;
        markets[s].current_time_ns = ev.timestamp_ns;
        fn(&markets[s], &arena, static_cast<std::uint32_t>(s), outputs.data() + s * signals.size());
      }
    }

    std::vector<std::uint64_t> pass_lat_ns;
    pass_lat_ns.reserve(passes);
    const auto start = std::chrono::steady_clock::now();
    volatile double sink = 0.0;
    for (std::size_t p = 0; p < passes; ++p) {
      const auto t0 = std::chrono::high_resolution_clock::now();
      for (std::size_t s = 0; s < n; ++s) {
        const auto ev = sims[s].NextEvent(1000);
        markets[s].instruments[0].bid = ev.bid;
        markets[s].instruments[0].ask = ev.ask;
        markets[s].current_time_ns = ev.timestamp_ns;
        double* row = outputs.data() + s * signals.size();
        fn(&markets[s], &arena, static_cast<std::uint32_t>(s), row);
        sink += row[signals.size() - 1];
      }
      const auto t1 = std::chrono::high_resolution_clock::now();
      pass_lat_ns.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    const auto end = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(end - start).count();
    const double total_updates = static_cast<double>(n) * static_cast<double>(passes);
    const double throughput_total = total_updates / sec;
    const double throughput_per_symbol = throughput_total / static_cast<double>(n);
    const double p50 = Percentile(pass_lat_ns, 0.50);
    const double p99 = Percentile(pass_lat_ns, 0.99);

    out << n << "," << passes << "," << throughput_total << "," << throughput_per_symbol << "," << p50 << "," << p99
        << "\n";
    (void)sink;
  }

  std::cout << "results_multisymbol_csv=" << out_csv << "\n";
  return 0;
}
