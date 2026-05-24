#include "jit_compiler.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

struct JitCompiler::Impl {
  std::string last_error;
  std::string last_ir_pre_opt;
  std::string last_ir_post_opt;
  JitFn fn = nullptr;
  ProgramFn program_fn = nullptr;

#ifdef JITSE_HAS_LLVM
  std::unique_ptr<llvm::orc::LLJIT> lljit;
  std::uint64_t compile_counter = 0;
  bool runtime_symbols_registered = false;
  bool host_has_avx2 = false;
#endif
};

JitCompiler::JitCompiler() : impl_(std::make_unique<Impl>()) {
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

  std::unordered_map<const FunctionCall*, std::int64_t> node_ids;
  std::unordered_map<std::string, llvm::Value*> signal_values;
  std::int64_t next_node_id = 1;
  bool use_avx2 = false;
};

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
  // Mirror MarketState/InstrumentState layout for direct loads in JIT IR.
  // InstrumentState: { bid, ask, last_price, volume, last_update_ns }
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  llvm::Type* i64 = llvm::Type::getInt64Ty(cg.llctx);
  llvm::StructType* instrument_ty = llvm::StructType::get(cg.llctx, {f64, f64, f64, f64, i64});
  llvm::ArrayType* instruments_arr_ty = llvm::ArrayType::get(instrument_ty, kMaxInstruments);
  llvm::StructType* market_ty = llvm::StructType::get(cg.llctx, {instruments_arr_ty, i64});
  llvm::PointerType* market_ptr_ty = llvm::PointerType::getUnqual(market_ty);

  llvm::Value* typed_market = cg.builder.CreateBitCast(cg.market_arg, market_ptr_ty, "market_typed");
  llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
  llvm::Value* sym = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(sym_id));
  llvm::Value* instruments_ptr = cg.builder.CreateStructGEP(market_ty, typed_market, 0, "instruments_ptr");
  llvm::Value* instrument_ptr =
      cg.builder.CreateInBoundsGEP(instruments_arr_ty, instruments_ptr, {zero, sym}, "instrument_ptr");
  llvm::Value* field_ptr =
      cg.builder.CreateStructGEP(instrument_ty, instrument_ptr, field_index, std::string(name) + "_ptr");
  return cg.builder.CreateLoad(f64, field_ptr, name);
}

