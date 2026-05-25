// stateful_lowering_ir_diff.cpp
//
// Dumps before/after IR for the canonical filtered_momentum.sig program under
// three configurations:
//   1. `lowering=none`  -- stateful ops stay as opaque runtime calls (P-1 baseline)
//   2. `lowering=all`   -- P0 IR lowering: opaque calls expanded into typed IR
//   3. `tier=specialized` -- P1 warm-loop specialization (lowering=all + warm
//                            invariant: warm-safe SMA/EMA/LAG branches stripped)
//
// For each config the tool prints the call count for each lowerable runtime
// helper (`jit_rt_ema_alpha`, `jit_rt_sma`/`jit_rt_sma_prepare`, `jit_rt_lag`)
// plus the count of warmup-guard primitives (`select`, `icmp eq i64`) in the
// post-O2 IR. The select/icmp counts are how we quantitatively verify that
// the specialized tier actually stripped the `is_init`/`is_full` branches.
//
// Outputs:
//   * Per-config IR files under <out_dir>/
//   * A console summary table
//   * `<out_dir>/tiered_specialization.md` -- a human-readable artifact that
//     pins the numbers to a specific signal+commit so we can detect
//     regressions and explain the win in the design doc.
//
// Mirrors the design of bench/run_cse_ir_diff.sh / cse_load_dedup_test.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "jit_compiler.h"
#include "lexer.h"
#include "parser.h"
#include "signal_program.h"

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open: " + path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int CountCalls(const std::string& ir, const std::string& fn_name) {
  // Match either `call ... @<fn_name>(...)` or `tail call ... @<fn_name>(...)`.
  std::regex re("call[^@\\n]*@" + fn_name + "\\(");
  int count = 0;
  auto begin = std::sregex_iterator(ir.begin(), ir.end(), re);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) ++count;
  return count;
}

// Token count, not regex match. We want raw `select ` / `icmp eq` opcodes as
// they appear in textual IR; this is robust enough for a sanity gauge across
// LLVM versions where opcode flag formatting changes.
int CountToken(const std::string& ir, const std::string& token) {
  int count = 0;
  std::size_t pos = 0;
  while ((pos = ir.find(token, pos)) != std::string::npos) {
    ++count;
    pos += token.size();
  }
  return count;
}

struct LoweringReport {
  std::string label;
  int ema_alpha_calls_pre = 0;
  int sma_calls_pre = 0;       // jit_rt_sma OR jit_rt_sma_prepare
  int lag_calls_pre = 0;
  int rolling_std_calls_pre = 0;
  int ema_alpha_calls_post = 0;
  int sma_calls_post = 0;
  int lag_calls_post = 0;
  int rolling_std_calls_post = 0;
  // Warmup-guard primitives in post-O2 IR. These are the operations P1's
  // assume_warm specialization aims to elide for warm-safe stateful nodes.
  int select_count_post = 0;
  int icmp_eq_i64_count_post = 0;
};

void FillReportFromIR(LoweringReport& r, const std::string& pre, const std::string& post) {
  r.ema_alpha_calls_pre  = CountCalls(pre,  "jit_rt_ema_alpha");
  r.sma_calls_pre        = CountCalls(pre,  "jit_rt_sma") + CountCalls(pre,  "jit_rt_sma_prepare");
  r.lag_calls_pre        = CountCalls(pre,  "jit_rt_lag");
  r.rolling_std_calls_pre  = CountCalls(pre,  "jit_rt_rolling_std");
  r.ema_alpha_calls_post = CountCalls(post, "jit_rt_ema_alpha");
  r.sma_calls_post       = CountCalls(post, "jit_rt_sma") + CountCalls(post, "jit_rt_sma_prepare");
  r.lag_calls_post       = CountCalls(post, "jit_rt_lag");
  r.rolling_std_calls_post = CountCalls(post, "jit_rt_rolling_std");
  // Use `select ` (with trailing space) so we don't accidentally match e.g.
  // function names containing "select" -- the IR opcode is always followed
  // by a result-type token. `icmp eq i64` is the warmup-guard form we emit
  // for `is_init`/`is_full` comparisons (`tick_index == 0`, `count == period`).
  r.select_count_post     = CountToken(post, "select ");
  r.icmp_eq_i64_count_post = CountToken(post, "icmp eq i64");
}

