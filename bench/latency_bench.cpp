// latency_bench.cpp
//
// P4 deliverable: per-call latency distribution artifact for one signal
// program, measured under either a closed-loop "as-fast-as-possible" mode
// or an open-loop coordinated-omission-aware mode.
//
// Why this exists (and why it's separate from signal_benchmark):
//   signal_benchmark.cpp records latency as `batch_ns / 64` -- the average
//   over 64 consecutive calls. That collapses tail samples into the mean
//   and is exactly the "loop-time-tick pattern that hides tail latency"
//   our diagnostic called out. P4 measures each call individually and
//   reports the full distribution. The two harnesses can coexist: this
//   one is the high-fidelity artifact; signal_benchmark stays the
//   throughput-oriented sweep.
//
// Methodology:
//   * Per-call timing via clock_gettime(CLOCK_MONOTONIC) on Linux /
//     std::chrono::steady_clock elsewhere. We use chrono uniformly for
//     portability; on Linux glibc it dispatches to clock_gettime VDSO
//     which is the same instruction stream as a raw syscall.
//   * Two modes:
//       - closed-loop (default): call fn() in a tight loop, record
//         elapsed for each call. Measures the JIT's intrinsic per-call
//         cost.
//       - open-loop CO-aware (--rate-hz=R): each event has a target
//         arrival time spaced 1/R seconds apart. We spin-wait until that
//         time, run fn(), and record `completion - target`. If a stall
//         pushes us behind, target_t keeps advancing, so the next event's
//         latency includes the queueing delay -- the standard
//         Coordinated-Omission correction (wrk2 / HdrHistogram pattern).
//   * Sample size: defaults to 1,000,000 events so p99.99 has ~100
//     samples in the tail.
//
// Outputs (--out-dir):
//   {stem}_latency_histogram.csv  -- raw bucket data (interp + JIT)
//   {stem}_latency_histogram.md   -- p50..p99.999 summary table
//   {stem}_latency_histogram.svg  -- overlaid CDFs

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "latency_histogram.h"
#include "lexer.h"
#include "market_sim.h"
#include "parser.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

struct Args {
  std::string signal_file;
  std::size_t events = 1'000'000;
  std::size_t warmup = 50'000;
  double rate_hz = 0.0;  // 0 == closed loop
  bool jit_only = false;
  bool interp_only = false;
  std::string out_dir;
};

[[noreturn]] void PrintUsageAndExit(const char* prog) {
  std::cerr << "Usage: " << prog
            << " <signal_file>"
            << " [--events=N] [--warmup=N]"
            << " [--rate-hz=R]"
            << " [--jit-only|--interp-only]"
            << " [--out-dir=DIR]\n";
  std::exit(2);
}

Args ParseArgs(int argc, char** argv) {
  Args a;
  std::vector<std::string> pos;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    if (s.rfind("--events=", 0) == 0) a.events = std::stoull(s.substr(9));
    else if (s.rfind("--warmup=", 0) == 0) a.warmup = std::stoull(s.substr(9));
    else if (s.rfind("--rate-hz=", 0) == 0) a.rate_hz = std::stod(s.substr(10));
    else if (s == "--jit-only") a.jit_only = true;
    else if (s == "--interp-only") a.interp_only = true;
    else if (s.rfind("--out-dir=", 0) == 0) a.out_dir = s.substr(10);
    else if (s == "-h" || s == "--help") PrintUsageAndExit(argv[0]);
    else pos.push_back(s);
  }
  if (pos.size() != 1) PrintUsageAndExit(argv[0]);
  a.signal_file = pos[0];
  return a;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open: " + path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct CompiledProgram {
  std::vector<jitse::SignalDef> signals;
  jitse::SymbolTable symbols;
  std::unique_ptr<jitse::JitCompiler> jit;
  jitse::JitCompiler::ProgramFn fn = nullptr;
};

CompiledProgram BuildAndCompile(const std::string& src) {
  CompiledProgram cp;
  cp.jit = std::make_unique<jitse::JitCompiler>();
  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  cp.signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(cp.signals);
  for (const auto& s : cp.signals)
    for (const auto& t : jitse::CollectTickerSymbols(s)) cp.symbols.RegisterOrGetId(t);
  for (auto& s : cp.signals) jitse::BindSymbolIds(s, cp.symbols);
  if (cp.jit->IsAvailable() &&
      cp.jit->CompileProgram(cp.signals, cp.symbols)) {
    cp.fn = cp.jit->GetProgramFunction();
  }
  return cp;
}

// Returns the current monotonic clock in ns since some unspecified epoch.
inline std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Spin-wait until NowNs() >= target_ns. Uses pause/yield mix for fairness.
inline void SpinUntil(std::uint64_t target_ns) {
  while (NowNs() < target_ns) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
  }
}

