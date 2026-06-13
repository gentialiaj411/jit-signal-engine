// P13: persistent JIT module cache.
//
// Asserts the three things a real cache needs:
//   1. CORRECTNESS. A program compiled with caching enabled produces
//      the same per-tick output as the same program compiled without
//      caching, AND a SECOND compile of the same program (with the
//      cache populated) produces the same output. The cache cannot
//      change observed behavior.
//   2. CACHE HIT. The second compile reports `LastCacheHit() == true`
//      and is materially faster than the first (we assert "warm
//      compile is no slower than half the cold compile" -- a low
//      bar that flag-trips obvious regressions while staying robust
//      to host noise).
//   3. CACHE KEY DISCRIMINATION. Two programs that differ in any
//      cache-relevant axis (program text, lowering flags) get
//      independent cache entries -- a compile of program B does NOT
//      reuse program A's bitcode. We check this by looking at the
//      on-disk filename: both programs leave a distinct .bc file in
//      the cache dir.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "ast_utils.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

constexpr std::size_t kTicks = 800;

int failures = 0;
#define EXPECT(cond)                                                                  \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " : " << #cond << "\n"; \
      ++failures;                                                                     \
    }                                                                                 \
  } while (0)

struct Program {
  std::vector<jitse::SignalDef> signals;
  jitse::SymbolTable symbols;
};

Program BuildProgram(const std::string& src) {
  Program p;
  auto parsed = jitse::ParseSignalProgram(src);
  p.signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(p.signals);
  for (const auto& s : p.signals) {
    for (const auto& t : jitse::CollectTickerSymbols(s)) p.symbols.RegisterOrGetId(t);
  }
  for (auto& s : p.signals) jitse::BindSymbolIds(s, p.symbols);
  return p;
}

std::vector<double> RunForLastSignal(jitse::JitCompiler::ProgramFn fn,
                                     const Program& prog) {
  jitse::MultiSymbolSignalContext arena(1);
  for (const auto& s : prog.signals) jitse::PrewarmSignalContext(arena, 0, s);
  jitse::MarketState market;
  jitse::MarketSimulator sim(/*seed=*/7777, /*n_instruments=*/2);
  std::vector<double> outs(prog.signals.size());
  std::vector<double> trace;
  trace.reserve(kTicks);
  for (std::size_t t = 0; t < kTicks; ++t) {
    const auto ev = sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    fn(&market, &arena, 0, outs.data());
    trace.push_back(outs.back());
  }
  return trace;
}

unsigned long CurrentProcessId() {
#if defined(_WIN32)
  return static_cast<unsigned long>(_getpid());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

bool BitEq(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    std::uint64_t ai, bi;
    std::memcpy(&ai, &a[i], sizeof(double));
    std::memcpy(&bi, &b[i], sizeof(double));
    if (ai != bi) {
      // NaN-aware: treat any-NaN == any-NaN.
      if (std::isnan(a[i]) && std::isnan(b[i])) continue;
      return false;
    }
  }
  return true;
}

std::size_t CountBitcodeFiles(const std::filesystem::path& dir) {
  std::size_t n = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".bc") ++n;
  }
  return n;
}

}  // namespace

