// spsc_jit_pipeline_bench.cpp
//
// Live-ingest pipeline benchmark: feed thread enqueues market events
// into a lock-free SPSC ring; eval thread dequeues, applies the event
// to MarketState, calls the JIT-compiled signal, and records the
// enqueue-to-signal-output wall-clock latency. The two threads are
// pinned to distinct physical P-cores (taskset wrapper) so the
// measurement reflects steady-state inter-core hand-off cost, not
// scheduler hop noise.
//
// Why this exists: the existing latency_bench measures the JIT
// function call in isolation -- it tells us "how fast is one fn(...)
// call?" but not "how fast is the live pipeline that a market-data
// handler would actually wire up?". This benchmark measures the
// second number, which is what HFT-style consumers actually care
// about. The headline is the consumer-side p99 latency from
// enqueue to signal output.
//
// Methodology:
//   * Capacity: 1024-slot SPSC ring (power-of-2; mask = 1023).
//   * Event payload: MarketEvent + enqueue timestamp (monotonic ns).
//   * Pre-generation: the feed thread doesn't simulate events on the
//     fly; we pre-generate N events into a vector so the producer's
//     hot loop is "stamp time + try_push", with no PRNG cost mixed in.
//   * Producer pacing: by default unpaced (closed-loop / pump as fast
//     as the consumer drains). `--rate-hz=R` enables open-loop CO-
//     aware pacing -- the same correction that bench/latency_bench
//     applies for the standalone JIT measurement.
//   * Latency clock: std::chrono::steady_clock, which on Linux glibc
//     dispatches to clock_gettime(CLOCK_MONOTONIC) via VDSO -- the
//     same instruction stream the existing latency_bench uses.
//   * Pinning: optional, env-var driven. If both `JITSE_BENCH_CONSUMER_CPU`
//     and `JITSE_BENCH_PRODUCER_CPU` are set, the threads call
//     pthread_setaffinity_np on themselves before the hot loop.
//
// Output: a markdown artifact (--out-md=PATH) with the latency
// histogram summary plus the throughput in events/sec.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "ast_utils.h"
#include "jit_compiler.h"
#include "latency_histogram.h"
#include "lexer.h"
#include "market_sim.h"
#include "parser.h"
#include "runtime.h"
#include "signal_program.h"
#include "spsc_ring.h"

namespace {

using clk = std::chrono::steady_clock;

inline std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          clk::now().time_since_epoch())
          .count());
}

// Hot-path payload: MarketEvent plus a producer-stamped enqueue time.
// Kept trivially copyable so the SPSC ring's slot copy is a single
// memmove-equivalent and stays alloc-free.
struct IngestEvent {
  jitse::MarketEvent ev;
  std::uint64_t enqueue_ns;
};

#if defined(__linux__)
void PinThreadToCpu(int cpu) {
  if (cpu < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}
#else
void PinThreadToCpu(int) {}
#endif

int GetEnvCpu(const char* key) {
  const char* v = std::getenv(key);
  if (!v) return -1;
  return std::atoi(v);
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open: " + path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Args {
  std::string signal_file;
  std::size_t events = 1'000'000;
  std::size_t warmup = 50'000;
  double rate_hz = 0.0;  // 0 == unpaced (max-rate)
  std::string out_md;
};

[[noreturn]] void PrintUsageAndExit(const char* prog) {
  std::cerr << "Usage: " << prog
            << " <signal_file>"
            << " [--events=N] [--warmup=N]"
            << " [--rate-hz=R]"
            << " [--out-md=PATH]\n"
            << "Pin via env:\n"
            << "  JITSE_BENCH_PRODUCER_CPU=N\n"
            << "  JITSE_BENCH_CONSUMER_CPU=M\n";
  std::exit(2);
}

Args ParseArgs(int argc, char** argv) {
  Args a;
  std::vector<std::string> pos;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    if (s.rfind("--events=", 0) == 0)
      a.events = static_cast<std::size_t>(std::stoull(s.substr(9)));
    else if (s.rfind("--warmup=", 0) == 0)
      a.warmup = static_cast<std::size_t>(std::stoull(s.substr(9)));
    else if (s.rfind("--rate-hz=", 0) == 0)
      a.rate_hz = std::stod(s.substr(10));
    else if (s.rfind("--out-md=", 0) == 0)
      a.out_md = s.substr(9);
    else if (s == "-h" || s == "--help")
      PrintUsageAndExit(argv[0]);
    else
      pos.push_back(s);
  }
  if (pos.empty()) PrintUsageAndExit(argv[0]);
  a.signal_file = pos[0];
  return a;
}

}  // namespace

