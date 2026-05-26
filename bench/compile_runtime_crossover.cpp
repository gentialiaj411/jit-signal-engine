// compile_runtime_crossover.cpp
//
// P5 deliverable: quantify the JIT's "compile cost amortizes over event
// count" claim. For one DSL program:
//   1. Measure JIT compile wall time, broken down into AST->IR, LLVM O2,
//      and ORC codegen phases.
//   2. Measure interpreter throughput (events/s).
//   3. Measure JIT throughput (events/s) once warm.
//   4. Compute the breakeven N at which
//        T_interp(N) = T_compile + T_jit(N)
//      i.e.  N* = T_compile / (per_event_interp - per_event_jit)
//   5. Emit a CSV/MD/SVG artifact showing the cumulative-cost curves
//      crossing at N*.
//
// Methodology details:
//   * Each measurement is a best-of-K run (K=5 by default) to mitigate
//     scheduler / TurboBoost noise. We use min, not mean, because
//     interpreter and JIT phase costs are both bounded below by their
//     "no contention" value and the noise is always positive.
//   * The compile timing uses the new JitCompiler::LastCompileTimings()
//     accessor (P5). Each compile gets a fresh JitCompiler so caches
//     from a previous compile don't bias the second one.
//   * The event timing uses a closed-loop hot loop, no batching, no
//     histogram (we already have that in latency_bench from P4 -- here
//     we want the steady-state per-event cost, not the distribution).
//
// Artifact:
//   {out-dir}/{stem}_crossover.csv  -- columns: events, t_interp_us,
//                                       t_jit_plus_compile_us
//   {out-dir}/{stem}_crossover.md   -- summary table + breakeven formula
//   {out-dir}/{stem}_crossover.svg  -- two cumulative-cost lines, with
//                                       the intersection at N* highlighted

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "parser.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

struct Args {
  std::string signal_file;
  std::size_t events_per_measure = 200'000;
  std::size_t compile_runs = 5;
  std::size_t event_runs = 5;
  std::string out_dir;
};

[[noreturn]] void Usage(const char* prog) {
  std::cerr << "Usage: " << prog
            << " <signal_file>"
               " [--events=N]"
               " [--compile-runs=K]"
               " [--event-runs=K]"
               " [--out-dir=DIR]\n";
  std::exit(2);
}

Args ParseArgs(int argc, char** argv) {
  Args a;
  std::vector<std::string> pos;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    if (s.rfind("--events=", 0) == 0) a.events_per_measure = std::stoull(s.substr(9));
    else if (s.rfind("--compile-runs=", 0) == 0) a.compile_runs = std::stoull(s.substr(15));
    else if (s.rfind("--event-runs=", 0) == 0) a.event_runs = std::stoull(s.substr(13));
    else if (s.rfind("--out-dir=", 0) == 0) a.out_dir = s.substr(10);
    else if (s == "-h" || s == "--help") Usage(argv[0]);
    else pos.push_back(s);
  }
  if (pos.size() != 1) Usage(argv[0]);
  a.signal_file = pos[0];
  return a;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open: " + path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct ParsedProgram {
  std::vector<jitse::SignalDef> signals;
  jitse::SymbolTable symbols;
};

ParsedProgram Parse(const std::string& src) {
  ParsedProgram p;
  auto parsed = jitse::ParseSignalProgram(src);
  p.signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(p.signals);
  for (const auto& s : p.signals)
    for (const auto& t : jitse::CollectTickerSymbols(s)) p.symbols.RegisterOrGetId(t);
  for (auto& s : p.signals) jitse::BindSymbolIds(s, p.symbols);
  return p;
}

struct CompileTimingStats {
  jitse::CompileTimings best;   // min across runs (per phase)
  jitse::CompileTimings median; // median total
};

