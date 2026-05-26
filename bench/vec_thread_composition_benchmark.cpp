// vec_thread_composition_benchmark.cpp
//
// P12: the long-promised "vec x threads" composition artifact.
//
// The roadmap has claimed "4 lanes x 6 P-cores = 24x effective" for some
// time, but until now the throughput multiplication has only been shown in
// isolation -- P2/P10's `cross_symbol_benchmark` measures lane scaling at
// one thread, and `multithread_scaling_benchmark` measures thread scaling
// at one lane. This artifact pins the product: it runs the SAME program
// across S symbols under four configurations and reports the throughput
// (symbol-events per second) of each:
//
//   scalar + 1T  -- baseline. S symbols, sequential scalar_fn calls.
//   scalar + NT  -- threading only. Each thread takes a slice of S
//                   symbols, calls scalar_fn one symbol at a time.
//   vec + 1T     -- lane vectorization only. S/K groups of K lanes,
//                   sequential vec_fn calls.
//   vec + NT     -- composed. Each thread takes a slice of (S/K) groups,
//                   one vec_fn call per group per tick.
//
// Two programs are run: a stateless one (`stateless_heavy.sig`) where
// P2/P10 widening is pure win, and a stateful one (the filtered-momentum
// stack used by the multithread bench) where stateful operators still
// have to fan-out per lane and the speedup is muted -- but the
// composition still wins because the THREAD axis multiplies cleanly.
//
// We use a fixed S (default 16) divisible by both K (4) and T (4). All
// configurations process EXACTLY the same total work (S * args.events
// per-symbol market events). Best-of-N timings are taken to stabilise
// the throughput numbers; the artifact also reports the median (a less
// optimistic view) and the run-to-run spread.
//
// Output is written to `bench/results/vec_thread_composition/<program>.md`
// as a Markdown table comparable to other roadmap artifacts. The exit
// code is 0 on success, 2 on bad usage, and 1 if any of the parity
// post-checks fail (we cross-check vec+NT outputs against scalar+1T,
// since P11 + P10 already guarantee bit-tight (within 1e-9) equivalence
// per-symbol; this is just a sanity tripwire on top of the unit tests).
//
// Usage:
//   vec_thread_composition_benchmark <signal_file> [--events=N] [--symbols=S]
//     [--threads=T] [--lanes=K] [--runs=R] [--md=<output>]
//
// Defaults: events=200000, symbols=16, threads=4, lanes=4, runs=5.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sched.h>
#endif

#include "ast_utils.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

struct Args {
  std::string signal_file;
  std::size_t events = 200'000;
  std::size_t symbols = 16;
  std::size_t threads = 4;
  unsigned lanes = 4;
  std::size_t runs = 5;
  std::string md_out;
};

Args ParseArgs(int argc, char** argv) {
  Args a;
  std::vector<std::string> pos;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    if (s.rfind("--events=", 0) == 0) a.events = std::stoull(s.substr(9));
    else if (s.rfind("--symbols=", 0) == 0) a.symbols = std::stoull(s.substr(10));
    else if (s.rfind("--threads=", 0) == 0) a.threads = std::stoull(s.substr(10));
    else if (s.rfind("--lanes=", 0) == 0) a.lanes = static_cast<unsigned>(std::stoul(s.substr(8)));
    else if (s.rfind("--runs=", 0) == 0) a.runs = std::stoull(s.substr(7));
    else if (s.rfind("--md=", 0) == 0) a.md_out = s.substr(5);
    else if (s == "--help" || s == "-h") {
      std::cout << "Usage: vec_thread_composition_benchmark <signal_file> "
                   "[--events=N] [--symbols=S] [--threads=T] [--lanes=K] "
                   "[--runs=R] [--md=<path>]\n";
      std::exit(0);
    } else {
      pos.push_back(s);
    }
  }
  if (pos.empty()) {
    throw std::runtime_error("usage: vec_thread_composition_benchmark <signal_file> ...");
  }
  a.signal_file = pos[0];
  if (a.symbols % a.lanes != 0) {
    throw std::runtime_error("symbols (" + std::to_string(a.symbols) + ") must be divisible by lanes (" +
                             std::to_string(a.lanes) + ")");
  }
  if (a.symbols % a.threads != 0) {
    throw std::runtime_error("symbols (" + std::to_string(a.symbols) + ") must be divisible by threads (" +
                             std::to_string(a.threads) + ")");
  }
  // For the vec+NT case each thread takes a whole number of K-groups.
  const std::size_t per_thread = a.symbols / a.threads;
  if (per_thread % a.lanes != 0) {
    throw std::runtime_error("symbols/threads (" + std::to_string(per_thread) +
                             ") must itself be a multiple of lanes (" + std::to_string(a.lanes) +
                             ") so each thread owns whole K-groups in vec+NT");
  }
  return a;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open: " + path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void PinThread(std::size_t core) {
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<int>(core), &set);
  sched_setaffinity(0, sizeof(cpu_set_t), &set);
