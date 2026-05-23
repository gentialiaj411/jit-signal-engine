#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <new>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

#include "ast_utils.h"
#include "interpreter.h"
#include "lexer.h"
#include "market_sim.h"
#include "parser.h"
#include "signal_backend.h"
#include "jit_compiler.h"
#include "signal_program.h"

namespace {
std::atomic<std::uint64_t> g_allocations{0};
thread_local bool g_count_allocations = false;

struct AllocationScope {
  explicit AllocationScope(bool enabled) : prev_(g_count_allocations) { g_count_allocations = enabled; }
  ~AllocationScope() { g_count_allocations = prev_; }
  bool prev_;
};

void ResetAllocationCounter() { g_allocations.store(0, std::memory_order_relaxed); }
std::uint64_t AllocationCount() { return g_allocations.load(std::memory_order_relaxed); }

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

bool PinCurrentThreadToCore(std::size_t core) {
#ifdef _WIN32
  if (core >= sizeof(DWORD_PTR) * 8) return false;
  const DWORD_PTR mask = (static_cast<DWORD_PTR>(1) << core);
  return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(static_cast<int>(core), &cpuset);
  return sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0;
#else
  (void)core;
  return false;
#endif
}

}  // namespace

void* operator new(std::size_t sz) {
  if (g_count_allocations) g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(sz)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t sz) {
  if (g_count_allocations) g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(sz)) return p;
  throw std::bad_alloc();
}
void operator delete[](void* p) noexcept { std::free(p); }

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      std::cerr << "Usage: signal_benchmark [--all-signals] [--pin-core N] <signal_file> [events] [csv_out] [signal_name]\n";
      return 1;
    }
    bool all_signals_mode = false;
    bool pin_requested = false;
    std::size_t pin_core = 0;
    std::vector<std::string> positional;
    positional.reserve(static_cast<std::size_t>(argc));
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--all-signals") {
        all_signals_mode = true;
      } else if (arg == "--pin-core") {
        if (i + 1 >= argc) throw std::runtime_error("--pin-core requires a core index");
        pin_requested = true;
        pin_core = static_cast<std::size_t>(std::stoull(argv[++i]));
      } else {
        positional.push_back(arg);
      }
    }
    if (positional.empty()) {
      throw std::runtime_error("Missing <signal_file> argument");
    }
    const std::string signal_file = positional[0];
    const std::size_t events = (positional.size() >= 2) ? static_cast<std::size_t>(std::stoull(positional[1])) : 200000;
    const std::string csv_out = (positional.size() >= 3) ? positional[2] : "";
    const std::string selected_signal = (positional.size() >= 4) ? positional[3] : "";
    if (pin_requested) {
      const bool pinned = PinCurrentThreadToCore(pin_core);
      std::cout << "thread_pinned=" << (pinned ? "true" : "false") << "\n";
      if (pinned) {
        std::cout << "thread_core=" << pin_core << "\n";
      }
    }

    std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(ReadFile(signal_file));
    std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);
    jitse::SignalDef* signal = &signals.back();
    if (!selected_signal.empty() && !all_signals_mode) {
      bool found = false;
      for (auto& s : signals) {
        if (s.name == selected_signal) {
          signal = &s;
          found = true;
          break;
        }
      }
      if (!found) throw std::runtime_error("Requested signal not found: " + selected_signal);
    }
    std::int64_t max_node_id = 0;
    if (all_signals_mode) {
      for (auto& s : signals) {
        max_node_id = std::max(max_node_id, jitse::AllocateNodeIds(s));
      }
    } else {
      max_node_id = jitse::AllocateNodeIds(*signal);
    }

    jitse::SymbolTable symbols;
    std::vector<std::string> tickers;
    if (all_signals_mode) {
      for (const auto& s : signals) {
        const auto ts = jitse::CollectTickerSymbols(s);
        tickers.insert(tickers.end(), ts.begin(), ts.end());
      }
    } else {
      const auto ts = jitse::CollectTickerSymbols(*signal);
      tickers.insert(tickers.end(), ts.begin(), ts.end());
    }
    for (const auto& t : tickers) symbols.RegisterOrGetId(t);
    if (tickers.empty()) symbols.RegisterOrGetId("AAPL");
    if (all_signals_mode) {
      for (auto& s : signals) jitse::BindSymbolIds(s, symbols);
    } else {
      jitse::BindSymbolIds(*signal, symbols);
    }

    jitse::Interpreter interp(symbols);
    jitse::SignalContext ctx;
    if (all_signals_mode) {
      for (const auto& s : signals) {
        jitse::PrewarmSignalContext(ctx, s);
      }
    } else {
      jitse::PrewarmSignalContext(ctx, *signal);
    }
    jitse::MarketState market;
    const std::size_t instrument_count = std::max<std::size_t>(1, tickers.size());
    std::vector<jitse::MarketEvent> replay;
    replay.reserve(events);
    {
      jitse::MarketSimulator replay_sim(42, instrument_count);
      for (std::size_t i = 0; i < events; ++i) {
        replay.push_back(replay_sim.NextEvent(1000));
      }
    }
    constexpr std::size_t kWarmupIters = 10000;
    constexpr std::size_t kBatch = 64;
    std::vector<std::uint64_t> latencies;
    latencies.reserve(events / kBatch + 1);
    volatile double sink = 0.0;

    {
      jitse::SignalContext warmup_ctx;
      if (all_signals_mode) {
        for (const auto& s : signals) {
          jitse::PrewarmSignalContext(warmup_ctx, s);
        }
      } else {
        jitse::PrewarmSignalContext(warmup_ctx, *signal);
      }
      jitse::MarketState warmup_market;
      jitse::MarketSimulator warmup_sim(99, instrument_count);
      volatile double warmup_sink = 0.0;
      for (std::size_t i = 0; i < kWarmupIters; ++i) {
        const auto ev = warmup_sim.NextEvent(1000);
        warmup_market.instruments[ev.instrument_id].bid = ev.bid;
        warmup_market.instruments[ev.instrument_id].ask = ev.ask;
        warmup_market.current_time_ns = ev.timestamp_ns;
        if (all_signals_mode) {
          for (const auto& s : signals) {
            warmup_sink += interp.Evaluate(s, warmup_market, warmup_ctx);
          }
        } else {
          warmup_sink += interp.Evaluate(*signal, warmup_market, warmup_ctx);
        }
      }
      (void)warmup_sink;
    }

    // Batch timing used to amortize timer-call overhead (~20-100ns per
    // clock() call) across 64 signal evaluations. Each recorded latency is
    // the mean of one batch.
    const auto start = std::chrono::steady_clock::now();
    ResetAllocationCounter();
    AllocationScope interp_alloc_scope(true);
    for (std::size_t i = 0; i < events; i += kBatch) {
      const std::size_t batch_count = std::min(kBatch, events - i);
      const auto t0 = std::chrono::high_resolution_clock::now();
      for (std::size_t j = 0; j < batch_count; ++j) {
        const auto& ev = replay[i + j];
        market.instruments[ev.instrument_id].bid = ev.bid;
        market.instruments[ev.instrument_id].ask = ev.ask;
        market.current_time_ns = ev.timestamp_ns;
        if (all_signals_mode) {
          for (const auto& s : signals) {
            sink += interp.Evaluate(s, market, ctx);
          }
        } else {
          sink += interp.Evaluate(*signal, market, ctx);
        }
      }
      const auto t1 = std::chrono::high_resolution_clock::now();
      const auto ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      latencies.push_back(ns / batch_count);
    }
    const auto end = std::chrono::steady_clock::now();
    const std::uint64_t interp_allocations = AllocationCount();
    const double sec = std::chrono::duration<double>(end - start).count();

    std::cout << "signal=" << (all_signals_mode ? std::string("<all_signals>") : signal->name) << "\n";
    std::cout << "events=" << events << "\n";
    std::cout << "throughput=" << static_cast<double>(events) / sec << "\n";
    std::cout << "lat_ns_p50=" << Percentile(latencies, 0.50) << "\n";
    std::cout << "lat_ns_p99=" << Percentile(latencies, 0.99) << "\n";
    std::cout << "lat_ns_p999=" << Percentile(latencies, 0.999) << "\n";
    std::cout << "sink=" << sink << "\n";
    std::cout << "allocations_interp=" << interp_allocations << "\n";

    // JIT path (auto-fallback if LLVM is unavailable or compile fails).
    double jit_throughput = std::numeric_limits<double>::quiet_NaN();
    double jit_p50 = std::numeric_limits<double>::quiet_NaN();
    double jit_p99 = std::numeric_limits<double>::quiet_NaN();
    double jit_p999 = std::numeric_limits<double>::quiet_NaN();
    double jit_sink_out = std::numeric_limits<double>::quiet_NaN();
    std::string jit_mode = "unavailable";
    std::string jit_error;

    jitse::JitCompiler program_jit;
    jitse::JitCompiler::ProgramFn program_fn = nullptr;
    std::size_t program_output_index = signals.size() - 1;
    if (!all_signals_mode) {
      for (std::size_t i = 0; i < signals.size(); ++i) {
        if (&signals[i] == signal) {
          program_output_index = i;
          break;
        }
      }
    }
    if (program_jit.IsAvailable()) {
      if (program_jit.CompileProgram(signals, symbols) &&
          (program_fn = program_jit.GetProgramFunction()) != nullptr) {
        jit_mode = "enabled";
        std::vector<std::uint64_t> jit_latencies;
        jit_latencies.reserve(events / kBatch + 1);
        volatile double jit_sink = 0.0;
        std::vector<double> jit_outputs(signals.size(), 0.0);
        jitse::SignalContext jit_ctx;
        if (all_signals_mode) {
          for (const auto& s : signals) {
            jitse::PrewarmSignalContext(jit_ctx, s);
          }
        } else {
          jitse::PrewarmSignalContext(jit_ctx, *signal);
        }
        jitse::MarketState jit_market;
        {
          jitse::SignalContext jit_warmup_ctx;
          if (all_signals_mode) {
            for (const auto& s : signals) {
              jitse::PrewarmSignalContext(jit_warmup_ctx, s);
            }
          } else {
            jitse::PrewarmSignalContext(jit_warmup_ctx, *signal);
          }
          jitse::MarketState jit_warmup_market;
          jitse::MarketSimulator jit_warmup_sim(99, instrument_count);
          volatile double jit_warmup_sink = 0.0;
          for (std::size_t i = 0; i < kWarmupIters; ++i) {
            const auto ev = jit_warmup_sim.NextEvent(1000);
            jit_warmup_market.instruments[ev.instrument_id].bid = ev.bid;
            jit_warmup_market.instruments[ev.instrument_id].ask = ev.ask;
            jit_warmup_market.current_time_ns = ev.timestamp_ns;
            program_fn(&jit_warmup_market, &jit_warmup_ctx, jit_outputs.data());
            jit_warmup_sink += jit_outputs[program_output_index];
          }
          (void)jit_warmup_sink;
        }

        // Batch timing used to amortize timer-call overhead (~20-100ns per
        // clock() call) across 64 signal evaluations. Each recorded latency is
        // the mean of one batch.
        const auto jit_start = std::chrono::steady_clock::now();
        ResetAllocationCounter();
        AllocationScope jit_alloc_scope(true);
        for (std::size_t i = 0; i < events; i += kBatch) {
          const std::size_t batch_count = std::min(kBatch, events - i);
          const auto t0 = std::chrono::high_resolution_clock::now();
          for (std::size_t j = 0; j < batch_count; ++j) {
            const auto& ev = replay[i + j];
            jit_market.instruments[ev.instrument_id].bid = ev.bid;
            jit_market.instruments[ev.instrument_id].ask = ev.ask;
            jit_market.current_time_ns = ev.timestamp_ns;
            program_fn(&jit_market, &jit_ctx, jit_outputs.data());
            jit_sink += jit_outputs[program_output_index];
          }
          const auto t1 = std::chrono::high_resolution_clock::now();
          const auto ns = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
          jit_latencies.push_back(ns / batch_count);
        }
        const auto jit_end = std::chrono::steady_clock::now();
        const std::uint64_t jit_allocations = AllocationCount();
        const double jit_sec = std::chrono::duration<double>(jit_end - jit_start).count();
        jit_throughput = static_cast<double>(events) / jit_sec;
        jit_p50 = Percentile(jit_latencies, 0.50);
        jit_p99 = Percentile(jit_latencies, 0.99);
        jit_p999 = Percentile(jit_latencies, 0.999);
        jit_sink_out = jit_sink;
        std::cout << "allocations_jit=" << jit_allocations << "\n";
      } else {
        jit_mode = "compile_failed";
        jit_error = program_jit.LastError();
      }
    } else {
      jit_error = program_jit.LastError();
    }

    std::cout << "jit_mode=" << jit_mode << "\n";
    if (!jit_error.empty()) {
      std::cout << "jit_error=" << jit_error << "\n";
    }
    std::cout << "jit_throughput=" << jit_throughput << "\n";
    std::cout << "jit_lat_ns_p50=" << jit_p50 << "\n";
    std::cout << "jit_lat_ns_p99=" << jit_p99 << "\n";
    std::cout << "jit_lat_ns_p999=" << jit_p999 << "\n";
    std::cout << "jit_sink=" << jit_sink_out << "\n";

    double hw_throughput = std::numeric_limits<double>::quiet_NaN();
    double hw_p50 = std::numeric_limits<double>::quiet_NaN();
    double hw_p99 = std::numeric_limits<double>::quiet_NaN();
    double hw_p999 = std::numeric_limits<double>::quiet_NaN();
    double hw_sink_out = std::numeric_limits<double>::quiet_NaN();

    // Handwritten baseline: direct C++ spread over instrument[0] and [1].
    if (tickers.size() >= 2) {
      std::vector<std::uint64_t> hw_latencies;
      hw_latencies.reserve(events / kBatch + 1);
      volatile double hw_sink = 0.0;
      jitse::MarketState hw_market;
      {
        jitse::MarketState hw_warmup_market;
        jitse::MarketSimulator hw_warmup_sim(99, tickers.size());
        volatile double hw_warmup_sink = 0.0;
        for (std::size_t i = 0; i < kWarmupIters; ++i) {
          const auto ev = hw_warmup_sim.NextEvent(1000);
          hw_warmup_market.instruments[ev.instrument_id].bid = ev.bid;
          hw_warmup_market.instruments[ev.instrument_id].ask = ev.ask;
          const double mid0 = (hw_warmup_market.instruments[0].bid + hw_warmup_market.instruments[0].ask) * 0.5;
          const double mid1 = (hw_warmup_market.instruments[1].bid + hw_warmup_market.instruments[1].ask) * 0.5;
          hw_warmup_sink += (mid0 - mid1);
        }
        (void)hw_warmup_sink;
      }
      // Batch timing used to amortize timer-call overhead (~20-100ns per
      // clock() call) across 64 signal evaluations. Each recorded latency is
      // the mean of one batch.
      const auto hw_start = std::chrono::steady_clock::now();
      for (std::size_t i = 0; i < events; i += kBatch) {
        const std::size_t batch_count = std::min(kBatch, events - i);
        const auto h0 = std::chrono::high_resolution_clock::now();
        for (std::size_t j = 0; j < batch_count; ++j) {
          const auto& ev = replay[i + j];
          hw_market.instruments[ev.instrument_id].bid = ev.bid;
          hw_market.instruments[ev.instrument_id].ask = ev.ask;
          const double mid0 = (hw_market.instruments[0].bid + hw_market.instruments[0].ask) * 0.5;
          const double mid1 = (hw_market.instruments[1].bid + hw_market.instruments[1].ask) * 0.5;
          hw_sink += (mid0 - mid1);
        }
        const auto h1 = std::chrono::high_resolution_clock::now();
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(h1 - h0).count());
        hw_latencies.push_back(ns / batch_count);
      }
      const auto hw_end = std::chrono::steady_clock::now();
      const double hw_sec = std::chrono::duration<double>(hw_end - hw_start).count();
      hw_throughput = static_cast<double>(events) / hw_sec;
      hw_p50 = Percentile(hw_latencies, 0.50);
      hw_p99 = Percentile(hw_latencies, 0.99);
      hw_p999 = Percentile(hw_latencies, 0.999);
      hw_sink_out = hw_sink;
      std::cout << "hw_throughput=" << hw_throughput << "\n";
      std::cout << "hw_lat_ns_p50=" << hw_p50 << "\n";
      std::cout << "hw_lat_ns_p99=" << hw_p99 << "\n";
      std::cout << "hw_lat_ns_p999=" << hw_p999 << "\n";
      std::cout << "hw_sink=" << hw_sink_out << "\n";
    }

    if (!csv_out.empty()) {
      const bool exists = static_cast<bool>(std::ifstream(csv_out));
      std::ofstream out(csv_out, std::ios::app);
      if (!out) throw std::runtime_error("Failed to open csv output: " + csv_out);
      if (!exists) {
        out << "signal,events,throughput,lat_ns_p50,lat_ns_p99,lat_ns_p999,sink,jit_mode,jit_throughput,jit_lat_ns_p50,jit_lat_ns_p99,jit_lat_ns_p999,jit_sink,hw_throughput,hw_lat_ns_p50,hw_lat_ns_p99,hw_lat_ns_p999,hw_sink\n";
      }
      out << (all_signals_mode ? std::string("<all_signals>") : signal->name) << "," << events << "," << (static_cast<double>(events) / sec) << ","
          << Percentile(latencies, 0.50) << "," << Percentile(latencies, 0.99) << ","
          << Percentile(latencies, 0.999) << "," << sink << ","
          << jit_mode << "," << jit_throughput << "," << jit_p50 << "," << jit_p99 << "," << jit_p999 << ","
          << jit_sink_out << ","
          << hw_throughput << "," << hw_p50 << "," << hw_p99 << "," << hw_p999 << "," << hw_sink_out << "\n";
    }
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 2;
  }
}
