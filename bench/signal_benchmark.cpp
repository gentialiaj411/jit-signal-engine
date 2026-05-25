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

double Percentile(const std::vector<double>& xs, double p) {
  if (xs.empty()) return 0.0;
  std::vector<double> sorted = xs;
  std::sort(sorted.begin(), sorted.end());
  const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
  return sorted[idx];
}

struct TimedRunResult {
  double throughput = 0.0;
  double lat_p50 = 0.0;
  double lat_p99 = 0.0;
  double lat_p999 = 0.0;
  double sink = 0.0;
};

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
      std::cerr << "Usage: signal_benchmark [--all-signals] [--pin-core N] [--measure-runs N] "
                   "[--lower-stateful=<none|all|sma,ema,lag>] "
                   "[--tier=<baseline|specialized>] "
                   "<signal_file> [events] [csv_out] [signal_name]\n";
      return 1;
    }
    bool all_signals_mode = false;
    bool pin_requested = false;
    std::size_t pin_core = 0;
    std::size_t measure_runs = 1;
    // P1 tier selection. "baseline" = current behavior (warmup branches kept).
    // "specialized" = tiered JIT: compile baseline, run warmup ticks, Promote
    // to the branch-stripped specialized function, then measure. The reported
    // jit_throughput reflects the specialized function in the measured window.
    std::string tier = "baseline";
    // P0: choose which stateful ops the JIT lowers inline into IR.
    // CLI overrides JITSE_LOWER_STATEFUL env var (the env var sets the default).
    auto parse_lowering = [](const std::string& spec) -> jitse::StatefulLoweringFlags {
      if (spec == "none" || spec == "off" || spec == "0" || spec.empty()) return jitse::StatefulLoweringFlags::kNone;
      if (spec == "all" || spec == "1") return jitse::StatefulLoweringFlags::kAll;
      jitse::StatefulLoweringFlags flags = jitse::StatefulLoweringFlags::kNone;
      std::string token;
      for (std::size_t i = 0; i <= spec.size(); ++i) {
        const char c = (i < spec.size()) ? spec[i] : ',';
        if (c == ',' || c == ' ') {
          if (token == "sma") flags = flags | jitse::StatefulLoweringFlags::kSma;
          else if (token == "ema") flags = flags | jitse::StatefulLoweringFlags::kEma;
          else if (token == "lag") flags = flags | jitse::StatefulLoweringFlags::kLag;
          token.clear();
        } else {
          token.push_back(c);
        }
      }
      return flags;
    };
    bool lowering_overridden = false;
    jitse::StatefulLoweringFlags lowering = jitse::StatefulLoweringFlags::kNone;
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
      } else if (arg == "--measure-runs") {
        if (i + 1 >= argc) throw std::runtime_error("--measure-runs requires a count");
        measure_runs = static_cast<std::size_t>(std::stoull(argv[++i]));
        if (measure_runs < 1) throw std::runtime_error("--measure-runs must be >= 1");
      } else if (arg.rfind("--lower-stateful=", 0) == 0) {
        lowering = parse_lowering(arg.substr(std::string("--lower-stateful=").size()));
        lowering_overridden = true;
      } else if (arg == "--lower-stateful") {
        if (i + 1 >= argc) throw std::runtime_error("--lower-stateful requires an arg");
        lowering = parse_lowering(argv[++i]);
        lowering_overridden = true;
      } else if (arg.rfind("--tier=", 0) == 0) {
        tier = arg.substr(std::string("--tier=").size());
      } else if (arg == "--tier") {
        if (i + 1 >= argc) throw std::runtime_error("--tier requires an arg");
        tier = argv[++i];
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
    // P1: when the specialized tier is selected, the tiered JIT runs a
    // whole-program static analysis to identify warm-safe stateful nodes,
    // which requires consistent, program-wide unique node IDs across all
    // signals. AllocateNodeIds (per-signal) starts at 1 for each signal and
    // would collide in the shared SignalContext; AllocateProgramNodeIds
    // assigns a single global ID space. The interpreter and the baseline JIT
    // tolerate per-signal IDs when only one signal is exercised, but the
    // specialized JIT does not. Use program-wide IDs when --tier=specialized.
    const bool tier_specialized_requested =
        (tier == "specialized" || tier == "spec" || tier == "tiered");
    if (tier_specialized_requested || all_signals_mode) {
      max_node_id = jitse::AllocateProgramNodeIds(signals);
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
    if (all_signals_mode || tier_specialized_requested) {
      for (auto& s : signals) jitse::BindSymbolIds(s, symbols);
    } else {
      jitse::BindSymbolIds(*signal, symbols);
    }

    jitse::Interpreter interp(symbols);
    jitse::SignalContext ctx;
    if (all_signals_mode || tier_specialized_requested) {
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
      if (all_signals_mode || tier_specialized_requested) {
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

    const auto run_interp_measurement = [&]() -> TimedRunResult {
      jitse::SignalContext measure_ctx;
      if (all_signals_mode || tier_specialized_requested) {
        for (const auto& s : signals) {
          jitse::PrewarmSignalContext(measure_ctx, s);
        }
      } else {
        jitse::PrewarmSignalContext(measure_ctx, *signal);
      }
      jitse::MarketState measure_market;
      std::vector<std::uint64_t> measure_latencies;
      measure_latencies.reserve(events / kBatch + 1);
      volatile double measure_sink = 0.0;
      const auto start = std::chrono::steady_clock::now();
      for (std::size_t i = 0; i < events; i += kBatch) {
        const std::size_t batch_count = std::min(kBatch, events - i);
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (std::size_t j = 0; j < batch_count; ++j) {
          const auto& ev = replay[i + j];
          measure_market.instruments[ev.instrument_id].bid = ev.bid;
          measure_market.instruments[ev.instrument_id].ask = ev.ask;
          measure_market.current_time_ns = ev.timestamp_ns;
          if (all_signals_mode) {
            for (const auto& s : signals) {
              measure_sink += interp.Evaluate(s, measure_market, measure_ctx);
            }
          } else {
            measure_sink += interp.Evaluate(*signal, measure_market, measure_ctx);
          }
        }
        const auto t1 = std::chrono::high_resolution_clock::now();
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        measure_latencies.push_back(ns / batch_count);
      }
      const auto end = std::chrono::steady_clock::now();
      const double sec = std::chrono::duration<double>(end - start).count();
      TimedRunResult result;
      result.throughput = static_cast<double>(events) / sec;
      result.lat_p50 = Percentile(measure_latencies, 0.50);
      result.lat_p99 = Percentile(measure_latencies, 0.99);
      result.lat_p999 = Percentile(measure_latencies, 0.999);
      result.sink = measure_sink;
      return result;
    };

    std::vector<double> interp_throughput_runs;
    interp_throughput_runs.reserve(measure_runs);
    std::vector<double> interp_p99_runs;
    interp_p99_runs.reserve(measure_runs);
    TimedRunResult interp_result{};
    ResetAllocationCounter();
    AllocationScope interp_alloc_scope(true);
    for (std::size_t run = 0; run < measure_runs; ++run) {
      interp_result = run_interp_measurement();
      interp_throughput_runs.push_back(interp_result.throughput);
      interp_p99_runs.push_back(interp_result.lat_p99);
      if (measure_runs > 1) {
        std::cout << "measure_interp_run=" << run << " throughput=" << interp_result.throughput
                  << " lat_ns_p99=" << interp_result.lat_p99 << "\n";
      }
    }
    const std::uint64_t interp_allocations = AllocationCount();
    latencies.clear();  // legacy CSV path uses final-run latencies only when measure_runs==1
    sink = interp_result.sink;

    std::cout << "signal=" << (all_signals_mode ? std::string("<all_signals>") : signal->name) << "\n";
    std::cout << "events=" << events << "\n";
    std::cout << "measure_runs=" << measure_runs << "\n";
    std::cout << "throughput=" << interp_result.throughput << "\n";
    std::cout << "lat_ns_p50=" << interp_result.lat_p50 << "\n";
    std::cout << "lat_ns_p99=" << interp_result.lat_p99 << "\n";
    std::cout << "lat_ns_p999=" << interp_result.lat_p999 << "\n";
    std::cout << "sink=" << sink << "\n";
    std::cout << "allocations_interp=" << interp_allocations << "\n";
    if (measure_runs > 1) {
      std::cout << "interp_throughput_median=" << Percentile(interp_throughput_runs, 0.50) << "\n";
      std::cout << "interp_throughput_p99=" << Percentile(interp_throughput_runs, 0.99) << "\n";
      std::cout << "interp_lat_ns_p99_median=" << Percentile(interp_p99_runs, 0.50) << "\n";
      std::cout << "interp_lat_ns_p99_p99=" << Percentile(interp_p99_runs, 0.99) << "\n";
    }

    // JIT path (auto-fallback if LLVM is unavailable or compile fails).
    double jit_throughput = std::numeric_limits<double>::quiet_NaN();
    double jit_p50 = std::numeric_limits<double>::quiet_NaN();
    double jit_p99 = std::numeric_limits<double>::quiet_NaN();
    double jit_p999 = std::numeric_limits<double>::quiet_NaN();
    double jit_sink_out = std::numeric_limits<double>::quiet_NaN();
    std::string jit_mode = "unavailable";
    std::string jit_error;

    const bool specialized_tier = (tier == "specialized" || tier == "spec" || tier == "tiered");
    std::cout << "tier=" << (specialized_tier ? "specialized" : "baseline") << "\n";

    jitse::JitCompiler program_jit;
    jitse::TieredProgramJit tjit;  // only used when specialized_tier
    if (lowering_overridden) {
      program_jit.SetStatefulLowering(lowering);
    }
    {
      const auto eff = program_jit.GetStatefulLowering();
      std::cout << "lower_stateful_sma=" << (jitse::HasFlag(eff, jitse::StatefulLoweringFlags::kSma) ? "1" : "0") << "\n";
      std::cout << "lower_stateful_ema=" << (jitse::HasFlag(eff, jitse::StatefulLoweringFlags::kEma) ? "1" : "0") << "\n";
      std::cout << "lower_stateful_lag=" << (jitse::HasFlag(eff, jitse::StatefulLoweringFlags::kLag) ? "1" : "0") << "\n";
    }
    jitse::JitCompiler::ProgramFn program_fn = nullptr;
    // For specialized tier, we keep `baseline_fn` to run the per-measure-run
    // warmup against (the specialized fn would produce undefined values if
    // invoked on a fresh SignalContext). `program_fn` is set to the
    // specialized fn for the measured loop.
    jitse::JitCompiler::ProgramFn baseline_fn = nullptr;
    std::int64_t specialized_warmup_ticks = 0;
    std::size_t program_output_index = signals.size() - 1;
    if (!all_signals_mode) {
      for (std::size_t i = 0; i < signals.size(); ++i) {
        if (&signals[i] == signal) {
          program_output_index = i;
          break;
        }
      }
    }

    const auto compile_for_tier = [&]() -> bool {
      if (!specialized_tier) {
        if (!program_jit.IsAvailable()) {
          jit_error = program_jit.LastError();
          return false;
        }
        if (!program_jit.CompileProgram(signals, symbols)) {
          jit_mode = "compile_failed";
          jit_error = program_jit.LastError();
          return false;
        }
        program_fn = program_jit.GetProgramFunction();
        baseline_fn = program_fn;
        return program_fn != nullptr;
      }
      if (!tjit.IsAvailable()) {
        jit_error = "tiered jit unavailable";
        return false;
      }
      // Use the same lowering selection (default kAll if not overridden, so the
      // specialization actually has something to strip).
      const jitse::StatefulLoweringFlags eff_lowering = lowering_overridden
          ? lowering
          : jitse::StatefulLoweringFlags::kAll;
      if (!tjit.Compile(signals, symbols, eff_lowering)) {
        jit_mode = "compile_failed";
        jit_error = "tiered baseline: " + tjit.LastError();
        return false;
      }
      baseline_fn = tjit.CurrentFunction();
      specialized_warmup_ticks = tjit.WarmupTickThreshold();
      if (!tjit.Promote()) {
        jit_mode = "compile_failed";
        jit_error = "tiered specialized: " + tjit.LastError();
        return false;
      }
      program_fn = tjit.CurrentFunction();
      std::cout << "specialized_warmup_ticks=" << specialized_warmup_ticks << "\n";
      return program_fn != nullptr && baseline_fn != nullptr;
    };

    if (compile_for_tier()) {
        jit_mode = "enabled";
        std::vector<std::uint64_t> jit_latencies;
        jit_latencies.reserve(events / kBatch + 1);
        volatile double jit_sink = 0.0;
        std::vector<double> jit_outputs(signals.size(), 0.0);
        jitse::MultiSymbolSignalContext jit_ctx(1);
        if (all_signals_mode || tier_specialized_requested) {
          for (const auto& s : signals) {
            jitse::PrewarmSignalContext(jit_ctx, 0, s);
          }
        } else {
          jitse::PrewarmSignalContext(jit_ctx, 0, *signal);
        }
        jitse::MarketState jit_market;
        {
          jitse::MultiSymbolSignalContext jit_warmup_ctx(1);
          if (all_signals_mode || tier_specialized_requested) {
            for (const auto& s : signals) {
              jitse::PrewarmSignalContext(jit_warmup_ctx, 0, s);
            }
          } else {
            jitse::PrewarmSignalContext(jit_warmup_ctx, 0, *signal);
          }
          jitse::MarketState jit_warmup_market;
          jitse::MarketSimulator jit_warmup_sim(99, instrument_count);
          volatile double jit_warmup_sink = 0.0;
          // For specialized tier: first run `specialized_warmup_ticks` on the
          // baseline fn to warm jit_warmup_ctx, then exercise the specialized
          // fn for the remaining iterations (also amortizes icache warm-up of
          // the specialized fn). For baseline tier, baseline_fn == program_fn.
          for (std::size_t i = 0; i < kWarmupIters; ++i) {
            const auto ev = jit_warmup_sim.NextEvent(1000);
            jit_warmup_market.instruments[ev.instrument_id].bid = ev.bid;
            jit_warmup_market.instruments[ev.instrument_id].ask = ev.ask;
            jit_warmup_market.current_time_ns = ev.timestamp_ns;
            auto* warm_fn = (specialized_tier && static_cast<std::int64_t>(i) < specialized_warmup_ticks)
                ? baseline_fn : program_fn;
            warm_fn(&jit_warmup_market, &jit_warmup_ctx, 0, jit_outputs.data());
            jit_warmup_sink += jit_outputs[program_output_index];
          }
          (void)jit_warmup_sink;
        }

        std::vector<double> jit_throughput_runs;
        jit_throughput_runs.reserve(measure_runs);
        std::vector<double> jit_p99_runs;
        jit_p99_runs.reserve(measure_runs);
        std::uint64_t jit_allocations = 0;
        for (std::size_t run = 0; run < measure_runs; ++run) {
          jitse::MultiSymbolSignalContext measure_jit_ctx(1);
          if (all_signals_mode || tier_specialized_requested) {
            for (const auto& s : signals) {
              jitse::PrewarmSignalContext(measure_jit_ctx, 0, s);
            }
          } else {
            jitse::PrewarmSignalContext(measure_jit_ctx, 0, *signal);
          }
          jitse::MarketState measure_jit_market;
          std::vector<std::uint64_t> measure_jit_latencies;
          measure_jit_latencies.reserve(events / kBatch + 1);
          volatile double measure_jit_sink = 0.0;

          // Specialized tier: per-run baseline warmup against measure_jit_ctx
          // to bring every warm-safe stateful node into steady state before
          // the timed loop starts. This MUST happen against the same ctx
          // we'll measure on (the specialized fn would otherwise read
          // uninitialized state on the first invocation).
          if (specialized_tier && specialized_warmup_ticks > 0) {
            jitse::MarketSimulator warmup_sim(7, instrument_count);
            for (std::int64_t w = 0; w < specialized_warmup_ticks; ++w) {
              const auto ev = warmup_sim.NextEvent(1000);
              measure_jit_market.instruments[ev.instrument_id].bid = ev.bid;
              measure_jit_market.instruments[ev.instrument_id].ask = ev.ask;
              measure_jit_market.current_time_ns = ev.timestamp_ns;
              baseline_fn(&measure_jit_market, &measure_jit_ctx, 0, jit_outputs.data());
            }
          }

          const auto jit_start = std::chrono::steady_clock::now();
          ResetAllocationCounter();
          AllocationScope jit_alloc_scope(true);
          for (std::size_t i = 0; i < events; i += kBatch) {
            const std::size_t batch_count = std::min(kBatch, events - i);
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (std::size_t j = 0; j < batch_count; ++j) {
              const auto& ev = replay[i + j];
              measure_jit_market.instruments[ev.instrument_id].bid = ev.bid;
              measure_jit_market.instruments[ev.instrument_id].ask = ev.ask;
              measure_jit_market.current_time_ns = ev.timestamp_ns;
              program_fn(&measure_jit_market, &measure_jit_ctx, 0, jit_outputs.data());
              measure_jit_sink += jit_outputs[program_output_index];
            }
            const auto t1 = std::chrono::high_resolution_clock::now();
            const auto ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            measure_jit_latencies.push_back(ns / batch_count);
          }
          const auto jit_end = std::chrono::steady_clock::now();
          jit_allocations = AllocationCount();
          const double jit_sec = std::chrono::duration<double>(jit_end - jit_start).count();
          const double run_throughput = static_cast<double>(events) / jit_sec;
          const double run_p99 = Percentile(measure_jit_latencies, 0.99);
          jit_throughput_runs.push_back(run_throughput);
          jit_p99_runs.push_back(run_p99);
          jit_throughput = run_throughput;
          jit_p50 = Percentile(measure_jit_latencies, 0.50);
          jit_p99 = run_p99;
          jit_p999 = Percentile(measure_jit_latencies, 0.999);
          jit_sink_out = measure_jit_sink;
          if (measure_runs > 1) {
            std::cout << "measure_jit_run=" << run << " throughput=" << run_throughput
                      << " lat_ns_p99=" << run_p99 << "\n";
          }
        }
        std::cout << "allocations_jit=" << jit_allocations << "\n";
        if (measure_runs > 1) {
          std::cout << "jit_throughput_median=" << Percentile(jit_throughput_runs, 0.50) << "\n";
          std::cout << "jit_throughput_p99=" << Percentile(jit_throughput_runs, 0.99) << "\n";
          std::cout << "jit_lat_ns_p99_median=" << Percentile(jit_p99_runs, 0.50) << "\n";
          std::cout << "jit_lat_ns_p99_p99=" << Percentile(jit_p99_runs, 0.99) << "\n";
          std::vector<double> speedups;
          speedups.reserve(measure_runs);
          for (std::size_t i = 0; i < measure_runs; ++i) {
            if (interp_throughput_runs[i] > 0.0) {
              speedups.push_back(jit_throughput_runs[i] / interp_throughput_runs[i]);
            }
          }
          if (!speedups.empty()) {
            std::cout << "speedup_median=" << Percentile(speedups, 0.50) << "\n";
            std::cout << "speedup_p99=" << Percentile(speedups, 0.99) << "\n";
          }
        }
    }
    // jit_mode / jit_error are already set by compile_for_tier() on failure.

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
      out << (all_signals_mode ? std::string("<all_signals>") : signal->name) << "," << events << "," << interp_result.throughput << ","
          << interp_result.lat_p50 << "," << interp_result.lat_p99 << ","
          << interp_result.lat_p999 << "," << sink << ","
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
