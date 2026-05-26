#include "jit_compiler.h"

#include "ast_clone.h"  // P13: AstCanonicalString for cache-key hashing
#include "ast_utils.h"

#include <algorithm>
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
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef JITSE_HAS_LLVM
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
// P9: target-machine + legacy PM headers for ASM emission via
// `addPassesToEmitFile`. The new PM does not yet have a public asm-
// emit entry, so the legacy PassManager remains the supported path.
#include <llvm/MC/TargetRegistry.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Utils/Cloning.h>
// P13: bitcode read/write for the persistent JIT module cache. We
// serialise the POST-O2 module so a hit skips both AST->IR emission
// and the LLVM optimisation pipeline; the ORC codegen step still
// runs on every compile, since that produces the actual fn pointer.
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#endif

namespace jitse {

std::int64_t ComputeProgramWarmupThreshold(const std::vector<SignalDef>& signals) {
  std::int64_t threshold = 0;
  std::function<void(const Expr&)> walk = [&](const Expr& expr) {
    if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
      walk(*u->operand);
      return;
    }
    if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
      walk(*b->left);
      walk(*b->right);
      return;
    }
    if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
      walk(*c->condition);
      walk(*c->then_branch);
      walk(*c->else_branch);
      return;
    }
    if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
      // Stateful, warmup-bounded operators in the JIT.
      const bool warmup_bounded =
          (fn->name == "sma" || fn->name == "ema" || fn->name == "lag" ||
           fn->name == "rolling_std" || fn->name == "rolling_min" ||
           fn->name == "rolling_max" || fn->name == "zscore" || fn->name == "vwap");
      if (warmup_bounded && fn->args.size() >= 2) {
        if (const auto* period_node = dynamic_cast<const NumberLiteral*>(fn->args[1].get())) {
          const std::int64_t period = static_cast<std::int64_t>(period_node->value);
          // EMA reaches steady state after 1 call; sma/lag/rolling_* take `period`.
          const std::int64_t op_threshold = (fn->name == "ema") ? 1 : period;
          if (op_threshold > threshold) threshold = op_threshold;
        }
      }
      for (const auto& a : fn->args) walk(*a);
      return;
    }
  };
  for (const auto& s : signals) walk(*s.body);
  return threshold;
}

namespace {
StatefulLoweringFlags ParseStatefulLoweringEnv() {
  const char* env = std::getenv("JITSE_LOWER_STATEFUL");
  if (env == nullptr || env[0] == '\0') return StatefulLoweringFlags::kNone;
  std::string val(env);
  if (val == "0" || val == "off" || val == "OFF" || val == "false") return StatefulLoweringFlags::kNone;
  if (val == "1" || val == "all" || val == "ALL") return StatefulLoweringFlags::kAll;
  // Comma-separated op list, e.g. "sma,ema,lag".
  StatefulLoweringFlags flags = StatefulLoweringFlags::kNone;
  std::string token;
  for (std::size_t i = 0; i <= val.size(); ++i) {
    const char c = (i < val.size()) ? val[i] : ',';
    if (c == ',' || c == ' ') {
      if (token == "sma") flags = flags | StatefulLoweringFlags::kSma;
      else if (token == "ema") flags = flags | StatefulLoweringFlags::kEma;
      else if (token == "lag") flags = flags | StatefulLoweringFlags::kLag;
      token.clear();
    } else {
      token.push_back(c);
    }
  }
  return flags;
}

// P9: emit the host-target assembly for `module` as a string, using
// the legacy PassManager + TargetMachine::addPassesToEmitFile path
// (the new PM does not yet expose a public asm-emit entry). The
// module is *cloned* before code generation so we can capture asm
// without disturbing the IR that ORC will subsequently consume.
//
// On any failure (unknown triple, no target backend registered)
// returns an empty string and writes the reason into `err_out`. The
// caller is free to ignore the failure -- ASM dump is a diagnostic
// feature, not a correctness gate.
#ifdef JITSE_HAS_LLVM
std::string EmitHostAsm(const llvm::Module& module, std::string& err_out) {
  // The asm pipeline mutates the module in flight (it inserts target-
  // specific instruction selection, ABI lowering passes, etc.), so we
  // operate on a clone.
  std::unique_ptr<llvm::Module> clone = llvm::CloneModule(module);
  if (!clone) {
    err_out = "CloneModule failed";
    return {};
  }

  // Resolve the host triple. We trust whatever the JIT set on the
  // module (matches the ORC code path).
#if LLVM_VERSION_MAJOR >= 21
  const llvm::Triple& triple = clone->getTargetTriple();
  const std::string triple_str = triple.str();
#else
  const std::string triple_str = clone->getTargetTriple();
  const llvm::Triple triple(triple_str);
#endif

  std::string lookup_err;
#if LLVM_VERSION_MAJOR >= 21
  const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookup_err);
#else
  const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple_str, lookup_err);
#endif
  if (!target) {
    err_out = "lookupTarget(" + triple_str + ") failed: " + lookup_err;
    return {};
  }

  // Build a fresh TargetMachine matching the host. We deliberately
  // pass `""` for the features string so codegen targets the
  // baseline ISA for the triple, which is what `llvm-mca` defaults
  // to and what we want in the checked-in artifact. The actual JIT
  // code path uses host features (set elsewhere); the asm dump is a
  // diagnostic snapshot, not the executed code.
  llvm::TargetOptions opts;
  std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
#if LLVM_VERSION_MAJOR >= 21
      triple, llvm::sys::getHostCPUName(), /*features=*/"", opts, llvm::Reloc::PIC_));
#else
      triple_str, llvm::sys::getHostCPUName(), /*features=*/"", opts, llvm::Reloc::PIC_));
#endif
  if (!tm) {
    err_out = "createTargetMachine failed for " + triple_str;
    return {};
  }
  clone->setDataLayout(tm->createDataLayout());

  llvm::SmallString<16384> buf;
  llvm::raw_svector_ostream os(buf);
  // CGFT_AssemblyFile in older LLVM; CodeGenFileType::AssemblyFile in
  // newer. The unscoped enum value 0 still corresponds to
  // AssemblyFile on every supported version, so we use the typed
  // constant when present.
#if LLVM_VERSION_MAJOR >= 18
  const auto file_type = llvm::CodeGenFileType::AssemblyFile;
#else
  const auto file_type = llvm::CGFT_AssemblyFile;
#endif
  // The buffer_ostream wrapper exists because addPassesToEmitFile
  // requires a stream that supports pwrite (random-access writes),
  // which raw_svector_ostream does not. buffer_ostream buffers
  // internally and flushes its captured writes on destruction, so we
  // confine it to a scope and read `buf` *after* the flush.
  {
    llvm::buffer_ostream bos(os);
    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, bos, /*DwoOut=*/nullptr, file_type)) {
      err_out = "addPassesToEmitFile rejected AssemblyFile (target lacks asm printer?)";
      return {};
    }
    pm.run(*clone);
  }
  return std::string(buf.str());
}
#endif

}  // namespace

struct JitCompiler::Impl {
  std::string last_error;
  std::string last_ir_pre_opt;
  std::string last_ir_post_opt;
  // P9: host-target assembly snapshot of the post-O2 module, captured
  // by `EmitHostAsm`. Mirrors the lifecycle of last_ir_post_opt: reset
  // by every Compile* entry, populated only on a successful compile.
  std::string last_asm;
  JitFn fn = nullptr;
  ProgramFn program_fn = nullptr;
  // P2: vectorized program function and its lane count. Only one of
  // `program_fn` / `program_fn_vec` is meaningful at a time -- compiling a
  // scalar program clears `program_fn_vec` and vice versa.
  ProgramFnVec program_fn_vec = nullptr;
  unsigned vec_lane_count = 0;
  StatefulLoweringFlags stateful_lowering = StatefulLoweringFlags::kNone;
  // P5: see CompileTimings doc in jit_compiler.h. Reset by every CompileProgram*
  // and Compile() entry; populated only on success.
  CompileTimings last_compile_timings{};

  // P13: persistent JIT module cache. `cache_dir` empty means
  // disabled (default). `last_cache_hit` records whether the most
  // recent successful CompileProgram* served from the on-disk cache.
  // See `EnableModuleCache` doc in jit_compiler.h.
  std::string cache_dir;
  bool last_cache_hit = false;

#ifdef JITSE_HAS_LLVM
  std::unique_ptr<llvm::orc::LLJIT> lljit;
  std::uint64_t compile_counter = 0;
  bool runtime_symbols_registered = false;
  bool host_has_avx2 = false;
#endif
};

// Forward declarations for the program-compile helper used by both the
// regular CompileProgram path and CompileProgramSpecialized. Defined inside
// the JITSE_HAS_LLVM block below.
#ifdef JITSE_HAS_LLVM
namespace {
bool CompileProgramImpl(
    JitCompiler::Impl& impl,
    const std::vector<SignalDef>& signals,
    const SymbolTable& symbols,
    StatefulLoweringFlags lowering,
    bool assume_warm,
    bool has_avx2,
    JitCompiler::ProgramFn& out_fn);

// P13: 64-bit FNV-1a of a string. We don't need a cryptographic
// hash here -- a 64-bit FNV-1a has good distribution on natural
// program text and the cost of a (vanishingly rare) collision is a
// recompile (ORC will fail to find the symbol and we'd fall back to
// the slow path; in practice an even rarer "wrong but well-typed"
// hit would be caught by the unit-test parity check).
inline std::uint64_t Fnv1aU64(const std::string& s) {
  constexpr std::uint64_t kOffset = 0xcbf29ce484222325ULL;
  constexpr std::uint64_t kPrime  = 0x100000001b3ULL;
  std::uint64_t h = kOffset;
  for (unsigned char c : s) {
    h ^= c;
    h *= kPrime;
  }
  return h;
}

inline std::string HexU64(std::uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

// P13: deterministic cache-key string. Combines the AST canonical
// form of every signal (in order), the lowering flag bits, the
// assume_warm bit, the lane count (1 for scalar), the host AVX2
// capability, and the LLVM major version. Anything that would make
// the post-O2 IR differ MUST be in here.
inline std::string BuildCacheKeyString(
    const std::vector<SignalDef>& signals,
    StatefulLoweringFlags lowering,
    bool assume_warm,
    bool has_avx2,
    unsigned lane_count) {
  std::string key;
  key.reserve(256);
  key.append("LLVM=");
  key.append(std::to_string(LLVM_VERSION_MAJOR));
  key.append(";lowering=");
  key.append(std::to_string(static_cast<unsigned>(lowering)));
  key.append(";warm=");
  key.append(assume_warm ? "1" : "0");
  key.append(";avx2=");
  key.append(has_avx2 ? "1" : "0");
  key.append(";lanes=");
  key.append(std::to_string(lane_count));
  for (const auto& s : signals) {
    key.append(";sig=");
    key.append(s.name);
    key.append("=");
    if (s.body) key.append(AstCanonicalString(*s.body));
  }
  return key;
}

inline std::string CacheFileNameForKey(std::uint64_t hash, const std::string& variant) {
  return HexU64(hash) + "." + variant + ".bc";
}

// P13: write `module` to `dst_path` atomically (temp+rename). LLVM's
// WriteBitcodeToFile takes a raw_ostream we open on a tempfile.
//
// Returns true on success. Errors are silent (caching is an
// optimisation, not a contract), but the bool lets the caller
// optionally surface them via `last_error` for debug visibility.
bool WriteModuleToCache(llvm::Module& module, const std::filesystem::path& dst_path) {
  std::error_code ec;
  std::filesystem::path tmp_path = dst_path;
  tmp_path += ".tmp";
  {
    std::error_code open_ec;
    llvm::raw_fd_ostream out(tmp_path.string(), open_ec);
    if (open_ec) return false;
    llvm::WriteBitcodeToFile(module, out);
    out.flush();
    if (out.has_error()) return false;
  }
  std::filesystem::rename(tmp_path, dst_path, ec);
  if (ec) {
    std::filesystem::remove(tmp_path);
    return false;
  }
  return true;
}

// P13: parse a bitcode file from `src_path` into a fresh Module
// living in `context`. Returns nullptr on any failure (file not
// found, malformed bitcode, version mismatch). The caller treats
// nullptr as a cache miss and falls back to the slow path.
std::unique_ptr<llvm::Module> ReadModuleFromCache(
    const std::filesystem::path& src_path, llvm::LLVMContext& context) {
  std::error_code ec;
  if (!std::filesystem::exists(src_path, ec) || ec) return nullptr;
  auto buf_or = llvm::MemoryBuffer::getFile(src_path.string());
  if (!buf_or) return nullptr;
  auto mod_or = llvm::parseBitcodeFile(buf_or.get()->getMemBufferRef(), context);
  if (!mod_or) {
    llvm::consumeError(mod_or.takeError());
    return nullptr;
  }
  return std::move(*mod_or);
}

}  // namespace
#endif

void JitCompiler::SetStatefulLowering(StatefulLoweringFlags flags) {
  impl_->stateful_lowering = flags;
}

StatefulLoweringFlags JitCompiler::GetStatefulLowering() const {
  return impl_->stateful_lowering;
}

JitCompiler::JitCompiler() : impl_(std::make_unique<Impl>()) {
  impl_->stateful_lowering = ParseStatefulLoweringEnv();
#ifdef JITSE_HAS_LLVM
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
  auto jit_or_err = llvm::orc::LLJITBuilder().create();
  if (!jit_or_err) {
    impl_->last_error = "Failed to create LLJIT: " + llvm::toString(jit_or_err.takeError());
    return;
  }
  impl_->lljit = std::move(*jit_or_err);
  // LLVM >= 19 dropped the StringMap<bool>& out-param overload of
  // `getHostCPUFeatures` in favor of returning the map directly. Keep
  // both compile paths so the codebase keeps building across LLVM
  // upgrades without forcing a hard floor on the LLVM version.
#if LLVM_VERSION_MAJOR >= 19
  {
    llvm::StringMap<bool> host_features = llvm::sys::getHostCPUFeatures();
    auto it = host_features.find("avx2");
    impl_->host_has_avx2 = (it != host_features.end()) && it->second;
  }
#else
  {
    llvm::StringMap<bool> host_features;
    if (llvm::sys::getHostCPUFeatures(host_features)) {
      auto it = host_features.find("avx2");
      impl_->host_has_avx2 = (it != host_features.end()) && it->second;
    } else {
      impl_->host_has_avx2 = false;
    }
  }
#endif
#else
  impl_->last_error = "LLVM support is disabled at build time";
#endif
}

JitCompiler::~JitCompiler() = default;

bool JitCompiler::IsAvailable() const {
#ifdef JITSE_HAS_LLVM
  return impl_->lljit != nullptr;
#else
  return false;
#endif
}

bool JitCompiler::HasAVX2() const {
#ifdef JITSE_HAS_LLVM
  if (!impl_->lljit) return false;
  const char* force_disable = std::getenv("JITSE_FORCE_DISABLE_AVX2");
  if (force_disable != nullptr && std::string(force_disable) == "1") return false;
  return impl_->host_has_avx2;
#else
  return false;
#endif
}

std::string JitCompiler::LastError() const { return impl_->last_error; }

#ifdef JITSE_HAS_LLVM
namespace {

struct CodegenContext {
  llvm::LLVMContext& llctx;
  llvm::Module& module;
  llvm::IRBuilder<>& builder;
  const SymbolTable& symbols;
  llvm::Value* market_arg;
  llvm::Value* arena_arg;
  llvm::Value* symbol_arg;
  llvm::Value* ctx_arg;

  llvm::FunctionCallee fn_mid;
  llvm::FunctionCallee fn_bid;
  llvm::FunctionCallee fn_ask;
  llvm::FunctionCallee fn_spread;
  llvm::FunctionCallee fn_ema;
  llvm::FunctionCallee fn_ema_alpha;
  llvm::FunctionCallee fn_sma;
  llvm::FunctionCallee fn_sma_prepare;
  llvm::FunctionCallee fn_rolling_std;
  llvm::FunctionCallee fn_zscore;
  llvm::FunctionCallee fn_rolling_min;
  llvm::FunctionCallee fn_rolling_max;
  llvm::FunctionCallee fn_vwap;
  llvm::FunctionCallee fn_lag;
  llvm::FunctionCallee fn_cross_above;
  llvm::FunctionCallee fn_cross_below;
  // P7: new operator runtime callees. Same calling convention as the
  // existing stateful runtime helpers: (ctx*, node_id i64, args..., period i64).
  llvm::FunctionCallee fn_rolling_corr;
  llvm::FunctionCallee fn_rolling_beta;
  llvm::FunctionCallee fn_kalman1d;
  // P0 lowered-state base accessors. Called once per JIT function; the IR
  // hoists the result and indexes by node_id for every per-op access.
  llvm::FunctionCallee fn_sma_lowered_base;
  llvm::FunctionCallee fn_ema_lowered_base;
  llvm::FunctionCallee fn_lag_lowered_base;

