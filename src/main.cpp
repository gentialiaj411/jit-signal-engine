#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

#include "ast_printer.h"
#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "lexer.h"
#include "market_sim.h"
#include "parser.h"
#include "signal_backend.h"
#include "signal_program.h"

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("Failed to open signal file: " + path);
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

double PercentileNs(std::vector<std::uint64_t> samples, double p) {
  if (samples.empty()) return 0.0;
  std::sort(samples.begin(), samples.end());
  const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
  return static_cast<double>(samples[idx]);
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

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      std::cout << "Usage: jit_signal_engine [--print-ast] [--dump-ir] [--dump-ir-pre] [--all-signals] [--pin-core N] <signal_file> [events] [signal_name]\n";
      return 1;
    }
    bool print_ast = false;
    bool dump_ir = false;
    bool dump_ir_pre = false;
    bool all_signals_mode = false;
    bool pin_requested = false;
    std::size_t pin_core = 0;
    std::vector<std::string> positional;
    positional.reserve(static_cast<std::size_t>(argc));
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--print-ast") {
        print_ast = true;
      } else if (arg == "--dump-ir") {
        dump_ir = true;
      } else if (arg == "--dump-ir-pre") {
        dump_ir_pre = true;
      } else if (arg == "--all-signals") {
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
    const std::string signal_path = positional[0];
    const std::size_t num_events =
        (positional.size() >= 2) ? static_cast<std::size_t>(std::stoull(positional[1])) : 100000;
    const std::string selected_signal = (positional.size() >= 3) ? positional[2] : "";
    if (pin_requested) {
      const bool pinned = PinCurrentThreadToCore(pin_core);
      std::cout << "thread_pinned=" << (pinned ? "true" : "false") << "\n";
      if (pinned) std::cout << "thread_core=" << pin_core << "\n";
    }

    const std::string src = ReadFile(signal_path);
    std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
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
      if (!found) {
        throw std::runtime_error("Requested signal not found: " + selected_signal);
      }
    }
    if (print_ast) {
      jitse::AstPrinter printer(std::cout);
      if (all_signals_mode) {
        for (const auto& s : signals) {
          std::cout << "signal " << s.name << ":\n";
          printer.Print(*s.body);
        }
      } else {
        printer.Print(*signal->body);
      }
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
    std::unordered_set<std::string> unique_tickers;
    if (all_signals_mode) {
      for (const auto& s : signals) {
        const auto tickers = jitse::CollectTickerSymbols(s);
        for (const auto& t : tickers) {
          symbols.RegisterOrGetId(t);
          unique_tickers.insert(t);
        }
      }
      if (unique_tickers.empty()) {
        symbols.RegisterOrGetId("AAPL");
        unique_tickers.insert("AAPL");
      }
    } else {
      const auto tickers = jitse::CollectTickerSymbols(*signal);
      for (const auto& t : tickers) {
        symbols.RegisterOrGetId(t);
        unique_tickers.insert(t);
      }
      if (tickers.empty()) {
        symbols.RegisterOrGetId("AAPL");
        unique_tickers.insert("AAPL");
      }
    }

    jitse::MarketState market;
    jitse::SignalContext ctx;
    if (all_signals_mode) {
      for (const auto& s : signals) {
        jitse::PrewarmSignalContext(ctx, s);
      }
    } else {
      jitse::PrewarmSignalContext(ctx, *signal);
    }
    jitse::Interpreter interp(symbols);
    std::size_t instrument_count = 1;
    instrument_count = std::max<std::size_t>(1, unique_tickers.size());
    std::vector<jitse::MarketEvent> replay;
    replay.reserve(num_events);
    {
      jitse::MarketSimulator sim(12345, instrument_count);
      for (std::size_t i = 0; i < num_events; ++i) {
        replay.push_back(sim.NextEvent(1000));
      }
    }

    auto backend = jitse::CreateLlvmBackend();
    jitse::JitCompiler program_jit;
    jitse::JitCompiler::ProgramFn program_fn = nullptr;
    bool use_jit = false;
    std::string jit_error;
    std::cout << "llvm_jit_available=" << (backend->IsAvailable() ? "true" : "false") << "\n";
    if (backend->IsAvailable()) {
      if (program_jit.CompileProgram(signals, symbols)) {
        program_fn = program_jit.GetProgramFunction();
        use_jit = (program_fn != nullptr);
        if (dump_ir_pre) program_jit.DumpLastIRPreOpt();
        if (dump_ir) program_jit.DumpLastIR();
      } else {
        jit_error = program_jit.LastError();
      }
    } else {
      jit_error = backend->LastError();
    }
    std::cout << "execution_mode=" << (use_jit ? "jit" : "interpreter") << "\n";
    if (!use_jit && !jit_error.empty()) {
      std::cout << "jit_fallback_reason=" << jit_error << "\n";
    }

    constexpr std::size_t kWarmupIters = 10000;
    constexpr std::size_t kBatch = 64;
    std::vector<std::uint64_t> samples_ns;
    samples_ns.reserve(num_events / kBatch + 1);
    double sink = 0.0;
    std::vector<double> outputs(signals.size(), 0.0);
    std::size_t program_output_index = signals.size() - 1;
    if (!all_signals_mode) {
      for (std::size_t i = 0; i < signals.size(); ++i) {
        if (&signals[i] == signal) {
          program_output_index = i;
          break;
        }
      }
    }

    {
      jitse::MarketState warmup_market;
      jitse::SignalContext warmup_ctx;
      if (all_signals_mode) {
        for (const auto& s : signals) {
          jitse::PrewarmSignalContext(warmup_ctx, s);
        }
      } else {
        jitse::PrewarmSignalContext(warmup_ctx, *signal);
      }
      jitse::MarketSimulator warmup_sim(99, instrument_count);
      volatile double warmup_sink = 0.0;
      for (std::size_t i = 0; i < kWarmupIters; ++i) {
        const jitse::MarketEvent ev = warmup_sim.NextEvent(1000);
        warmup_market.instruments[ev.instrument_id].bid = ev.bid;
        warmup_market.instruments[ev.instrument_id].ask = ev.ask;
        warmup_market.current_time_ns = ev.timestamp_ns;
        if (use_jit) {
          program_fn(&warmup_market, &warmup_ctx, outputs.data());
          warmup_sink += outputs[program_output_index];
        } else if (all_signals_mode) {
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
    const auto bench_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < num_events; i += kBatch) {
      const std::size_t batch_count = std::min(kBatch, num_events - i);
      const auto t0 = std::chrono::high_resolution_clock::now();
      for (std::size_t j = 0; j < batch_count; ++j) {
        const jitse::MarketEvent& ev = replay[i + j];
        market.instruments[ev.instrument_id].bid = ev.bid;
        market.instruments[ev.instrument_id].ask = ev.ask;
        market.current_time_ns = ev.timestamp_ns;
        if (use_jit) {
          program_fn(&market, &ctx, outputs.data());
          sink += outputs[program_output_index];
        } else if (all_signals_mode) {
          for (const auto& s : signals) {
            sink += interp.Evaluate(s, market, ctx);
          }
        } else {
          sink += interp.Evaluate(*signal, market, ctx);
        }
      }
      const auto t1 = std::chrono::high_resolution_clock::now();
      const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      samples_ns.push_back(ns / batch_count);
    }
    const auto bench_end = std::chrono::steady_clock::now();
    const double elapsed_s = std::chrono::duration<double>(bench_end - bench_start).count();

    const double p50 = PercentileNs(samples_ns, 0.50);
    const double p99 = PercentileNs(samples_ns, 0.99);
    const double p999 = PercentileNs(samples_ns, 0.999);
    const double throughput = static_cast<double>(num_events) / elapsed_s;

    std::cout << "signal=" << (all_signals_mode ? std::string("<all_signals>") : signal->name) << "\n";
    std::cout << "events=" << num_events << "\n";
    std::cout << "throughput_eval_per_sec=" << throughput << "\n";
    std::cout << "latency_ns_p50=" << p50 << "\n";
    std::cout << "latency_ns_p99=" << p99 << "\n";
    std::cout << "latency_ns_p999=" << p999 << "\n";
    std::cout << "sink=" << sink << "\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 2;
  }
}