// Per-call latency loop. `interval_ns == 0` means closed-loop (record
// completion-time minus loop-start-of-call time). Nonzero means
// CO-aware open-loop: target advances by interval_ns each iteration; the
// recorded latency is completion-time minus that target.
template <typename Fn>
void RunLatencyLoop(const std::vector<jitse::MarketEvent>& events,
                    jitse::MarketState& market,
                    jitse::LatencyHistogram& hist,
                    std::uint64_t interval_ns,
                    Fn&& body /* (event) */) {
  if (interval_ns == 0) {
    for (const auto& ev : events) {
      const std::uint64_t t0 = NowNs();
      market.instruments[ev.instrument_id].bid = ev.bid;
      market.instruments[ev.instrument_id].ask = ev.ask;
      market.current_time_ns = ev.timestamp_ns;
      body(ev);
      const std::uint64_t t1 = NowNs();
      hist.Add(t1 - t0);
    }
  } else {
    // CO-aware open loop.
    const std::uint64_t t_start = NowNs();
    std::uint64_t target = t_start;
    for (const auto& ev : events) {
      // Only spin if we're early. If we've already fallen behind target,
      // we record large latencies for these queued-up events; that's the
      // CO correction.
      const std::uint64_t now = NowNs();
      if (now < target) SpinUntil(target);
      market.instruments[ev.instrument_id].bid = ev.bid;
      market.instruments[ev.instrument_id].ask = ev.ask;
      market.current_time_ns = ev.timestamp_ns;
      body(ev);
      const std::uint64_t done = NowNs();
      hist.Add(done - target);
      target += interval_ns;
    }
  }
}

struct ConfigResult {
  std::string label;
  jitse::LatencyHistogram hist;
  double wall_seconds = 0.0;
  volatile double sink = 0.0;
};

}  // namespace