  std::unordered_map<const FunctionCall*, std::int64_t> node_ids;
  std::unordered_map<std::string, llvm::Value*> signal_values;
  struct CachedMarketFieldLoad {
    llvm::Value* value = nullptr;
    llvm::BasicBlock* block = nullptr;
  };
  // Whole-program fused path: one load per field at function entry, reused across blocks.
  std::unordered_map<std::uint64_t, llvm::Value*> program_market_field_cache;
  // Single-signal path: memo only within the current basic block (SIMD lowers create siblings).
  std::unordered_map<std::uint64_t, CachedMarketFieldLoad> market_field_cache;
  bool use_program_market_cache = false;
  std::int64_t next_node_id = 1;
  bool use_avx2 = false;

  // P0 lowering config and per-function base-pointer cache.
  StatefulLoweringFlags lowering = StatefulLoweringFlags::kNone;
  llvm::Value* sma_base_cached = nullptr;
  llvm::Value* ema_base_cached = nullptr;
  llvm::Value* lag_base_cached = nullptr;

  // P1 (profile-guided specialization). When true, the lowered IR emitters
  // assume every stateful node listed in `warm_safe_calls` is in steady
  // state and drop the warmup-guard branches around it. The caller is
  // responsible for not invoking the specialized function before every
  // warm-safe node has been warmed by the baseline path.
  //
  // Only stateful ops that are NOT lexically nested inside a Conditional
  // then-branch or else-branch are included in `warm_safe_calls`. A stateful
  // op that lives inside a `if X then A else B` branch is updated only on
  // ticks where the condition selects that branch, so the harness cannot
  // guarantee a fixed warmup tick count for it. For those nodes we fall back
  // to the baseline IR even in assume_warm mode.
  bool assume_warm = false;
  std::unordered_set<const FunctionCall*> warm_safe_calls;

  // P2 (cross-symbol vectorization). When `lane_count > 1`, every IR `double`
  // becomes a `<lane_count x double>`. Supports lane_count in {2, 4, 8}.
  // The `per_lane_market_arg` points at an array of `lane_count`
  // `MarketState*` pointers; the i-th lane's market data is loaded from
  // `per_lane_market_arg[i]`. Note that the scalar `market_arg` is unused
  // when vectorized; the codegen uses `per_lane_market_arg` exclusively.
  //
  // P10 (per-lane scalarized fan-out for stateful ops). Stateful ops
  // (sma/ema/lag/rolling_*/vwap/cross_*/rolling_corr/rolling_beta/
  // kalman1d) execute one per lane via `EmitScalarizedFanOut`: K
  // extractelements, K runtime helper calls against per-lane scalar
  // SignalContexts (jit_rt_symbol_ctx(arena, base_symbol + lane)), and
  // K insertelements. There is no aliasing across lanes -- each lane
  // operates on a fully independent state slot. The only stateful path
  // that is still rejected in vector mode is the P0 lowered-IR variant
  // (kSma/kEma/kLag), which caches scalar base pointers that don't map
  // cleanly to K-lane fan-out.
  unsigned lane_count = 1;
  llvm::Value* per_lane_market_arg = nullptr;

  // P11: per-program-compile cache of stateful runtime-call results
  // keyed by FunctionCall::node_id. AllocateNodeIds aliases the
  // node-id of structurally-equal stateful subtrees when the first
  // occurrence is at an unconditional position (top-level or inside
  // a Conditional `cond`), so EmitExpr can return the cached SSA
  // value for any second occurrence without re-emitting the runtime
  // call. Because the FIRST emit happens at an unconditional
  // BasicBlock that dominates all later branches, the cached Value
  // remains valid SSA in any dominated use site.
  //
  // node_ids are globally unique across one program-compile (the
  // dedup pass only aliases within ONE signal body), so this cache
  // can be shared across signal-body emissions in the program-fused
  // compile without risk of key collision.
  std::unordered_map<std::int64_t, llvm::Value*> stateful_emit_cache;
};

// P2 helpers. They are no-ops in scalar mode (lane_count == 1) so any caller
// site can be widened by replacing `f64` -> VecTy(cg) and constant builders
// with SplatConst(cg, ...) without breaking the scalar path.
inline llvm::Type* VecTy(CodegenContext& cg) {
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  if (cg.lane_count <= 1) return f64;
  return llvm::FixedVectorType::get(f64, cg.lane_count);
}
inline llvm::Constant* SplatConst(CodegenContext& cg, double v) {
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  llvm::Constant* c = llvm::ConstantFP::get(f64, v);
  if (cg.lane_count <= 1) return c;
  return llvm::ConstantVector::getSplat(
      llvm::ElementCount::getFixed(cg.lane_count), c);
}
inline llvm::Constant* SplatNaN(CodegenContext& cg) {
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  llvm::Constant* c = llvm::ConstantFP::getNaN(f64);
  if (cg.lane_count <= 1) return c;
  return llvm::ConstantVector::getSplat(
      llvm::ElementCount::getFixed(cg.lane_count), c);
}
inline bool IsVectorized(const CodegenContext& cg) { return cg.lane_count > 1; }
[[noreturn]] inline void RejectInVector(const std::string& op_name) {
  throw std::runtime_error(
      "vectorized JIT does not support stateful op '" + op_name +
      "' for this code path. Stateful ops are supported in vectorized "
      "mode via per-lane scalarized fan-out (P10); this specific op is "
      "still rejected (likely because it interacts with the P0 lowered-"
      "state base pointers, which are scalar-only). "
      "Use scalar CompileProgram for programs containing this op.");
}

// P10: per-lane scalarized fan-out of a stateful operator in
// vectorized mode.
//
// A vectorized program processes K symbols per call by widening every
// `double` to `<K x double>`. Stateless ops can be vectorized
// directly (one IR op runs on all K lanes). Stateful ops cannot:
// each lane has its own per-symbol SignalContext slot, so the state
// for lane i and lane j live in completely independent memory
// regions, which means there is no single `<K x double>` load/store
// that can read or write all K lane states at once. The natural
// resolution is to scalarize the stateful op across lanes: for each
// lane, extract the scalar input, look up the lane's per-symbol
// SignalContext, call the existing scalar `jit_rt_*` helper, then
// insert the scalar result into the output vector.
//
// LLVM's loop vectorizer cannot do this for us because the runtime
// helper calls are opaque (`jit_rt_*` are extern "C" function calls
// with side effects and an unknown summary). We do the fan-out at IR
// emission time.
//
// The helper is parameterized over a `build_per_lane_call` callable
// that, given (lane, per-lane scalar SignalContext*, per-lane scalar
// MarketState*), returns a scalar double Value*. The helper builds
// the necessary per-lane lookups (jit_rt_symbol_ctx for the lane,
// load of per_lane_market[lane]) once per lane and assembles the K
// scalars into the final `<K x double>` result.
//
// Per-lane symbol_id is `base_symbol + lane`, matching the SoA
// layout where lane i processes symbol base_symbol+i. node_id is
// shared across lanes (each stateful FunctionCall has exactly one
// node_id; the lane disambiguator is the per-symbol SignalContext
// returned by jit_rt_symbol_ctx).
template <typename BuildLane>
llvm::Value* EmitScalarizedFanOut(CodegenContext& cg, BuildLane build_per_lane_call,
                                  const char* result_name = "fanout_result") {
  llvm::Type* i32 = llvm::Type::getInt32Ty(cg.llctx);
  llvm::Type* i64 = llvm::Type::getInt64Ty(cg.llctx);
  llvm::Type* ctx_ptr_ty = llvm::PointerType::getUnqual(cg.llctx);
  llvm::Type* arena_ptr_ty = llvm::PointerType::getUnqual(cg.llctx);
  llvm::Type* mst_ptr_ty = llvm::PointerType::getUnqual(cg.llctx);

  // The per-lane SignalContext lookup goes through the same runtime
  // helper that the scalar-program entry uses (jit_rt_symbol_ctx).
  // We declare-or-insert it locally so the fan-out is self-contained.
  auto rt_symbol_ctx_ty = llvm::FunctionType::get(ctx_ptr_ty, {arena_ptr_ty, i32}, false);
  auto rt_symbol_ctx = cg.module.getOrInsertFunction("jit_rt_symbol_ctx", rt_symbol_ctx_ty);

  llvm::Value* result = llvm::UndefValue::get(VecTy(cg));
  for (unsigned lane = 0; lane < cg.lane_count; ++lane) {
    // lane_sym = (i32) base_symbol + lane. cg.symbol_arg is i32 in
    // the vectorized program entry (the scalar entry uses i32 too),
    // so no cast is needed except for the lane constant.
    llvm::Value* lane_offset = llvm::ConstantInt::get(i32, lane);
    llvm::Value* lane_sym =
        cg.builder.CreateAdd(cg.symbol_arg, lane_offset, "lane_sym");

    // Per-lane scalar SignalContext for stateful runtime helpers.
    llvm::Value* lane_ctx = cg.builder.CreateCall(
        rt_symbol_ctx, {cg.arena_arg, lane_sym}, "lane_ctx");

    // Per-lane scalar MarketState pointer for ops that need market
    // data (vwap is the only one in this category today).
    llvm::Value* lane_idx_i64 = llvm::ConstantInt::get(i64, lane);
    llvm::Value* lane_market_slot = cg.builder.CreateInBoundsGEP(
        mst_ptr_ty, cg.per_lane_market_arg, lane_idx_i64, "lane_mst_slot");
    llvm::Value* lane_market = cg.builder.CreateLoad(
        mst_ptr_ty, lane_market_slot, "lane_mst");

    llvm::Value* scalar_result = build_per_lane_call(lane, lane_ctx, lane_market);

    result = cg.builder.CreateInsertElement(
        result, scalar_result, llvm::ConstantInt::get(i32, lane),
        (std::string(result_name) + "_ins").c_str());
  }
  return result;
}

// Extracts the `lane`-th scalar element from a `<K x double>`. Pass-
// through when `v` is already scalar (which happens when the stateful
// op has, e.g., a literal-period second argument).
inline llvm::Value* ExtractLane(CodegenContext& cg, llvm::Value* v, unsigned lane) {
  if (!v->getType()->isVectorTy()) return v;
  return cg.builder.CreateExtractElement(
      v, llvm::ConstantInt::get(llvm::Type::getInt32Ty(cg.llctx), lane),
      "lane_x");
}

// Walks `signals` and returns the subset of stateful FunctionCalls that are
// NOT lexically nested under any Conditional then-branch or else-branch (the
// condition itself is fine because it is evaluated unconditionally by the
// JIT for `if cond then A else B`). These nodes are guaranteed to be pushed
// every tick, so after `period` ticks they are unconditionally warm.
std::unordered_set<const FunctionCall*> CollectWarmSafeStatefulCalls(
    const std::vector<SignalDef>& signals) {
  std::unordered_set<const FunctionCall*> safe;
  std::function<void(const Expr&, int)> walk = [&](const Expr& expr, int conditional_depth) {
    if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
      walk(*u->operand, conditional_depth);
      return;
    }
    if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
      walk(*b->left, conditional_depth);
      walk(*b->right, conditional_depth);
      return;
    }
    if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
      // The condition is always evaluated, so it inherits depth.
      walk(*c->condition, conditional_depth);
      walk(*c->then_branch, conditional_depth + 1);
      walk(*c->else_branch, conditional_depth + 1);
      return;
    }
    if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
      const bool stateful_lowered =
          (fn->name == "sma" || fn->name == "ema" || fn->name == "lag");
      if (stateful_lowered && conditional_depth == 0) {
        safe.insert(fn);
      }
      for (const auto& a : fn->args) walk(*a, conditional_depth);
      return;
    }
  };
  for (const auto& s : signals) walk(*s.body, 0);
  return safe;
}

std::uint64_t MarketFieldCacheKey(std::size_t sym_id, unsigned field_index) {
  return (static_cast<std::uint64_t>(sym_id) << 2) | static_cast<std::uint64_t>(field_index);
}

std::int64_t GetNodeId(CodegenContext& cg, const FunctionCall* fn) {
  if (fn->node_id > 0) {
    return fn->node_id;
  }
  auto it = cg.node_ids.find(fn);
  if (it != cg.node_ids.end()) {
    return it->second;
  }
  const std::int64_t id = cg.next_node_id++;
  fn->node_id = id;
  cg.node_ids.emplace(fn, id);
  return id;
}

llvm::Value* EmitMarketFieldLoad(CodegenContext& cg, std::size_t sym_id, unsigned field_index, const char* name) {
  const std::uint64_t cache_key = MarketFieldCacheKey(sym_id, field_index);
  if (cg.use_program_market_cache) {
    if (auto it = cg.program_market_field_cache.find(cache_key); it != cg.program_market_field_cache.end()) {
      return it->second;
    }
  } else {
    llvm::BasicBlock* const current_bb = cg.builder.GetInsertBlock();
    if (auto it = cg.market_field_cache.find(cache_key); it != cg.market_field_cache.end()) {
      if (it->second.block == current_bb && it->second.value != nullptr) {
        return it->second.value;
      }
    }
  }

  // Mirror MarketState/InstrumentState layout for direct loads in JIT IR.
  // InstrumentState in runtime.h: alignas(64) { bid, ask, last_price, volume,
  // last_update_ns } -- the alignas(64) pads sizeof(InstrumentState) from
  // the natural 40 bytes to 64 bytes so consecutive instruments live one
  // cache line apart. We MUST reflect that 24-byte tail-padding in the IR
  // struct or GEP arithmetic stride mismatches and `instruments[sym_id]`
  // loads from the wrong address for any sym_id > 0. The
  // node_state_layout_test pins sizeof(InstrumentState) == 64 to keep this
  // matched.
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  llvm::Type* i64 = llvm::Type::getInt64Ty(cg.llctx);
  llvm::Type* i32 = llvm::Type::getInt32Ty(cg.llctx);
  llvm::Type* i8 = llvm::Type::getInt8Ty(cg.llctx);
  llvm::Type* tail_pad_ty = llvm::ArrayType::get(i8, 24);
  llvm::StructType* instrument_ty = llvm::StructType::get(
      cg.llctx, {f64, f64, f64, f64, i64, tail_pad_ty});
  llvm::ArrayType* instruments_arr_ty = llvm::ArrayType::get(instrument_ty, kMaxInstruments);
  llvm::StructType* market_ty = llvm::StructType::get(cg.llctx, {instruments_arr_ty, i64});
  llvm::PointerType* market_ptr_ty = llvm::PointerType::getUnqual(market_ty);
  llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
  llvm::Value* sym = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(sym_id));

  llvm::Value* loaded = nullptr;
  if (IsVectorized(cg)) {
    // P2: build a <K x double> by loading one scalar per lane from a
    // different MarketState pointer. The per-lane pointers live in a
    // K-element array passed as `per_lane_market_arg`. LLVM cannot lower
    // this into a single SIMD load (the underlying memory is on K
    // independent MarketState allocations) but it can often fold the K
    // scalar loads into a gather on AVX2; either way, the wider lanes
    // amortize the ALU work after the load.
    llvm::PointerType* mst_ptr_ptr_ty = llvm::PointerType::getUnqual(cg.llctx);
    llvm::Value* result_vec = llvm::UndefValue::get(VecTy(cg));
    for (unsigned lane = 0; lane < cg.lane_count; ++lane) {
      llvm::Value* lane_idx = llvm::ConstantInt::get(i64, lane);
      llvm::Value* slot_ptr = cg.builder.CreateInBoundsGEP(
          mst_ptr_ptr_ty, cg.per_lane_market_arg, lane_idx,
          (std::string(name) + "_slot_ptr").c_str());
      llvm::Value* mst_i = cg.builder.CreateLoad(mst_ptr_ptr_ty, slot_ptr,
          (std::string(name) + "_mst").c_str());
      llvm::Value* typed_mst = cg.builder.CreateBitCast(mst_i, market_ptr_ty,
          (std::string(name) + "_mst_typed").c_str());
      llvm::Value* instruments_ptr = cg.builder.CreateStructGEP(
          market_ty, typed_mst, 0, "instruments_ptr");
      llvm::Value* instrument_ptr = cg.builder.CreateInBoundsGEP(
          instruments_arr_ty, instruments_ptr, {zero, sym}, "instrument_ptr");
      llvm::Value* field_ptr = cg.builder.CreateStructGEP(
          instrument_ty, instrument_ptr, field_index,
          (std::string(name) + "_field_ptr").c_str());
      llvm::Value* scalar_load = cg.builder.CreateLoad(f64, field_ptr,
          (std::string(name) + "_scalar").c_str());
      result_vec = cg.builder.CreateInsertElement(
          result_vec, scalar_load, llvm::ConstantInt::get(i32, lane),
          (std::string(name) + "_lane" + std::to_string(lane)).c_str());
    }
    loaded = result_vec;
  } else {
    llvm::Value* typed_market = cg.builder.CreateBitCast(cg.market_arg, market_ptr_ty, "market_typed");
    llvm::Value* instruments_ptr = cg.builder.CreateStructGEP(market_ty, typed_market, 0, "instruments_ptr");
    llvm::Value* instrument_ptr =
        cg.builder.CreateInBoundsGEP(instruments_arr_ty, instruments_ptr, {zero, sym}, "instrument_ptr");
    llvm::Value* field_ptr =
        cg.builder.CreateStructGEP(instrument_ty, instrument_ptr, field_index, std::string(name) + "_ptr");
    loaded = cg.builder.CreateLoad(f64, field_ptr, name);
  }

  if (cg.use_program_market_cache) {
    cg.program_market_field_cache.emplace(cache_key, loaded);
  } else {
    cg.market_field_cache.insert_or_assign(
        cache_key, CodegenContext::CachedMarketFieldLoad{loaded, cg.builder.GetInsertBlock()});
  }
  return loaded;
}

