; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr nocapture readonly %market, ptr %arena, i32 %symbol_id, ptr nocapture writeonly %outputs) local_unnamed_addr {
entry:
  %sma_buffer_ptr_addr32 = alloca ptr, align 8
  %sma_size_addr33 = alloca i64, align 8
  %sma_buffer_ptr_addr = alloca ptr, align 8
  %sma_size_addr = alloca i64, align 8
  %ctx = tail call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %bid = load double, ptr %market, align 8
  %ask_ptr = getelementptr inbounds { double, double, double, double, i64 }, ptr %market, i64 0, i32 1
  %ask = load double, ptr %ask_ptr, align 8
  %mid_sum = fadd double %bid, %ask
  %mid = fmul double %mid_sum, 5.000000e-01
  %sma_full = call i1 @jit_rt_sma_prepare(ptr %ctx, i64 1, double %mid, i64 20, ptr nonnull %sma_buffer_ptr_addr, ptr nonnull %sma_size_addr)
  br i1 %sma_full, label %sma_simd, label %sma_merge

sma_simd:                                         ; preds = %entry
  %sma_buffer_ptr = load ptr, ptr %sma_buffer_ptr_addr, align 8
  %sma_vload = load <4 x double>, ptr %sma_buffer_ptr, align 32
  %sma_vsum = fadd <4 x double> %sma_vload, zeroinitializer
  %sma_chunk_ptr3 = getelementptr inbounds double, ptr %sma_buffer_ptr, i64 4
  %sma_vload4 = load <4 x double>, ptr %sma_chunk_ptr3, align 32
  %sma_vsum5 = fadd <4 x double> %sma_vsum, %sma_vload4
  %sma_chunk_ptr6 = getelementptr inbounds double, ptr %sma_buffer_ptr, i64 8
  %sma_vload7 = load <4 x double>, ptr %sma_chunk_ptr6, align 32
  %sma_vsum8 = fadd <4 x double> %sma_vsum5, %sma_vload7
  %sma_chunk_ptr9 = getelementptr inbounds double, ptr %sma_buffer_ptr, i64 12
  %sma_vload10 = load <4 x double>, ptr %sma_chunk_ptr9, align 32
  %sma_vsum11 = fadd <4 x double> %sma_vsum8, %sma_vload10
  %sma_chunk_ptr12 = getelementptr inbounds double, ptr %sma_buffer_ptr, i64 16
  %sma_vload13 = load <4 x double>, ptr %sma_chunk_ptr12, align 32
  %sma_vsum14 = fadd <4 x double> %sma_vsum11, %sma_vload13
  %shift = shufflevector <4 x double> %sma_vsum14, <4 x double> poison, <4 x i32> <i32 1, i32 poison, i32 poison, i32 poison>
  %0 = fadd <4 x double> %sma_vsum14, %shift
  %shift68 = shufflevector <4 x double> %sma_vsum14, <4 x double> poison, <4 x i32> <i32 2, i32 poison, i32 poison, i32 poison>
  %1 = fadd <4 x double> %shift68, %0
  %shift69 = shufflevector <4 x double> %sma_vsum14, <4 x double> poison, <4 x i32> <i32 3, i32 poison, i32 poison, i32 poison>
  %2 = fadd <4 x double> %shift69, %1
  %sma_lane_add20 = extractelement <4 x double> %2, i64 0
  %sma_simd_mean = fdiv double %sma_lane_add20, 2.000000e+01
  br label %sma_merge

sma_merge:                                        ; preds = %entry, %sma_simd
  %sma_out = phi double [ %sma_simd_mean, %sma_simd ], [ 0x7FF8000000000000, %entry ]
  store double %sma_out, ptr %outputs, align 8
  %ema = call double @jit_rt_ema_alpha(ptr %ctx, i64 2, double %mid, double 1.250000e-01, i64 15)
  %out_ptr23 = getelementptr double, ptr %outputs, i64 1
  store double %ema, ptr %out_ptr23, align 8
  %lag = call double @jit_rt_lag(ptr %ctx, i64 3, double %mid, i64 7)
  %out_ptr26 = getelementptr double, ptr %outputs, i64 2
  store double %lag, ptr %out_ptr26, align 8
  %sma_full34 = call i1 @jit_rt_sma_prepare(ptr %ctx, i64 4, double %mid, i64 20, ptr nonnull %sma_buffer_ptr_addr32, ptr nonnull %sma_size_addr33)
  br i1 %sma_full34, label %sma_simd29, label %sma_merge31

sma_simd29:                                       ; preds = %sma_merge
  %sma_buffer_ptr35 = load ptr, ptr %sma_buffer_ptr_addr32, align 8
  %sma_vload37 = load <4 x double>, ptr %sma_buffer_ptr35, align 32
  %sma_vsum38 = fadd <4 x double> %sma_vload37, zeroinitializer
  %sma_chunk_ptr39 = getelementptr inbounds double, ptr %sma_buffer_ptr35, i64 4
  %sma_vload40 = load <4 x double>, ptr %sma_chunk_ptr39, align 32
  %sma_vsum41 = fadd <4 x double> %sma_vsum38, %sma_vload40
  %sma_chunk_ptr42 = getelementptr inbounds double, ptr %sma_buffer_ptr35, i64 8
  %sma_vload43 = load <4 x double>, ptr %sma_chunk_ptr42, align 32
  %sma_vsum44 = fadd <4 x double> %sma_vsum41, %sma_vload43
  %sma_chunk_ptr45 = getelementptr inbounds double, ptr %sma_buffer_ptr35, i64 12
  %sma_vload46 = load <4 x double>, ptr %sma_chunk_ptr45, align 32
  %sma_vsum47 = fadd <4 x double> %sma_vsum44, %sma_vload46
  %sma_chunk_ptr48 = getelementptr inbounds double, ptr %sma_buffer_ptr35, i64 16
  %sma_vload49 = load <4 x double>, ptr %sma_chunk_ptr48, align 32
  %sma_vsum50 = fadd <4 x double> %sma_vsum47, %sma_vload49
  %shift70 = shufflevector <4 x double> %sma_vsum50, <4 x double> poison, <4 x i32> <i32 1, i32 poison, i32 poison, i32 poison>
  %3 = fadd <4 x double> %sma_vsum50, %shift70
  %shift71 = shufflevector <4 x double> %sma_vsum50, <4 x double> poison, <4 x i32> <i32 2, i32 poison, i32 poison, i32 poison>
  %4 = fadd <4 x double> %shift71, %3
  %shift72 = shufflevector <4 x double> %sma_vsum50, <4 x double> poison, <4 x i32> <i32 3, i32 poison, i32 poison, i32 poison>
  %5 = fadd <4 x double> %shift72, %4
  %sma_lane_add58 = extractelement <4 x double> %5, i64 0
  %sma_simd_mean59 = fdiv double %sma_lane_add58, 2.000000e+01
  br label %sma_merge31

sma_merge31:                                      ; preds = %sma_merge, %sma_simd29
  %sma_out60 = phi double [ %sma_simd_mean59, %sma_simd29 ], [ 0x7FF8000000000000, %sma_merge ]
  %ema63 = call double @jit_rt_ema_alpha(ptr %ctx, i64 5, double %mid, double 1.250000e-01, i64 15)
  %sub = fsub double %sma_out60, %ema63
  %lag66 = call double @jit_rt_lag(ptr %ctx, i64 6, double %mid, i64 7)
  %add = fadd double %sub, %lag66
  %out_ptr67 = getelementptr double, ptr %outputs, i64 3
  store double %add, ptr %out_ptr67, align 8
  ret void
}

declare ptr @jit_rt_symbol_ctx(ptr, i32) local_unnamed_addr

declare double @jit_rt_ema_alpha(ptr, i64, double, double, i64) local_unnamed_addr

declare i1 @jit_rt_sma_prepare(ptr, i64, double, i64, ptr, ptr) local_unnamed_addr

declare double @jit_rt_lag(ptr, i64, double, i64) local_unnamed_addr
