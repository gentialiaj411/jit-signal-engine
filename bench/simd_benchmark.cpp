#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "interpreter.h"
#include "jit_compiler.h"
#include "lexer.h"
#include "market_sim.h"
#include "parser.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

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

double Percentile(std::vector<std::uint64_t> xs, double p) {
  std::sort(xs.begin(), xs.end());
  if (xs.empty()) return 0.0;
  const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(xs.size() - 1));
  return static_cast<double>(xs[idx]);
}

template <typename Fn>
void RunTimed(Fn&& fn, std::size_t events, double& throughput, double& p50, double& p99) {
  constexpr std::size_t kBatch = 64;
  std::vector<std::uint64_t> latencies;
  latencies.reserve(events / kBatch + 1);
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < events; i += kBatch) {
    const std::size_t n = std::min(kBatch, events - i);
    const auto t0 = std::chrono::high_resolution_clock::now();
    fn(i, n);
    const auto t1 = std::chrono::high_resolution_clock::now();
    const auto ns =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    latencies.push_back(ns / n);
  }
  const auto end = std::chrono::steady_clock::now();
  const double sec = std::chrono::duration<double>(end - start).count();
  throughput = static_cast<double>(events) / sec;
  p50 = Percentile(latencies, 0.50);
  p99 = Percentile(latencies, 0.99);
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t events = (argc >= 2) ? static_cast<std::size_t>(std::stoull(argv[1])) : 1000000;
  const std::string out_csv = (argc >= 3) ? argv[2] : "./bench/results_simd.csv";

  jitse::SymbolTable symbols;
  symbols.RegisterOrGetId("AAPL");
  jitse::Lexer lexer("signal s = sma(mid(AAPL), 64)");
  jitse::Parser parser(lexer.Tokenize());
  jitse::SignalDef signal = parser.ParseSignalDef();
  jitse::AllocateNodeIds(signal);
  jitse::BindSymbolIds(signal, symbols);

  jitse::MarketState market;
  std::vector<jitse::MarketEvent> replay;
  replay.reserve(events);
  {
    jitse::MarketSimulator sim(42, 1);
    for (std::size_t i = 0; i < events; ++i) replay.push_back(sim.NextEvent(1000));
  }

  std::ofstream out(out_csv);
  out << "operator,mode,events,throughput,lat_ns_p50,lat_ns_p99,jit_available,avx2_enabled\n";

  // Interpreter
  {
    jitse::Interpreter interp(symbols);
    jitse::SignalContext ctx;
    jitse::PrewarmSignalContext(ctx, signal);
    for (std::size_t i = 0; i < 10000; ++i) {
      const auto& ev = replay[i % replay.size()];
      market.instruments[0].bid = ev.bid;
      market.instruments[0].ask = ev.ask;
      market.current_time_ns = ev.timestamp_ns;
      (void)interp.Evaluate(signal, market, ctx);
    }
    volatile double sink = 0.0;
    double thr = 0.0, p50 = 0.0, p99 = 0.0;
    RunTimed(
        [&](std::size_t i, std::size_t n) {
          for (std::size_t j = 0; j < n; ++j) {
            const auto& ev = replay[i + j];
            market.instruments[0].bid = ev.bid;
            market.instruments[0].ask = ev.ask;
            market.current_time_ns = ev.timestamp_ns;
            sink += interp.Evaluate(signal, market, ctx);
          }
        },
        events,
        thr,
        p50,
        p99);
    out << "sma,interpreter," << events << "," << thr << "," << p50 << "," << p99 << ",false,false\n";
  }

  auto run_jit_mode = [&](bool force_scalar, const char* mode_name) {
    SetForceDisableAvx2(force_scalar);
    jitse::JitCompiler jit;
    if (!jit.IsAvailable()) {
      out << "sma," << mode_name << "," << events << ",nan,nan,nan,false,false\n";
      return;
    }
    if (!jit.Compile(signal, symbols) || jit.GetFunction() == nullptr) {
      out << "sma," << mode_name << "," << events << ",nan,nan,nan,true," << (jit.HasAVX2() ? "true" : "false")
          << "\n";
      return;
    }
    auto fn = jit.GetFunction();
    jitse::SignalContext ctx;
    jitse::PrewarmSignalContext(ctx, signal);
    for (std::size_t i = 0; i < 10000; ++i) {
      const auto& ev = replay[i % replay.size()];
      market.instruments[0].bid = ev.bid;
      market.instruments[0].ask = ev.ask;
      market.current_time_ns = ev.timestamp_ns;
      (void)fn(&market, &ctx);
    }
    volatile double sink = 0.0;
    double thr = 0.0, p50 = 0.0, p99 = 0.0;
    RunTimed(
        [&](std::size_t i, std::size_t n) {
          for (std::size_t j = 0; j < n; ++j) {
            const auto& ev = replay[i + j];
            market.instruments[0].bid = ev.bid;
            market.instruments[0].ask = ev.ask;
            market.current_time_ns = ev.timestamp_ns;
            sink += fn(&market, &ctx);
          }
        },
        events,
        thr,
        p50,
        p99);
    out << "sma," << mode_name << "," << events << "," << thr << "," << p50 << "," << p99 << ",true,"
        << (jit.HasAVX2() ? "true" : "false") << "\n";
  };

  run_jit_mode(true, "scalar_jit");
  run_jit_mode(false, "vector_jit");
  SetForceDisableAvx2(false);

  std::cout << "results_simd_csv=" << out_csv << "\n";
  return 0;
}