void PrewarmProgramMarketLoads(CodegenContext& cg, const std::vector<SignalDef>& signals) {
  std::unordered_set<std::size_t> sym_ids;
  for (const auto& signal : signals) {
    for (const auto& ticker : CollectTickerSymbols(signal)) {
      sym_ids.insert(cg.symbols.LookupId(ticker));
    }
  }
  for (std::size_t sym_id : sym_ids) {
    (void)EmitMarketFieldLoad(cg, sym_id, 0, "bid");
    (void)EmitMarketFieldLoad(cg, sym_id, 1, "ask");
  }
}

// ----------------------------------------------------------------------------
// P0 lowered-state codegen helpers.
//
// Layout must match the static_asserts in runtime.cpp:
//   SmaStateLowered { double* buffer; double sum; i64 head; i64 count; i64 capacity; }  sizeof=40
//   EmaStateLowered { double value; i64 initialized; }                                   sizeof=16
//   LagStateLowered { double* buffer; i64 head; i64 count; i64 capacity; }              sizeof=32
//
// We use opaque-pointer-friendly explicit StructTypes and `inbounds GEP` so
// LLVM's alias analysis and load/store widening can see through the layout
// even though we never declare these types in the runtime's IR header.
// ----------------------------------------------------------------------------

llvm::StructType* SmaStateTy(CodegenContext& cg) {
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  llvm::Type* i64 = llvm::Type::getInt64Ty(cg.llctx);
  llvm::Type* p   = llvm::PointerType::getUnqual(cg.llctx);
  return llvm::StructType::get(cg.llctx, {p, f64, i64, i64, i64});
}
llvm::StructType* EmaStateTy(CodegenContext& cg) {
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  llvm::Type* i64 = llvm::Type::getInt64Ty(cg.llctx);
  return llvm::StructType::get(cg.llctx, {f64, i64});
}
llvm::StructType* LagStateTy(CodegenContext& cg) {
  llvm::Type* i64 = llvm::Type::getInt64Ty(cg.llctx);
  llvm::Type* p   = llvm::PointerType::getUnqual(cg.llctx);
  return llvm::StructType::get(cg.llctx, {p, i64, i64, i64});
}

// Materialize a lowered-state base pointer once per function and cache it.
// The cached call is anchored *right after* the %ctx CallInst that defines
// cg.ctx_arg, which guarantees domination over all later uses regardless of
// whether the program-fused path has already emitted market-prewarm loads
// in the entry block. (Hoisting to the absolute start of the entry block
// would put the base call before %ctx itself was defined.)
llvm::Value* EmitLoweredBaseHoisted(CodegenContext& cg, llvm::FunctionCallee callee, const char* name) {
  llvm::IRBuilder<>::InsertPointGuard guard(cg.builder);
  llvm::Instruction* anchor = llvm::dyn_cast<llvm::Instruction>(cg.ctx_arg);
  if (anchor != nullptr) {
    cg.builder.SetInsertPoint(anchor->getNextNonDebugInstruction());
  } else {
    // Defensive fallback: hoist to the start of the entry block. This path
    // is only reachable if cg.ctx_arg ever ceased to be a CallInst, which
    // we don't currently expect.
    llvm::Function* cur_fn = cg.builder.GetInsertBlock()->getParent();
    cg.builder.SetInsertPoint(&cur_fn->getEntryBlock(), cur_fn->getEntryBlock().getFirstInsertionPt());
  }
  return cg.builder.CreateCall(callee, {cg.ctx_arg}, name);
}

llvm::Value* GetSmaBase(CodegenContext& cg) {
  if (cg.sma_base_cached != nullptr) return cg.sma_base_cached;
  cg.sma_base_cached = EmitLoweredBaseHoisted(cg, cg.fn_sma_lowered_base, "sma_base");
  return cg.sma_base_cached;
}
llvm::Value* GetEmaBase(CodegenContext& cg) {
  if (cg.ema_base_cached != nullptr) return cg.ema_base_cached;
  cg.ema_base_cached = EmitLoweredBaseHoisted(cg, cg.fn_ema_lowered_base, "ema_base");
  return cg.ema_base_cached;
}
llvm::Value* GetLagBase(CodegenContext& cg) {
  if (cg.lag_base_cached != nullptr) return cg.lag_base_cached;
  cg.lag_base_cached = EmitLoweredBaseHoisted(cg, cg.fn_lag_lowered_base, "lag_base");
  return cg.lag_base_cached;
}

// Lowered SMA: ring-buffer mean with running sum, NaN until full.
// Mirrors RingStatsPushPrepared + RingStatsMean + the NaN-until-full gate
// from jit_rt_sma, all inline. Period is a compile-time constant so the
// modulus collapses to a constant-divisor urem (or even a sub-and-select
// after LLVM lowering when period is a power-of-2).
//
// When cg.assume_warm is set AND this `fn` node is in cg.warm_safe_calls
// (P1 specialization), the count load, the was_full / is_full comparisons,
// the old-or-zero select, the count update, and the NaN-or-mean select are
// all eliminated. The IR becomes the strict steady-state ring update: load
// slot, subtract from sum, add x, store, return mean. LLVM cannot prove
// these branches dead from static information alone; the specialization
// expresses the monotonic runtime invariant.
llvm::Value* EmitLoweredSma(CodegenContext& cg, const FunctionCall* fn, llvm::Value* x,
                            std::int64_t node_id, std::int64_t period) {
  const bool warm = cg.assume_warm && cg.warm_safe_calls.count(fn) > 0;
  llvm::IRBuilder<>& B = cg.builder;
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  llvm::Type* i64 = llvm::Type::getInt64Ty(cg.llctx);
  llvm::StructType* sty = SmaStateTy(cg);
  llvm::Value* base = GetSmaBase(cg);

  llvm::Value* nid = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(node_id));
  llvm::Value* state_ptr = B.CreateInBoundsGEP(sty, base, {nid}, "sma_state_ptr");

  llvm::Value* buf_pp   = B.CreateStructGEP(sty, state_ptr, 0, "sma_buf_pp");
  llvm::Value* sum_pp   = B.CreateStructGEP(sty, state_ptr, 1, "sma_sum_pp");
  llvm::Value* head_pp  = B.CreateStructGEP(sty, state_ptr, 2, "sma_head_pp");
  llvm::Value* count_pp = B.CreateStructGEP(sty, state_ptr, 3, "sma_count_pp");
  llvm::Type* ptr_ty    = llvm::PointerType::getUnqual(cg.llctx);

  llvm::Value* buf   = B.CreateLoad(ptr_ty, buf_pp,  "sma_buf");
  llvm::Value* sum   = B.CreateLoad(f64,    sum_pp,  "sma_sum");
  llvm::Value* head  = B.CreateLoad(i64,    head_pp, "sma_head");

  llvm::Value* per_v = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(period));
  llvm::Value* slot_ptr = B.CreateInBoundsGEP(f64, buf, head, "sma_slot");

  if (warm) {
    // Steady-state path: count == period invariant. No was_full / is_full
    // branches, no count update, no NaN-or-mean select. Arithmetic ordering
    // (sum + x - old) is preserved to match the baseline IR bit-exactly.
    llvm::Value* old_val = B.CreateLoad(f64, slot_ptr, "sma_old");
    llvm::Value* sum_plus_x = B.CreateFAdd(sum, x, "sma_sum_plus_x");
    llvm::Value* sum_new    = B.CreateFSub(sum_plus_x, old_val, "sma_sum_new");
    B.CreateStore(x, slot_ptr);

    llvm::Value* head_plus = B.CreateAdd(head, llvm::ConstantInt::get(i64, 1), "sma_head_plus");
    llvm::Value* head_new  = B.CreateURem(head_plus, per_v, "sma_head_new");

    B.CreateStore(sum_new,  sum_pp);
    B.CreateStore(head_new, head_pp);

    return B.CreateFDiv(sum_new, llvm::ConstantFP::get(f64, static_cast<double>(period)), "sma_mean_warm");
  }

  llvm::Value* count = B.CreateLoad(i64, count_pp, "sma_count");
  llvm::Value* was_full = B.CreateICmpEQ(count, per_v, "sma_was_full");

  // old = was_full ? buf[head] : 0.0
  llvm::Value* old_val  = B.CreateLoad(f64, slot_ptr, "sma_old");
  llvm::Value* old_or_zero = B.CreateSelect(was_full, old_val, llvm::ConstantFP::get(f64, 0.0), "sma_old_or_zero");

  // sum_new = sum + x - old_or_zero
  llvm::Value* sum_plus_x = B.CreateFAdd(sum, x, "sma_sum_plus_x");
  llvm::Value* sum_new    = B.CreateFSub(sum_plus_x, old_or_zero, "sma_sum_new");

  // count_new = was_full ? count : count + 1
  llvm::Value* count_plus_one = B.CreateAdd(count, llvm::ConstantInt::get(i64, 1), "sma_count_plus_one");
  llvm::Value* count_new = B.CreateSelect(was_full, count, count_plus_one, "sma_count_new");

  // buf[head] = x
  B.CreateStore(x, slot_ptr);

  // head_new = (head + 1) % period
  llvm::Value* head_plus = B.CreateAdd(head, llvm::ConstantInt::get(i64, 1), "sma_head_plus");
  llvm::Value* head_new  = B.CreateURem(head_plus, per_v, "sma_head_new");

  B.CreateStore(sum_new,  sum_pp);
  B.CreateStore(head_new, head_pp);
  B.CreateStore(count_new,count_pp);

  // result = (count_new == period) ? sum_new / period : NaN
  llvm::Value* is_full = B.CreateICmpEQ(count_new, per_v, "sma_is_full");
  llvm::Value* mean = B.CreateFDiv(sum_new, llvm::ConstantFP::get(f64, static_cast<double>(period)), "sma_mean");
  llvm::Value* nan_v = llvm::ConstantFP::getNaN(f64);
  return B.CreateSelect(is_full, mean, nan_v, "sma_out");
}

// Lowered EMA. First call: store x, return x. Subsequent: alpha*x + (1-alpha)*prev.
// Alpha is a compile-time constant because period is a NumberLiteral.
//
// When cg.assume_warm is set AND `fn` is in cg.warm_safe_calls (P1
// specialization), the init load, is_init compare, init-or-blend select, and
// init store are all eliminated. The IR becomes a single
// `alpha*x + (1-alpha)*prev` plus the value store.
llvm::Value* EmitLoweredEma(CodegenContext& cg, const FunctionCall* fn, llvm::Value* x,
                            std::int64_t node_id, std::int64_t period) {
  const bool warm = cg.assume_warm && cg.warm_safe_calls.count(fn) > 0;
  llvm::IRBuilder<>& B = cg.builder;
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  llvm::Type* i64 = llvm::Type::getInt64Ty(cg.llctx);
  llvm::StructType* sty = EmaStateTy(cg);
  llvm::Value* base = GetEmaBase(cg);

  llvm::Value* nid = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(node_id));
  llvm::Value* state_ptr = B.CreateInBoundsGEP(sty, base, {nid}, "ema_state_ptr");

  llvm::Value* val_pp  = B.CreateStructGEP(sty, state_ptr, 0, "ema_val_pp");
  llvm::Value* init_pp = B.CreateStructGEP(sty, state_ptr, 1, "ema_init_pp");

  llvm::Value* prev    = B.CreateLoad(f64, val_pp,  "ema_prev");

  const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
  llvm::Value* alpha_v = llvm::ConstantFP::get(f64, alpha);
  llvm::Value* one_minus_alpha = llvm::ConstantFP::get(f64, 1.0 - alpha);

  // blended = alpha*x + (1-alpha)*prev   (use fma intrinsic via Function decl)
  llvm::Value* ax  = B.CreateFMul(alpha_v, x, "ema_ax");
  llvm::Value* bp  = B.CreateFMul(one_minus_alpha, prev, "ema_bp");
  llvm::Value* blended = B.CreateFAdd(ax, bp, "ema_blended");

  if (warm) {
    // is_init invariant is 1 in steady state. Skip the is_init load, the
    // compare, the select, and the init-store (it's already 1).
    (void)init_pp;
    B.CreateStore(blended, val_pp);
    return blended;
  }

  llvm::Value* init    = B.CreateLoad(i64, init_pp, "ema_init");
  llvm::Value* is_init = B.CreateICmpNE(init, llvm::ConstantInt::get(i64, 0), "ema_is_init");

  llvm::Value* new_val = B.CreateSelect(is_init, blended, x, "ema_new");
  B.CreateStore(new_val, val_pp);
  B.CreateStore(llvm::ConstantInt::get(i64, 1), init_pp);
  return new_val;
}

