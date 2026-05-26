#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "runtime.h"

namespace jitse {

// P0 (stateful-operator IR lowering). A bitmask selecting which operators
// the JIT should emit inline IR for instead of calling the C runtime.
//
// Default (kNone) preserves the pre-P0 behavior: every stateful op is an
// extern "C" call to jit_rt_*. The fuzz/parity test suite gates kAll.
//
// Note: lowering is opt-in to keep parity invariants visible. Numerical
// equivalence to the runtime path is asserted by stateful_lowering_parity_test.
enum class StatefulLoweringFlags : unsigned {
  kNone = 0,
  kSma  = 1u << 0,
  kEma  = 1u << 1,
  kLag  = 1u << 2,
  kAll  = kSma | kEma | kLag,
};
inline StatefulLoweringFlags operator|(StatefulLoweringFlags a, StatefulLoweringFlags b) {
  return static_cast<StatefulLoweringFlags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
inline bool HasFlag(StatefulLoweringFlags v, StatefulLoweringFlags bit) {
  return (static_cast<unsigned>(v) & static_cast<unsigned>(bit)) != 0;
}

// P1 (profile-guided specialization).
//
// A JitProfile describes runtime properties of the program that the JIT may
// use to elide guard branches that LLVM cannot prove unreachable from static
// information alone. The supported property is `assume_warm`: every stateful
// operator (sma/ema/lag) in the program has been called at least
// `period` (sma/lag) or 1 (ema) times, so every guard branch around
// "is this state initialized / is the ring buffer full?" is dead in steady
// state.
//
// This is the same property V8 TurboFan, LuaJIT, and Hotspot's tiered
// compilers exploit when they recompile a hot function with profile data:
// invariants that are monotonic ("once warm, always warm") can be assumed
// after a sufficient number of observations, without a deopt path, because
// they cannot be invalidated by later input.
//
// Safety: a function compiled with `assume_warm=true` produces undefined
// values if called BEFORE every stateful node has reached steady state.
// The harness (TieredProgramJit / caller) is responsible for not invoking
// the specialized function until the warmup-tick threshold is met.
struct JitProfile {
  bool assume_warm = false;
};

// Returns the minimum number of ticks the program must be evaluated before
// every stateful operator is in steady state (count >= capacity for sma/lag,
// initialized == 1 for ema). Conservative: scans every stateful op in
// `signals` and returns max(period over sma/lag, max(1, ema_period) over ema).
// Callers may use this as the safe lower bound before invoking a function
// compiled with `assume_warm=true`.
std::int64_t ComputeProgramWarmupThreshold(const std::vector<SignalDef>& signals);

// P2 (cross-symbol vectorization).
//
// A vectorized program is the same DSL program compiled to process K symbols
// per call by widening every IR `double` to `<K x double>`. Each lane has its
// own MarketState pointer and its own per-symbol SignalContext slot. Lanes
// share the same SignalDef AST and the same compiled function.
//
// Scope: BOTH stateless AND stateful signals are supported.
//   * Stateless ops (mid/bid/ask/spread/+,-,*,/,abs,sqrt,log/comparisons/
//     logicals/conditionals) are widened to `<K x double>` directly --
//     one IR op runs on all K lanes simultaneously (P2).
//   * Stateful ops (sma/ema/lag/rolling_*/vwap/cross_*/rolling_corr/
//     rolling_beta/kalman1d) are implemented via per-lane scalarized
//     fan-out (P10): for each lane the JIT extracts the scalar input,
//     looks up the lane's per-symbol SignalContext via
//     jit_rt_symbol_ctx(arena, base_symbol + lane), calls the existing
//     scalar `jit_rt_*` runtime helper, and inserts the result back
//     into the K-wide output. Each lane's state lives in an
//     independent SignalContext slot, so no cross-lane aliasing.
//
// One remaining vectorization restriction: when `SetStatefulLowering`
// has enabled the P0 inline-IR lowering for any of sma/ema/lag, the
// vectorized compile rejects programs containing the lowered op. The
// P0 lowered IR caches scalar base pointers and threads them through
// scalar arithmetic; widening that to K lanes is non-trivial and is
// deliberately not attempted. With lowering off (the default), every
// stateful op is supported in vectorized mode.
//
// What you get for stateless programs:
//   * `<K x double>` arithmetic, comparisons, and select for conditionals.
//   * Market loads built from K scalar reads (LLVM may further optimize
//     these into a gather on AVX2; we don't depend on that.)
//   * A single call processes K symbols, where the scalar path would do
//     K separate calls. Throughput on stateless workloads scales close to
//     linearly with K.
//
// Layout:
//   * `per_lane_market` is a pointer to K consecutive `MarketState*` pointers.
//   * `arena` carries K per-symbol SignalContexts at indices
//     [base_symbol .. base_symbol + K). For pure-stateless MVP this is unused
//     but the parameter is kept for forward-compatibility with the per-lane
//     scalarization extension.
//   * `outputs` is laid out signal-major / lane-minor:
//         outputs[signal_index * K + lane]
//     so a `<K x double>` vector store of one signal writes K contiguous
//     doubles.

// P5 (compile-time vs runtime tradeoff measurement).
//
// CompileTimings captures the wall-clock breakdown of the most recent
// successful CompileProgram / CompileProgramSpecialized / CompileProgram-
// Vectorized call. All values are nanoseconds measured on the calling
// thread via std::chrono::steady_clock. Sum across the three phases is
// always <= total_ns (the difference is bookkeeping outside the timed
// scopes, which is negligible in practice but not exactly zero).
//
// The three phases correspond to the natural cost centers in an
// LLVM-based JIT:
//
//   ast_to_ir_ns    - building the llvm::Module: walking the SignalDef AST,
//                     creating LLVM types, emitting llvm::Function bodies,
//                     verifyFunction(). O(program size). For our DSL this
//                     is typically the smallest of the three.
//   llvm_opt_ns     - running `PassBuilder::buildPerModuleDefaultPipeline(O2)`
//                     on the emitted module. This is what most "JIT compile
//                     time" perception is: SROA, GVN, instcombine, the
//                     inliner, the loop unroller, etc. For our DSL this is
//                     usually the largest phase.
//   orc_codegen_ns  - LLJIT's machine-code generation: addIRModule schedules
//                     the module through ORC, which invokes the MCJIT-style
//                     ISel + register allocator and writes executable memory.
//                     The first lookup() blocks until codegen completes.
//                     For our DSL this is comparable to llvm_opt_ns.
struct CompileTimings {
  std::uint64_t ast_to_ir_ns = 0;
  std::uint64_t llvm_opt_ns = 0;
  std::uint64_t orc_codegen_ns = 0;
  std::uint64_t total_ns = 0;
};

class JitCompiler {
 public:
  using JitFn = double (*)(const MarketState*, MultiSymbolSignalContext*, std::uint32_t);
  using ProgramFn = void (*)(const MarketState*, MultiSymbolSignalContext*, std::uint32_t, double*);
  // P2: vectorized entry point. See class-level comment for parameter layout.
  using ProgramFnVec = void (*)(
      const MarketState* const* /* per_lane_market */,
      MultiSymbolSignalContext* /* arena */,
      std::uint32_t /* base_symbol */,
      double* /* outputs */);

  JitCompiler();
  ~JitCompiler();

  JitCompiler(const JitCompiler&) = delete;
  JitCompiler& operator=(const JitCompiler&) = delete;

  bool IsAvailable() const;
  bool HasAVX2() const;
  std::string LastError() const;

  // P0: select which stateful operators emit inline IR instead of runtime calls.
  // Overrides the JITSE_LOWER_STATEFUL env var. Must be called before Compile/CompileProgram.
  void SetStatefulLowering(StatefulLoweringFlags flags);
  StatefulLoweringFlags GetStatefulLowering() const;

  // Compile one signal expression into native code.
  bool Compile(const SignalDef& signal, const SymbolTable& symbols);
  bool CompileProgram(const std::vector<SignalDef>& signals, const SymbolTable& symbols);

  // P1: specialize CompileProgram with the supplied profile. Currently the
  // only profile property is `assume_warm`, which elides the warmup-guard
  // branches in the lowered sma/ema/lag IR. State arrays MUST already be
  // warm before the returned function is invoked; see ComputeProgramWarmup-
  // Threshold(). The caller is also responsible for matching node IDs and
  // symbol bindings between the baseline and specialized compiles (use the
  // same `signals`/`symbols` you passed to CompileProgram).
  bool CompileProgramSpecialized(
      const std::vector<SignalDef>& signals,
      const SymbolTable& symbols,
      const JitProfile& profile);

  // P2: compile the program in vectorized mode that processes `lane_count`
  // symbols per call. See ProgramFnVec / the file-level P2 comment for the
  // parameter layout. `lane_count` must be 2, 4, or 8; 4 is the canonical
  // choice on AVX2 (one `<4 x double>` register = one ymm). Fails (returns
  // false, sets LastError()) for programs containing any stateful op.
  // The compiled function pointer is retrieved via GetProgramVectorizedFunction().
  bool CompileProgramVectorized(
      const std::vector<SignalDef>& signals,
      const SymbolTable& symbols,
      unsigned lane_count);

  void DumpLastIR() const;
  void DumpLastIRPreOpt() const;
  const std::string& LastIRPostOpt() const;
  const std::string& LastIRPreOpt() const;

  // P9: host-target assembly for the most recent successful compile.
  // Captured by cloning the post-O2 module and running it through
  // `TargetMachine::addPassesToEmitFile(AssemblyFile)`. The asm
  // reflects what the LLVM backend would emit for the host CPU; the
  // actual JIT-resident machine code may differ slightly when the
  // host has features that the asm-dump TargetMachine did not enable
  // (we deliberately pass an empty features string so the dump
  // matches the baseline ISA that `llvm-mca` defaults to). Empty
  // string on any failure (target backend not registered, clone
  // failure, etc.) -- the asm dump is a diagnostic, not a hard
  // requirement.
  void DumpLastAsm() const;
  const std::string& LastAsm() const;

  JitFn GetFunction() const;
  ProgramFn GetProgramFunction() const;
  // P2: returns the currently compiled vectorized function, or nullptr if
  // CompileProgramVectorized has not been called or failed. Note: the
  // vectorized fn shares storage with the scalar GetProgramFunction(); a
  // single JitCompiler instance holds at most one program at a time.
  ProgramFnVec GetProgramVectorizedFunction() const;
  // P2: the lane count of the last successful CompileProgramVectorized(), or
  // 0 if no vectorized compile has succeeded.
  unsigned VectorizedLaneCount() const;

  // P5: the per-phase compile timings of the MOST RECENT successful
  // CompileProgram / CompileProgramSpecialized / CompileProgramVectorized
  // call. Zeroed before the next compile and on Compile() errors. The
  // single-signal Compile() path also reports timings; the breakdown
  // semantics are identical (AST->IR, LLVM O2, ORC codegen).
  CompileTimings LastCompileTimings() const;

  // P13: persistent JIT module cache.
  //
  // When enabled, every CompileProgram* call computes a deterministic
  // hash over (AST canonical string, lowering flags, assume_warm flag,
  // lane count, host AVX2 capability, LLVM version) and uses that hash
  // to look up a previously-compiled post-O2 LLVM bitcode file under
  // `cache_dir`. On a hit, the AST -> IR + LLVM O2 phases are skipped
  // entirely; the cached bitcode is parsed and handed straight to ORC,
  // which is the only phase that produces a real fn pointer. On a
  // miss, the normal compile runs and the post-O2 module is written
  // atomically (write-to-temp + rename) so a concurrent compile of the
  // same key sees a fully-consistent bitcode file.
  //
  // The cache key includes everything that influences the post-O2 IR
  // shape (compiler flags, lowering, lane count, AVX2 feature). It
  // does NOT include the JIT impl pointer or any per-instance state,
  // so multiple JitCompiler instances (across runs of the same
  // process, or across separate process invocations) share the cache
  // transparently.
  //
  // `cache_dir` is created if it doesn't exist. Pass an empty string
  // to disable caching (default). Bitcode files are named
  // `<hash>.<variant>.bc` where variant is one of {scalar, warm,
  // vec<K>}. Compatibility across LLVM versions is enforced by
  // including the LLVM major version in the hash input -- bitcode is
  // not stable across LLVM major versions.
  //
  // `LastCacheHit()` returns true iff the most recent successful
  // CompileProgram* call was served from the on-disk cache. Useful for
  // tests / benchmarks; the `last_compile_timings.total_ns` is
  // populated on both hit and miss paths so the speedup is directly
  // measurable.
  void EnableModuleCache(const std::string& cache_dir);
  void DisableModuleCache();
  bool ModuleCacheEnabled() const;
  bool LastCacheHit() const;

  // The Impl name is public (forward-declared only) so internal compile
  // helpers in jit_compiler.cpp can take `Impl&` parameters. The struct
  // body remains in the .cpp; clients cannot inspect it.
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// P1: a two-tier JIT wrapper. Compiles a baseline (with full warmup-guard
// branches) immediately, and lazily compiles a branch-stripped specialized
// version on demand. The active function pointer is selected via an atomic
// load so multi-threaded readers see a consistent baseline-or-specialized
// implementation without needing a mutex on the hot path.
//
// Usage:
//   TieredProgramJit tjit;
//   tjit.Compile(signals, symbols);          // compile baseline
//   for (i = 0; i < N; ++i) {
//     ProgramFn fn = tjit.CurrentFunction(); // baseline initially
//     fn(market, ctx, sym, outputs);
//     if (i + 1 == tjit.WarmupTickThreshold()) {
//       tjit.Promote();                       // recompile + swap to specialized
//     }
//   }
//
// Lifetime: the caller retains ownership of `signals` and `symbols`. They
// MUST remain alive (and unmodified beyond node_id/symbol_id annotations)
// from Compile() through the last call to CurrentFunction(). Compile() does
// not clone the AST, so both the baseline and specialized JIT compile against
// the exact same FunctionCall*, ensuring node_ids and state-slot indices are
// identical to whatever the caller has prewarmed in the SignalContext.
//
// Precondition: the caller has called AllocateProgramNodeIds(signals) and
// BindSymbolIds on every signal before invoking Compile(). The baseline
// compile still has the lazy GetNodeId fallback, but the specialized compile
// requires pre-assigned IDs because it runs static analysis to identify
// warm-safe stateful nodes; lazy allocation would yield inconsistent IDs.
class TieredProgramJit {
 public:
  using ProgramFn = JitCompiler::ProgramFn;

  TieredProgramJit();
  ~TieredProgramJit();

  TieredProgramJit(const TieredProgramJit&) = delete;
  TieredProgramJit& operator=(const TieredProgramJit&) = delete;

  bool IsAvailable() const;

  // Compiles the baseline immediately with the requested lowering flags.
  // Stores the inlined signals + symbols internally for the later Promote().
  bool Compile(const std::vector<SignalDef>& signals, const SymbolTable& symbols,
               StatefulLoweringFlags lowering = StatefulLoweringFlags::kAll);

  // Compiles the specialized (assume_warm) version and atomically swaps the
  // active function pointer. Returns true on success or if already promoted.
  // The caller MUST guarantee the program has been run for at least
  // WarmupTickThreshold() ticks against the same SignalContext before any
  // subsequent invocation of CurrentFunction().
  bool Promote();

  // Returns the currently active function pointer (baseline pre-Promote(),
  // specialized post-Promote()).
  ProgramFn CurrentFunction() const;

  // The minimum tick count after which calling Promote() is guaranteed safe.
  std::int64_t WarmupTickThreshold() const;

  bool IsPromoted() const;
  std::string LastError() const;

  // Diagnostics: access the baseline and specialized IR strings.
  const std::string& BaselineIRPostOpt() const;
  const std::string& SpecializedIRPostOpt() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace jitse
