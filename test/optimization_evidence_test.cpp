// P9: optimization-evidence test.
//
// The CSE-load-dedup test already proves that LLVM's GVN/CSE
// eliminates redundant market loads on `filtered_momentum`. This test
// generalizes the idea: we ask the post-O2 IR specific questions and
// assert specific answers, so the test fails loudly if a future LLVM
// upgrade or codegen tweak regresses a pipeline-level invariant we
// claim publicly.
//
// Three checks:
//
//   1. DEFAULT lowering (kNone): stateful ops sma/ema/lag appear as
//      `call double @jit_rt_*` in post-O2 IR. The runtime-helper path
//      is the contract; if the IR no longer contains the call, the
//      executable would link-fail at ORC-lookup time.
//
//   2. FULL lowering (kAll): every sma/ema/lag call is replaced by
//      inline IR. We require zero `jit_rt_sma`, `jit_rt_ema`,
//      `jit_rt_lag` calls in post-O2 IR. This is the load-bearing
//      claim of P0 (whole-program inline lowering). If a regression
//      ever silently restores a runtime call, the test fires.
//
//   3. ASM dump non-empty + contains x86 SSE/AVX float arithmetic.
//      We require `mulsd|addsd|subsd|divsd` or the vector-form
//      equivalents to be present, proving the captured asm reflects
//      real floating-point codegen rather than an empty stub.
//
// These checks are intentionally tight. They use ECMAScript regex
// against the post-O2 IR string and the captured asm string, both of
// which are accessible via JitCompiler::LastIRPostOpt() /
// LastAsm() (P9 additions).

#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ast_utils.h"
#include "jit_compiler.h"
#include "signal_program.h"

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open: " + path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int CountMatches(const std::string& haystack, const std::regex& re) {
  auto it = std::sregex_iterator(haystack.begin(), haystack.end(), re);
  return static_cast<int>(std::distance(it, std::sregex_iterator()));
}

// True if any of the regex alternatives is found at least once.
bool ContainsAny(const std::string& s, const std::regex& re) {
  return std::regex_search(s, re);
}

struct CompileResult {
  std::string ir_post_opt;
  std::string asm_dump;
};

// Compile filtered_momentum.sig with the given lowering flags and
// return the captured post-opt IR + asm. Bail-out helpers throw so
// the test's catch-all can report a clean error.
CompileResult CompileFilteredMomentum(jitse::StatefulLoweringFlags flags) {
  std::vector<jitse::SignalDef> parsed =
      jitse::ParseSignalProgram(ReadFile("examples/filtered_momentum.sig"));
  std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);

  jitse::SymbolTable symbols;
  for (const auto& s : signals) {
    for (const auto& t : jitse::CollectTickerSymbols(s)) symbols.RegisterOrGetId(t);
  }
  for (auto& s : signals) jitse::BindSymbolIds(s, symbols);

  jitse::JitCompiler jit;
  jit.SetStatefulLowering(flags);
  if (!jit.CompileProgram(signals, symbols)) {
    throw std::runtime_error("CompileProgram failed: " + jit.LastError());
  }
  return CompileResult{jit.LastIRPostOpt(), jit.LastAsm()};
}

}  // namespace