// Lowered LAG. Returns NaN until the buffer is full, then returns the value
// stored `period` ticks ago (which is the value currently at buf[head], since
// head is the next-write slot in a circular buffer of size `period`).
//
// When cg.assume_warm is set AND `fn` is in cg.warm_safe_calls (P1
// specialization), the count load, is_full compare, NaN-or-buf select,
// count update, and count store are eliminated; the IR becomes a straight
// buffer swap.
llvm::Value* EmitLoweredLag(CodegenContext& cg, const FunctionCall* fn, llvm::Value* x,
                            std::int64_t node_id, std::int64_t period) {
  const bool warm = cg.assume_warm && cg.warm_safe_calls.count(fn) > 0;
  llvm::IRBuilder<>& B = cg.builder;
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  llvm::Type* i64 = llvm::Type::getInt64Ty(cg.llctx);
  llvm::StructType* sty = LagStateTy(cg);
  llvm::Value* base = GetLagBase(cg);

  llvm::Value* nid = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(node_id));
  llvm::Value* state_ptr = B.CreateInBoundsGEP(sty, base, {nid}, "lag_state_ptr");

  llvm::Value* buf_pp   = B.CreateStructGEP(sty, state_ptr, 0, "lag_buf_pp");
  llvm::Value* head_pp  = B.CreateStructGEP(sty, state_ptr, 1, "lag_head_pp");
  llvm::Value* count_pp = B.CreateStructGEP(sty, state_ptr, 2, "lag_count_pp");
  llvm::Type* ptr_ty    = llvm::PointerType::getUnqual(cg.llctx);

  llvm::Value* buf   = B.CreateLoad(ptr_ty, buf_pp,   "lag_buf");
  llvm::Value* head  = B.CreateLoad(i64,    head_pp,  "lag_head");
  llvm::Value* per_v = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(period));

  llvm::Value* slot_ptr   = B.CreateInBoundsGEP(f64, buf, head, "lag_slot");
  llvm::Value* lagged_buf = B.CreateLoad(f64, slot_ptr, "lag_lagged_buf");

  if (warm) {
    // Steady state: buffer is always full, so we always return buf[head] and
    // never update `count`. Just rotate the buffer.
    (void)count_pp;
    B.CreateStore(x, slot_ptr);
    llvm::Value* head_plus = B.CreateAdd(head, llvm::ConstantInt::get(i64, 1), "lag_head_plus");
    llvm::Value* head_new  = B.CreateURem(head_plus, per_v, "lag_head_new");
    B.CreateStore(head_new, head_pp);
    return lagged_buf;
  }

  llvm::Value* count   = B.CreateLoad(i64, count_pp, "lag_count");
  llvm::Value* is_full = B.CreateICmpEQ(count, per_v, "lag_is_full");

  llvm::Value* nan_v  = llvm::ConstantFP::getNaN(f64);
  llvm::Value* lagged = B.CreateSelect(is_full, lagged_buf, nan_v, "lag_lagged");

  B.CreateStore(x, slot_ptr);
  llvm::Value* head_plus = B.CreateAdd(head, llvm::ConstantInt::get(i64, 1), "lag_head_plus");
  llvm::Value* head_new  = B.CreateURem(head_plus, per_v, "lag_head_new");
  B.CreateStore(head_new, head_pp);

  llvm::Value* count_plus = B.CreateAdd(count, llvm::ConstantInt::get(i64, 1), "lag_count_plus");
  llvm::Value* count_new  = B.CreateSelect(is_full, count, count_plus, "lag_count_new");
  B.CreateStore(count_new, count_pp);

  return lagged;
}