int main(int argc, char** argv) try {
  const Args args = ParseArgs(argc, argv);
  const std::string src = ReadFile(args.signal_file);
  const std::string stem =
      std::filesystem::path(args.signal_file).stem().string();

  std::cout << "signal=" << args.signal_file
            << " events=" << args.events
            << " warmup=" << args.warmup;
  if (args.rate_hz > 0) std::cout << " rate_hz=" << args.rate_hz;
  std::cout << "\n";

  CompiledProgram cp = BuildAndCompile(src);
  const std::size_t num_signals = cp.signals.size();
  const std::size_t output_index = num_signals - 1;
  const bool jit_available = cp.fn != nullptr;

  // Pre-generate event stream so timing measures fn-only work.
  jitse::MarketSimulator sim(/*seed=*/0xFEEDFACEull, /*instruments=*/4);
  std::vector<jitse::MarketEvent> events_vec;
  events_vec.reserve(args.events + args.warmup);
  for (std::size_t i = 0; i < args.events + args.warmup; ++i) {
    events_vec.push_back(sim.NextEvent(1000));
  }
  const std::size_t warmup_n = args.warmup;
  const std::size_t measure_n = args.events;
  std::vector<jitse::MarketEvent> warmup_events(
      events_vec.begin(), events_vec.begin() + warmup_n);
  std::vector<jitse::MarketEvent> measure_events(
      events_vec.begin() + warmup_n, events_vec.end());

  const std::uint64_t interval_ns =
      args.rate_hz > 0.0
          ? static_cast<std::uint64_t>(1e9 / args.rate_hz + 0.5)
          : 0;

  std::vector<ConfigResult> results;

  // ---- Interpreter ----
  if (!args.jit_only) {
    ConfigResult r;
    r.label = "interpreter";
    jitse::Interpreter interp(cp.symbols);
    jitse::MultiSymbolSignalContext interp_arena(1);
    for (const auto& sig : cp.signals)
      jitse::PrewarmSignalContext(interp_arena, 0, sig);
    jitse::MarketState m{};
    volatile double s_sink = 0.0;
    // Warmup.
    for (const auto& ev : warmup_events) {
      m.instruments[ev.instrument_id].bid = ev.bid;
      m.instruments[ev.instrument_id].ask = ev.ask;
      m.current_time_ns = ev.timestamp_ns;
      double last = 0.0;
      for (const auto& sig : cp.signals)
        last = interp.Evaluate(sig, m, interp_arena, /*symbol_id=*/0);
      s_sink += last;
    }
    // Measurement.
    const auto t0 = std::chrono::steady_clock::now();
    RunLatencyLoop(measure_events, m, r.hist, interval_ns,
                   [&](const jitse::MarketEvent&) {
                     double last = 0.0;
                     for (const auto& sig : cp.signals)
                       last = interp.Evaluate(sig, m, interp_arena, /*symbol_id=*/0);
                     s_sink += last;
                   });
    const auto t1 = std::chrono::steady_clock::now();
    r.wall_seconds = std::chrono::duration<double>(t1 - t0).count();
    r.sink = s_sink;
    results.push_back(std::move(r));
  }

  // ---- JIT ----
  if (!args.interp_only && jit_available) {
    ConfigResult r;
    r.label = "jit";
    jitse::MultiSymbolSignalContext arena(1);
    for (const auto& sig : cp.signals)
      jitse::PrewarmSignalContext(arena, 0, sig);
    std::vector<double> outs(num_signals, 0.0);
    jitse::MarketState m{};
    volatile double s_sink = 0.0;
    // Warmup.
    for (const auto& ev : warmup_events) {
      m.instruments[ev.instrument_id].bid = ev.bid;
      m.instruments[ev.instrument_id].ask = ev.ask;
      m.current_time_ns = ev.timestamp_ns;
      cp.fn(&m, &arena, 0, outs.data());
      s_sink += outs[output_index];
    }
    // Measurement.
    const auto t0 = std::chrono::steady_clock::now();
    RunLatencyLoop(measure_events, m, r.hist, interval_ns,
                   [&](const jitse::MarketEvent&) {
                     cp.fn(&m, &arena, 0, outs.data());
                     s_sink += outs[output_index];
                   });
    const auto t1 = std::chrono::steady_clock::now();
    r.wall_seconds = std::chrono::duration<double>(t1 - t0).count();
    r.sink = s_sink;
    results.push_back(std::move(r));
  } else if (!args.interp_only && !jit_available) {
    std::cerr << "WARNING: JIT unavailable, skipping JIT measurement\n";
  }

  // ---- Print stdout summary ----
  for (const auto& r : results) {
    r.hist.WriteMarkdownSummary(std::cout, "[" + r.label + "]");
    std::cout << "  wall_s=" << r.wall_seconds
              << "  throughput_eps=" << (measure_n / r.wall_seconds)
              << "  sink=" << r.sink << "\n";
  }

  // ---- Artifacts ----
  if (!args.out_dir.empty()) {
    std::filesystem::create_directories(args.out_dir);
    {
      std::ofstream csv(args.out_dir + "/" + stem + "_latency_histogram.csv");
      bool first = true;
      for (const auto& r : results) {
        // Write a single combined CSV with a "label" column.
        std::ostringstream s;
        r.hist.WriteCsv(s, r.label);
        const std::string body = s.str();
        if (first) {
          csv << body;
          first = false;
        } else {
          // Skip the header from the second series; the label column lets
          // a reader distinguish rows.
          std::size_t nl = body.find('\n');
          if (nl != std::string::npos) csv << body.substr(nl + 1);
        }
      }
    }
    {
      std::ofstream md(args.out_dir + "/" + stem + "_latency_histogram.md");
      md << "# Latency distribution: `" << stem << "`\n\n";
      md << "Events: " << measure_n << "  \n";
      md << "Warmup: " << warmup_n << "  \n";
      md << "Mode: "
         << (interval_ns == 0
                 ? "closed-loop (back-to-back calls)"
                 : ("open-loop CO-aware @ " +
                    std::to_string(args.rate_hz) + " Hz"))
         << "  \n";
      md << "Source: `" << args.signal_file << "`  \n";
      md << "Methodology: per-call timing via "
            "`std::chrono::steady_clock::now()`. The histogram is "
            "HdrHistogram-style log-linear with 4 precision bits "
            "(~6% bucket width). See "
            "[`docs/latency_distribution.md`](../../../docs/latency_distribution.md) "
            "for full methodology.\n\n";
      for (const auto& r : results) r.hist.WriteMarkdownSummary(md, r.label);
      md << "## Throughput cross-check\n\n";
      md << "| Configuration | Wall time (s) | Throughput (events/s) | Sink |\n";
      md << "|---|---:|---:|---:|\n";
      for (const auto& r : results) {
        md << "| " << r.label << " | " << r.wall_seconds
           << " | " << (measure_n / r.wall_seconds)
           << " | " << r.sink << " |\n";
      }
      md << "\n## CDF\n\n";
      md << "![CDF](" << stem << "_latency_histogram.svg)\n";
    }
    {
      std::ofstream svg(args.out_dir + "/" + stem + "_latency_histogram.svg");
      std::vector<const jitse::LatencyHistogram*> series;
      std::vector<std::string> labels;
      for (const auto& r : results) {
        series.push_back(&r.hist);
        labels.push_back(r.label);
      }
      jitse::LatencyHistogram::WriteSvgCdf(svg, series, labels);
    }
    std::cout << "wrote artifacts to " << args.out_dir << "/\n";
  }

  return 0;
} catch (const std::exception& ex) {
  std::cerr << "error: " << ex.what() << "\n";
  return 1;
}