LoweringReport BuildBaseline(jitse::StatefulLoweringFlags flags, const std::string& label,
                             const std::string& signal_src, const std::string& out_pre,
                             const std::string& out_post) {
  jitse::JitCompiler jit;
  jit.SetStatefulLowering(flags);

  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(signal_src);
  std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(signals);
  jitse::SymbolTable symbols;
  for (const auto& s : signals) {
    for (const auto& t : jitse::CollectTickerSymbols(s)) symbols.RegisterOrGetId(t);
  }
  for (auto& s : signals) jitse::BindSymbolIds(s, symbols);

  if (!jit.CompileProgram(signals, symbols)) {
    throw std::runtime_error("CompileProgram failed for " + label + ": " + jit.LastError());
  }

  const std::string& pre = jit.LastIRPreOpt();
  const std::string& post = jit.LastIRPostOpt();
  { std::ofstream o(out_pre);  o << pre;  }
  { std::ofstream o(out_post); o << post; }

  LoweringReport r;
  r.label = label;
  FillReportFromIR(r, pre, post);
  return r;
}

// Build with TieredProgramJit and dump the post-O2 IR of the *specialized*
// (assume_warm) compilation. Pre-O2 dump is omitted here -- the specialized
// pre-O2 IR is structurally the same as the baseline pre-O2 IR (it diverges
// at the lowered SMA/EMA/LAG branches only after macro expansion), and
// what we care about for the artifact is the post-O2 evidence.
LoweringReport BuildSpecialized(jitse::StatefulLoweringFlags flags, const std::string& label,
                                const std::string& signal_src, const std::string& out_post) {
  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(signal_src);
  std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);
  // P1 requirement: caller MUST pre-assign program-wide node IDs and bind
  // symbols before TieredProgramJit::Compile; the JIT does not clone.
  jitse::AllocateProgramNodeIds(signals);
  jitse::SymbolTable symbols;
  for (const auto& s : signals) {
    for (const auto& t : jitse::CollectTickerSymbols(s)) symbols.RegisterOrGetId(t);
  }
  for (auto& s : signals) jitse::BindSymbolIds(s, symbols);

  jitse::TieredProgramJit tjit;
  if (!tjit.IsAvailable()) {
    throw std::runtime_error("tiered jit unavailable");
  }
  if (!tjit.Compile(signals, symbols, flags)) {
    throw std::runtime_error("tiered baseline compile failed for " + label + ": " + tjit.LastError());
  }
  if (!tjit.Promote()) {
    throw std::runtime_error("tiered specialized compile failed for " + label + ": " + tjit.LastError());
  }

  const std::string& post = tjit.SpecializedIRPostOpt();
  { std::ofstream o(out_post); o << post; }

  LoweringReport r;
  r.label = label;
  // No pre-O2 dump for the specialized config (see comment above).
  FillReportFromIR(r, /*pre=*/"", post);
  return r;
}

}  // namespace

// Strip the leading directory and the trailing `.sig` (if any) so that the
// generated IR/markdown file names stay readable for arbitrary input signals.
std::string SignalStem(const std::string& path) {
  std::filesystem::path p(path);
  std::string stem = p.stem().string();
  if (stem.empty()) stem = p.filename().string();
  return stem.empty() ? std::string("signal") : stem;
}

void WriteMarkdownReport(const std::string& md_path, const std::string& signal_file,
                         const LoweringReport& none, const LoweringReport& all_flags,
                         const LoweringReport& spec) {
  std::ofstream md(md_path);
  md << "# Tiered specialization (P1) -- IR-level evidence\n\n";
  md << "Signal: `" << signal_file << "`\n\n";
  md << "This artifact is auto-generated by `stateful_lowering_ir_diff`. It\n";
  md << "compares the post-O2 LLVM IR of three configurations:\n\n";
  md << "1. `lowering=none`  -- baseline P-1 (opaque `jit_rt_*` calls).\n";
  md << "2. `lowering=all`   -- P0 IR lowering (calls expanded into typed IR\n";
  md << "                        with full `is_init`/`is_full` guards).\n";
  md << "3. `tier=specialized` -- P1 warm-loop specialization (lowering=all +\n";
  md << "                          warm-safe stateful nodes stripped of their\n";
  md << "                          warmup branches).\n\n";
  md << "## Lowered runtime calls (post-O2)\n\n";
  md << "| helper | none | all | specialized |\n";
  md << "|--------|------|-----|-------------|\n";
  md << "| `jit_rt_ema_alpha` | " << none.ema_alpha_calls_post
     << " | " << all_flags.ema_alpha_calls_post
     << " | " << spec.ema_alpha_calls_post << " |\n";
  md << "| `jit_rt_sma*`      | " << none.sma_calls_post
     << " | " << all_flags.sma_calls_post
     << " | " << spec.sma_calls_post << " |\n";
  md << "| `jit_rt_lag`       | " << none.lag_calls_post
     << " | " << all_flags.lag_calls_post
     << " | " << spec.lag_calls_post << " |\n";
  md << "| `jit_rt_rolling_std` (control) | " << none.rolling_std_calls_post
     << " | " << all_flags.rolling_std_calls_post
     << " | " << spec.rolling_std_calls_post << " |\n\n";
  md << "Once `lowering=all` is enabled, opaque calls for the lowered ops\n";
  md << "should drop to 0; the `rolling_std` row is a control (not lowered).\n";
  md << "The `specialized` column is expected to match `all` for call counts\n";
  md << "(specialization changes guards, not the lowered op itself).\n\n";
  md << "## Warmup-guard primitives (post-O2)\n\n";
  md << "| primitive | none | all | specialized |\n";
  md << "|-----------|------|-----|-------------|\n";
  md << "| `select`        | " << none.select_count_post
     << " | " << all_flags.select_count_post
     << " | " << spec.select_count_post << " |\n";
  md << "| `icmp eq i64`   | " << none.icmp_eq_i64_count_post
     << " | " << all_flags.icmp_eq_i64_count_post
     << " | " << spec.icmp_eq_i64_count_post << " |\n\n";
  md << "`select` counts the materialized warmup-branch results, and\n";
  md << "`icmp eq i64` counts the `tick_index == 0` / `count == period`\n";
  md << "guard comparisons. Both should drop sharply between `all` and\n";
  md << "`specialized` -- that drop is the win P1 buys for the warm loop.\n";
  md << "(Some `select`/`icmp eq` instances remain in the specialized IR\n";
  md << "for stateful ops that are nested under conditional branches and\n";
  md << "therefore are NOT warm-safe; those are left untouched on purpose.)\n";
}