llvm::Value* EmitExpr(const Expr& expr, CodegenContext& cg) {
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  llvm::Type* i64 = llvm::Type::getInt64Ty(cg.llctx);
  // P2: in vector mode `result_ty` is <K x double>; otherwise it equals f64.
  // Most binary ops are type-polymorphic in LLVM (CreateFAdd works on either),
  // but boolean-to-double widening (CreateUIToFP) needs the result type to be
  // explicit, hence VecTy(cg).
  llvm::Type* result_ty = VecTy(cg);

  if (const auto* n = dynamic_cast<const NumberLiteral*>(&expr)) {
    return SplatConst(cg, n->value);
  }

  if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
    llvm::Value* v = EmitExpr(*u->operand, cg);
    if (u->kind == UnaryOpKind::Plus) {
      return v;
    }
    return cg.builder.CreateFNeg(v, "neg");
  }

  if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
    llvm::Value* l = EmitExpr(*b->left, cg);
    llvm::Value* r = EmitExpr(*b->right, cg);
    switch (b->kind) {
      case BinaryOpKind::Add:
        return cg.builder.CreateFAdd(l, r, "add");
      case BinaryOpKind::Sub:
        return cg.builder.CreateFSub(l, r, "sub");
      case BinaryOpKind::Mul:
        return cg.builder.CreateFMul(l, r, "mul");
      case BinaryOpKind::Div:
        return cg.builder.CreateFDiv(l, r, "div");
      case BinaryOpKind::Gt: {
        llvm::Value* c = cg.builder.CreateFCmpOGT(l, r, "gt");
        return cg.builder.CreateUIToFP(c, result_ty, "gt_f");
      }
      case BinaryOpKind::Lt: {
        llvm::Value* c = cg.builder.CreateFCmpOLT(l, r, "lt");
        return cg.builder.CreateUIToFP(c, result_ty, "lt_f");
      }
      case BinaryOpKind::Gte: {
        llvm::Value* c = cg.builder.CreateFCmpOGE(l, r, "gte");
        return cg.builder.CreateUIToFP(c, result_ty, "gte_f");
      }
      case BinaryOpKind::Lte: {
        llvm::Value* c = cg.builder.CreateFCmpOLE(l, r, "lte");
        return cg.builder.CreateUIToFP(c, result_ty, "lte_f");
      }
      case BinaryOpKind::Eq: {
        // Approx-equality to mirror interpreter semantics.
        llvm::Value* d = cg.builder.CreateFSub(l, r, "eq_diff");
        llvm::Function* fabs_fn = llvm::Intrinsic::getDeclaration(&cg.module, llvm::Intrinsic::fabs, {result_ty});
        llvm::Value* ad = cg.builder.CreateCall(fabs_fn, {d}, "eq_abs");
        llvm::Value* c = cg.builder.CreateFCmpOLT(ad, SplatConst(cg, 1e-12), "eq");
        return cg.builder.CreateUIToFP(c, result_ty, "eq_f");
      }
      case BinaryOpKind::NotEq: {
        llvm::Value* d = cg.builder.CreateFSub(l, r, "neq_diff");
        llvm::Function* fabs_fn = llvm::Intrinsic::getDeclaration(&cg.module, llvm::Intrinsic::fabs, {result_ty});
        llvm::Value* ad = cg.builder.CreateCall(fabs_fn, {d}, "neq_abs");
        llvm::Value* c = cg.builder.CreateFCmpOGE(ad, SplatConst(cg, 1e-12), "neq");
        return cg.builder.CreateUIToFP(c, result_ty, "neq_f");
      }
      case BinaryOpKind::And: {
        llvm::Value* lnz = cg.builder.CreateFCmpUNE(l, SplatConst(cg, 0.0), "and_l");
        llvm::Value* rnz = cg.builder.CreateFCmpUNE(r, SplatConst(cg, 0.0), "and_r");
        llvm::Value* both = cg.builder.CreateAnd(lnz, rnz, "and");
        return cg.builder.CreateUIToFP(both, result_ty, "and_f");
      }
      case BinaryOpKind::Or: {
        llvm::Value* lnz = cg.builder.CreateFCmpUNE(l, SplatConst(cg, 0.0), "or_l");
        llvm::Value* rnz = cg.builder.CreateFCmpUNE(r, SplatConst(cg, 0.0), "or_r");
        llvm::Value* either = cg.builder.CreateOr(lnz, rnz, "or");
        return cg.builder.CreateUIToFP(either, result_ty, "or_f");
      }
    }
  }

  if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
    if (IsVectorized(cg)) {
      // P2: per-lane branches can't be represented with CondBr+PHI because
      // different lanes may want different sides. Use lane-wise select; both
      // branches are evaluated unconditionally. This is correct as long as
      // neither branch has side effects -- which holds in the MVP because
      // stateful ops are rejected up front.
      llvm::Value* cond_v = EmitExpr(*c->condition, cg);
      llvm::Value* cond_mask = cg.builder.CreateFCmpUNE(cond_v, SplatConst(cg, 0.0), "ifcond_mask");
      llvm::Value* then_v = EmitExpr(*c->then_branch, cg);
      llvm::Value* else_v = EmitExpr(*c->else_branch, cg);
      return cg.builder.CreateSelect(cond_mask, then_v, else_v, "ifsel");
    }
    llvm::Function* cur_fn = cg.builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(cg.llctx, "then", cur_fn);
    llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(cg.llctx, "else", cur_fn);
    llvm::BasicBlock* merge_bb = llvm::BasicBlock::Create(cg.llctx, "ifcont", cur_fn);

    llvm::Value* cond_v = EmitExpr(*c->condition, cg);
    // Match interpreter semantics: NaN condition should be treated as "true"
    // because interpreter uses (cond != 0.0).
    llvm::Value* cond_b = cg.builder.CreateFCmpUNE(cond_v, llvm::ConstantFP::get(f64, 0.0), "ifcond");
    cg.builder.CreateCondBr(cond_b, then_bb, else_bb);

    cg.builder.SetInsertPoint(then_bb);
    llvm::Value* then_v = EmitExpr(*c->then_branch, cg);
    cg.builder.CreateBr(merge_bb);
    then_bb = cg.builder.GetInsertBlock();

    cg.builder.SetInsertPoint(else_bb);
    llvm::Value* else_v = EmitExpr(*c->else_branch, cg);
    cg.builder.CreateBr(merge_bb);
    else_bb = cg.builder.GetInsertBlock();

    cg.builder.SetInsertPoint(merge_bb);
    llvm::PHINode* phi = cg.builder.CreatePHI(f64, 2, "iftmp");
    phi->addIncoming(then_v, then_bb);
    phi->addIncoming(else_v, else_bb);
    return phi;
  }

  if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
    // P11: if this stateful FunctionCall's node_id has already been
    // emitted in this compile, return the cached SSA Value rather
    // than building a second runtime call. AllocateNodeIds aliases
    // the node_id of structurally-equal stateful subtrees within
    // one signal body when the FIRST occurrence is unconditional
    // (top-level or inside a Conditional's `cond`), which by
    // construction means the cached Value's BasicBlock dominates
    // all later uses, so SSA dominance is preserved.
    //
    // Non-stateful FunctionCalls (mid/bid/ask/spread/abs/sqrt/log)
    // have node_id == -1 (set by the AST default; AllocateNodeIds
    // doesn't touch them) so they skip the cache and re-emit. That
    // is the right behavior: they have no side effects, the
    // existing market-field cache already memoizes their loads.
    if (fn->node_id > 0) {
      auto it = cg.stateful_emit_cache.find(fn->node_id);
      if (it != cg.stateful_emit_cache.end()) {
        return it->second;
      }
    }
    auto cache_stateful = [&](llvm::Value* v) -> llvm::Value* {
      if (fn->node_id > 0 && v != nullptr) {
        cg.stateful_emit_cache[fn->node_id] = v;
      }
      return v;
    };

    if (fn->name == "mid" || fn->name == "bid" || fn->name == "ask" || fn->name == "spread") {
      if (fn->args.size() != 1) {
        throw std::runtime_error(fn->name + "() expects exactly one argument");
      }
      const auto* id_expr = dynamic_cast<const IdentifierExpr*>(fn->args[0].get());
      if (id_expr == nullptr) {
        throw std::runtime_error(fn->name + "() argument must be ticker identifier");
      }
      std::size_t sym_id =
          (fn->symbol_id >= 0) ? static_cast<std::size_t>(fn->symbol_id) : cg.symbols.LookupId(id_expr->name);
      if (fn->name == "bid") return EmitMarketFieldLoad(cg, sym_id, 0, "bid");
      if (fn->name == "ask") return EmitMarketFieldLoad(cg, sym_id, 1, "ask");
      if (fn->name == "mid") {
        llvm::Value* bid = EmitMarketFieldLoad(cg, sym_id, 0, "mid_bid");
        llvm::Value* ask = EmitMarketFieldLoad(cg, sym_id, 1, "mid_ask");
        llvm::Value* sum = cg.builder.CreateFAdd(bid, ask, "mid_sum");
        return cg.builder.CreateFMul(sum, SplatConst(cg, 0.5), "mid");
      }
      llvm::Value* bid = EmitMarketFieldLoad(cg, sym_id, 0, "spr_bid");
      llvm::Value* ask = EmitMarketFieldLoad(cg, sym_id, 1, "spr_ask");
      return cg.builder.CreateFSub(ask, bid, "spread");
    }
    if (fn->name == "vwap") {
      if (fn->args.size() != 2) {
        throw std::runtime_error("vwap() expects two args");
      }
      const auto* id_expr = dynamic_cast<const IdentifierExpr*>(fn->args[0].get());
      if (id_expr == nullptr) {
        throw std::runtime_error("vwap() first arg must be ticker identifier");
      }
      const auto* period_node = dynamic_cast<const NumberLiteral*>(fn->args[1].get());
      if (!period_node) {
        throw std::runtime_error("vwap() period must be numeric literal");
      }
      const int period = static_cast<int>(period_node->value);
      if (period <= 0 || std::fabs(period_node->value - static_cast<double>(period)) > 1e-12) {
        throw std::runtime_error("vwap() period must be positive integer");
      }
      std::size_t sym_id =
          (fn->symbol_id >= 0) ? static_cast<std::size_t>(fn->symbol_id) : cg.symbols.LookupId(id_expr->name);
      llvm::Value* node_id = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(GetNodeId(cg, fn)));
      llvm::Value* sym_id_v = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(sym_id));
      llvm::Value* per = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(period));
      if (IsVectorized(cg)) {
        // P10: per-lane scalarized fan-out. Each lane has its own
        // MarketState pointer; the ticker (sym_id) is shared across
        // lanes because the SoA layout maps lane i's instrument data
        // to instruments[sym_id] of lane i's MarketState.
        return cache_stateful(EmitScalarizedFanOut(
            cg,
            [&](unsigned /*lane*/, llvm::Value* lane_ctx, llvm::Value* lane_market) {
              return cg.builder.CreateCall(
                  cg.fn_vwap, {lane_market, lane_ctx, node_id, sym_id_v, per},
                  "vwap_lane");
            },
            "vwap_vec"));
      }
      return cache_stateful(cg.builder.CreateCall(
          cg.fn_vwap, {cg.market_arg, cg.ctx_arg, node_id, sym_id_v, per}, "vwap"));
    }

    if (fn->name == "abs") {
      if (fn->args.size() != 1) throw std::runtime_error("abs() expects one argument");
      llvm::Value* x = EmitExpr(*fn->args[0], cg);
      llvm::Function* fabs_fn = llvm::Intrinsic::getDeclaration(&cg.module, llvm::Intrinsic::fabs, {result_ty});
      return cg.builder.CreateCall(fabs_fn, {x}, "abs");
    }
    if (fn->name == "sqrt") {
      if (fn->args.size() != 1) throw std::runtime_error("sqrt() expects one argument");
      llvm::Value* x = EmitExpr(*fn->args[0], cg);
      llvm::Function* sqrt_fn = llvm::Intrinsic::getDeclaration(&cg.module, llvm::Intrinsic::sqrt, {result_ty});
      return cg.builder.CreateCall(sqrt_fn, {x}, "sqrt");
    }
    if (fn->name == "log") {
      if (fn->args.size() != 1) throw std::runtime_error("log() expects one argument");
      llvm::Value* x = EmitExpr(*fn->args[0], cg);
      llvm::Function* log_fn = llvm::Intrinsic::getDeclaration(&cg.module, llvm::Intrinsic::log, {result_ty});
      return cg.builder.CreateCall(log_fn, {x}, "log");
    }

    if (fn->name == "ema" || fn->name == "sma" || fn->name == "rolling_std" || fn->name == "zscore" ||
        fn->name == "lag" ||
        fn->name == "rolling_min" || fn->name == "rolling_max") {
      if (fn->args.size() != 2) {
        throw std::runtime_error(fn->name + "() expects two args");
      }
      const auto* period_node = dynamic_cast<const NumberLiteral*>(fn->args[1].get());
      if (!period_node) {
        throw std::runtime_error(fn->name + "() period must be numeric literal");
      }
      const int period = static_cast<int>(period_node->value);
      if (period <= 0 || std::fabs(period_node->value - static_cast<double>(period)) > 1e-12) {
        throw std::runtime_error(fn->name + "() period must be positive integer");
      }
      llvm::Value* x = EmitExpr(*fn->args[0], cg);
      const std::int64_t node_id_val = GetNodeId(cg, fn);
      llvm::Value* node_id = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(node_id_val));
      llvm::Value* per = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(period));

      // P10: vectorized stateful via per-lane scalarized fan-out. We
      // intentionally bypass two scalar-IR-specific code paths in
      // vector mode and continue to reject them:
      //   1. P0 lowered stateful (kSma/kEma/kLag): the lowered-state
      //      base pointers are cached as scalar IR values, and the
      //      inline IR threads them through across lanes is
      //      non-trivial. Stick to runtime helpers under vector mode.
      //   2. The AVX2 sma SIMD-prep path: it constructs a vector
      //      reduction over the ring buffer of ONE lane; vectorizing
      //      across lanes too would require nesting `<K x double>`
      //      inside a `<4 x double>` reduction which LLVM does not
      //      cleanly handle. Fall back to the scalar runtime call.
      if (IsVectorized(cg)) {
        if (HasFlag(cg.lowering, StatefulLoweringFlags::kSma) && fn->name == "sma") {
          RejectInVector("sma+kSma");
        }
        if (HasFlag(cg.lowering, StatefulLoweringFlags::kEma) && fn->name == "ema") {
          RejectInVector("ema+kEma");
        }
        if (HasFlag(cg.lowering, StatefulLoweringFlags::kLag) && fn->name == "lag") {
          RejectInVector("lag+kLag");
        }
        // Pick the right scalar runtime callee and emit fan-out.
        llvm::FunctionCallee callee = cg.fn_sma;
        const char* lane_name = "rt_lane";
        const bool is_ema = (fn->name == "ema");
        if (fn->name == "ema") { callee = cg.fn_ema_alpha; lane_name = "ema_lane"; }
        else if (fn->name == "sma") { callee = cg.fn_sma; lane_name = "sma_lane"; }
        else if (fn->name == "rolling_std") { callee = cg.fn_rolling_std; lane_name = "rstd_lane"; }
        else if (fn->name == "zscore") { callee = cg.fn_zscore; lane_name = "zscore_lane"; }
        else if (fn->name == "lag") { callee = cg.fn_lag; lane_name = "lag_lane"; }
        else if (fn->name == "rolling_min") { callee = cg.fn_rolling_min; lane_name = "rmin_lane"; }
        else /* rolling_max */ { callee = cg.fn_rolling_max; lane_name = "rmax_lane"; }
        const double alpha = is_ema ? (2.0 / (static_cast<double>(period) + 1.0)) : 0.0;
        llvm::Value* alpha_v = is_ema ? llvm::ConstantFP::get(f64, alpha) : nullptr;
        return cache_stateful(EmitScalarizedFanOut(
            cg,
            [&](unsigned lane, llvm::Value* lane_ctx, llvm::Value* /*lane_market*/) {
              llvm::Value* lane_x = ExtractLane(cg, x, lane);
              if (is_ema) {
                return cg.builder.CreateCall(
                    callee, {lane_ctx, node_id, lane_x, alpha_v, per}, lane_name);
              }
              return cg.builder.CreateCall(
                  callee, {lane_ctx, node_id, lane_x, per}, lane_name);
            },
            (fn->name + "_vec").c_str()));
      }
      if (fn->name == "ema") {
        if (HasFlag(cg.lowering, StatefulLoweringFlags::kEma)) {
          return cache_stateful(EmitLoweredEma(cg, fn, x, node_id_val, period));
        }
        const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
        llvm::Value* alpha_v = llvm::ConstantFP::get(f64, alpha);
        return cache_stateful(cg.builder.CreateCall(
            cg.fn_ema_alpha, {cg.ctx_arg, node_id, x, alpha_v, per}, "ema"));
      }
      if (fn->name == "sma") {
        if (HasFlag(cg.lowering, StatefulLoweringFlags::kSma)) {
          return cache_stateful(EmitLoweredSma(cg, fn, x, node_id_val, period));
        }
        if (!cg.use_avx2 || period < 4) {
          return cache_stateful(cg.builder.CreateCall(
              cg.fn_sma, {cg.ctx_arg, node_id, x, per}, "sma"));
        }

        llvm::Function* cur_fn = cg.builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* simd_bb = llvm::BasicBlock::Create(cg.llctx, "sma_simd", cur_fn);
        llvm::BasicBlock* scalar_nan_bb = llvm::BasicBlock::Create(cg.llctx, "sma_nan", cur_fn);
        llvm::BasicBlock* merge_bb = llvm::BasicBlock::Create(cg.llctx, "sma_merge", cur_fn);

        llvm::IRBuilder<> entry_builder(&cur_fn->getEntryBlock(), cur_fn->getEntryBlock().begin());
        llvm::Type* ptr_ty = llvm::PointerType::getUnqual(cg.llctx);
        llvm::Value* buffer_ptr_addr = entry_builder.CreateAlloca(ptr_ty, nullptr, "sma_buffer_ptr_addr");
        llvm::Value* size_addr = entry_builder.CreateAlloca(i64, nullptr, "sma_size_addr");

        llvm::Value* full = cg.builder.CreateCall(
            cg.fn_sma_prepare, {cg.ctx_arg, node_id, x, per, buffer_ptr_addr, size_addr}, "sma_full");
        cg.builder.CreateCondBr(full, simd_bb, scalar_nan_bb);

        cg.builder.SetInsertPoint(scalar_nan_bb);
        llvm::Value* nan_v = llvm::ConstantFP::getNaN(f64);
        cg.builder.CreateBr(merge_bb);

        cg.builder.SetInsertPoint(simd_bb);
        llvm::Value* buf_ptr = cg.builder.CreateLoad(ptr_ty, buffer_ptr_addr, "sma_buffer_ptr");
        llvm::Type* f64_ptr_ty = llvm::PointerType::getUnqual(f64);
        llvm::Value* buf_f64_ptr = cg.builder.CreateBitCast(buf_ptr, f64_ptr_ty, "sma_buffer_f64");
        llvm::Value* sum_scalar = llvm::ConstantFP::get(f64, 0.0);
        llvm::Type* vec4_ty = llvm::FixedVectorType::get(f64, 4);
        llvm::Value* sum_vec = llvm::Constant::getNullValue(vec4_ty);
        llvm::Type* vec4_ptr_ty = llvm::PointerType::getUnqual(vec4_ty);

        const int vec_chunks = period / 4;
        const int vec_tail = period % 4;
        for (int c = 0; c < vec_chunks; ++c) {
          llvm::Value* idx_v = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(c * 4));
          llvm::Value* chunk_ptr = cg.builder.CreateInBoundsGEP(f64, buf_f64_ptr, idx_v, "sma_chunk_ptr");
          llvm::Value* chunk_vec_ptr = cg.builder.CreateBitCast(chunk_ptr, vec4_ptr_ty, "sma_chunk_vec_ptr");
          llvm::Value* v = cg.builder.CreateLoad(vec4_ty, chunk_vec_ptr, "sma_vload");
          sum_vec = cg.builder.CreateFAdd(sum_vec, v, "sma_vsum");
        }

        for (int lane = 0; lane < 4; ++lane) {
          llvm::Value* lane_v = cg.builder.CreateExtractElement(sum_vec, static_cast<std::uint64_t>(lane), "sma_lane");
          sum_scalar = cg.builder.CreateFAdd(sum_scalar, lane_v, "sma_lane_add");
        }
        for (int t = 0; t < vec_tail; ++t) {
          llvm::Value* idx_v = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(vec_chunks * 4 + t));
          llvm::Value* p = cg.builder.CreateInBoundsGEP(f64, buf_f64_ptr, idx_v, "sma_tail_ptr");
          llvm::Value* x_tail = cg.builder.CreateLoad(f64, p, "sma_tail");
          sum_scalar = cg.builder.CreateFAdd(sum_scalar, x_tail, "sma_tail_add");
        }

        llvm::Value* denom = llvm::ConstantFP::get(f64, static_cast<double>(period));
        llvm::Value* mean = cg.builder.CreateFDiv(sum_scalar, denom, "sma_simd_mean");
        cg.builder.CreateBr(merge_bb);

        cg.builder.SetInsertPoint(merge_bb);
        llvm::PHINode* phi = cg.builder.CreatePHI(f64, 2, "sma_out");
        phi->addIncoming(mean, simd_bb);
        phi->addIncoming(nan_v, scalar_nan_bb);
        return cache_stateful(phi);
      }
      if (fn->name == "rolling_std") {
        return cache_stateful(cg.builder.CreateCall(
            cg.fn_rolling_std, {cg.ctx_arg, node_id, x, per}, "rstd"));
      }
      if (fn->name == "zscore") {
        return cache_stateful(cg.builder.CreateCall(
            cg.fn_zscore, {cg.ctx_arg, node_id, x, per}, "zscore"));
      }
      if (fn->name == "lag") {
        if (HasFlag(cg.lowering, StatefulLoweringFlags::kLag)) {
          return cache_stateful(EmitLoweredLag(cg, fn, x, node_id_val, period));
        }
        return cache_stateful(cg.builder.CreateCall(
            cg.fn_lag, {cg.ctx_arg, node_id, x, per}, "lag"));
      }
      if (fn->name == "rolling_min") {
        return cache_stateful(cg.builder.CreateCall(
            cg.fn_rolling_min, {cg.ctx_arg, node_id, x, per}, "rmin"));
      }
      return cache_stateful(cg.builder.CreateCall(
          cg.fn_rolling_max, {cg.ctx_arg, node_id, x, per}, "rmax"));
    }
    if (fn->name == "cross_above" || fn->name == "cross_below") {
      if (fn->args.size() != 2) {
        throw std::runtime_error(fn->name + "() expects two args");
      }
      llvm::Value* a = EmitExpr(*fn->args[0], cg);
      llvm::Value* b = EmitExpr(*fn->args[1], cg);
      llvm::Value* node_id = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(GetNodeId(cg, fn)));
      llvm::FunctionCallee cross_callee =
          (fn->name == "cross_above") ? cg.fn_cross_above : cg.fn_cross_below;
      const char* lane_name =
          (fn->name == "cross_above") ? "cross_above_lane" : "cross_below_lane";
      if (IsVectorized(cg)) {
        // P10: each lane has its own cross_* state slot in its per-
        // symbol SignalContext. Extract per-lane (a, b), call the
        // scalar runtime helper against the per-lane context.
        return cache_stateful(EmitScalarizedFanOut(
            cg,
            [&](unsigned lane, llvm::Value* lane_ctx, llvm::Value* /*lane_market*/) {
              llvm::Value* la = ExtractLane(cg, a, lane);
              llvm::Value* lb = ExtractLane(cg, b, lane);
              return cg.builder.CreateCall(
                  cross_callee, {lane_ctx, node_id, la, lb}, lane_name);
            },
            (fn->name + "_vec").c_str()));
      }
      return cache_stateful(cg.builder.CreateCall(
          cross_callee, {cg.ctx_arg, node_id, a, b}, lane_name));
    }

    // P7: rolling_corr(x, y, period), rolling_beta(x, y, period).
    if (fn->name == "rolling_corr" || fn->name == "rolling_beta") {
      if (fn->args.size() != 3) {
        throw std::runtime_error(fn->name + "() expects three args: " + fn->name + "(x, y, period)");
      }
      const auto* period_node = dynamic_cast<const NumberLiteral*>(fn->args[2].get());
      if (!period_node) {
        throw std::runtime_error(fn->name + "() period must be numeric literal");
      }
      const int period = static_cast<int>(period_node->value);
      if (period <= 0 || std::fabs(period_node->value - static_cast<double>(period)) > 1e-12) {
        throw std::runtime_error(fn->name + "() period must be positive integer");
      }
      llvm::Value* x = EmitExpr(*fn->args[0], cg);
      llvm::Value* y = EmitExpr(*fn->args[1], cg);
      llvm::Value* node_id = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(GetNodeId(cg, fn)));
      llvm::Value* per = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(period));
      llvm::FunctionCallee pair_callee =
          (fn->name == "rolling_corr") ? cg.fn_rolling_corr : cg.fn_rolling_beta;
      const char* lane_name =
          (fn->name == "rolling_corr") ? "rcorr_lane" : "rbeta_lane";
      if (IsVectorized(cg)) {
        // P10: per-lane fan-out. RollingPairState lives in the per-
        // symbol SignalContext, so lane i and lane j have independent
        // state without any aliasing concern.
        return cache_stateful(EmitScalarizedFanOut(
            cg,
            [&](unsigned lane, llvm::Value* lane_ctx, llvm::Value* /*lane_market*/) {
              llvm::Value* lx = ExtractLane(cg, x, lane);
              llvm::Value* ly = ExtractLane(cg, y, lane);
              return cg.builder.CreateCall(
                  pair_callee, {lane_ctx, node_id, lx, ly, per}, lane_name);
            },
            (fn->name + "_vec").c_str()));
      }
      return cache_stateful(cg.builder.CreateCall(
          pair_callee, {cg.ctx_arg, node_id, x, y, per}, lane_name));
    }

    // P7: kalman1d(x, q, r). q and r are compile-time literals; we hoist
    // them into the IR as ConstantFP. The JIT calls back into
    // jit_rt_kalman1d which performs the textbook predict-update step
    // (see Kalman1dStep in runtime.cpp). The parity test gates that the
    // result matches the interpreter bit-for-bit.
    if (fn->name == "kalman1d") {
      if (fn->args.size() != 3) {
        throw std::runtime_error("kalman1d() expects three args: kalman1d(x, q, r)");
      }
      const auto* q_node = dynamic_cast<const NumberLiteral*>(fn->args[1].get());
      const auto* r_node = dynamic_cast<const NumberLiteral*>(fn->args[2].get());
      if (!q_node || !r_node) {
        throw std::runtime_error("kalman1d() q and r must be numeric literals");
      }
      const double q = q_node->value;
      const double r = r_node->value;
      if (q < 0.0 || r <= 0.0) {
        throw std::runtime_error("kalman1d() requires q >= 0 and r > 0");
      }
      llvm::Value* x = EmitExpr(*fn->args[0], cg);
      llvm::Value* node_id = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(GetNodeId(cg, fn)));
      llvm::Value* q_v = llvm::ConstantFP::get(f64, q);
      llvm::Value* r_v = llvm::ConstantFP::get(f64, r);
      if (IsVectorized(cg)) {
        // P10: per-lane fan-out. Kalman1dState lives in the per-symbol
        // SignalContext (one filter per lane). q and r are the SAME
        // literals across lanes (they parameterize the filter, not the
        // signal), so we capture them once and reuse them per lane.
        return cache_stateful(EmitScalarizedFanOut(
            cg,
            [&](unsigned lane, llvm::Value* lane_ctx, llvm::Value* /*lane_market*/) {
              llvm::Value* lx = ExtractLane(cg, x, lane);
              return cg.builder.CreateCall(
                  cg.fn_kalman1d, {lane_ctx, node_id, lx, q_v, r_v}, "kalman1d_lane");
            },
            "kalman1d_vec"));
      }
      return cache_stateful(cg.builder.CreateCall(
          cg.fn_kalman1d, {cg.ctx_arg, node_id, x, q_v, r_v}, "kalman1d"));
    }

    throw std::runtime_error("Unsupported function in JIT: " + fn->name);
  }

  if (const auto* id = dynamic_cast<const IdentifierExpr*>(&expr)) {
    auto it = cg.signal_values.find(id->name);
    if (it != cg.signal_values.end()) {
      return it->second;
    }
    throw std::runtime_error("Unresolved identifier in JIT expression: " + id->name);
  }

  throw std::runtime_error("Unknown AST node in JIT emit");
}

std::string ToString(llvm::Error err) {
  std::string msg;
  llvm::raw_string_ostream os(msg);
  os << err;
  return os.str();
}

}  // namespace
#endif

