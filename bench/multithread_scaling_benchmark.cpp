#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef __linux__
#include <sched.h>
#endif

#include "ast_utils.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "multithread_eval.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

bool PinCurrentThreadToCore(std::size_t core) {
#ifdef __linux__
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(static_cast<int>(core), &cpuset);
  return sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0;
#else
  (void)core;
  return false;
#endif
}

double Percentile(std::vector<std::uint64_t> xs, double p) {
  std::sort(xs.begin(), xs.end());
  if (xs.empty()) return 0.0;
  const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(xs.size() - 1));
  return static_cast<double>(xs[idx]);
}

struct ProgramFixture {
  jitse::JitCompiler jit;
  std::vector<jitse::SignalDef> signals;
  jitse::SymbolTable symbols;
  jitse::JitCompiler::ProgramFn fn = nullptr;

  ProgramFixture() {
  const std::string src =
      "signal short_ma = ema(mid(AAPL), 10)\n"
      "signal long_ma = ema(mid(AAPL), 60)\n"
      "signal vol = rolling_std(mid(AAPL), 30)\n"
      "signal raw = short_ma - long_ma\n"
      "signal filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0\n";
  const auto parsed = jitse::ParseSignalProgram(src);
  signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(signals);
  symbols.RegisterOrGetId("AAPL");
  for (auto& s : signals) jitse::BindSymbolIds(s, symbols);
  if (!jit.IsAvailable() || !jit.CompileProgram(signals, symbols) || jit.GetProgramFunction() == nullptr) {
    throw std::runtime_error("LLVM JIT unavailable");
  }
  fn = jit.GetProgramFunction();
  }
};

struct SharedPassState {
  std::vector<jitse::MarketState>* markets = nullptr;
  std::vector<jitse::MarketSimulator>* sims = nullptr;
  jitse::JitCompiler::ProgramFn fn = nullptr;
  std::size_t outputs_per_symbol = 0;
  double* outputs = nullptr;
  std::size_t pin_core_base = 2;
};

void RunOnePassShard(
    const jitse::SymbolShard& shard,
    jitse::MultiSymbolSignalContext& thread_arena,
    const SharedPassState& shared) {
  const std::size_t osp = shared.outputs_per_symbol;
  for (std::size_t global_s = shard.begin; global_s < shard.end; ++global_s) {
    const auto ev = (*shared.sims)[global_s].NextEvent(1000);
    auto& m = (*shared.markets)[global_s];
    m.instruments[0].bid = ev.bid;
    m.instruments[0].ask = ev.ask;
    m.current_time_ns = ev.timestamp_ns;
    const std::uint32_t local_s = static_cast<std::uint32_t>(global_s - shard.begin);
    shared.fn(&m, &thread_arena, local_s, shared.outputs + global_s * osp);
  }
}

struct BenchResult {
  std::size_t threads = 1;
  std::size_t symbols = 0;
  double duration_sec = 0.0;
  std::size_t passes = 0;
  double throughput_symbols_per_sec = 0.0;
  double pass_lat_ns_p50 = 0.0;
  double pass_lat_ns_p99 = 0.0;
};

