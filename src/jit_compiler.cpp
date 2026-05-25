#include "jit_compiler.h"

#include "ast_utils.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
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
}  // namespace

struct JitCompiler::Impl {
  std::string last_error;
  std::string last_ir_pre_opt;
  std::string last_ir_post_opt;
  JitFn fn = nullptr;
  ProgramFn program_fn = nullptr;
  // P2: vectorized program function and its lane count. Only one of
  // `program_fn` / `program_fn_vec` is meaningful at a time -- compiling a
  // scalar program clears `program_fn_vec` and vice versa.
  ProgramFnVec program_fn_vec = nullptr;
  unsigned vec_lane_count = 0;
  StatefulLoweringFlags stateful_lowering = StatefulLoweringFlags::kNone;

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
  llvm::StringMap<bool> host_features;
  if (llvm::sys::getHostCPUFeatures(host_features)) {
    auto it = host_features.find("avx2");
    impl_->host_has_avx2 = (it != host_features.end()) && it->second;
  } else {
    impl_->host_has_avx2 = false;
  }
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
  // becomes a `<lane_count x double>`. The MVP supports lane_count in {2, 4,
  // 8}. The `per_lane_market_arg` points at an array of `lane_count`
  // `MarketState*` pointers; the i-th lane's market data is loaded from
  // `per_lane_market_arg[i]`. Note that the scalar `market_arg` is unused
  // when vectorized; the codegen uses `per_lane_market_arg` exclusively.
  //
  // The MVP rejects stateful ops in vectorized mode -- per-lane state lives
  // in a per-symbol SignalContext slot and cannot be loaded/stored via a
  // single `<K x double>` op; supporting that requires a per-lane scalarized
  // fan-out which is left as a follow-up.
  unsigned lane_count = 1;
  llvm::Value* per_lane_market_arg = nullptr;
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
      "vectorized JIT (P2) does not yet support stateful op '" + op_name +
      "'; the MVP supports only stateless arithmetic + market loads. "
      "Use scalar CompileProgram for programs containing this op.");
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
      if (IsVectorized(cg)) RejectInVector("vwap");
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
      return cg.builder.CreateCall(cg.fn_vwap, {cg.market_arg, cg.ctx_arg, node_id, sym_id_v, per}, "vwap");
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
      if (IsVectorized(cg)) RejectInVector(fn->name);
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
      if (fn->name == "ema") {
        if (HasFlag(cg.lowering, StatefulLoweringFlags::kEma)) {
          return EmitLoweredEma(cg, fn, x, node_id_val, period);
        }
        const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
        llvm::Value* alpha_v = llvm::ConstantFP::get(f64, alpha);
        return cg.builder.CreateCall(cg.fn_ema_alpha, {cg.ctx_arg, node_id, x, alpha_v, per}, "ema");
      }
      if (fn->name == "sma") {
        if (HasFlag(cg.lowering, StatefulLoweringFlags::kSma)) {
          return EmitLoweredSma(cg, fn, x, node_id_val, period);
        }
        if (!cg.use_avx2 || period < 4) {
          return cg.builder.CreateCall(cg.fn_sma, {cg.ctx_arg, node_id, x, per}, "sma");
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
        return phi;
      }
      if (fn->name == "rolling_std") return cg.builder.CreateCall(cg.fn_rolling_std, {cg.ctx_arg, node_id, x, per}, "rstd");
      if (fn->name == "zscore") return cg.builder.CreateCall(cg.fn_zscore, {cg.ctx_arg, node_id, x, per}, "zscore");
      if (fn->name == "lag") {
        if (HasFlag(cg.lowering, StatefulLoweringFlags::kLag)) {
          return EmitLoweredLag(cg, fn, x, node_id_val, period);
        }
        return cg.builder.CreateCall(cg.fn_lag, {cg.ctx_arg, node_id, x, per}, "lag");
      }
      if (fn->name == "rolling_min") return cg.builder.CreateCall(cg.fn_rolling_min, {cg.ctx_arg, node_id, x, per}, "rmin");
      return cg.builder.CreateCall(cg.fn_rolling_max, {cg.ctx_arg, node_id, x, per}, "rmax");
    }
    if (fn->name == "cross_above" || fn->name == "cross_below") {
      if (IsVectorized(cg)) RejectInVector(fn->name);
      if (fn->args.size() != 2) {
        throw std::runtime_error(fn->name + "() expects two args");
      }
      llvm::Value* a = EmitExpr(*fn->args[0], cg);
      llvm::Value* b = EmitExpr(*fn->args[1], cg);
      llvm::Value* node_id = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(GetNodeId(cg, fn)));
      if (fn->name == "cross_above") {
        return cg.builder.CreateCall(cg.fn_cross_above, {cg.ctx_arg, node_id, a, b}, "cross_above");
      }
      return cg.builder.CreateCall(cg.fn_cross_below, {cg.ctx_arg, node_id, a, b}, "cross_below");
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
  module->setTargetTriple(llvm::sys::getDefaultTargetTriple());

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
  if (!impl.lljit) {
    impl.last_error = "LLJIT is not initialized";
    return false;
  }
  if (signals.empty()) {
    impl.last_error = "CompileProgram requires at least one signal";
    return false;
  }

  auto context = std::make_unique<llvm::LLVMContext>();
  auto module = std::make_unique<llvm::Module>("jit_signal_program_module", *context);
  module->setDataLayout(impl.lljit->getDataLayout());
  module->setTargetTriple(llvm::sys::getDefaultTargetTriple());

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
  const std::string fn_name =
      std::string(assume_warm ? "signal_program_warm_func_" : "signal_program_func_") +
      std::to_string(++impl.compile_counter);
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

  out_fn = sym->toPtr<JitCompiler::ProgramFn>();
  impl.last_error.clear();
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
// Stateful ops are rejected up front via EmitExpr's RejectInVector path.
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
  module->setTargetTriple(llvm::sys::getDefaultTargetTriple());

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

  // Even though the MVP rejects stateful ops, we still need to populate the
  // runtime-call FunctionCallees because EmitExpr references them when
  // checking the per-op name -- the RejectInVector early-return runs before
  // the FunctionCallee is actually invoked, but the CodegenContext struct
  // requires the field to be initialized. Use the same type signatures as
  // the scalar path.
  llvm::Type* ctx_ptr = llvm::PointerType::getUnqual(*context);
  llvm::Type* market_ptr = llvm::PointerType::getUnqual(*context);
  auto rt_md_ty = llvm::FunctionType::get(f64, {market_ptr, i64}, false);
  auto rt_state_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, i64}, false);
  auto rt_sma_prepare_ty = llvm::FunctionType::get(
      llvm::Type::getInt1Ty(*context), {ctx_ptr, i64, f64, i64, market_ptr, market_ptr}, false);
  auto rt_ema_alpha_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64, i64}, false);
  auto rt_vwap_ty = llvm::FunctionType::get(f64, {market_ptr, ctx_ptr, i64, i64, i64}, false);
  auto rt_cross_ty = llvm::FunctionType::get(f64, {ctx_ptr, i64, f64, f64}, false);
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