bool JitCompiler::Compile(const SignalDef& signal, const SymbolTable& symbols) {
  impl_->fn = nullptr;
  impl_->program_fn = nullptr;
  impl_->last_ir_pre_opt.clear();
  impl_->last_ir_post_opt.clear();
  impl_->last_asm.clear();
#ifndef JITSE_HAS_LLVM
  (void)signal;
  (void)symbols;
  impl_->last_error = "LLVM support is disabled at build time";
  return false;
#else
  if (!impl_->lljit) {
    impl_->last_error = "LLJIT is not initialized";
    return false;
  }

  auto context = std::make_unique<llvm::LLVMContext>();
  auto module = std::make_unique<llvm::Module>("jit_signal_module", *context);
  module->setDataLayout(impl_->lljit->getDataLayout());
  // LLVM 21 made the `Module::setTargetTriple(string)` overload
  // explicit-only; wrap the triple so this builds on both old and
  // new LLVM headers.
#if LLVM_VERSION_MAJOR >= 21
  module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
#else
  module->setTargetTriple(llvm::sys::getDefaultTargetTriple());
#endif

  llvm::IRBuilder<> builder(*context);
  llvm::Type* f64 = llvm::Type::getDoubleTy(*context);
  llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
  llvm::Type* market_ptr = llvm::PointerType::getUnqual(*context);
  llvm::Type* arena_ptr = llvm::PointerType::getUnqual(*context);
  llvm::Type* ctx_ptr = llvm::PointerType::getUnqual(*context);
  llvm::Type* u32 = llvm::Type::getInt32Ty(*context);

  auto* fn_ty = llvm::FunctionType::get(f64, {market_ptr, arena_ptr, u32}, false);
  const std::string fn_name = "signal_func_" + std::to_string(++impl_->compile_counter);
  llvm::Function* fn = llvm::Function::Create(fn_ty, llvm::Function::ExternalLinkage, fn_name, module.get());

  auto it = fn->arg_begin();
  llvm::Value* market_arg = it++;
  llvm::Value* arena_arg = it++;
  llvm::Value* symbol_arg = it++;
  market_arg->setName("market");
  arena_arg->setName("arena");
  symbol_arg->setName("symbol_id");

  auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
  builder.SetInsertPoint(entry);

  // Declare runtime entry points.
  auto rt_md_ty = llvm::FunctionType::get(f64, {market_ptr, i64}, false);
  auto rt_symbol_ctx_ty = llvm::FunctionType::get(ctx_ptr, {arena_ptr, u32}, false);
  auto rt_state_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, i64}, false);
  auto rt_sma_prepare_ty = llvm::FunctionType::get(
      llvm::Type::getInt1Ty(*context), {ctx_ptr, i64, f64, i64, market_ptr, market_ptr}, false);
  auto rt_ema_alpha_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64, i64}, false);
  auto rt_vwap_ty = llvm::FunctionType::get(f64, {market_ptr, ctx_ptr, i64, i64, i64}, false);
  auto rt_cross_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64}, false);
  // P7: rolling_corr/beta take (ctx, node_id, x, y, period); kalman1d
  // takes (ctx, node_id, x, q_lit, r_lit). Both shapes are routed through
  // extern "C" runtime calls (no IR lowering for P7 ops).
  auto rt_pair_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64, i64}, false);
  auto rt_kalman_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64, f64}, false);
  auto rt_lowered_base_ty = llvm::FunctionType::get(ctx_ptr, {ctx_ptr}, false);

  llvm::Value* ctx_arg =
      builder.CreateCall(module->getOrInsertFunction("jit_rt_symbol_ctx", rt_symbol_ctx_ty), {arena_arg, symbol_arg}, "ctx");

  CodegenContext cg{
      *context,
      *module,
      builder,
      symbols,
      market_arg,
      arena_arg,
      symbol_arg,
      ctx_arg,
      module->getOrInsertFunction("jit_rt_mid", rt_md_ty),
      module->getOrInsertFunction("jit_rt_bid", rt_md_ty),
      module->getOrInsertFunction("jit_rt_ask", rt_md_ty),
      module->getOrInsertFunction("jit_rt_spread", rt_md_ty),
      module->getOrInsertFunction("jit_rt_ema", rt_state_ty),
      module->getOrInsertFunction("jit_rt_ema_alpha", rt_ema_alpha_ty),
      module->getOrInsertFunction("jit_rt_sma", rt_state_ty),
      module->getOrInsertFunction("jit_rt_sma_prepare", rt_sma_prepare_ty),
      module->getOrInsertFunction("jit_rt_rolling_std", rt_state_ty),
      module->getOrInsertFunction("jit_rt_zscore", rt_state_ty),
      module->getOrInsertFunction("jit_rt_rolling_min", rt_state_ty),
      module->getOrInsertFunction("jit_rt_rolling_max", rt_state_ty),
      module->getOrInsertFunction("jit_rt_vwap", rt_vwap_ty),
      module->getOrInsertFunction("jit_rt_lag", rt_state_ty),
      module->getOrInsertFunction("jit_rt_cross_above", rt_cross_ty),
      module->getOrInsertFunction("jit_rt_cross_below", rt_cross_ty),
      module->getOrInsertFunction("jit_rt_rolling_corr", rt_pair_ty),
      module->getOrInsertFunction("jit_rt_rolling_beta", rt_pair_ty),
      module->getOrInsertFunction("jit_rt_kalman1d", rt_kalman_ty),
      module->getOrInsertFunction("jit_rt_sma_lowered_base", rt_lowered_base_ty),
      module->getOrInsertFunction("jit_rt_ema_lowered_base", rt_lowered_base_ty),
      module->getOrInsertFunction("jit_rt_lag_lowered_base", rt_lowered_base_ty),
      {},
      {},
      {},
      {},
      false,
      1,
      HasAVX2(),
      impl_->stateful_lowering,
      nullptr,
      nullptr,
      nullptr,
      /*assume_warm=*/false,
      /*warm_safe_calls=*/{},
      /*lane_count=*/1,
      /*per_lane_market_arg=*/nullptr,
      /*stateful_emit_cache=*/{},
  };

  llvm::Value* ret_v = nullptr;
  try {
    ret_v = EmitExpr(*signal.body, cg);
  } catch (const std::exception& ex) {
    impl_->last_error = ex.what();
    return false;
  }
  builder.CreateRet(ret_v);

  if (llvm::verifyFunction(*fn, &llvm::errs())) {
    impl_->last_error = "LLVM verifyFunction failed";
    return false;
  }

  {
    std::string ir_str;
    llvm::raw_string_ostream ir_stream(ir_str);
    module->print(ir_stream, nullptr);
    impl_->last_ir_pre_opt = ir_stream.str();
  }

  // Optimize module with O2 pipeline.
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb;
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  mpm.run(*module, mam);

  {
    std::string ir_str;
    llvm::raw_string_ostream ir_stream(ir_str);
    module->print(ir_stream, nullptr);
    impl_->last_ir_post_opt = ir_stream.str();
  }
  // P9: capture host asm of the post-O2 module BEFORE ORC moves it.
  {
    std::string asm_err;
    impl_->last_asm = EmitHostAsm(*module, asm_err);
    if (impl_->last_asm.empty() && !asm_err.empty()) {
      // Asm dump failure is non-fatal; surface the cause as a // diagnostic comment in last_asm so `--dump-asm` users still see
      // why their dump is empty.
      impl_->last_asm = "; asm dump unavailable: " + asm_err + "\n";
    }
  }

  auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::move(context));
  if (!impl_->runtime_symbols_registered) {
    llvm::orc::SymbolMap rt_symbols;
    auto intern = [&](const char* name, void* fptr) {
    auto sym = impl_->lljit->mangleAndIntern(name);
      rt_symbols[sym] = {
          llvm::orc::ExecutorAddr::fromPtr(fptr),
          llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
    };
    intern("jit_rt_mid", reinterpret_cast<void*>(&::jitse::jit_rt_mid));
    intern("jit_rt_symbol_ctx", reinterpret_cast<void*>(&::jitse::jit_rt_symbol_ctx));
    intern("jit_rt_bid", reinterpret_cast<void*>(&::jitse::jit_rt_bid));
    intern("jit_rt_ask", reinterpret_cast<void*>(&::jitse::jit_rt_ask));
    intern("jit_rt_spread", reinterpret_cast<void*>(&::jitse::jit_rt_spread));
    intern("jit_rt_ema", reinterpret_cast<void*>(&::jitse::jit_rt_ema));
    intern("jit_rt_ema_alpha", reinterpret_cast<void*>(&::jitse::jit_rt_ema_alpha));
    intern("jit_rt_sma", reinterpret_cast<void*>(&::jitse::jit_rt_sma));
    intern("jit_rt_sma_prepare", reinterpret_cast<void*>(&::jitse::jit_rt_sma_prepare));
    intern("jit_rt_rolling_std", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_std));
    intern("jit_rt_zscore", reinterpret_cast<void*>(&::jitse::jit_rt_zscore));
    intern("jit_rt_rolling_min", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_min));
    intern("jit_rt_rolling_max", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_max));
    intern("jit_rt_vwap", reinterpret_cast<void*>(&::jitse::jit_rt_vwap));
    intern("jit_rt_lag", reinterpret_cast<void*>(&::jitse::jit_rt_lag));
    intern("jit_rt_cross_above", reinterpret_cast<void*>(&::jitse::jit_rt_cross_above));
    intern("jit_rt_cross_below", reinterpret_cast<void*>(&::jitse::jit_rt_cross_below));
    // P7: register the new runtime entry points with the ORC dylib.
    intern("jit_rt_rolling_corr", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_corr));
    intern("jit_rt_rolling_beta", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_beta));
    intern("jit_rt_kalman1d", reinterpret_cast<void*>(&::jitse::jit_rt_kalman1d));
    intern("jit_rt_sma_lowered_base", reinterpret_cast<void*>(&::jitse::jit_rt_sma_lowered_base));
    intern("jit_rt_ema_lowered_base", reinterpret_cast<void*>(&::jitse::jit_rt_ema_lowered_base));
    intern("jit_rt_lag_lowered_base", reinterpret_cast<void*>(&::jitse::jit_rt_lag_lowered_base));
    if (auto err = impl_->lljit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(rt_symbols)))) {
      impl_->last_error = "Failed to register runtime symbols: " + ToString(std::move(err));
      return false;
    }
    impl_->runtime_symbols_registered = true;
  }

  if (auto err = impl_->lljit->addIRModule(std::move(tsm))) {
    impl_->last_error = "Failed to add IR module: " + ToString(std::move(err));
    return false;
  }

  auto sym = impl_->lljit->lookup(fn_name);
  if (!sym) {
    impl_->last_error = "Failed to lookup compiled symbol: " + ToString(sym.takeError());
    return false;
  }

  impl_->fn = sym->toPtr<JitFn>();
  impl_->last_error.clear();
  return true;
#endif
}