BenchResult RunBenchmark(
    const ProgramFixture& fx,
    std::size_t n_symbols,
    std::size_t n_threads,
    double min_duration_sec,
    std::size_t pin_core_base) {
  if (n_threads < 1) {
    throw std::runtime_error("n_threads must be >= 1");
  }
  if (n_threads > n_symbols) {
    throw std::runtime_error("n_threads cannot exceed n_symbols");
  }

  std::vector<jitse::MarketState> markets(n_symbols);
  std::vector<jitse::MarketSimulator> sims;
  sims.reserve(n_symbols);
  for (std::size_t s = 0; s < n_symbols; ++s) {
    sims.emplace_back(static_cast<std::uint64_t>(9000 + s), 1);
  }

  const std::size_t osp = fx.signals.size();
  std::vector<double> outputs(n_symbols * osp, 0.0);

  // Warmup (single-threaded full sweep).
  jitse::MultiSymbolSignalContext warmup_arena(n_symbols);
  for (std::size_t s = 0; s < n_symbols; ++s) {
    for (const auto& sig : fx.signals) {
      jitse::PrewarmSignalContext(warmup_arena, static_cast<std::uint32_t>(s), sig);
    }
  }
  SharedPassState shared{&markets, &sims, fx.fn, osp, outputs.data(), pin_core_base};
  const jitse::SymbolShard full_shard{0, n_symbols};
  for (std::size_t w = 0; w < 50; ++w) {
    RunOnePassShard(full_shard, warmup_arena, shared);
  }

  std::vector<std::uint64_t> pass_lat_ns;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(min_duration_sec);

  if (n_threads == 1) {
    jitse::MultiSymbolSignalContext arena(n_symbols);
    for (std::size_t s = 0; s < n_symbols; ++s) {
      for (const auto& sig : fx.signals) {
        jitse::PrewarmSignalContext(arena, static_cast<std::uint32_t>(s), sig);
      }
    }
    PinCurrentThreadToCore(pin_core_base);
    const auto start = std::chrono::steady_clock::now();
    std::size_t passes = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto t0 = std::chrono::high_resolution_clock::now();
      jitse::SymbolShard full{0, n_symbols};
      RunOnePassShard(full, arena, shared);
      const auto t1 = std::chrono::high_resolution_clock::now();
      pass_lat_ns.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
      ++passes;
    }
    const auto end = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(end - start).count();
    BenchResult r;
    r.threads = 1;
    r.symbols = n_symbols;
    r.duration_sec = sec;
    r.passes = passes;
    r.throughput_symbols_per_sec = static_cast<double>(n_symbols) * static_cast<double>(passes) / sec;
    r.pass_lat_ns_p50 = Percentile(pass_lat_ns, 0.50);
    r.pass_lat_ns_p99 = Percentile(pass_lat_ns, 0.99);
    return r;
  }

  struct ThreadCtx {
    jitse::SymbolShard shard;
    jitse::MultiSymbolSignalContext arena;
    explicit ThreadCtx(std::size_t shard_size) : arena(shard_size) {}
  };

  std::vector<ThreadCtx> ctxs;
  ctxs.reserve(n_threads);
  for (std::size_t t = 0; t < n_threads; ++t) {
    auto shard = jitse::ComputeSymbolShard(n_symbols, n_threads, t);
    ctxs.emplace_back(shard.Size());
    ctxs.back().shard = shard;
    for (std::size_t local_s = 0; local_s < shard.Size(); ++local_s) {
      for (const auto& sig : fx.signals) {
        jitse::PrewarmSignalContext(ctxs.back().arena, static_cast<std::uint32_t>(local_s), sig);
      }
    }
  }

  const auto start = std::chrono::steady_clock::now();
  std::size_t passes = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> workers;
    workers.reserve(n_threads);
    for (std::size_t t = 0; t < n_threads; ++t) {
      workers.emplace_back([&, t]() {
        PinCurrentThreadToCore(pin_core_base + t);
        RunOnePassShard(ctxs[t].shard, ctxs[t].arena, shared);
      });
    }
    for (auto& w : workers) {
      w.join();
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    pass_lat_ns.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    ++passes;
  }
  const auto end = std::chrono::steady_clock::now();
  const double sec = std::chrono::duration<double>(end - start).count();
  BenchResult r;
  r.threads = n_threads;
  r.symbols = n_symbols;
  r.duration_sec = sec;
  r.passes = passes;
  r.throughput_symbols_per_sec = static_cast<double>(n_symbols) * static_cast<double>(passes) / sec;
  r.pass_lat_ns_p50 = Percentile(pass_lat_ns, 0.50);
  r.pass_lat_ns_p99 = Percentile(pass_lat_ns, 0.99);
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t n_symbols = 10000;
  std::size_t pin_core_base = 2;
  double min_duration_sec = 60.0;
  std::vector<std::size_t> thread_counts = {1, 2, 4, 8, 16};

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--symbols" && i + 1 < argc) {
      n_symbols = static_cast<std::size_t>(std::stoull(argv[++i]));
    } else if (arg == "--seconds" && i + 1 < argc) {
      min_duration_sec = std::stod(argv[++i]);
    } else if (arg == "--pin-core-base" && i + 1 < argc) {
      pin_core_base = static_cast<std::size_t>(std::stoull(argv[++i]));
    } else if (arg == "--threads" && i + 1 < argc) {
      thread_counts.clear();
      thread_counts.push_back(static_cast<std::size_t>(std::stoull(argv[++i])));
    } else if (arg == "--thread-sweep") {
      thread_counts = {1, 2, 4, 8, 16};
    } else {
      std::cerr << "Usage: multithread_scaling_benchmark [--symbols N] [--seconds S] "
                   "[--pin-core-base C] [--threads N | --thread-sweep]\n";
      return 1;
    }
  }

  try {
    const ProgramFixture fx;
    std::cout << "n_symbols=" << n_symbols << "\n";
    std::cout << "min_duration_sec=" << min_duration_sec << "\n";
    std::cout << "pin_core_base=" << pin_core_base << "\n";
    std::cout << "outputs_per_symbol=" << fx.signals.size() << "\n";

    double baseline_throughput = 0.0;
    for (std::size_t tcount : thread_counts) {
      if (tcount > n_symbols) {
        std::cerr << "skip_threads=" << tcount << " (exceeds n_symbols)\n";
        continue;
      }
      if (pin_core_base + tcount > 24) {
        std::cerr << "skip_threads=" << tcount << " (pin_core_base + threads exceeds 24 cpus)\n";
        continue;
      }
      const BenchResult r = RunBenchmark(fx, n_symbols, tcount, min_duration_sec, pin_core_base);
      if (tcount == 1) {
        baseline_throughput = r.throughput_symbols_per_sec;
      }
      const double scaling_vs_1 = baseline_throughput > 0.0
                                    ? r.throughput_symbols_per_sec / baseline_throughput
                                    : 0.0;
      const double efficiency_pct =
          (tcount > 0 && baseline_throughput > 0.0)
              ? (r.throughput_symbols_per_sec / (baseline_throughput * static_cast<double>(tcount))) * 100.0
              : 0.0;
      std::cout << "threads=" << r.threads << " passes=" << r.passes << " duration_sec=" << r.duration_sec
                << " throughput_symbols_per_sec=" << r.throughput_symbols_per_sec
                << " scaling_vs_1thread=" << scaling_vs_1 << " efficiency_pct=" << efficiency_pct
                << " pass_lat_ns_p50=" << r.pass_lat_ns_p50 << " pass_lat_ns_p99=" << r.pass_lat_ns_p99 << "\n";
    }
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 2;
  }
}