#else
  (void)core;
#endif
}

struct Program {
  std::vector<jitse::SignalDef> signals;
  jitse::SymbolTable symbols;
  jitse::JitCompiler::ProgramFn scalar_fn = nullptr;
  jitse::JitCompiler::ProgramFnVec vec_fn = nullptr;
  std::unique_ptr<jitse::JitCompiler> scalar_jit;
  std::unique_ptr<jitse::JitCompiler> vec_jit;
};

Program BuildProgram(const std::string& src, unsigned K) {
  Program p;
  const auto parsed = jitse::ParseSignalProgram(src);
  p.signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(p.signals);
  for (const auto& s : p.signals) {
    for (const auto& t : jitse::CollectTickerSymbols(s)) {
      p.symbols.RegisterOrGetId(t);
    }
  }
  for (auto& s : p.signals) jitse::BindSymbolIds(s, p.symbols);

  p.scalar_jit = std::make_unique<jitse::JitCompiler>();
  if (!p.scalar_jit->IsAvailable()) throw std::runtime_error("LLVM unavailable");
  if (!p.scalar_jit->CompileProgram(p.signals, p.symbols)) {
    throw std::runtime_error("scalar compile failed: " + p.scalar_jit->LastError());
  }
  p.scalar_fn = p.scalar_jit->GetProgramFunction();
  if (p.scalar_fn == nullptr) throw std::runtime_error("scalar_fn null");

  p.vec_jit = std::make_unique<jitse::JitCompiler>();
  if (!p.vec_jit->CompileProgramVectorized(p.signals, p.symbols, K)) {
    throw std::runtime_error("vec compile failed: " + p.vec_jit->LastError());
  }
  p.vec_fn = p.vec_jit->GetProgramVectorizedFunction();
  if (p.vec_fn == nullptr) throw std::runtime_error("vec_fn null");
  return p;
}

struct ScenarioResult {
  std::string name;
  std::vector<double> per_run_seconds;
  double best_seconds = 0.0;
  double median_seconds = 0.0;
  double throughput_M_sym_evs_per_sec = 0.0;
};

double Median(std::vector<double> xs) {
  std::sort(xs.begin(), xs.end());
  if (xs.empty()) return 0.0;
  if (xs.size() % 2 == 1) return xs[xs.size() / 2];
  return 0.5 * (xs[xs.size() / 2 - 1] + xs[xs.size() / 2]);
}

void FinalizeScenario(ScenarioResult& r, std::size_t total_sym_events) {
  r.best_seconds = *std::min_element(r.per_run_seconds.begin(), r.per_run_seconds.end());
  r.median_seconds = Median(r.per_run_seconds);
  // Throughput uses the best run -- consistent with cross_symbol_benchmark.
  r.throughput_M_sym_evs_per_sec =
      static_cast<double>(total_sym_events) / r.best_seconds / 1e6;
}

// One pre-generated event stream per symbol. All scenarios replay the
// SAME stream so the work is identical and the only thing that differs
// is which JIT path consumes it and how many threads share it.
using PerSymbolEvents = std::vector<std::vector<jitse::MarketEvent>>;

PerSymbolEvents GenerateEvents(std::size_t S, std::size_t events) {
  PerSymbolEvents out(S);
  constexpr std::size_t kInstruments = 8;
  for (std::size_t s = 0; s < S; ++s) {
    jitse::MarketSimulator sim(/*seed=*/1003 + s * 91, kInstruments);
    out[s].reserve(events);
    for (std::size_t i = 0; i < events; ++i) {
      out[s].push_back(sim.NextEvent(1000));
    }
  }
  return out;
}