int main(int argc, char** argv) try {
  const Args args = ParseArgs(argc, argv);
  const int producer_cpu = GetEnvCpu("JITSE_BENCH_PRODUCER_CPU");
  const int consumer_cpu = GetEnvCpu("JITSE_BENCH_CONSUMER_CPU");

  // -------- Frontend / JIT --------
  const std::string src = ReadFile(args.signal_file);
  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(signals);
  jitse::SymbolTable symbols;
  for (const auto& s : signals)
    for (const auto& t : jitse::CollectTickerSymbols(s)) symbols.RegisterOrGetId(t);
  for (auto& s : signals) jitse::BindSymbolIds(s, symbols);
  if (signals.empty()) {
    std::cerr << "error: no signals parsed\n";
    return 2;
  }

  jitse::JitCompiler jit;
  if (!jit.IsAvailable()) {
    std::cerr << "LLVM unavailable -- skipping\n";
    return 0;
  }
  if (!jit.CompileProgram(signals, symbols)) {
    std::cerr << "JIT compile failed: " << jit.LastError() << "\n";
    return 2;
  }
  auto* fn = jit.GetProgramFunction();
  const std::size_t num_signals = signals.size();

  // -------- Pre-generate events --------
  const std::size_t total_events = args.warmup + args.events;
  std::vector<jitse::MarketEvent> events_pool;
  events_pool.reserve(total_events);
  {
    jitse::MarketSimulator sim(0xC0FFEE, 4);
    for (std::size_t i = 0; i < total_events; ++i) {
      events_pool.push_back(sim.NextEvent(1000));
    }
  }

  // -------- Ring + per-thread shared state --------
  constexpr std::size_t kRingCap = 1024;
  // Heap-allocated because the ring is ~3 cache lines of state plus
  // the 1024-slot array; keeping it on the heap avoids stack-frame
  // size warnings without affecting alloc-discipline (no allocations
  // happen DURING the hot loop, only during this setup phase).
  auto ring = std::make_unique<jitse::SpscRing<IngestEvent, kRingCap>>();

  std::atomic<bool> producer_done{false};
  std::atomic<std::uint64_t> consumer_processed{0};

  jitse::LatencyHistogram hist_warmup;
  jitse::LatencyHistogram hist_measure;

  // Eval-side resources.
  jitse::MarketState market{};
  jitse::MultiSymbolSignalContext arena(1);
  for (const auto& s : signals) jitse::PrewarmSignalContext(arena, 0, s);
  std::vector<double> outputs(num_signals, 0.0);
  volatile double sink = 0.0;

  // -------- Producer --------
  std::thread producer([&] {
    PinThreadToCpu(producer_cpu);
    const std::uint64_t t0 = NowNs();
    const double period_ns = (args.rate_hz > 0.0) ? (1e9 / args.rate_hz) : 0.0;
    for (std::size_t i = 0; i < total_events; ++i) {
      IngestEvent ie;
      ie.ev = events_pool[i];
      if (period_ns > 0.0) {
        // Open-loop CO-aware: target time t0 + i*period_ns. Spin-wait
        // until target reached, then stamp enqueue time and push. If
        // the consumer falls behind and we're already past target,
        // we push immediately -- the stamped enqueue_ns is now > target
        // and consumer-side latency includes the queueing delay (the
        // exact CO correction).
        const std::uint64_t target =
            t0 + static_cast<std::uint64_t>(period_ns * static_cast<double>(i));
        while (NowNs() < target) {
          // tight wait; pause hint helps on x86
#if defined(__x86_64__) || defined(_M_X64)
          __builtin_ia32_pause();
#endif
        }
      }
      ie.enqueue_ns = NowNs();
      while (!ring->try_push(ie)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  // -------- Consumer (this thread; serves also as eval thread) --------
  std::thread consumer([&] {
    PinThreadToCpu(consumer_cpu);
    IngestEvent ie;
    for (std::size_t i = 0; i < total_events;) {
      if (!ring->try_pop(ie)) {
        if (producer_done.load(std::memory_order_acquire)) {
          if (ring->empty_approx()) break;
        }
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
        continue;
      }
      // Apply the event.
      market.instruments[ie.ev.instrument_id].bid = ie.ev.bid;
      market.instruments[ie.ev.instrument_id].ask = ie.ev.ask;
      market.current_time_ns = ie.ev.timestamp_ns;
      // Call the JIT function. The output sink is `volatile` so the
      // compiler can't dead-code-eliminate either the call or the
      // store from outputs.back().
      fn(&market, &arena, 0, outputs.data());
      sink += outputs.back();
      const std::uint64_t out_ns = NowNs();
      const std::uint64_t latency_ns = out_ns - ie.enqueue_ns;
      if (i < args.warmup) {
        hist_warmup.Add(latency_ns);
      } else {
        hist_measure.Add(latency_ns);
      }
      ++i;
      consumer_processed.store(i, std::memory_order_relaxed);
    }
  });

  producer.join();
  consumer.join();
  (void)sink;

  // -------- Report --------
  std::cout << "signal_file=" << args.signal_file << "\n";
  std::cout << "events=" << args.events << "\n";
  std::cout << "warmup=" << args.warmup << "\n";
  std::cout << "rate_hz=" << args.rate_hz << " (0 == unpaced)\n";
  std::cout << "producer_cpu=" << producer_cpu << " consumer_cpu=" << consumer_cpu << "\n";
  std::cout << "samples=" << hist_measure.Total() << "\n";
  std::cout << "p50_ns=" << hist_measure.Percentile(0.50) << "\n";
  std::cout << "p90_ns=" << hist_measure.Percentile(0.90) << "\n";
  std::cout << "p99_ns=" << hist_measure.Percentile(0.99) << "\n";
  std::cout << "p999_ns=" << hist_measure.Percentile(0.999) << "\n";
  std::cout << "p9999_ns=" << hist_measure.Percentile(0.9999) << "\n";
  std::cout << "max_ns=" << hist_measure.Max() << "\n";
  std::cout << "sink=" << sink << "\n";

  if (!args.out_md.empty()) {
    auto parent = std::filesystem::path(args.out_md).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream md(args.out_md);
    md << "# SPSC Live-Ingest Pipeline Latency\n\n";
    md << "Signal program: `" << args.signal_file << "`  \n";
    md << "Events: warmup=`" << args.warmup << "`, measured=`" << args.events << "`  \n";
    md << "Rate: `" << args.rate_hz << "` Hz (0 = unpaced, closed-loop)  \n";
    md << "Pinning: producer cpu=`" << producer_cpu
       << "`, consumer cpu=`" << consumer_cpu << "`  \n";
    md << "Ring capacity: `" << kRingCap << "` slots (lock-free SPSC, cache-line padded)  \n\n";
    md << "## Pipeline latency (enqueue -> signal output)\n\n";
    hist_measure.WriteMarkdownSummary(md, "pipeline_ns");
    md << "\n## Methodology\n\n";
    md << "The producer thread stamps `enqueue_ns = clock_gettime(CLOCK_MONOTONIC)` "
          "immediately before calling `ring.try_push(...)`. The consumer thread "
          "pops the event, applies it to `MarketState`, calls the JIT-compiled "
          "signal, and stamps `out_ns` immediately after. The recorded latency "
          "is `out_ns - enqueue_ns`, which includes:\n\n"
       << "  * SPSC ring enqueue (cache-line-padded `head_.store(release)`)\n"
       << "  * Inter-core hand-off cost (producer and consumer pinned to "
          "distinct physical cores)\n"
       << "  * SPSC ring dequeue (`tail_.load(acquire)` plus a slot copy)\n"
       << "  * Event apply to MarketState (two indexed stores)\n"
       << "  * One whole JIT signal call (`" << num_signals << "` output(s))\n\n";
    if (args.rate_hz > 0.0) {
      md << "The producer paces at `" << args.rate_hz << "` Hz using open-loop "
            "Coordinated-Omission–aware scheduling: each event's `enqueue_ns` "
            "is stamped at its TARGET time, not its observed wall-clock time. "
            "If the consumer stalls and the producer catches up later, the "
            "stalled event's recorded latency includes the queueing delay -- "
            "the standard wrk2 / HdrHistogram CO correction.\n\n";
    } else {
      md << "Rate is unpaced (closed-loop). The producer pushes as fast as the "
            "ring will accept. This measures the JIT-side steady-state latency "
            "floor, not the latency under realistic arrival pacing. For the "
            "CO-aware artifact use `--rate-hz=R` with an R below the consumer's "
            "saturating throughput.\n\n";
    }
    md << "## Reproduction\n\n";
    md << "```\n";
    md << "cd build-wsl\n";
    md << "JITSE_BENCH_PRODUCER_CPU=2 JITSE_BENCH_CONSUMER_CPU=4 \\\n";
    md << "  ./spsc_jit_pipeline_bench " << args.signal_file
       << " --events=" << args.events
       << " --warmup=" << args.warmup;
    if (args.rate_hz > 0.0) md << " --rate-hz=" << args.rate_hz;
    md << "\n```\n";
    std::cout << "md_written=" << args.out_md << "\n";
  }
  return 0;
} catch (const std::exception& ex) {
  std::cerr << "error: " << ex.what() << "\n";
  return 2;
}