llvm::Value* EmitExpr(const Expr& expr, CodegenContext& cg) {
  llvm::Type* f64 = llvm::Type::getDoubleTy(cg.llctx);
  llvm::Type* i64 = llvm::Type::getInt64Ty(cg.llctx);

  if (const auto* n = dynamic_cast<const NumberLiteral*>(&expr)) {
    return llvm::ConstantFP::get(f64, n->value);
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
        return cg.builder.CreateUIToFP(c, f64, "gt_f");
      }
      case BinaryOpKind::Lt: {
        llvm::Value* c = cg.builder.CreateFCmpOLT(l, r, "lt");
        return cg.builder.CreateUIToFP(c, f64, "lt_f");
      }
      case BinaryOpKind::Gte: {
        llvm::Value* c = cg.builder.CreateFCmpOGE(l, r, "gte");
        return cg.builder.CreateUIToFP(c, f64, "gte_f");
      }
      case BinaryOpKind::Lte: {
        llvm::Value* c = cg.builder.CreateFCmpOLE(l, r, "lte");
        return cg.builder.CreateUIToFP(c, f64, "lte_f");
      }
      case BinaryOpKind::Eq: {
        // Approx-equality to mirror interpreter semantics.
        llvm::Value* d = cg.builder.CreateFSub(l, r, "eq_diff");
        llvm::Function* fabs_fn = llvm::Intrinsic::getDeclaration(&cg.module, llvm::Intrinsic::fabs, {f64});
        llvm::Value* ad = cg.builder.CreateCall(fabs_fn, {d}, "eq_abs");
        llvm::Value* c = cg.builder.CreateFCmpOLT(ad, llvm::ConstantFP::get(f64, 1e-12), "eq");
        return cg.builder.CreateUIToFP(c, f64, "eq_f");
      }
      case BinaryOpKind::NotEq: {
        llvm::Value* d = cg.builder.CreateFSub(l, r, "neq_diff");
        llvm::Function* fabs_fn = llvm::Intrinsic::getDeclaration(&cg.module, llvm::Intrinsic::fabs, {f64});
        llvm::Value* ad = cg.builder.CreateCall(fabs_fn, {d}, "neq_abs");
        llvm::Value* c = cg.builder.CreateFCmpOGE(ad, llvm::ConstantFP::get(f64, 1e-12), "neq");
        return cg.builder.CreateUIToFP(c, f64, "neq_f");
      }
      case BinaryOpKind::And: {
        llvm::Value* lnz = cg.builder.CreateFCmpUNE(l, llvm::ConstantFP::get(f64, 0.0), "and_l");
        llvm::Value* rnz = cg.builder.CreateFCmpUNE(r, llvm::ConstantFP::get(f64, 0.0), "and_r");
        llvm::Value* both = cg.builder.CreateAnd(lnz, rnz, "and");
        return cg.builder.CreateUIToFP(both, f64, "and_f");
      }
      case BinaryOpKind::Or: {
        llvm::Value* lnz = cg.builder.CreateFCmpUNE(l, llvm::ConstantFP::get(f64, 0.0), "or_l");
        llvm::Value* rnz = cg.builder.CreateFCmpUNE(r, llvm::ConstantFP::get(f64, 0.0), "or_r");
        llvm::Value* either = cg.builder.CreateOr(lnz, rnz, "or");
        return cg.builder.CreateUIToFP(either, f64, "or_f");
      }
    }
  }

  if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
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
        return cg.builder.CreateFMul(sum, llvm::ConstantFP::get(f64, 0.5), "mid");
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
      return cg.builder.CreateCall(cg.fn_vwap, {cg.market_arg, cg.ctx_arg, node_id, sym_id_v, per}, "vwap");
    }

    if (fn->name == "abs") {
      if (fn->args.size() != 1) throw std::runtime_error("abs() expects one argument");
      llvm::Value* x = EmitExpr(*fn->args[0], cg);
      llvm::Function* fabs_fn = llvm::Intrinsic::getDeclaration(&cg.module, llvm::Intrinsic::fabs, {f64});
      return cg.builder.CreateCall(fabs_fn, {x}, "abs");
    }
    if (fn->name == "sqrt") {
      if (fn->args.size() != 1) throw std::runtime_error("sqrt() expects one argument");
      llvm::Value* x = EmitExpr(*fn->args[0], cg);
      llvm::Function* sqrt_fn = llvm::Intrinsic::getDeclaration(&cg.module, llvm::Intrinsic::sqrt, {f64});
      return cg.builder.CreateCall(sqrt_fn, {x}, "sqrt");
    }
    if (fn->name == "log") {
      if (fn->args.size() != 1) throw std::runtime_error("log() expects one argument");
      llvm::Value* x = EmitExpr(*fn->args[0], cg);
      llvm::Function* log_fn = llvm::Intrinsic::getDeclaration(&cg.module, llvm::Intrinsic::log, {f64});
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
      llvm::Value* node_id = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(GetNodeId(cg, fn)));
      llvm::Value* per = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(period));
      if (fn->name == "ema") {
        const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
        llvm::Value* alpha_v = llvm::ConstantFP::get(f64, alpha);
        return cg.builder.CreateCall(cg.fn_ema_alpha, {cg.ctx_arg, node_id, x, alpha_v, per}, "ema");
      }
      if (fn->name == "sma") {
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
      if (fn->name == "lag") return cg.builder.CreateCall(cg.fn_lag, {cg.ctx_arg, node_id, x, per}, "lag");
      if (fn->name == "rolling_min") return cg.builder.CreateCall(cg.fn_rolling_min, {cg.ctx_arg, node_id, x, per}, "rmin");
      return cg.builder.CreateCall(cg.fn_rolling_max, {cg.ctx_arg, node_id, x, per}, "rmax");
    }
    if (fn->name == "cross_above" || fn->name == "cross_below") {
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
      {},
      {},
      1,
      HasAVX2(),
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

bool JitCompiler::CompileProgram(const std::vector<SignalDef>& signals, const SymbolTable& symbols) {
  impl_->fn = nullptr;
  impl_->program_fn = nullptr;
  impl_->last_ir_pre_opt.clear();
  impl_->last_ir_post_opt.clear();
#ifndef JITSE_HAS_LLVM
  (void)signals;
  (void)symbols;
  impl_->last_error = "LLVM support is disabled at build time";
  return false;
#else
  if (!impl_->lljit) {
    impl_->last_error = "LLJIT is not initialized";
    return false;
  }
  if (signals.empty()) {
    impl_->last_error = "CompileProgram requires at least one signal";
    return false;
  }

  auto context = std::make_unique<llvm::LLVMContext>();
  auto module = std::make_unique<llvm::Module>("jit_signal_program_module", *context);
  module->setDataLayout(impl_->lljit->getDataLayout());
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
  const std::string fn_name = "signal_program_func_" + std::to_string(++impl_->compile_counter);
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
      {},
      {},
      1,
      HasAVX2(),
  };

  try {
    for (std::size_t i = 0; i < signals.size(); ++i) {
      llvm::Value* value = EmitExpr(*signals[i].body, cg);
      cg.signal_values[signals[i].name] = value;
      llvm::Value* idx_v = llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(i));
      llvm::Value* out_ptr_i = builder.CreateGEP(f64, out_arg, idx_v, "out_ptr");
      builder.CreateStore(value, out_ptr_i);
    }
  } catch (const std::exception& ex) {
    impl_->last_error = ex.what();
    return false;
  }
  builder.CreateRetVoid();

  if (llvm::verifyFunction(*fn, &llvm::errs())) {
    impl_->last_error = "LLVM verifyFunction failed (program)";
    return false;
  }

  {
    std::string ir_str;
    llvm::raw_string_ostream ir_stream(ir_str);
    module->print(ir_stream, nullptr);
    impl_->last_ir_pre_opt = ir_stream.str();
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

  impl_->program_fn = sym->toPtr<ProgramFn>();
  impl_->last_error.clear();
  return true;
#endif
}

JitCompiler::JitFn JitCompiler::GetFunction() const { return impl_->fn; }
JitCompiler::ProgramFn JitCompiler::GetProgramFunction() const { return impl_->program_fn; }

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

}  // namespace jitse