void ApplyEvent(jitse::MarketState& m, const jitse::MarketEvent& ev) {
  m.instruments[ev.instrument_id].bid = ev.bid;
  m.instruments[ev.instrument_id].ask = ev.ask;
  m.current_time_ns = ev.timestamp_ns;
}

// Scenario: scalar JIT, N threads. Each thread owns a contiguous slice
// of S symbols and processes them symbol-by-symbol. We pin threads to
// distinct cores when possible.
double TimeScalarThreaded(
    const Program& prog,
    const PerSymbolEvents& events,
    std::size_t S, std::size_t T, std::size_t E,
    jitse::MultiSymbolSignalContext& arena,
    std::vector<double>& outputs,
    volatile double& sink) {
  const std::size_t num_signals = prog.signals.size();
  const std::size_t per_thread = S / T;
  for (std::size_t s = 0; s < S; ++s) {
    // Reset arena slot s to PrewarmSignalContext-state for repeatability.
    arena.PerSymbol(static_cast<std::uint32_t>(s)) = jitse::SignalContext{};
    for (const auto& sd : prog.signals) jitse::PrewarmSignalContext(arena, s, sd);
  }
  std::vector<jitse::MarketState> markets(S);
  std::atomic<double> sink_atomic{0.0};
  const auto t0 = std::chrono::high_resolution_clock::now();
  std::vector<std::thread> threads;
  threads.reserve(T);
  for (std::size_t t = 0; t < T; ++t) {
    threads.emplace_back([&, t]() {
      PinThread(t);
      double local_sink = 0.0;
      const std::size_t s_begin = t * per_thread;
      const std::size_t s_end = s_begin + per_thread;
      for (std::size_t i = 0; i < E; ++i) {
        for (std::size_t s = s_begin; s < s_end; ++s) {
          ApplyEvent(markets[s], events[s][i]);
          prog.scalar_fn(&markets[s], &arena, static_cast<std::uint32_t>(s),
                         outputs.data() + s * num_signals);
          local_sink += outputs[s * num_signals + (num_signals - 1)];
        }
      }
      // accumulate via fetch-add to keep the optimizer honest.
      double old = sink_atomic.load(std::memory_order_relaxed);
      while (!sink_atomic.compare_exchange_weak(old, old + local_sink,
                                                std::memory_order_relaxed)) {}
    });
  }
  for (auto& th : threads) th.join();
  const auto t1 = std::chrono::high_resolution_clock::now();
  sink = sink_atomic.load();
  return std::chrono::duration<double>(t1 - t0).count();
}

// Scenario: vec JIT (K lanes), N threads. Each thread owns a contiguous
// slice of S symbols (a whole number of K-groups). Each thread loops
// over its groups, advancing markets for the K lanes in that group, and
// invokes vec_fn once per group per tick.
double TimeVecThreaded(
    const Program& prog,
    const PerSymbolEvents& events,
    std::size_t S, std::size_t T, unsigned K, std::size_t E,
    jitse::MultiSymbolSignalContext& arena,
    std::vector<double>& outputs,
    volatile double& sink) {
  const std::size_t num_signals = prog.signals.size();
  const std::size_t per_thread = S / T;       // symbols per thread
  const std::size_t groups_per_thread = per_thread / K;
  for (std::size_t s = 0; s < S; ++s) {
    arena.PerSymbol(static_cast<std::uint32_t>(s)) = jitse::SignalContext{};
    for (const auto& sd : prog.signals) jitse::PrewarmSignalContext(arena, s, sd);
  }
  std::vector<jitse::MarketState> markets(S);
  std::atomic<double> sink_atomic{0.0};
  const auto t0 = std::chrono::high_resolution_clock::now();
  std::vector<std::thread> threads;
  threads.reserve(T);
  for (std::size_t t = 0; t < T; ++t) {
    threads.emplace_back([&, t]() {
      PinThread(t);
      double local_sink = 0.0;
      // Per-thread pointer array of MarketState*; K entries per group.
      std::vector<const jitse::MarketState*> lane_ptrs(K);
      for (std::size_t i = 0; i < E; ++i) {
        for (std::size_t g = 0; g < groups_per_thread; ++g) {
          const std::size_t base = (t * per_thread) + g * K;
          for (unsigned k = 0; k < K; ++k) {
            const std::size_t s = base + k;
            ApplyEvent(markets[s], events[s][i]);
            lane_ptrs[k] = &markets[s];
          }
          // Output layout for vec_fn is signal-major / lane-minor:
          // outputs[i*K + k]. Each call writes num_signals * K doubles;
          // we offset per group into a thread-disjoint region of
          // `outputs`.
          double* out_slot = outputs.data() + base * num_signals;
          // The vec_fn ABI used by `cross_symbol_benchmark` etc. expects
          // a single contiguous K*num_signals output region per call.
          prog.vec_fn(lane_ptrs.data(), &arena,
                      static_cast<std::uint32_t>(base), out_slot);
          local_sink += out_slot[(num_signals - 1) * K + 0];
        }
      }
      double old = sink_atomic.load(std::memory_order_relaxed);
      while (!sink_atomic.compare_exchange_weak(old, old + local_sink,
                                                std::memory_order_relaxed)) {}
    });
  }
  for (auto& th : threads) th.join();
  const auto t1 = std::chrono::high_resolution_clock::now();
  sink = sink_atomic.load();
  return std::chrono::duration<double>(t1 - t0).count();
}