CompileTimingStats MeasureCompile(const ParsedProgram& prog,
                                  std::size_t runs) {
  // Each run uses its own JitCompiler so the LLJIT module cache is fresh.
  // This is essential: ORC caches modules across compiles within one
  // LLJIT instance, which would skew the second timing dramatically.
  std::vector<jitse::CompileTimings> samples;
  samples.reserve(runs);
  for (std::size_t i = 0; i < runs; ++i) {
    auto jit = std::make_unique<jitse::JitCompiler>();
    if (!jit->IsAvailable())
      throw std::runtime_error("JIT not available");
    jit->SetStatefulLowering(jitse::StatefulLoweringFlags::kAll);
    if (!jit->CompileProgram(prog.signals, prog.symbols))
      throw std::runtime_error("compile failed: " + jit->LastError());
    samples.push_back(jit->LastCompileTimings());
  }
  CompileTimingStats out;
  auto pick_min_phase = [&](auto member) {
    std::uint64_t m = samples[0].*member;
    for (const auto& s : samples) if ((s.*member) < m) m = s.*member;
    return m;
  };
  out.best.ast_to_ir_ns   = pick_min_phase(&jitse::CompileTimings::ast_to_ir_ns);
  out.best.llvm_opt_ns    = pick_min_phase(&jitse::CompileTimings::llvm_opt_ns);
  out.best.orc_codegen_ns = pick_min_phase(&jitse::CompileTimings::orc_codegen_ns);
  out.best.total_ns       = pick_min_phase(&jitse::CompileTimings::total_ns);
  std::vector<std::uint64_t> totals;
  for (const auto& s : samples) totals.push_back(s.total_ns);
  std::sort(totals.begin(), totals.end());
  out.median.total_ns = totals[totals.size() / 2];
  return out;
}

double MeasureInterpEventNs(const ParsedProgram& prog,
                            std::size_t events, std::size_t runs) {
  jitse::Interpreter interp(prog.symbols);
  jitse::MarketSimulator sim(0xC0DECAFEull, 4);
  std::vector<jitse::MarketEvent> evs;
  evs.reserve(events);
  for (std::size_t i = 0; i < events; ++i) evs.push_back(sim.NextEvent(1000));

  double best_ns_per_event = std::numeric_limits<double>::infinity();
  for (std::size_t r = 0; r < runs; ++r) {
    jitse::MultiSymbolSignalContext arena(1);
    for (const auto& sig : prog.signals)
      jitse::PrewarmSignalContext(arena, 0, sig);
    jitse::MarketState m{};
    volatile double sink = 0.0;
    // Warmup: ~5% of events.
    const std::size_t warmup = events / 20;
    for (std::size_t i = 0; i < warmup; ++i) {
      const auto& ev = evs[i];
      m.instruments[ev.instrument_id].bid = ev.bid;
      m.instruments[ev.instrument_id].ask = ev.ask;
      m.current_time_ns = ev.timestamp_ns;
      double last = 0.0;
      for (const auto& sig : prog.signals)
        last = interp.Evaluate(sig, m, arena, 0);
      sink += last;
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = warmup; i < events; ++i) {
      const auto& ev = evs[i];
      m.instruments[ev.instrument_id].bid = ev.bid;
      m.instruments[ev.instrument_id].ask = ev.ask;
      m.current_time_ns = ev.timestamp_ns;
      double last = 0.0;
      for (const auto& sig : prog.signals)
        last = interp.Evaluate(sig, m, arena, 0);
      sink += last;
    }
    const auto t1 = std::chrono::steady_clock::now();
    (void)sink;
    const double ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    const double per_event = ns / static_cast<double>(events - warmup);
    if (per_event < best_ns_per_event) best_ns_per_event = per_event;
  }
  return best_ns_per_event;
}

