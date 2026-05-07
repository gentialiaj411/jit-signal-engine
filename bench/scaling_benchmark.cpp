#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "interpreter.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_backend.h"
#include "jit_compiler.h"
#include "signal_program.h"

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open: " + path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      std::cerr << "Usage: scaling_benchmark [--all-signals] <signal_file> [events] [signal_name]\n";
      return 1;
    }
    bool all_signals_mode = false;
    std::vector<std::string> positional;
    positional.reserve(static_cast<std::size_t>(argc));
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--all-signals") {
        all_signals_mode = true;
      } else {
        positional.push_back(arg);
      }
    }
    if (positional.empty()) {
      throw std::runtime_error("Missing <signal_file> argument");
    }
    const std::string signal_file = positional[0];
    const std::size_t events = (positional.size() >= 2) ? static_cast<std::size_t>(std::stoull(positional[1])) : 200000;
    const std::string selected_signal = (positional.size() >= 3) ? positional[2] : "";

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

    const std::vector<std::size_t> requested_scales = {100, 500, 1000, 5000};
    std::cout << "signal=" << (all_signals_mode ? std::string("<all_signals>") : signal->name) << "\n";
    std::cout << "events_per_scale=" << events << "\n";
    std::cout << "columns=n_instruments,mode,throughput_eval_per_sec,avg_latency_ns,sink\n";

    for (const std::size_t n : requested_scales) {
      if (n > jitse::kMaxInstruments) {
        std::cout << n << ",skipped,nan,nan,nan\n";
        continue;
      }
      jitse::SymbolTable symbols;
      // Register synthetic symbol set S0..S(n-1) and required tickers used in the signal.
      for (std::size_t i = 0; i < n; ++i) symbols.RegisterOrGetId("S" + std::to_string(i));
      if (all_signals_mode) {
        for (const auto& s : signals) {
          for (const auto& t : jitse::CollectTickerSymbols(s)) symbols.RegisterOrGetId(t);
        }
      } else {
        for (const auto& t : jitse::CollectTickerSymbols(*signal)) symbols.RegisterOrGetId(t);
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
      jitse::MarketSimulator sim(1337, n);
      constexpr std::size_t kWarmupIters = 10000;
      constexpr std::size_t kBatchSize = 64;

      volatile double warmup_sink = 0.0;
      jitse::MarketState warmup_market;
      jitse::SignalContext warmup_ctx;
      if (all_signals_mode) {
        for (const auto& s : signals) {
          jitse::PrewarmSignalContext(warmup_ctx, s);
        }
      } else {
        jitse::PrewarmSignalContext(warmup_ctx, *signal);
      }
      jitse::MarketSimulator warmup_sim(7331, n);
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

      // Batch timing used to amortize timer-call overhead (~20-100ns per
      // clock() call) across 64 signal evaluations. Each recorded latency is
      // the mean of one batch.
      const auto t0 = std::chrono::steady_clock::now();
      volatile double sink = 0.0;
      for (std::size_t i = 0; i < events; i += kBatchSize) {
        const std::size_t batch_end = std::min(i + kBatchSize, events);
        for (std::size_t j = i; j < batch_end; ++j) {
          const auto ev = sim.NextEvent(1000);
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
      }
      const auto t1 = std::chrono::steady_clock::now();
      const double sec = std::chrono::duration<double>(t1 - t0).count();
      const double throughput = static_cast<double>(events) / sec;
      const double avg_latency_ns = (sec * 1e9) / static_cast<double>(events);
      std::cout << n << ",interpreter," << throughput << "," << avg_latency_ns << "," << sink << "\n";

      auto backend = jitse::CreateLlvmBackend();
      jitse::JitCompiler program_jit;
      jitse::JitCompiler::ProgramFn program_fn = nullptr;
      if (all_signals_mode) {
        if (!program_jit.IsAvailable() || !program_jit.CompileProgram(signals, symbols) ||
            (program_fn = program_jit.GetProgramFunction()) == nullptr) {
          std::cout << n << ",jit_unavailable,nan,nan,nan\n";
          continue;
        }
      } else if (!backend->IsAvailable() || !backend->Compile(*signal, symbols) || backend->GetFunction() == nullptr) {
        std::cout << n << ",jit_unavailable,nan,nan,nan\n";
        continue;
      }

      auto fn = backend->GetFunction();
      jitse::MarketState jit_market;
      jitse::SignalContext jit_ctx;
      if (all_signals_mode) {
        for (const auto& s : signals) {
          jitse::PrewarmSignalContext(jit_ctx, s);
        }
      } else {
        jitse::PrewarmSignalContext(jit_ctx, *signal);
      }
      jitse::MarketSimulator jit_sim(1337, n);
      volatile double jit_warmup_sink = 0.0;
      jitse::MarketState jit_warmup_market;
      jitse::SignalContext jit_warmup_ctx;
      if (all_signals_mode) {
        for (const auto& s : signals) {
          jitse::PrewarmSignalContext(jit_warmup_ctx, s);
        }
      } else {
        jitse::PrewarmSignalContext(jit_warmup_ctx, *signal);
      }
      jitse::MarketSimulator jit_warmup_sim(7331, n);
      std::vector<double> outputs;
      if (all_signals_mode) {
        outputs.assign(signals.size(), 0.0);
      }
      for (std::size_t i = 0; i < kWarmupIters; ++i) {
        const auto ev = jit_warmup_sim.NextEvent(1000);
        jit_warmup_market.instruments[ev.instrument_id].bid = ev.bid;
        jit_warmup_market.instruments[ev.instrument_id].ask = ev.ask;
        jit_warmup_market.current_time_ns = ev.timestamp_ns;
        if (all_signals_mode) {
          std::fill(outputs.begin(), outputs.end(), 0.0);
          program_fn(&jit_warmup_market, &jit_warmup_ctx, outputs.data());
          jit_warmup_sink += outputs.back();
        } else {
          jit_warmup_sink += fn(&jit_warmup_market, &jit_warmup_ctx);
        }
      }
      (void)jit_warmup_sink;

      // Batch timing used to amortize timer-call overhead (~20-100ns per
      // clock() call) across 64 signal evaluations. Each recorded latency is
      // the mean of one batch.
      const auto j0 = std::chrono::steady_clock::now();
      volatile double jit_sink = 0.0;
      for (std::size_t i = 0; i < events; i += kBatchSize) {
        const std::size_t batch_end = std::min(i + kBatchSize, events);
        for (std::size_t j = i; j < batch_end; ++j) {
          const auto ev = jit_sim.NextEvent(1000);
          jit_market.instruments[ev.instrument_id].bid = ev.bid;
          jit_market.instruments[ev.instrument_id].ask = ev.ask;
          jit_market.current_time_ns = ev.timestamp_ns;
          if (all_signals_mode) {
            std::fill(outputs.begin(), outputs.end(), 0.0);
            program_fn(&jit_market, &jit_ctx, outputs.data());
            jit_sink += outputs.back();
          } else {
            jit_sink += fn(&jit_market, &jit_ctx);
          }
        }
      }
      const auto j1 = std::chrono::steady_clock::now();
      const double jsec = std::chrono::duration<double>(j1 - j0).count();
      const double jthroughput = static_cast<double>(events) / jsec;
      const double jlat_ns = (jsec * 1e9) / static_cast<double>(events);
      std::cout << n << ",jit," << jthroughput << "," << jlat_ns << "," << jit_sink << "\n";
    }
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 2;
  }
}