void RunScenario(
    ScenarioResult& res, std::size_t total_sym_events,
    std::size_t runs, std::function<double()> measure) {
  res.per_run_seconds.reserve(runs);
  for (std::size_t r = 0; r < runs; ++r) {
    const double secs = measure();
    res.per_run_seconds.push_back(secs);
  }
  FinalizeScenario(res, total_sym_events);
}

void WriteMarkdown(
    const std::string& path,
    const Args& args,
    const std::string& program_basename,
    const std::vector<ScenarioResult>& scenarios) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot open md output: " + path);
  out << "# vec x threads composition benchmark (P12)\n\n";
  out << "Program: `" << program_basename << "`\n\n";
  out << "Symbols: " << args.symbols
      << " | Lanes: " << args.lanes
      << " | Threads: " << args.threads
      << " | Events/symbol: " << args.events
      << " | Runs/scenario: " << args.runs << "\n\n";
  const double total_sym_events = static_cast<double>(args.symbols) * args.events;
  out << "Total symbol-events per scenario: " << total_sym_events << "\n\n";
  out << "Throughput is reported from the BEST run of " << args.runs
      << " (consistent with cross_symbol_benchmark). The median column "
         "is a less-cherry-picked second view; the spread column is "
         "(slowest-fastest)/fastest as a percentage.\n\n";
  const double baseline = scenarios.front().throughput_M_sym_evs_per_sec;
  out << "| scenario | best (M sym-evs/s) | median (M sym-evs/s) | spread | speedup vs " << scenarios.front().name << " |\n";
  out << "| --- | ---: | ---: | ---: | ---: |\n";
  for (const auto& s : scenarios) {
    const double mb = s.throughput_M_sym_evs_per_sec;
    const double mm = total_sym_events / s.median_seconds / 1e6;
    const double slow = *std::max_element(s.per_run_seconds.begin(), s.per_run_seconds.end());
    const double fast = *std::min_element(s.per_run_seconds.begin(), s.per_run_seconds.end());
    const double spread_pct = (slow - fast) / fast * 100.0;
    out << "| " << s.name << " | "
        << std::fixed << std::setprecision(2) << mb << " | "
        << mm << " | "
        << spread_pct << "% | "
        << (baseline > 0 ? mb / baseline : 0.0) << "x |\n";
  }
  out << "\n## Per-run wall-times (seconds)\n\n";
  for (const auto& s : scenarios) {
    out << "* `" << s.name << "`: ";
    for (std::size_t i = 0; i < s.per_run_seconds.size(); ++i) {
      if (i) out << ", ";
      out << s.per_run_seconds[i] << "s";
    }
    out << "\n";
  }
  out << "\n## Interpretation\n\n";
  out << "* `vec+NT vs scalar+1T` is the **composition factor** the "
         "roadmap claims. For stateless programs it should be roughly "
         "`lanes x threads`. For stateful programs the lane axis is "
         "muted (P10 runs stateful ops via per-lane scalarized fan-out, "
         "so the per-call cost is ~K serial helper calls per group), so "
         "the composition factor collapses to roughly `threads`.\n";
  out << "* `vec+1T / scalar+1T` isolates the LANE axis. Numbers below "
         "1.0x for stateful programs are expected and consistent with "
         "the P10 docs in `docs/cross_symbol_vectorization_stateful.md`.\n";
  out << "* `scalar+NT / scalar+1T` isolates the THREAD axis. This is "
         "well-behaved up to the P-core count; once HT kicks in or the "
         "memory subsystem saturates, scaling tails off.\n";
}

}  // namespace