double MeasureJitEventNs(const ParsedProgram& prog,
                         std::size_t events, std::size_t runs) {
  // Per-event measurement only -- we do NOT include the compile time
  // here, because compile time is measured separately and accounted for
  // in the crossover formula.
  auto jit = std::make_unique<jitse::JitCompiler>();
  if (!jit->IsAvailable()) throw std::runtime_error("JIT not available");
  jit->SetStatefulLowering(jitse::StatefulLoweringFlags::kAll);
  if (!jit->CompileProgram(prog.signals, prog.symbols))
    throw std::runtime_error("compile failed: " + jit->LastError());
  auto fn = jit->GetProgramFunction();
  const std::size_t num_signals = prog.signals.size();
  const std::size_t out_idx = num_signals - 1;

  jitse::MarketSimulator sim(0xC0DECAFEull, 4);
  std::vector<jitse::MarketEvent> evs;
  evs.reserve(events);
  for (std::size_t i = 0; i < events; ++i) evs.push_back(sim.NextEvent(1000));

  double best_ns_per_event = std::numeric_limits<double>::infinity();
  for (std::size_t r = 0; r < runs; ++r) {
    jitse::MultiSymbolSignalContext arena(1);
    for (const auto& sig : prog.signals)
      jitse::PrewarmSignalContext(arena, 0, sig);
    std::vector<double> outs(num_signals, 0.0);
    jitse::MarketState m{};
    volatile double sink = 0.0;
    const std::size_t warmup = events / 20;
    for (std::size_t i = 0; i < warmup; ++i) {
      const auto& ev = evs[i];
      m.instruments[ev.instrument_id].bid = ev.bid;
      m.instruments[ev.instrument_id].ask = ev.ask;
      m.current_time_ns = ev.timestamp_ns;
      fn(&m, &arena, 0, outs.data());
      sink += outs[out_idx];
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = warmup; i < events; ++i) {
      const auto& ev = evs[i];
      m.instruments[ev.instrument_id].bid = ev.bid;
      m.instruments[ev.instrument_id].ask = ev.ask;
      m.current_time_ns = ev.timestamp_ns;
      fn(&m, &arena, 0, outs.data());
      sink += outs[out_idx];
    }
    const auto t1 = std::chrono::steady_clock::now();
    (void)sink;
    const double ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    const double per_event = ns / static_cast<double>(events - warmup);
    if (per_event < best_ns_per_event) best_ns_per_event = per_event;
  }
  return best_ns_per_event;
}

// Returns the crossover event count: the N at which interpreter
// cumulative time equals JIT cumulative time including one-shot compile.
// Returns -1.0 if JIT is not faster per-event (no crossover exists in
// the positive integers).
double BreakevenN(double compile_ns, double interp_ns_per_event,
                  double jit_ns_per_event) {
  if (jit_ns_per_event >= interp_ns_per_event) return -1.0;
  return compile_ns / (interp_ns_per_event - jit_ns_per_event);
}

