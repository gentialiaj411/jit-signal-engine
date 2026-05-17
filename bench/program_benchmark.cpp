#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open: " + path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

double Percentile(std::vector<std::uint64_t> xs, double p) {
  std::sort(xs.begin(), xs.end());
  if (xs.empty()) return 0.0;
  const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(xs.size() - 1));
  return static_cast<double>(xs[idx]);
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    const std::string signal_file = (argc >= 2) ? argv[1] : "examples/filtered_momentum.sig";

    std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(ReadFile(signal_file));
    std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);

    const std::size_t events = 500000;
    constexpr std::size_t kWarmupIters = 10000;
    constexpr std::size_t kBatch = 64;

    jitse::SymbolTable symbols;
    std::vector<std::string> tickers;
    for (const auto& s : signals) {
      const auto ts = jitse::CollectTickerSymbols(s);
      tickers.insert(tickers.end(), ts.begin(), ts.end());
    }
    for (const auto& t : tickers) symbols.RegisterOrGetId(t);
    if (tickers.empty()) symbols.RegisterOrGetId("AAPL");
    const std::size_t instrument_count = std::max<std::size_t>(1, tickers.size());

    for (auto& s : signals) {
      jitse::AllocateNodeIds(s);
    }

    // --- PATH A: N separate Compile() calls ---
    std::vector<std::unique_ptr<jitse::JitCompiler>> separate_jits;
    separate_jits.reserve(signals.size());
    std::vector<jitse::JitCompiler::JitFn> separate_fns;
    separate_fns.reserve(signals.size());
    for (const auto& s : signals) {
      separate_jits.push_back(std::make_unique<jitse::JitCompiler>());
      auto& jit = *separate_jits.back();
      if (!jit.IsAvailable() || !jit.Compile(s, symbols) || jit.GetFunction() == nullptr) {
        std::cout << "jit_mode=unavailable\n";
        std::cout << "separate_throughput=" << std::numeric_limits<double>::quiet_NaN() << "\n";
        std::cout << "separate_lat_ns_p50=" << std::numeric_limits<double>::quiet_NaN() << "\n";
        std::cout << "separate_lat_ns_p99=" << std::numeric_limits<double>::quiet_NaN() << "\n";
        std::cout << "program_throughput=" << std::numeric_limits<double>::quiet_NaN() << "\n";
        std::cout << "program_lat_ns_p50=" << std::numeric_limits<double>::quiet_NaN() << "\n";
        std::cout << "program_lat_ns_p99=" << std::numeric_limits<double>::quiet_NaN() << "\n";
        std::cout << "speedup=" << std::numeric_limits<double>::quiet_NaN() << "\n";
        return 0;
      }
      separate_fns.push_back(jit.GetFunction());
    }

    jitse::MarketState sep_market;
    std::vector<jitse::SignalContext> sep_ctxs(signals.size());
    for (std::size_t i = 0; i < signals.size(); ++i) {
      jitse::PrewarmSignalContext(sep_ctxs[i], signals[i]);
    }
    jitse::MarketSimulator sep_sim(42, instrument_count);

    std::vector<std::uint64_t> sep_latencies;
    sep_latencies.reserve(events / kBatch + 1);
    volatile double sep_sink = 0.0;

    {
      jitse::MarketState warmup_market;
      std::vector<jitse::SignalContext> warmup_ctxs(signals.size());
      for (std::size_t i = 0; i < signals.size(); ++i) {
        jitse::PrewarmSignalContext(warmup_ctxs[i], signals[i]);
      }
      jitse::MarketSimulator warmup_sim(99, instrument_count);
      volatile double warmup_sink = 0.0;
      for (std::size_t i = 0; i < kWarmupIters; ++i) {
        const auto ev = warmup_sim.NextEvent(1000);
        warmup_market.instruments[ev.instrument_id].bid = ev.bid;
        warmup_market.instruments[ev.instrument_id].ask = ev.ask;
        warmup_market.current_time_ns = ev.timestamp_ns;
        for (std::size_t s = 0; s < separate_fns.size(); ++s) {
          warmup_sink += separate_fns[s](&warmup_market, &warmup_ctxs[s]);
        }
      }
      (void)warmup_sink;
    }

    const auto sep_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < events; i += kBatch) {
      const std::size_t batch_count = std::min(kBatch, events - i);
      const auto t0 = std::chrono::high_resolution_clock::now();
      for (std::size_t j = 0; j < batch_count; ++j) {
        const auto ev = sep_sim.NextEvent(1000);
        sep_market.instruments[ev.instrument_id].bid = ev.bid;
        sep_market.instruments[ev.instrument_id].ask = ev.ask;
        sep_market.current_time_ns = ev.timestamp_ns;
        for (std::size_t s = 0; s < separate_fns.size(); ++s) {
          sep_sink += separate_fns[s](&sep_market, &sep_ctxs[s]);
        }
      }
      const auto t1 = std::chrono::high_resolution_clock::now();
      const auto ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      sep_latencies.push_back(ns / batch_count);
    }
    const auto sep_end = std::chrono::steady_clock::now();
    const double sep_sec = std::chrono::duration<double>(sep_end - sep_start).count();
    const double throughput_separate = static_cast<double>(events) / sep_sec;
    const double lat_p50_separate = Percentile(sep_latencies, 0.50);
    const double lat_p99_separate = Percentile(sep_latencies, 0.99);

    // --- PATH B: One CompileProgram() call ---
    jitse::JitCompiler program_jit;
    if (!program_jit.IsAvailable() || !program_jit.CompileProgram(signals, symbols) ||
        program_jit.GetProgramFunction() == nullptr) {
      std::cout << "jit_mode=unavailable\n";
      std::cout << "separate_throughput=" << throughput_separate << "\n";
      std::cout << "separate_lat_ns_p50=" << lat_p50_separate << "\n";
      std::cout << "separate_lat_ns_p99=" << lat_p99_separate << "\n";
      std::cout << "program_throughput=" << std::numeric_limits<double>::quiet_NaN() << "\n";
      std::cout << "program_lat_ns_p50=" << std::numeric_limits<double>::quiet_NaN() << "\n";
      std::cout << "program_lat_ns_p99=" << std::numeric_limits<double>::quiet_NaN() << "\n";
      std::cout << "speedup=" << std::numeric_limits<double>::quiet_NaN() << "\n";
      return 0;
    }
    jitse::JitCompiler::ProgramFn program_fn = program_jit.GetProgramFunction();

    jitse::SignalContext prog_ctx;
    for (const auto& s : signals) {
      jitse::PrewarmSignalContext(prog_ctx, s);
    }
    jitse::MarketState prog_market;
    jitse::MarketSimulator prog_sim(42, instrument_count);
    std::vector<double> outputs(signals.size(), 0.0);

    std::vector<std::uint64_t> prog_latencies;
    prog_latencies.reserve(events / kBatch + 1);
    volatile double prog_sink = 0.0;

    {
      jitse::SignalContext warmup_ctx;
      for (const auto& s : signals) {
        jitse::PrewarmSignalContext(warmup_ctx, s);
      }
      jitse::MarketState warmup_market;
      jitse::MarketSimulator warmup_sim(99, instrument_count);
      volatile double warmup_sink = 0.0;
      for (std::size_t i = 0; i < kWarmupIters; ++i) {
        const auto ev = warmup_sim.NextEvent(1000);
        warmup_market.instruments[ev.instrument_id].bid = ev.bid;
        warmup_market.instruments[ev.instrument_id].ask = ev.ask;
        warmup_market.current_time_ns = ev.timestamp_ns;
        program_fn(&warmup_market, &warmup_ctx, outputs.data());
        warmup_sink += outputs.back();
      }
      (void)warmup_sink;
    }

    const auto prog_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < events; i += kBatch) {
      const std::size_t batch_count = std::min(kBatch, events - i);
      const auto t0 = std::chrono::high_resolution_clock::now();
      for (std::size_t j = 0; j < batch_count; ++j) {
        const auto ev = prog_sim.NextEvent(1000);
        prog_market.instruments[ev.instrument_id].bid = ev.bid;
        prog_market.instruments[ev.instrument_id].ask = ev.ask;
        prog_market.current_time_ns = ev.timestamp_ns;
        program_fn(&prog_market, &prog_ctx, outputs.data());
        prog_sink += outputs.back();
      }
      const auto t1 = std::chrono::high_resolution_clock::now();
      const auto ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      prog_latencies.push_back(ns / batch_count);
    }
    const auto prog_end = std::chrono::steady_clock::now();
    const double prog_sec = std::chrono::duration<double>(prog_end - prog_start).count();
    const double throughput_program = static_cast<double>(events) / prog_sec;
    const double lat_p50_program = Percentile(prog_latencies, 0.50);
    const double lat_p99_program = Percentile(prog_latencies, 0.99);

    std::cout << "jit_mode=available\n";
    std::cout << "separate_throughput=" << throughput_separate << "\n";
    std::cout << "separate_lat_ns_p50=" << lat_p50_separate << "\n";
    std::cout << "separate_lat_ns_p99=" << lat_p99_separate << "\n";
    std::cout << "program_throughput=" << throughput_program << "\n";
    std::cout << "program_lat_ns_p50=" << lat_p50_program << "\n";
    std::cout << "program_lat_ns_p99=" << lat_p99_program << "\n";
    std::cout << "speedup=" << (throughput_program / throughput_separate) << "\n";
    (void)sep_sink;
    (void)prog_sink;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 2;
  }
}
