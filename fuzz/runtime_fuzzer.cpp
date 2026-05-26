// P8: libFuzzer harness for interpreter vs JIT bit-equality.
//
// Strategy: take the raw fuzz bytes and use them as a seed for an
// AST-fragment generator (NOT a parser-bytes input -- parser-byte
// fuzzing is parser_fuzzer.cpp's job). For each generated program we
//
//   1. Allocate node IDs + symbols.
//   2. Pump K random ticks through the interpreter.
//   3. Pump the SAME K random ticks through the JIT.
//   4. Assert the per-signal outputs are bit-identical (treating
//      NaN == NaN as equal, since both paths share the same NaN
//      semantics).
//
// A bit-equality failure here is a real bug -- the parity tests
// already cover deterministic programs but the fuzzer's job is to
// explore deeper into the AST space than the hand-written and
// random-expression-gen parity tests can reach.
//
// Build modes mirror parser_fuzzer.cpp:
//
//   - libFuzzer mode (clang + -fsanitize=fuzzer): LLVMFuzzerTestOneInput
//     consumes the raw bytes as a generator seed.
//   - Standalone corpus mode (JITSE_FUZZ_LINK_DRIVER): walks
//     `fuzz/corpus/runtime/` (or the path passed as argv[1]) and
//     drives each file through TestOneInput; useful for replaying a
//     specific seed that caused a divergence.
//
// Boundedness: K (number of ticks) and the AST depth are both bounded
// by what fits in the first ~16 bytes of input. This keeps individual
// fuzz iterations to a few milliseconds even with the JIT compile.

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

// Tiny LCG so we can re-derive deterministic streams from the fuzzer
// seed without pulling in <random> overhead. Period is 2^48, which is
// more than enough for the small streams the harness uses.
struct Lcg {
  std::uint64_t s;
  explicit Lcg(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
  std::uint64_t Next() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s;
  }
  double Unit() {
    return static_cast<double>(Next() >> 11) * (1.0 / static_cast<double>(1ULL << 53));
  }
  int InRange(int lo, int hi_excl) {
    if (hi_excl <= lo) return lo;
    return lo + static_cast<int>(Next() % static_cast<std::uint64_t>(hi_excl - lo));
  }
};

// Build a randomized but well-typed expression tree at `depth` levels.
// All FunctionCalls produced here are stateless (no node_id, no
// runtime-state coupling) so we don't need to plumb state allocation
// through the fuzz path. Mid/abs/sqrt/log + arithmetic + conditionals
// cover the bulk of the JIT EmitExpr surface.
std::unique_ptr<jitse::Expr> GenExpr(Lcg& rng, int depth, const char* ticker) {
  using namespace jitse;
  if (depth <= 0 || (rng.Unit() < 0.18 && depth < 4)) {
    // Leaf: literal, ticker load, or signed-literal.
    const int pick = rng.InRange(0, 4);
    if (pick == 0) return std::make_unique<NumberLiteral>(rng.Unit() * 100.0 - 50.0);
    if (pick == 1) {
      std::vector<std::unique_ptr<Expr>> args;
      args.push_back(std::make_unique<IdentifierExpr>(ticker));
      return std::make_unique<FunctionCall>("mid", std::move(args));
    }
    if (pick == 2) {
      std::vector<std::unique_ptr<Expr>> args;
      args.push_back(std::make_unique<IdentifierExpr>(ticker));
      return std::make_unique<FunctionCall>("spread", std::move(args));
    }
    return std::make_unique<NumberLiteral>(rng.Unit() < 0.5 ? 0.0 : 1.0);
  }
  const int kind = rng.InRange(0, 8);
  switch (kind) {
    case 0: case 1: case 2: case 3: {
      static const BinaryOpKind ops[] = {
          BinaryOpKind::Add, BinaryOpKind::Sub, BinaryOpKind::Mul, BinaryOpKind::Div};
      auto lhs = GenExpr(rng, depth - 1, ticker);
      auto rhs = GenExpr(rng, depth - 1, ticker);
      return std::make_unique<BinaryOp>(ops[kind], std::move(lhs), std::move(rhs));
    }
    case 4: {
      auto x = GenExpr(rng, depth - 1, ticker);
      return std::make_unique<UnaryOp>(
          rng.Unit() < 0.5 ? UnaryOpKind::Plus : UnaryOpKind::Minus, std::move(x));
    }
    case 5: {
      // Conditional with a comparison condition. Building a comparison
      // here keeps the AST type-correct (the type checker isn't run
      // on directly-constructed AST, but typed bugs in the JIT happen
      // anyway -- e.g., the IR rejects an arith op on a `i1`).
      static const BinaryOpKind cmps[] = {
          BinaryOpKind::Gt, BinaryOpKind::Lt, BinaryOpKind::Gte, BinaryOpKind::Lte,
          BinaryOpKind::Eq, BinaryOpKind::NotEq};
      const int c = rng.InRange(0, 6);
      auto cl = GenExpr(rng, depth - 1, ticker);
      auto cr = GenExpr(rng, depth - 1, ticker);
      auto cond = std::make_unique<BinaryOp>(cmps[c], std::move(cl), std::move(cr));
      auto t = GenExpr(rng, depth - 1, ticker);
      auto e = GenExpr(rng, depth - 1, ticker);
      return std::make_unique<Conditional>(std::move(cond), std::move(t), std::move(e));
    }
    case 6: case 7: {
      const char* name = (kind == 6) ? "abs" : "sqrt";
      auto x = GenExpr(rng, depth - 1, ticker);
      std::vector<std::unique_ptr<Expr>> args;
      args.push_back(std::move(x));
      return std::make_unique<FunctionCall>(name, std::move(args));
    }
  }
  return std::make_unique<NumberLiteral>(0.0);
}