#ifdef JITSE_HAS_LLVM
namespace {
// Shared body for CompileProgram and CompileProgramSpecialized. Differs only
// in the `assume_warm` flag threaded into CodegenContext.
bool CompileProgramImpl(
    JitCompiler::Impl& impl,
    const std::vector<SignalDef>& signals,
    const SymbolTable& symbols,
    StatefulLoweringFlags lowering,
    bool assume_warm,
    bool has_avx2,
    JitCompiler::ProgramFn& out_fn) {
  // P5: reset per-phase compile timings up front. The struct is populated
  // only at the end of a successful compile; any early-return failure path
  // leaves the timings zeroed, which is the same contract the doc
  // promises ("Zeroed before the next compile").
  impl.last_compile_timings = {};
  impl.last_cache_hit = false;
  if (!impl.lljit) {
    impl.last_error = "LLJIT is not initialized";
    return false;
  }
  if (signals.empty()) {
    impl.last_error = "CompileProgram requires at least one signal";
    return false;
  }

  using clk = std::chrono::steady_clock;
  const auto t_compile_start = clk::now();
  // P13: declared out here so the timings tail can reference them on
  // both cache-hit and cache-miss paths. On a cache hit `t_ir_done`
  // collapses to `t_compile_start` (no IR build happened in this
  // call) and `t_opt_done` is updated right after the bitcode load.
  clk::time_point t_ir_done = t_compile_start;

  // P13: persistent module cache. Compute a deterministic fn_name
  // from the program's canonical hash so on-disk bitcode files and
  // the ORC dylib symbol name share an identity.
  const bool cache_enabled = !impl.cache_dir.empty();
  std::string fn_name;
  std::filesystem::path cache_path;
  if (cache_enabled) {
    const std::string key =
        BuildCacheKeyString(signals, lowering, assume_warm, has_avx2, /*lane_count=*/1);
    const std::uint64_t hash = Fnv1aU64(key);
    fn_name = std::string(assume_warm ? "signal_program_warm_func_" : "signal_program_func_") + HexU64(hash);
    cache_path = std::filesystem::path(impl.cache_dir) /
                 CacheFileNameForKey(hash, assume_warm ? "warm" : "scalar");
    // In-memory ORC fast path: if this lljit already has the
    // symbol (a previous Compile in this JitCompiler instance, or
    // a previously-loaded cached module), reuse without touching
    // disk and without re-codegen.
    if (auto sym = impl.lljit->lookup(fn_name)) {
      out_fn = sym->toPtr<JitCompiler::ProgramFn>();
      impl.last_cache_hit = true;
      impl.last_error.clear();
      impl.last_compile_timings.total_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t_compile_start).count());
      return true;
    } else {
      llvm::consumeError(sym.takeError());
    }
  } else {
    fn_name = std::string(assume_warm ? "signal_program_warm_func_" : "signal_program_func_") +
              std::to_string(++impl.compile_counter);
  }

  auto context = std::make_unique<llvm::LLVMContext>();
  std::unique_ptr<llvm::Module> module;
  bool loaded_from_cache = false;
  if (cache_enabled) {
    module = ReadModuleFromCache(cache_path, *context);
    if (module) {
      loaded_from_cache = true;
      // Replay the data layout in case the bitcode was written with
      // a slightly different default. The target triple is already
      // baked into the bitcode by `WriteBitcodeToFile`; we leave it
      // alone to preserve the cached module's identity.
      module->setDataLayout(impl.lljit->getDataLayout());
    }
  }
  if (!loaded_from_cache) {
    module = std::make_unique<llvm::Module>("jit_signal_program_module", *context);
    module->setDataLayout(impl.lljit->getDataLayout());
    // LLVM 21 made the `Module::setTargetTriple(string)` overload
    // explicit-only; wrap the triple so this builds on both old and
    // new LLVM headers.
#if LLVM_VERSION_MAJOR >= 21
    module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
#else
    module->setTargetTriple(llvm::sys::getDefaultTargetTriple());
#endif
  }

  // ----- IR build + LLVM O2 pipeline + ASM capture -----
  // On a cache hit this whole block is skipped: the bitcode we just
  // parsed IS the post-O2 module. The tail (ORC ingest + lookup)
  // runs in both branches and is what actually produces the fn ptr.
  if (!loaded_from_cache) {

  llvm::IRBuilder<> builder(*context);
  llvm::Type* f64 = llvm::Type::getDoubleTy(*context);
  llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
  llvm::Type* market_ptr = llvm::PointerType::getUnqual(*context);
  llvm::Type* arena_ptr = llvm::PointerType::getUnqual(*context);
  llvm::Type* ctx_ptr = llvm::PointerType::getUnqual(*context);
  llvm::Type* u32 = llvm::Type::getInt32Ty(*context);
  llvm::Type* out_ptr = llvm::PointerType::getUnqual(f64);

  auto* fn_ty =
      llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {market_ptr, arena_ptr, u32, out_ptr}, false);
  llvm::Function* fn = llvm::Function::Create(fn_ty, llvm::Function::ExternalLinkage, fn_name, module.get());

  auto it = fn->arg_begin();
  llvm::Value* market_arg = it++;
  llvm::Value* arena_arg = it++;
  llvm::Value* symbol_arg = it++;
  llvm::Value* out_arg = it++;
  market_arg->setName("market");
  arena_arg->setName("arena");
  symbol_arg->setName("symbol_id");
  out_arg->setName("outputs");

  auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
  builder.SetInsertPoint(entry);

  auto rt_md_ty = llvm::FunctionType::get(f64, {market_ptr, i64}, false);
  auto rt_symbol_ctx_ty = llvm::FunctionType::get(ctx_ptr, {arena_ptr, u32}, false);
  auto rt_state_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, i64}, false);
  auto rt_sma_prepare_ty = llvm::FunctionType::get(
      llvm::Type::getInt1Ty(*context), {ctx_ptr, i64, f64, i64, market_ptr, market_ptr}, false);
  auto rt_ema_alpha_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64, i64}, false);
  auto rt_vwap_ty = llvm::FunctionType::get(f64, {market_ptr, ctx_ptr, i64, i64, i64}, false);
  auto rt_cross_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64}, false);
  // P7: rolling_corr/beta take (ctx, node_id, x, y, period); kalman1d
  // takes (ctx, node_id, x, q_lit, r_lit). Both shapes are routed through
  // extern "C" runtime calls (no IR lowering for P7 ops).
  auto rt_pair_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64, i64}, false);
  auto rt_kalman_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64, f64}, false);
  auto rt_lowered_base_ty = llvm::FunctionType::get(ctx_ptr, {ctx_ptr}, false);

  llvm::Value* ctx_arg =
      builder.CreateCall(module->getOrInsertFunction("jit_rt_symbol_ctx", rt_symbol_ctx_ty), {arena_arg, symbol_arg}, "ctx");

  CodegenContext cg{
      *context,
      *module,
      builder,
      symbols,
      market_arg,
      arena_arg,
      symbol_arg,
      ctx_arg,
      module->getOrInsertFunction("jit_rt_mid", rt_md_ty),
      module->getOrInsertFunction("jit_rt_bid", rt_md_ty),
      module->getOrInsertFunction("jit_rt_ask", rt_md_ty),
      module->getOrInsertFunction("jit_rt_spread", rt_md_ty),
      module->getOrInsertFunction("jit_rt_ema", rt_state_ty),
      module->getOrInsertFunction("jit_rt_ema_alpha", rt_ema_alpha_ty),
      module->getOrInsertFunction("jit_rt_sma", rt_state_ty),
      module->getOrInsertFunction("jit_rt_sma_prepare", rt_sma_prepare_ty),
      module->getOrInsertFunction("jit_rt_rolling_std", rt_state_ty),
      module->getOrInsertFunction("jit_rt_zscore", rt_state_ty),
      module->getOrInsertFunction("jit_rt_rolling_min", rt_state_ty),
      module->getOrInsertFunction("jit_rt_rolling_max", rt_state_ty),
      module->getOrInsertFunction("jit_rt_vwap", rt_vwap_ty),
      module->getOrInsertFunction("jit_rt_lag", rt_state_ty),
      module->getOrInsertFunction("jit_rt_cross_above", rt_cross_ty),
      module->getOrInsertFunction("jit_rt_cross_below", rt_cross_ty),
      module->getOrInsertFunction("jit_rt_rolling_corr", rt_pair_ty),
      module->getOrInsertFunction("jit_rt_rolling_beta", rt_pair_ty),
      module->getOrInsertFunction("jit_rt_kalman1d", rt_kalman_ty),
      module->getOrInsertFunction("jit_rt_sma_lowered_base", rt_lowered_base_ty),
      module->getOrInsertFunction("jit_rt_ema_lowered_base", rt_lowered_base_ty),
      module->getOrInsertFunction("jit_rt_lag_lowered_base", rt_lowered_base_ty),
      {},
      {},
      {},
      {},
      false,
      1,
      has_avx2,
      lowering,
      nullptr,
      nullptr,
      nullptr,
      assume_warm,
      assume_warm ? CollectWarmSafeStatefulCalls(signals)
                  : std::unordered_set<const FunctionCall*>{},
      /*lane_count=*/1,
      /*per_lane_market_arg=*/nullptr,
      /*stateful_emit_cache=*/{},
  };

  try {
    cg.use_program_market_cache = true;
    PrewarmProgramMarketLoads(cg, signals);
    for (std::size_t i = 0; i < signals.size(); ++i) {
      llvm::Value* value = EmitExpr(*signals[i].body, cg);
      cg.signal_values[signals[i].name] = value;
      llvm::Value* idx_v = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(i));
      llvm::Value* out_ptr_i = builder.CreateGEP(f64, out_arg, idx_v, "out_ptr");
      builder.CreateStore(value, out_ptr_i);
    }
  } catch (const std::exception& ex) {
    impl.last_error = ex.what();
    return false;
  }
  builder.CreateRetVoid();

  if (llvm::verifyFunction(*fn, &llvm::errs())) {
    impl.last_error = "LLVM verifyFunction failed (program)";
    return false;
  }

  {
    std::string ir_str;
    llvm::raw_string_ostream ir_stream(ir_str);
    module->print(ir_stream, nullptr);
    impl.last_ir_pre_opt = ir_stream.str();
  }

  t_ir_done = clk::now();

  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb;
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  mpm.run(*module, mam);

  {
    std::string ir_str;
    llvm::raw_string_ostream ir_stream(ir_str);
    module->print(ir_stream, nullptr);
    impl.last_ir_post_opt = ir_stream.str();
  }
  // P9: capture host asm of the post-O2 module BEFORE ORC moves it.
  {
    std::string asm_err;
    impl.last_asm = EmitHostAsm(*module, asm_err);
    (void)asm_err;
  }

  // P13: persist the post-O2 module to the cache for future runs.
  // Best-effort: a failure here just means the next compile will
  // also be a miss; correctness is unaffected.
  if (cache_enabled) {
    WriteModuleToCache(*module, cache_path);
  }

  }  // end if (!loaded_from_cache)

  // On a cache hit there is no post-O2 IR to capture from a fresh
  // build, but the user still expects `LastIRPostOpt()` to return a
  // sensible value for diagnostics. Render the cached module's IR
  // here as a one-time cost on the hit path so the contract holds.
  if (loaded_from_cache) {
    std::string ir_str;
    llvm::raw_string_ostream ir_stream(ir_str);
    module->print(ir_stream, nullptr);
    impl.last_ir_pre_opt.clear();
    impl.last_ir_post_opt = ir_stream.str();
    impl.last_asm.clear();  // ASM dump on hit is not worth the legacy-PM cost.
  }

  const auto t_opt_done = clk::now();

  auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::move(context));
  if (!impl.runtime_symbols_registered) {
    llvm::orc::SymbolMap rt_symbols;
    auto intern = [&](const char* name, void* fptr) {
      auto sym = impl.lljit->mangleAndIntern(name);
      rt_symbols[sym] = {
          llvm::orc::ExecutorAddr::fromPtr(fptr),
          llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
    };
    intern("jit_rt_mid", reinterpret_cast<void*>(&::jitse::jit_rt_mid));
    intern("jit_rt_symbol_ctx", reinterpret_cast<void*>(&::jitse::jit_rt_symbol_ctx));
    intern("jit_rt_bid", reinterpret_cast<void*>(&::jitse::jit_rt_bid));
    intern("jit_rt_ask", reinterpret_cast<void*>(&::jitse::jit_rt_ask));
    intern("jit_rt_spread", reinterpret_cast<void*>(&::jitse::jit_rt_spread));
    intern("jit_rt_ema", reinterpret_cast<void*>(&::jitse::jit_rt_ema));
    intern("jit_rt_ema_alpha", reinterpret_cast<void*>(&::jitse::jit_rt_ema_alpha));
    intern("jit_rt_sma", reinterpret_cast<void*>(&::jitse::jit_rt_sma));
    intern("jit_rt_sma_prepare", reinterpret_cast<void*>(&::jitse::jit_rt_sma_prepare));
    intern("jit_rt_rolling_std", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_std));
    intern("jit_rt_zscore", reinterpret_cast<void*>(&::jitse::jit_rt_zscore));
    intern("jit_rt_rolling_min", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_min));
    intern("jit_rt_rolling_max", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_max));
    intern("jit_rt_vwap", reinterpret_cast<void*>(&::jitse::jit_rt_vwap));
    intern("jit_rt_lag", reinterpret_cast<void*>(&::jitse::jit_rt_lag));
    intern("jit_rt_cross_above", reinterpret_cast<void*>(&::jitse::jit_rt_cross_above));
    intern("jit_rt_cross_below", reinterpret_cast<void*>(&::jitse::jit_rt_cross_below));
    // P7: register the new runtime entry points with the ORC dylib.
    intern("jit_rt_rolling_corr", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_corr));
    intern("jit_rt_rolling_beta", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_beta));
    intern("jit_rt_kalman1d", reinterpret_cast<void*>(&::jitse::jit_rt_kalman1d));
    intern("jit_rt_sma_lowered_base", reinterpret_cast<void*>(&::jitse::jit_rt_sma_lowered_base));
    intern("jit_rt_ema_lowered_base", reinterpret_cast<void*>(&::jitse::jit_rt_ema_lowered_base));
    intern("jit_rt_lag_lowered_base", reinterpret_cast<void*>(&::jitse::jit_rt_lag_lowered_base));
    if (auto err = impl.lljit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(rt_symbols)))) {
      impl.last_error = "Failed to register runtime symbols: " + ToString(std::move(err));
      return false;
    }
    impl.runtime_symbols_registered = true;
  }

  if (auto err = impl.lljit->addIRModule(std::move(tsm))) {
    impl.last_error = "Failed to add IR module: " + ToString(std::move(err));
    return false;
  }

  auto sym = impl.lljit->lookup(fn_name);
  if (!sym) {
    impl.last_error = "Failed to lookup compiled symbol: " + ToString(sym.takeError());
    return false;
  }

  const auto t_codegen_done = clk::now();
  using nsd = std::chrono::nanoseconds;
  impl.last_compile_timings.ast_to_ir_ns =
      static_cast<std::uint64_t>(std::chrono::duration_cast<nsd>(t_ir_done - t_compile_start).count());
  impl.last_compile_timings.llvm_opt_ns =
      static_cast<std::uint64_t>(std::chrono::duration_cast<nsd>(t_opt_done - t_ir_done).count());
  impl.last_compile_timings.orc_codegen_ns =
      static_cast<std::uint64_t>(std::chrono::duration_cast<nsd>(t_codegen_done - t_opt_done).count());
  impl.last_compile_timings.total_ns =
      static_cast<std::uint64_t>(std::chrono::duration_cast<nsd>(t_codegen_done - t_compile_start).count());

  out_fn = sym->toPtr<JitCompiler::ProgramFn>();
  impl.last_error.clear();
  impl.last_cache_hit = loaded_from_cache;
  return true;
}

// P2 (cross-symbol vectorization). Mirrors CompileProgramImpl but emits the
// program with a `<K x double>` IR value type for every expression. The
// function signature is
//     void program_vec(const MarketState* const* per_lane_market,
//                      MultiSymbolSignalContext* arena,
//                      std::uint32_t base_symbol,
//                      double* outputs);
// and the outputs layout is signal-major / lane-minor (see jit_compiler.h).
//
// Stateful ops use per-lane scalarized fan-out (P10) by default. The only
// rejection paths that remain in vector mode are the P0 lowered-IR
// variants (kSma/kEma/kLag) whose scalar base-pointer caches cannot be
// composed cleanly with the K-lane fan-out.
bool CompileProgramVectorizedImpl(
    JitCompiler::Impl& impl,
    const std::vector<SignalDef>& signals,
    const SymbolTable& symbols,
    unsigned lane_count,
    StatefulLoweringFlags lowering,
    bool has_avx2,
    JitCompiler::ProgramFnVec& out_fn) {
  if (!impl.lljit) {
    impl.last_error = "LLJIT is not initialized";
    return false;
  }
  if (signals.empty()) {
    impl.last_error = "CompileProgramVectorized requires at least one signal";
    return false;
  }
  if (lane_count != 2 && lane_count != 4 && lane_count != 8) {
    impl.last_error = "CompileProgramVectorized: lane_count must be 2, 4, or 8";
    return false;
  }

  auto context = std::make_unique<llvm::LLVMContext>();
  auto module = std::make_unique<llvm::Module>("jit_signal_program_vec_module", *context);
  module->setDataLayout(impl.lljit->getDataLayout());
  // LLVM 21 made the `Module::setTargetTriple(string)` overload
  // explicit-only; wrap the triple so this builds on both old and
  // new LLVM headers.
#if LLVM_VERSION_MAJOR >= 21
  module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
#else
  module->setTargetTriple(llvm::sys::getDefaultTargetTriple());
#endif

  llvm::IRBuilder<> builder(*context);
  llvm::Type* f64 = llvm::Type::getDoubleTy(*context);
  llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
  llvm::Type* per_lane_market_ptr_ty = llvm::PointerType::getUnqual(*context);
  llvm::Type* arena_ptr = llvm::PointerType::getUnqual(*context);
  llvm::Type* u32 = llvm::Type::getInt32Ty(*context);
  llvm::Type* out_ptr = llvm::PointerType::getUnqual(f64);
  llvm::Type* vec_ty = llvm::FixedVectorType::get(f64, lane_count);

  auto* fn_ty = llvm::FunctionType::get(
      llvm::Type::getVoidTy(*context),
      {per_lane_market_ptr_ty, arena_ptr, u32, out_ptr}, false);
  const std::string fn_name =
      std::string("signal_program_vec") + std::to_string(lane_count) +
      "_func_" + std::to_string(++impl.compile_counter);
  llvm::Function* fn = llvm::Function::Create(
      fn_ty, llvm::Function::ExternalLinkage, fn_name, module.get());

  auto it = fn->arg_begin();
  llvm::Value* per_lane_market_arg = it++;
  llvm::Value* arena_arg = it++;
  llvm::Value* symbol_arg = it++;
  llvm::Value* out_arg = it++;
  per_lane_market_arg->setName("per_lane_market");
  arena_arg->setName("arena");
  symbol_arg->setName("base_symbol");
  out_arg->setName("outputs");

  auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
  builder.SetInsertPoint(entry);

  // We populate all the runtime-call FunctionCallees because, since P10,
  // vectorized programs DO call them through the per-lane scalarized
  // fan-out. The scalar path's type signatures still apply unchanged
  // because the fan-out extracts each lane into a scalar before
  // calling. The IR builder for the vectorized entry uses the same
  // declarations as the scalar entry; the ORC layer interns the same
  // C entry points (jit_rt_sma, jit_rt_ema_alpha, ...) for both.
  llvm::Type* ctx_ptr = llvm::PointerType::getUnqual(*context);
  llvm::Type* market_ptr = llvm::PointerType::getUnqual(*context);
  auto rt_md_ty = llvm::FunctionType::get(f64, {market_ptr, i64}, false);
  auto rt_state_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, i64}, false);
  auto rt_sma_prepare_ty = llvm::FunctionType::get(
      llvm::Type::getInt1Ty(*context), {ctx_ptr, i64, f64, i64, market_ptr, market_ptr}, false);
  auto rt_ema_alpha_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64, i64}, false);
  auto rt_vwap_ty = llvm::FunctionType::get(f64, {market_ptr, ctx_ptr, i64, i64, i64}, false);
  auto rt_cross_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64}, false);
  // P7: rolling_corr/beta take (ctx, node_id, x, y, period); kalman1d
  // takes (ctx, node_id, x, q_lit, r_lit). Both shapes are routed through
  // extern "C" runtime calls (no IR lowering for P7 ops).
  auto rt_pair_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64, i64}, false);
  auto rt_kalman_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64, f64}, false);
  auto rt_lowered_base_ty = llvm::FunctionType::get(ctx_ptr, {ctx_ptr}, false);

  // ctx_arg is unused in the stateless MVP, but populated so CodegenContext
  // is consistent. We don't call jit_rt_symbol_ctx here because the vector
  // entry doesn't have a single per-symbol ctx (it has K of them).
  llvm::Value* ctx_arg_placeholder = llvm::Constant::getNullValue(ctx_ptr);

  CodegenContext cg{
      *context,
      *module,
      builder,
      symbols,
      /*market_arg=*/nullptr,  // unused in vector mode; per_lane_market_arg is used instead
      arena_arg,
      symbol_arg,
      ctx_arg_placeholder,
      module->getOrInsertFunction("jit_rt_mid", rt_md_ty),
      module->getOrInsertFunction("jit_rt_bid", rt_md_ty),
      module->getOrInsertFunction("jit_rt_ask", rt_md_ty),
      module->getOrInsertFunction("jit_rt_spread", rt_md_ty),
      module->getOrInsertFunction("jit_rt_ema", rt_state_ty),
      module->getOrInsertFunction("jit_rt_ema_alpha", rt_ema_alpha_ty),
      module->getOrInsertFunction("jit_rt_sma", rt_state_ty),
      module->getOrInsertFunction("jit_rt_sma_prepare", rt_sma_prepare_ty),
      module->getOrInsertFunction("jit_rt_rolling_std", rt_state_ty),
      module->getOrInsertFunction("jit_rt_zscore", rt_state_ty),
      module->getOrInsertFunction("jit_rt_rolling_min", rt_state_ty),
      module->getOrInsertFunction("jit_rt_rolling_max", rt_state_ty),
      module->getOrInsertFunction("jit_rt_vwap", rt_vwap_ty),
      module->getOrInsertFunction("jit_rt_lag", rt_state_ty),
      module->getOrInsertFunction("jit_rt_cross_above", rt_cross_ty),
      module->getOrInsertFunction("jit_rt_cross_below", rt_cross_ty),
      module->getOrInsertFunction("jit_rt_rolling_corr", rt_pair_ty),
      module->getOrInsertFunction("jit_rt_rolling_beta", rt_pair_ty),
      module->getOrInsertFunction("jit_rt_kalman1d", rt_kalman_ty),
      module->getOrInsertFunction("jit_rt_sma_lowered_base", rt_lowered_base_ty),
      module->getOrInsertFunction("jit_rt_ema_lowered_base", rt_lowered_base_ty),
      module->getOrInsertFunction("jit_rt_lag_lowered_base", rt_lowered_base_ty),
      {},
      {},
      {},
      {},
      false,
      1,
      has_avx2,
      lowering,
      nullptr,
      nullptr,
      nullptr,
      /*assume_warm=*/false,
      std::unordered_set<const FunctionCall*>{},
      lane_count,
      per_lane_market_arg,
      /*stateful_emit_cache=*/{},
  };

  try {
    cg.use_program_market_cache = true;
    PrewarmProgramMarketLoads(cg, signals);
    for (std::size_t i = 0; i < signals.size(); ++i) {
      llvm::Value* value = EmitExpr(*signals[i].body, cg);
      cg.signal_values[signals[i].name] = value;
      // Outputs layout: signal-major / lane-minor.
      // outputs[i * K .. i * K + K - 1] receives signal i's <K x double>.
      llvm::Value* idx_v = llvm::ConstantInt::get(
          i64, static_cast<std::uint64_t>(i) * lane_count);
      llvm::Value* out_ptr_i = builder.CreateInBoundsGEP(
          f64, out_arg, idx_v, ("out_ptr_sig" + std::to_string(i)).c_str());
      // CRITICAL: an unaligned vector store. By default LLVM infers the
      // natural alignment of <K x double> (32 bytes for K=4), but the
      // `outputs` buffer is supplied by the caller as a plain `double*`
      // which only guarantees 8-byte alignment (e.g. std::vector<double>
      // uses the default 8-byte alignment). A 32-byte-aligned `movapd`
      // emitted against an 8-byte-aligned buffer segfaults. We force the
      // alignment to that of `double` so LLVM picks the unaligned form
      // (`movupd` on x86) which costs the same on modern CPUs but is safe
      // for any caller-provided buffer.
      builder.CreateAlignedStore(value, out_ptr_i, llvm::MaybeAlign(alignof(double)));
    }
  } catch (const std::exception& ex) {
    impl.last_error = ex.what();
    return false;
  }
  builder.CreateRetVoid();
  (void)vec_ty;  // referenced via VecTy(cg) inside EmitExpr/EmitMarketFieldLoad

  if (llvm::verifyFunction(*fn, &llvm::errs())) {
    impl.last_error = "LLVM verifyFunction failed (program vec)";
    return false;
  }

  {
    std::string ir_str;
    llvm::raw_string_ostream ir_stream(ir_str);
    module->print(ir_stream, nullptr);
    impl.last_ir_pre_opt = ir_stream.str();
  }

  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb;
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  mpm.run(*module, mam);

  {
    std::string ir_str;
    llvm::raw_string_ostream ir_stream(ir_str);
    module->print(ir_stream, nullptr);
    impl.last_ir_post_opt = ir_stream.str();
  }
  // P9: capture host asm of the post-O2 module BEFORE ORC moves it.
  {
    std::string asm_err;
    impl.last_asm = EmitHostAsm(*module, asm_err);
    (void)asm_err;
  }

  auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::move(context));
  if (!impl.runtime_symbols_registered) {
    // Vector MVP has no runtime symbol dependencies (all calls were
    // rejected up front), but we still register so that any future vectorized
    // helper introduced behaves correctly. Mirrors the scalar path.
    llvm::orc::SymbolMap rt_symbols;
    auto intern = [&](const char* name, void* fptr) {
      auto sym = impl.lljit->mangleAndIntern(name);
      rt_symbols[sym] = {
          llvm::orc::ExecutorAddr::fromPtr(fptr),
          llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
    };
    intern("jit_rt_mid", reinterpret_cast<void*>(&::jitse::jit_rt_mid));
    intern("jit_rt_symbol_ctx", reinterpret_cast<void*>(&::jitse::jit_rt_symbol_ctx));
    intern("jit_rt_bid", reinterpret_cast<void*>(&::jitse::jit_rt_bid));
    intern("jit_rt_ask", reinterpret_cast<void*>(&::jitse::jit_rt_ask));
    intern("jit_rt_spread", reinterpret_cast<void*>(&::jitse::jit_rt_spread));
    intern("jit_rt_ema", reinterpret_cast<void*>(&::jitse::jit_rt_ema));
    intern("jit_rt_ema_alpha", reinterpret_cast<void*>(&::jitse::jit_rt_ema_alpha));
    intern("jit_rt_sma", reinterpret_cast<void*>(&::jitse::jit_rt_sma));
    intern("jit_rt_sma_prepare", reinterpret_cast<void*>(&::jitse::jit_rt_sma_prepare));
    intern("jit_rt_rolling_std", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_std));
    intern("jit_rt_zscore", reinterpret_cast<void*>(&::jitse::jit_rt_zscore));
    intern("jit_rt_rolling_min", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_min));
    intern("jit_rt_rolling_max", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_max));
    intern("jit_rt_vwap", reinterpret_cast<void*>(&::jitse::jit_rt_vwap));
    intern("jit_rt_lag", reinterpret_cast<void*>(&::jitse::jit_rt_lag));
    intern("jit_rt_cross_above", reinterpret_cast<void*>(&::jitse::jit_rt_cross_above));
    intern("jit_rt_cross_below", reinterpret_cast<void*>(&::jitse::jit_rt_cross_below));
    // P7: register the new runtime entry points with the ORC dylib.
    intern("jit_rt_rolling_corr", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_corr));
    intern("jit_rt_rolling_beta", reinterpret_cast<void*>(&::jitse::jit_rt_rolling_beta));
    intern("jit_rt_kalman1d", reinterpret_cast<void*>(&::jitse::jit_rt_kalman1d));
    intern("jit_rt_sma_lowered_base", reinterpret_cast<void*>(&::jitse::jit_rt_sma_lowered_base));
    intern("jit_rt_ema_lowered_base", reinterpret_cast<void*>(&::jitse::jit_rt_ema_lowered_base));
    intern("jit_rt_lag_lowered_base", reinterpret_cast<void*>(&::jitse::jit_rt_lag_lowered_base));
    if (auto err = impl.lljit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(rt_symbols)))) {
      impl.last_error = "Failed to register runtime symbols: " + ToString(std::move(err));
      return false;
    }
    impl.runtime_symbols_registered = true;
  }

  if (auto err = impl.lljit->addIRModule(std::move(tsm))) {
    impl.last_error = "Failed to add IR module: " + ToString(std::move(err));
    return false;
  }

  auto sym = impl.lljit->lookup(fn_name);
  if (!sym) {
    impl.last_error = "Failed to lookup compiled symbol: " + ToString(sym.takeError());
    return false;
  }

  out_fn = sym->toPtr<JitCompiler::ProgramFnVec>();
  impl.last_error.clear();
  return true;
}
}  // namespace
#endif