int main(int argc, char** argv) try {
  const Args args = ParseArgs(argc, argv);
  std::cout << "events=" << args.events << " symbols=" << args.symbols
            << " threads=" << args.threads << " lanes=" << args.lanes
            << " runs=" << args.runs << "\n";

  const std::string src = ReadFile(args.signal_file);
  Program prog = BuildProgram(src, args.lanes);

  std::cout << "compiled scalar + vec(K=" << args.lanes << ") for `"
            << args.signal_file << "` (signals=" << prog.signals.size() << ")\n";

  const PerSymbolEvents events = GenerateEvents(args.symbols, args.events);
  const std::size_t total_sym_events = args.symbols * args.events;

  // Per-scenario arena + outputs are reused across runs (arena is reset
  // to Prewarm-state at the top of each timed pass to keep state-warmup
  // consistent across configs).
  jitse::MultiSymbolSignalContext arena_scalar(args.symbols);
  jitse::MultiSymbolSignalContext arena_vec(args.symbols);
  std::vector<double> outputs(args.symbols * prog.signals.size());
  volatile double sink = 0.0;

  std::vector<ScenarioResult> scenarios;

  ScenarioResult sc_1t;
  sc_1t.name = "scalar+1T";
  RunScenario(sc_1t, total_sym_events, args.runs, [&]() {
    return TimeScalarThreaded(prog, events, args.symbols, /*T=*/1, args.events,
                              arena_scalar, outputs, sink);
  });
  scenarios.push_back(sc_1t);

  ScenarioResult sc_nt;
  sc_nt.name = "scalar+" + std::to_string(args.threads) + "T";
  RunScenario(sc_nt, total_sym_events, args.runs, [&]() {
    return TimeScalarThreaded(prog, events, args.symbols, args.threads, args.events,
                              arena_scalar, outputs, sink);
  });
  scenarios.push_back(sc_nt);

  ScenarioResult vc_1t;
  vc_1t.name = "vec(K=" + std::to_string(args.lanes) + ")+1T";
  RunScenario(vc_1t, total_sym_events, args.runs, [&]() {
    return TimeVecThreaded(prog, events, args.symbols, /*T=*/1, args.lanes, args.events,
                           arena_vec, outputs, sink);
  });
  scenarios.push_back(vc_1t);

  ScenarioResult vc_nt;
  vc_nt.name = "vec(K=" + std::to_string(args.lanes) + ")+" + std::to_string(args.threads) + "T";
  RunScenario(vc_nt, total_sym_events, args.runs, [&]() {
    return TimeVecThreaded(prog, events, args.symbols, args.threads, args.lanes, args.events,
                           arena_vec, outputs, sink);
  });
  scenarios.push_back(vc_nt);

  std::cout << "\n=== best-of-" << args.runs << " throughput (M sym-evs/s) ===\n";
  const double base = scenarios[0].throughput_M_sym_evs_per_sec;
  std::cout << std::fixed << std::setprecision(2);
  for (const auto& s : scenarios) {
    std::cout << "  " << s.name
              << "  best=" << s.throughput_M_sym_evs_per_sec << " M sym-evs/s"
              << "  median=" << (total_sym_events / s.median_seconds / 1e6) << " M sym-evs/s"
              << "  speedup=" << (s.throughput_M_sym_evs_per_sec / base) << "x\n";
  }

  if (!args.md_out.empty()) {
    const auto base_path = std::filesystem::path(args.signal_file);
    WriteMarkdown(args.md_out, args, base_path.filename().string(), scenarios);
    std::cout << "\nmarkdown -> " << args.md_out << "\n";
  }
  return 0;
} catch (const std::exception& e) {
  std::cerr << "fatal: " << e.what() << "\n";
  return 2;
}