void WriteSvg(std::ostream& out, double compile_ns, double interp_ns_per_event,
              double jit_ns_per_event, double breakeven_n,
              const std::string& signal_label) {
  const int W = 720;
  const int H = 420;
  const int ML = 80, MR = 30, MT = 40, MB = 60;
  const int PW = W - ML - MR;
  const int PH = H - MT - MB;

  // X axis: event count in log10. Range = [1, 10 * breakeven_n] so the
  // crossover lands near the middle if it's finite.
  const double x_lo_n = 1.0;
  double x_hi_n;
  if (breakeven_n > 0.0) {
    x_hi_n = std::max(breakeven_n * 10.0, 1000.0);
  } else {
    x_hi_n = 1.0e9;  // arbitrary 1B if no crossover
  }
  const double log_x_lo = std::log10(x_lo_n);
  const double log_x_hi = std::log10(x_hi_n);
  auto x_for_n = [&](double n) {
    const double t = (std::log10(std::max(n, 1.0)) - log_x_lo) /
                     (log_x_hi - log_x_lo);
    return ML + t * PW;
  };

  // Y axis: cumulative time (ns) in log10. Range computed from data.
  auto interp_total = [&](double n) { return interp_ns_per_event * n; };
  auto jit_total = [&](double n) { return compile_ns + jit_ns_per_event * n; };
  const double y_lo_ns = std::min(compile_ns * 0.5, interp_ns_per_event);
  const double y_hi_ns = std::max(interp_total(x_hi_n), jit_total(x_hi_n));
  const double log_y_lo = std::log10(std::max(y_lo_ns, 1.0));
  const double log_y_hi = std::log10(std::max(y_hi_ns, 10.0));
  auto y_for_ns = [&](double ns) {
    const double t = (std::log10(std::max(ns, 1.0)) - log_y_lo) /
                     (log_y_hi - log_y_lo);
    return MT + (1.0 - t) * PH;
  };

  out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W
      << "\" height=\"" << H << "\" viewBox=\"0 0 " << W << ' ' << H
      << "\" font-family=\"sans-serif\" font-size=\"12\">\n"
      << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n"
      << "<text x=\"" << (W / 2) << "\" y=\"20\" text-anchor=\"middle\" "
         "font-weight=\"bold\">"
      << "Compile vs interpret crossover: " << signal_label << "</text>\n";
  out << "<rect x=\"" << ML << "\" y=\"" << MT << "\" width=\"" << PW
      << "\" height=\"" << PH
      << "\" fill=\"none\" stroke=\"#888\"/>\n";
  // X axis ticks at each decade.
  for (double lx = std::floor(log_x_lo); lx <= log_x_hi + 1e-9; lx += 1.0) {
    const double xp = ML + ((lx - log_x_lo) / (log_x_hi - log_x_lo)) * PW;
    out << "<line x1=\"" << xp << "\" y1=\"" << MT << "\" x2=\"" << xp
        << "\" y2=\"" << (MT + PH) << "\" stroke=\"#eee\"/>\n";
    out << "<text x=\"" << xp << "\" y=\"" << (MT + PH + 16)
        << "\" text-anchor=\"middle\">10^" << static_cast<int>(lx)
        << "</text>\n";
  }
  // Y axis ticks at each decade.
  for (double ly = std::floor(log_y_lo); ly <= log_y_hi + 1e-9; ly += 1.0) {
    const double yp = MT + (1.0 - (ly - log_y_lo) / (log_y_hi - log_y_lo)) * PH;
    out << "<line x1=\"" << ML << "\" y1=\"" << yp << "\" x2=\"" << (ML + PW)
        << "\" y2=\"" << yp << "\" stroke=\"#eee\"/>\n";
    const double v = std::pow(10.0, ly);
    out << "<text x=\"" << (ML - 8) << "\" y=\"" << (yp + 4)
        << "\" text-anchor=\"end\">";
    if (v >= 1e9) out << (v / 1e9) << " s";
    else if (v >= 1e6) out << (v / 1e6) << " ms";
    else if (v >= 1e3) out << (v / 1e3) << " us";
    else out << v << " ns";
    out << "</text>\n";
  }
  // Axis labels.
  out << "<text x=\"" << (ML + PW / 2) << "\" y=\"" << (H - 16)
      << "\" text-anchor=\"middle\">events processed</text>\n"
      << "<text x=\"22\" y=\"" << (MT + PH / 2)
      << "\" text-anchor=\"middle\" transform=\"rotate(-90 22 "
      << (MT + PH / 2) << ")\">cumulative wall time</text>\n";

  // Interpreter line (no compile cost).
  out << "<line x1=\"" << x_for_n(x_lo_n) << "\" y1=\"" << y_for_ns(interp_total(x_lo_n))
      << "\" x2=\"" << x_for_n(x_hi_n) << "\" y2=\"" << y_for_ns(interp_total(x_hi_n))
      << "\" stroke=\"#1f77b4\" stroke-width=\"2\"/>\n";

  // JIT line (starts at compile_ns).
  out << "<line x1=\"" << x_for_n(x_lo_n) << "\" y1=\"" << y_for_ns(jit_total(x_lo_n))
      << "\" x2=\"" << x_for_n(x_hi_n) << "\" y2=\"" << y_for_ns(jit_total(x_hi_n))
      << "\" stroke=\"#d62728\" stroke-width=\"2\"/>\n";

  // Breakeven marker.
  if (breakeven_n > 0.0) {
    const double xc = x_for_n(breakeven_n);
    const double yc = y_for_ns(interp_total(breakeven_n));
    out << "<line x1=\"" << xc << "\" y1=\"" << MT << "\" x2=\"" << xc
        << "\" y2=\"" << (MT + PH) << "\" stroke=\"#2ca02c\" stroke-dasharray=\"4 4\"/>\n";
    out << "<circle cx=\"" << xc << "\" cy=\"" << yc << "\" r=\"5\" fill=\"#2ca02c\"/>\n";
    char buf[80];
    std::snprintf(buf, sizeof(buf), "breakeven N* = %.0f", breakeven_n);
    out << "<text x=\"" << (xc + 8) << "\" y=\"" << (yc - 8)
        << "\" fill=\"#2ca02c\" font-weight=\"bold\">" << buf << "</text>\n";
  }
  // Legend.
  const int lx = ML + 12, ly = MT + 16;
  out << "<rect x=\"" << lx << "\" y=\"" << (ly - 10)
      << "\" width=\"14\" height=\"3\" fill=\"#1f77b4\"/>"
      << "<text x=\"" << (lx + 20) << "\" y=\"" << (ly - 2)
      << "\">interpreter (no compile)</text>\n"
      << "<rect x=\"" << lx << "\" y=\"" << (ly + 6)
      << "\" width=\"14\" height=\"3\" fill=\"#d62728\"/>"
      << "<text x=\"" << (lx + 20) << "\" y=\"" << (ly + 14)
      << "\">JIT (compile + execution)</text>\n";
  out << "</svg>\n";
}

}  // namespace