int main() {
  jitse::JitCompiler probe;
  if (!probe.IsAvailable()) {
    std::cout << "optimization_evidence_test=skip (no LLVM)\n";
    return 0;
  }

  try {
    // Regex set:
    //   - jit_rt_(sma|ema|lag) calls in IR
    //   - x86 scalar-double FP arithmetic in asm
    // P0 has several jit_rt_* variants per stateful op:
    //   sma  -> jit_rt_sma, jit_rt_sma_prepare
    //   ema  -> jit_rt_ema, jit_rt_ema_alpha (the precomputed-alpha
    //           form the JIT actually emits when the period is known)
    //   lag  -> jit_rt_lag
    // The regexes match any of these variants while staying tight
    // enough to not collide with unrelated symbols (rolling_std,
    // rolling_min, etc.).
    static const std::regex kRtCallRe(R"(call\s+(double|void)\s+@jit_rt_(sma|ema|lag))");
    static const std::regex kRtSmaRe(R"(@jit_rt_sma(_prepare)?\b)");
    static const std::regex kRtEmaRe(R"(@jit_rt_ema(_alpha)?\b)");
    static const std::regex kRtLagRe(R"(@jit_rt_lag\b)");
    static const std::regex kFpArithRe(
        R"(\b(mulsd|addsd|subsd|divsd|vmulsd|vaddsd|vsubsd|vdivsd|vfmadd\d+\w+))");

    // -------------------------------------------------------------------
    // Check 1: DEFAULT lowering -- stateful ops are runtime calls.
    // -------------------------------------------------------------------
    const auto def_r = CompileFilteredMomentum(jitse::StatefulLoweringFlags::kNone);
    const int default_sma = CountMatches(def_r.ir_post_opt, kRtSmaRe);
    const int default_ema = CountMatches(def_r.ir_post_opt, kRtEmaRe);
    const int default_lag = CountMatches(def_r.ir_post_opt, kRtLagRe);
    std::cout << "default_rt_sma=" << default_sma << "\n";
    std::cout << "default_rt_ema=" << default_ema << "\n";
    std::cout << "default_rt_lag=" << default_lag << "\n";
    // filtered_momentum.sig uses ema(...,10), ema(...,60), rolling_std(...,30),
    // and (under inlining) a couple of `mid` reads. We require at least one
    // ema call to appear in the IR -- the exact count depends on how the
    // signal is laid out and is not the invariant we care about.
    if (default_ema < 1) {
      std::cerr << "FAIL: default lowering produced no jit_rt_ema call\n";
      return 1;
    }

    // -------------------------------------------------------------------
    // Check 2: FULL lowering -- IR contains zero sma/ema/lag runtime
    // calls. The C runtime helpers are still present in the process,
    // but the lowered IR no longer reaches for them.
    // -------------------------------------------------------------------
    const auto full_r = CompileFilteredMomentum(jitse::StatefulLoweringFlags::kAll);
    const int full_sma = CountMatches(full_r.ir_post_opt, kRtSmaRe);
    const int full_ema = CountMatches(full_r.ir_post_opt, kRtEmaRe);
    const int full_lag = CountMatches(full_r.ir_post_opt, kRtLagRe);
    std::cout << "full_rt_sma=" << full_sma << "\n";
    std::cout << "full_rt_ema=" << full_ema << "\n";
    std::cout << "full_rt_lag=" << full_lag << "\n";
    if (full_sma != 0 || full_ema != 0 || full_lag != 0) {
      std::cerr << "FAIL: full lowering still contains runtime sma/ema/lag calls\n";
      return 1;
    }

    // -------------------------------------------------------------------
    // Check 3: asm dump is non-empty and contains FP arithmetic. The
    // exact instruction mix is host-dependent (AVX2 vs SSE2 vs the
    // baseline our asm-dump TargetMachine uses), so we accept any of
    // the scalar/SSE/AVX double-precision arithmetic mnemonics.
    // -------------------------------------------------------------------
    std::cout << "default_asm_bytes=" << def_r.asm_dump.size() << "\n";
    std::cout << "full_asm_bytes=" << full_r.asm_dump.size() << "\n";
    if (def_r.asm_dump.empty() || full_r.asm_dump.empty()) {
      std::cerr << "FAIL: asm dump is empty\n";
      return 1;
    }
    if (!ContainsAny(full_r.asm_dump, kFpArithRe)) {
      std::cerr << "FAIL: full-lowering asm contains no FP arithmetic mnemonics\n";
      std::cerr << "First 400 chars of asm:\n"
                << full_r.asm_dump.substr(0, 400) << "\n";
      return 1;
    }

    // Catch-all sanity: still nonzero runtime calls (any) in the rt-only
    // path, since stateful ops dominate. This is a noisy check; we just
    // require >= one call instruction overall.
    static const std::regex kAnyCallRe(R"(call\s+(double|void)\s+@jit_rt_)");
    const int default_any = CountMatches(def_r.ir_post_opt, kAnyCallRe);
    if (default_any < 1) {
      std::cerr << "FAIL: default lowering produced no jit_rt_* calls at all\n";
      return 1;
    }
    std::cout << "default_total_rt_calls=" << default_any << "\n";

    std::cout << "optimization_evidence_test=pass\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 2;
  }
}