int main() {
  // Create a fresh per-test cache dir under the system temp.
  const auto tmp =
      std::filesystem::temp_directory_path() / ("jitse-p13-cache-" + std::to_string(CurrentProcessId()));
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);
  std::cout << "cache dir: " << tmp << "\n";

  const std::string src_a =
      "signal short = ema(mid(AAPL), 10)\n"
      "signal long = ema(mid(AAPL), 30)\n"
      "signal s = short - long\n";

  // A second, structurally different program -- must NOT hit program
  // A's cache file.
  const std::string src_b =
      "signal vol = rolling_std(mid(AAPL), 20)\n"
      "signal s = vol * 2.0\n";

  Program prog_a = BuildProgram(src_a);
  Program prog_b = BuildProgram(src_b);

  // --- Reference: compile without caching, capture trace.
  jitse::JitCompiler ref_jit;
  if (!ref_jit.IsAvailable()) {
    std::cout << "LLVM unavailable -- skipping P13 test\n";
    return 0;
  }
  EXPECT(ref_jit.CompileProgram(prog_a.signals, prog_a.symbols));
  EXPECT(!ref_jit.LastCacheHit());  // baseline compile: no cache enabled
  const auto trace_ref = RunForLastSignal(ref_jit.GetProgramFunction(), prog_a);

  // --- Cold compile with caching enabled. Must produce identical
  //     trace AND create a single .bc on disk AND report NO cache hit.
  jitse::JitCompiler cold_jit;
  EXPECT(cold_jit.IsAvailable());
  cold_jit.EnableModuleCache(tmp.string());
  EXPECT(cold_jit.ModuleCacheEnabled());
  EXPECT(cold_jit.CompileProgram(prog_a.signals, prog_a.symbols));
  EXPECT(!cold_jit.LastCacheHit());
  const auto trace_cold = RunForLastSignal(cold_jit.GetProgramFunction(), prog_a);
  EXPECT(BitEq(trace_ref, trace_cold));
  const auto cold_total_ns = cold_jit.LastCompileTimings().total_ns;
  std::cout << "cold compile total_ns = " << cold_total_ns << "\n";
  EXPECT(CountBitcodeFiles(tmp) == 1);

  // --- Warm compile with caching enabled. Must hit, produce identical
  //     trace, NOT add a second .bc, and be materially faster.
  jitse::JitCompiler warm_jit;
  warm_jit.EnableModuleCache(tmp.string());
  EXPECT(warm_jit.CompileProgram(prog_a.signals, prog_a.symbols));
  EXPECT(warm_jit.LastCacheHit());
  const auto trace_warm = RunForLastSignal(warm_jit.GetProgramFunction(), prog_a);
  EXPECT(BitEq(trace_ref, trace_warm));
  const auto warm_total_ns = warm_jit.LastCompileTimings().total_ns;
  std::cout << "warm compile total_ns = " << warm_total_ns << "\n";
  EXPECT(CountBitcodeFiles(tmp) == 1);
  // The warm compile still runs ORC codegen (that's what produces the
  // fn pointer), so it isn't free. But it MUST be faster than the cold
  // compile, which had to do AST->IR and the whole O2 pipeline on top.
  // A 2x lower bound is conservative enough to be noise-robust on the
  // CI hosts we run on while still failing if the cache silently
  // recompiled.
  if (cold_total_ns > 0) {
    const double ratio = static_cast<double>(warm_total_ns) /
                         static_cast<double>(cold_total_ns);
    std::cout << "warm/cold ratio = " << ratio << "\n";
    EXPECT(ratio < 0.5);
  }

  // --- Second-program compile in the same cache dir. Must MISS,
  //     produce its own bitcode file (now 2 in the dir), and run
  //     correctly.
  jitse::JitCompiler other_jit;
  other_jit.EnableModuleCache(tmp.string());
  EXPECT(other_jit.CompileProgram(prog_b.signals, prog_b.symbols));
  EXPECT(!other_jit.LastCacheHit());  // distinct cache key
  const auto trace_b = RunForLastSignal(other_jit.GetProgramFunction(), prog_b);
  // Sanity: rolling_std of a positive series should be non-negative;
  // post-warmup it should be finite. Just check at least one finite
  // sample exists (we already know the cache is byte-distinct above).
  std::size_t finite_count = 0;
  for (double v : trace_b) if (std::isfinite(v)) ++finite_count;
  EXPECT(finite_count > 0);
  EXPECT(CountBitcodeFiles(tmp) == 2);

  // --- Warm compile of program B against the warm cache. Must hit.
  jitse::JitCompiler warm_b_jit;
  warm_b_jit.EnableModuleCache(tmp.string());
  EXPECT(warm_b_jit.CompileProgram(prog_b.signals, prog_b.symbols));
  EXPECT(warm_b_jit.LastCacheHit());
  const auto trace_b_warm = RunForLastSignal(warm_b_jit.GetProgramFunction(), prog_b);
  EXPECT(BitEq(trace_b, trace_b_warm));

  // Clean up.
  std::filesystem::remove_all(tmp);

  if (failures > 0) {
    std::cerr << "jit_module_cache_test: " << failures << " failures\n";
    return 1;
  }
  std::cout << "jit_module_cache_test: PASSED\n";
  return 0;
}