int main(int argc, char** argv) try {
  const Args args = ParseArgs(argc, argv);
  const std::string src = ReadFile(args.signal_file);
  const ParsedProgram prog = Parse(src);
  const std::string stem =
      std::filesystem::path(args.signal_file).stem().string();

  std::cout << "signal=" << args.signal_file
            << "  events=" << args.events_per_measure
            << "  compile_runs=" << args.compile_runs
            << "  event_runs=" << args.event_runs << "\n";

  // ---- Compile timing ----
  const auto ct = MeasureCompile(prog, args.compile_runs);
  std::cout << "compile (best-of-" << args.compile_runs << ")\n"
            << "  ast_to_ir_ns   = " << ct.best.ast_to_ir_ns << "\n"
            << "  llvm_opt_ns    = " << ct.best.llvm_opt_ns << "\n"
            << "  orc_codegen_ns = " << ct.best.orc_codegen_ns << "\n"
            << "  total_ns_best  = " << ct.best.total_ns << "\n"
            << "  total_ns_med   = " << ct.median.total_ns << "\n";

  // ---- Per-event timing ----
  const double interp_ns =
      MeasureInterpEventNs(prog, args.events_per_measure, args.event_runs);
  const double jit_ns =
      MeasureJitEventNs(prog, args.events_per_measure, args.event_runs);
  std::cout << "per-event (best-of-" << args.event_runs << ")\n"
            << "  interp_ns_per_event = " << interp_ns << "\n"
            << "  jit_ns_per_event    = " << jit_ns << "\n";

  const double break_n =
      BreakevenN(static_cast<double>(ct.best.total_ns), interp_ns, jit_ns);
  if (break_n > 0.0)
    std::cout << "breakeven_events = " << static_cast<std::uint64_t>(break_n) << "\n";
  else
    std::cout << "breakeven_events = NONE (JIT not faster per-event on this build)\n";

  // ---- Artifacts ----
  if (!args.out_dir.empty()) {
    std::filesystem::create_directories(args.out_dir);
    const std::string base = args.out_dir + "/" + stem + "_crossover";

    {
      std::ofstream csv(base + ".csv");
      csv << "events,interp_cum_ns,jit_cum_ns\n";
      // Sample on log spacing so both curves and the crossover are visible
      // at every magnitude.
      static const double kPoints[] = {1, 10, 100, 1000, 10000, 100000,
                                       1e6, 1e7};
      for (double n : kPoints) {
        const double i_ns = interp_ns * n;
        const double j_ns = static_cast<double>(ct.best.total_ns) + jit_ns * n;
        csv << static_cast<std::uint64_t>(n) << ',' << i_ns << ',' << j_ns << '\n';
      }
      if (break_n > 0.0) {
        const double i_ns = interp_ns * break_n;
        const double j_ns = static_cast<double>(ct.best.total_ns) + jit_ns * break_n;
        csv << static_cast<std::uint64_t>(break_n) << ',' << i_ns << ',' << j_ns
            << "  # breakeven\n";
      }
    }

    {
      std::ofstream md(base + ".md");
      md << "# Compile-vs-interpret crossover: `" << stem << "`\n\n"
         << "Source: `" << args.signal_file << "`  \n"
         << "Methodology: see "
            "[`docs/compile_runtime_crossover.md`](../../../docs/compile_runtime_crossover.md). "
            "Compile time is best-of-" << args.compile_runs
         << "; per-event time is best-of-" << args.event_runs
         << " over " << args.events_per_measure << " events each "
         << "(5% warmup excluded, closed-loop, no batching).\n\n";
      md << "## Compile breakdown (best run)\n\n"
         << "| Phase | ns | % of total |\n|---|---:|---:|\n";
      auto pct = [&](std::uint64_t v) {
        return ct.best.total_ns == 0 ? 0.0 :
            100.0 * static_cast<double>(v) / static_cast<double>(ct.best.total_ns);
      };
      md << "| AST -> IR emission | " << ct.best.ast_to_ir_ns << " | "
         << pct(ct.best.ast_to_ir_ns) << "% |\n";
      md << "| LLVM O2 pipeline | " << ct.best.llvm_opt_ns << " | "
         << pct(ct.best.llvm_opt_ns) << "% |\n";
      md << "| ORC codegen + lookup | " << ct.best.orc_codegen_ns << " | "
         << pct(ct.best.orc_codegen_ns) << "% |\n";
      md << "| **total (best of " << args.compile_runs << ")** | **"
         << ct.best.total_ns << "** | 100% |\n";
      md << "| total (median of " << args.compile_runs << ") | "
         << ct.median.total_ns << " | |\n\n";

      md << "## Per-event cost (best run)\n\n"
         << "| Configuration | ns/event |\n|---|---:|\n"
         << "| interpreter | " << interp_ns << " |\n"
         << "| JIT (warm)  | " << jit_ns << " |\n\n";

      md << "## Crossover\n\n";
      if (break_n > 0.0) {
        md << "`N* = T_compile / (per_event_interp - per_event_jit)`  \n";
        md << "`N* = " << ct.best.total_ns << " / ("
           << interp_ns << " - " << jit_ns << ") = **"
           << static_cast<std::uint64_t>(break_n) << " events**`\n\n";
        const double break_seconds =
            static_cast<double>(ct.best.total_ns) / 1e9 +
            jit_ns * break_n / 1e9;
        md << "Equivalently: the JIT pays for itself after about "
           << break_seconds << " s of warm event processing (assuming "
                               "all warm calls go to the JIT).\n\n";
        md << "If you only process **less than " << static_cast<std::uint64_t>(break_n)
           << "** events per session, the interpreter is the better choice "
              "(no compile to pay for).\n\n";
      } else {
        md << "**No crossover exists**: the JIT did not beat the interpreter "
              "per-event on this signal. This typically happens when the "
              "program is dominated by runtime helpers the JIT cannot "
              "inline (see [`docs/runtime_call_profile.md`](../../../docs/runtime_call_profile.md)).\n\n";
      }
      md << "## Plot\n\n![crossover](" << stem << "_crossover.svg)\n";
    }

    {
      std::ofstream svg(base + ".svg");
      WriteSvg(svg, static_cast<double>(ct.best.total_ns), interp_ns, jit_ns,
               break_n, stem);
    }
    std::cout << "wrote artifacts to " << args.out_dir << "/\n";
  }
  return 0;
} catch (const std::exception& ex) {
  std::cerr << "error: " << ex.what() << "\n";
  return 1;
}