bool JitCompiler::CompileProgram(const std::vector<SignalDef>& signals, const SymbolTable& symbols) {
  impl_->fn = nullptr;
  impl_->program_fn = nullptr;
  impl_->program_fn_vec = nullptr;
  impl_->vec_lane_count = 0;
  impl_->last_ir_pre_opt.clear();
  impl_->last_ir_post_opt.clear();
  impl_->last_asm.clear();
#ifndef JITSE_HAS_LLVM
  (void)signals;
  (void)symbols;
  impl_->last_error = "LLVM support is disabled at build time";
  return false;
#else
  return CompileProgramImpl(
      *impl_, signals, symbols, impl_->stateful_lowering,
      /*assume_warm=*/false, HasAVX2(), impl_->program_fn);
#endif
}

bool JitCompiler::CompileProgramSpecialized(
    const std::vector<SignalDef>& signals,
    const SymbolTable& symbols,
    const JitProfile& profile) {
  impl_->fn = nullptr;
  impl_->program_fn = nullptr;
  impl_->program_fn_vec = nullptr;
  impl_->vec_lane_count = 0;
  impl_->last_ir_pre_opt.clear();
  impl_->last_ir_post_opt.clear();
  impl_->last_asm.clear();
#ifndef JITSE_HAS_LLVM
  (void)signals;
  (void)symbols;
  (void)profile;
  impl_->last_error = "LLVM support is disabled at build time";
  return false;
#else
  return CompileProgramImpl(
      *impl_, signals, symbols, impl_->stateful_lowering,
      profile.assume_warm, HasAVX2(), impl_->program_fn);
#endif
}

bool JitCompiler::CompileProgramVectorized(
    const std::vector<SignalDef>& signals,
    const SymbolTable& symbols,
    unsigned lane_count) {
  impl_->fn = nullptr;
  impl_->program_fn = nullptr;
  impl_->program_fn_vec = nullptr;
  impl_->vec_lane_count = 0;
  impl_->last_ir_pre_opt.clear();
  impl_->last_ir_post_opt.clear();
  impl_->last_asm.clear();
#ifndef JITSE_HAS_LLVM
  (void)signals;
  (void)symbols;
  (void)lane_count;
  impl_->last_error = "LLVM support is disabled at build time";
  return false;
#else
  const bool ok = CompileProgramVectorizedImpl(
      *impl_, signals, symbols, lane_count, impl_->stateful_lowering,
      HasAVX2(), impl_->program_fn_vec);
  if (ok) impl_->vec_lane_count = lane_count;
  return ok;
#endif
}

JitCompiler::JitFn JitCompiler::GetFunction() const { return impl_->fn; }
JitCompiler::ProgramFn JitCompiler::GetProgramFunction() const { return impl_->program_fn; }
JitCompiler::ProgramFnVec JitCompiler::GetProgramVectorizedFunction() const { return impl_->program_fn_vec; }
unsigned JitCompiler::VectorizedLaneCount() const { return impl_->vec_lane_count; }
CompileTimings JitCompiler::LastCompileTimings() const { return impl_->last_compile_timings; }

void JitCompiler::EnableModuleCache(const std::string& cache_dir) {
  impl_->cache_dir = cache_dir;
  if (!cache_dir.empty()) {
    // Make sure the dir exists. We don't fail Compile if it can't be
    // created (caching is opt-in and non-essential); we just log the
    // error to `last_error` lazily on the first cache miss.
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    (void)ec;
  }
}

void JitCompiler::DisableModuleCache() {
  impl_->cache_dir.clear();
  impl_->last_cache_hit = false;
}

bool JitCompiler::ModuleCacheEnabled() const { return !impl_->cache_dir.empty(); }
bool JitCompiler::LastCacheHit() const { return impl_->last_cache_hit; }

void JitCompiler::DumpLastIR() const {
#ifdef JITSE_HAS_LLVM
  llvm::errs() << impl_->last_ir_post_opt;
#endif
}

void JitCompiler::DumpLastIRPreOpt() const {
#ifdef JITSE_HAS_LLVM
  llvm::errs() << impl_->last_ir_pre_opt;
#endif
}

const std::string& JitCompiler::LastIRPostOpt() const { return impl_->last_ir_post_opt; }
const std::string& JitCompiler::LastIRPreOpt() const { return impl_->last_ir_pre_opt; }

void JitCompiler::DumpLastAsm() const {
#ifdef JITSE_HAS_LLVM
  llvm::errs() << impl_->last_asm;
#endif
}

const std::string& JitCompiler::LastAsm() const { return impl_->last_asm; }

// ============================================================================
// TieredProgramJit
// ----------------------------------------------------------------------------
// Two-tier wrapper. The baseline compile is owned by `baseline_jit_` and is
// invoked immediately on Compile(); the specialized (assume_warm) compile is
// owned by `specialized_jit_` and is lazily built by Promote(). The active
// function pointer is published via `active_fn_.store(..., release)` and
// readers do `active_fn_.load(acquire)`, so a multi-threaded evaluator can
// switch tiers between batches without any explicit synchronization on the
// hot path.
// ============================================================================
struct TieredProgramJit::Impl {
  JitCompiler baseline_jit;
  JitCompiler specialized_jit;

  // Non-owning pointers to the caller's signals + symbols. They must outlive
  // this TieredProgramJit instance. Holding pointers (instead of cloning)
  // guarantees that node_ids the JIT uses at compile time are the same
  // node_ids the caller observed when prewarming the SignalContext.
  const std::vector<SignalDef>* signals = nullptr;
  const SymbolTable* symbols = nullptr;
  StatefulLoweringFlags lowering = StatefulLoweringFlags::kAll;
  std::int64_t warmup_threshold = 0;

  std::atomic<ProgramFn> active_fn{nullptr};
  std::atomic<bool> promoted{false};
  bool compiled = false;
  std::string last_error;
};

TieredProgramJit::TieredProgramJit() : impl_(std::make_unique<Impl>()) {}
TieredProgramJit::~TieredProgramJit() = default;

bool TieredProgramJit::IsAvailable() const {
  return impl_->baseline_jit.IsAvailable() && impl_->specialized_jit.IsAvailable();
}

bool TieredProgramJit::Compile(
    const std::vector<SignalDef>& signals,
    const SymbolTable& symbols,
    StatefulLoweringFlags lowering) {
  impl_->active_fn.store(nullptr, std::memory_order_release);
  impl_->promoted.store(false, std::memory_order_release);
  impl_->compiled = false;

  if (!IsAvailable()) {
    impl_->last_error = "JIT is unavailable";
    return false;
  }

  // Hold non-owning pointers to the caller's signals. We do NOT clone or
  // mutate node_ids; the caller is responsible for having allocated them.
  // The baseline compile and the specialized compile both walk the SAME
  // FunctionCall* nodes, guaranteeing that node-ID-based state slot indexing
  // is consistent with whatever the caller has prewarmed in the
  // SignalContext.
  impl_->signals = &signals;
  impl_->symbols = &symbols;
  impl_->lowering = lowering;
  impl_->warmup_threshold = ComputeProgramWarmupThreshold(signals);

  impl_->baseline_jit.SetStatefulLowering(lowering);
  if (!impl_->baseline_jit.CompileProgram(signals, symbols)) {
    impl_->last_error = "baseline compile failed: " + impl_->baseline_jit.LastError();
    return false;
  }
  ProgramFn baseline_fn = impl_->baseline_jit.GetProgramFunction();
  if (baseline_fn == nullptr) {
    impl_->last_error = "baseline compile returned null program fn";
    return false;
  }
  impl_->active_fn.store(baseline_fn, std::memory_order_release);
  impl_->compiled = true;
  impl_->last_error.clear();
  return true;
}

bool TieredProgramJit::Promote() {
  if (!impl_->compiled) {
    impl_->last_error = "Promote() before Compile()";
    return false;
  }
  if (impl_->promoted.load(std::memory_order_acquire)) {
    return true;  // already promoted, idempotent
  }
  impl_->specialized_jit.SetStatefulLowering(impl_->lowering);
  JitProfile profile;
  profile.assume_warm = true;
  if (!impl_->specialized_jit.CompileProgramSpecialized(*impl_->signals, *impl_->symbols, profile)) {
    impl_->last_error = "specialized compile failed: " + impl_->specialized_jit.LastError();
    return false;
  }
  ProgramFn specialized_fn = impl_->specialized_jit.GetProgramFunction();
  if (specialized_fn == nullptr) {
    impl_->last_error = "specialized compile returned null program fn";
    return false;
  }
  // Atomic publish: any thread that subsequently calls CurrentFunction()
  // sees the specialized fn. Threads currently mid-call to the baseline fn
  // are unaffected (they'll finish on baseline and pick up specialized next
  // iteration).
  impl_->active_fn.store(specialized_fn, std::memory_order_release);
  impl_->promoted.store(true, std::memory_order_release);
  impl_->last_error.clear();
  return true;
}

TieredProgramJit::ProgramFn TieredProgramJit::CurrentFunction() const {
  return impl_->active_fn.load(std::memory_order_acquire);
}

std::int64_t TieredProgramJit::WarmupTickThreshold() const {
  return impl_->warmup_threshold;
}

bool TieredProgramJit::IsPromoted() const {
  return impl_->promoted.load(std::memory_order_acquire);
}

std::string TieredProgramJit::LastError() const { return impl_->last_error; }

const std::string& TieredProgramJit::BaselineIRPostOpt() const {
  return impl_->baseline_jit.LastIRPostOpt();
}
const std::string& TieredProgramJit::SpecializedIRPostOpt() const {
  return impl_->specialized_jit.LastIRPostOpt();
}

}  // namespace jitse