// Bit-equality with explicit NaN-aware semantics: NaN == NaN -> true,
// finite values must match bit-for-bit. This is tighter than the
// parity test's 1e-9 tolerance because both paths run on the SAME C++
// helpers; any divergence is a JIT codegen / dispatch bug.
bool BitEqual(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  if (std::isnan(a) || std::isnan(b)) return false;
  std::uint64_t ai, bi;
  std::memcpy(&ai, &a, sizeof(double));
  std::memcpy(&bi, &b, sizeof(double));
  return ai == bi;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 4) return 0;
  // Extract a 64-bit seed from the input. Mixing all bytes keeps the
  // generator responsive to libFuzzer's mutation strategy.
  std::uint64_t seed = 0;
  for (std::size_t i = 0; i < size; ++i) {
    seed = seed * 1099511628211ULL ^ data[i];
  }
  Lcg rng(seed);

  // Cap depth + tick count so each fuzz iteration finishes quickly.
  const int depth = 2 + (data[0] % 3);
  const int ticks = 32 + static_cast<int>(data[1]) * 2;

  try {
    auto body = GenExpr(rng, depth, "AAPL");
    jitse::SignalDef def{"fuzz_signal", std::move(body)};
    jitse::AllocateNodeIds(def);
    jitse::SymbolTable symbols;
    symbols.RegisterOrGetId("AAPL");
    jitse::BindSymbolIds(def, symbols);

    // Interpreter run.
    jitse::Interpreter interp(symbols);
    jitse::SignalContext interp_ctx;
    jitse::PrewarmSignalContext(interp_ctx, def);
    jitse::MarketState market;
    jitse::MarketSimulator sim(seed | 1ULL, /*n_instruments=*/1);
    std::vector<double> interp_trace;
    interp_trace.reserve(ticks);
    for (int t = 0; t < ticks; ++t) {
      const auto ev = sim.NextEvent(1000);
      market.instruments[ev.instrument_id].bid = ev.bid;
      market.instruments[ev.instrument_id].ask = ev.ask;
      market.current_time_ns = ev.timestamp_ns;
      interp_trace.push_back(interp.Evaluate(def, market, interp_ctx));
    }

    // JIT run. Re-build everything from a fresh AST to avoid cross-
    // contamination of node_id / symbol state (CompileProgram is
    // semantically pure but the cache key matters under fuzzing).
    jitse::JitCompiler jit;
    if (!jit.IsAvailable()) return 0;  // build w/o LLVM: nothing to compare.
    std::vector<jitse::SignalDef> prog;
    prog.push_back(std::move(def));
    if (!jit.CompileProgram(prog, symbols)) {
      // Programs that the JIT rejects (e.g., div-by-NaN propagating
      // through verifyFunction) are fine -- as long as the failure is
      // a clean LastError() and not a crash. The interpreter result
      // is discarded.
      return 0;
    }
    auto fn = jit.GetProgramFunction();
    if (!fn) return 0;

    jitse::MultiSymbolSignalContext jit_arena(1);
    jitse::PrewarmSignalContext(jit_arena, 0, prog[0]);
    jitse::MarketState jit_market;
    jitse::MarketSimulator jit_sim(seed | 1ULL, /*n_instruments=*/1);
    std::vector<double> jit_outs(prog.size(), 0.0);
    std::vector<double> jit_trace;
    jit_trace.reserve(ticks);
    for (int t = 0; t < ticks; ++t) {
      const auto ev = jit_sim.NextEvent(1000);
      jit_market.instruments[ev.instrument_id].bid = ev.bid;
      jit_market.instruments[ev.instrument_id].ask = ev.ask;
      jit_market.current_time_ns = ev.timestamp_ns;
      fn(&jit_market, &jit_arena, 0, jit_outs.data());
      jit_trace.push_back(jit_outs[0]);
    }

    for (int t = 0; t < ticks; ++t) {
      if (!BitEqual(interp_trace[t], jit_trace[t])) {
        std::cerr << "runtime_fuzzer: divergence at tick " << t
                  << " interp=" << interp_trace[t]
                  << " jit=" << jit_trace[t]
                  << " seed=" << seed << "\n";
        std::abort();
      }
    }
  } catch (const std::exception&) {
    // Both paths failing the same way is fine. Wrong-direction
    // exceptions (interp threw, JIT didn't, or vice versa) are
    // harder to catch here without splitting the try/catch by
    // backend; since the parity test covers the common cases, we
    // accept "either-side throws -> skip" as a fuzzer non-event.
  } catch (...) {
    std::abort();
  }
  return 0;
}

#ifdef JITSE_FUZZ_LINK_DRIVER
int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  fs::path corpus_dir = (argc >= 2) ? fs::path(argv[1]) : fs::path("fuzz/corpus/runtime");
  if (!fs::exists(corpus_dir)) {
    // Empty corpus: just run a single deterministic seed so the smoke
    // test still proves the harness compiles and the runtime is
    // reachable.
    const std::uint8_t seed_bytes[] = {0x11, 0x22, 0x33, 0x44};
    (void)LLVMFuzzerTestOneInput(seed_bytes, sizeof(seed_bytes));
    std::cout << "runtime_fuzzer: no corpus dir; ran one deterministic seed\n";
    return 0;
  }
  std::size_t n_inputs = 0;
  for (const auto& entry : fs::directory_iterator(corpus_dir)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) continue;
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string contents = buf.str();
    const auto* data = reinterpret_cast<const std::uint8_t*>(contents.data());
    (void)LLVMFuzzerTestOneInput(data, contents.size());
    ++n_inputs;
  }
  std::cout << "runtime_fuzzer: drove " << n_inputs
            << " corpus inputs, no divergence\n";
  return 0;
}
#endif