int main(int argc, char** argv) {
  try {
    const std::string signal_file = (argc >= 2) ? argv[1] : "examples/filtered_momentum.sig";
    const std::string out_dir = (argc >= 3) ? argv[2] : "bench/results/lowering_evidence";
    std::filesystem::create_directories(out_dir);

    const std::string src = ReadFile(signal_file);
    const std::string stem = SignalStem(signal_file);

    LoweringReport none = BuildBaseline(
        jitse::StatefulLoweringFlags::kNone, "lowering=none", src,
        out_dir + "/" + stem + ".none.pre.ll",
        out_dir + "/" + stem + ".none.post.ll");
    LoweringReport all_flags = BuildBaseline(
        jitse::StatefulLoweringFlags::kAll, "lowering=all", src,
        out_dir + "/" + stem + ".all.pre.ll",
        out_dir + "/" + stem + ".all.post.ll");
    LoweringReport spec = BuildSpecialized(
        jitse::StatefulLoweringFlags::kAll, "tier=specialized", src,
        out_dir + "/" + stem + ".specialized.post.ll");

    auto print_row = [](const char* fn_name, int none_pre, int all_pre,
                        int none_post, int all_post, int spec_post) {
      std::cout << "  " << fn_name
                << ": pre[none]=" << none_pre
                << " pre[all]=" << all_pre
                << " | post[none]=" << none_post
                << " post[all]=" << all_post
                << " post[spec]=" << spec_post
                << "\n";
    };

    std::cout << "stateful_lowering_ir_diff: " << signal_file << "\n";
    print_row("jit_rt_ema_alpha (lowerable)",
              none.ema_alpha_calls_pre, all_flags.ema_alpha_calls_pre,
              none.ema_alpha_calls_post, all_flags.ema_alpha_calls_post,
              spec.ema_alpha_calls_post);
    print_row("jit_rt_sma*       (lowerable)",
              none.sma_calls_pre, all_flags.sma_calls_pre,
              none.sma_calls_post, all_flags.sma_calls_post,
              spec.sma_calls_post);
    print_row("jit_rt_lag        (lowerable)",
              none.lag_calls_pre, all_flags.lag_calls_pre,
              none.lag_calls_post, all_flags.lag_calls_post,
              spec.lag_calls_post);
    print_row("jit_rt_rolling_std (control / not lowered)",
              none.rolling_std_calls_pre, all_flags.rolling_std_calls_pre,
              none.rolling_std_calls_post, all_flags.rolling_std_calls_post,
              spec.rolling_std_calls_post);

    std::cout << "  select (post-O2)            "
              << ": post[none]=" << none.select_count_post
              << " post[all]=" << all_flags.select_count_post
              << " post[spec]=" << spec.select_count_post
              << "\n";
    std::cout << "  icmp eq i64 (post-O2)       "
              << ": post[none]=" << none.icmp_eq_i64_count_post
              << " post[all]=" << all_flags.icmp_eq_i64_count_post
              << " post[spec]=" << spec.icmp_eq_i64_count_post
              << "\n";

    const std::string md_path = out_dir + "/tiered_specialization.md";
    WriteMarkdownReport(md_path, signal_file, none, all_flags, spec);
    std::cout << "IR dumps written to " << out_dir << "/\n";
    std::cout << "markdown summary: " << md_path << "\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 2;
  }
}
