; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr %market, ptr %arena, i32 %symbol_id, ptr %outputs) {
entry:
  %sma_buffer_ptr_addr32 = alloca ptr, align 8
  %sma_size_addr33 = alloca i64, align 8
  %sma_buffer_ptr_addr = alloca ptr, align 8
  %sma_size_addr = alloca i64, align 8
  %ctx = call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %instruments_ptr = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr, i64 0, i64 0
  %bid_ptr = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr, i32 0, i32 0
  %bid = load double, ptr %bid_ptr, align 8
  %instruments_ptr1 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr2 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr1, i64 0, i64 0
  %ask_ptr = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr2, i32 0, i32 1
  %ask = load double, ptr %ask_ptr, align 8
  %mid_sum = fadd double %bid, %ask
  %mid = fmul double %mid_sum, 5.000000e-01
  %sma_full = call i1 @jit_rt_sma_prepare(ptr %ctx, i64 1, double %mid, i64 20, ptr %sma_buffer_ptr_addr, ptr %sma_size_addr)
  br i1 %sma_full, label %sma_simd, label %sma_nan

sma_simd:                                         ; preds = %entry
  %sma_buffer_ptr = load ptr, ptr %sma_buffer_ptr_addr, align 8
  %sma_chunk_ptr = getelementptr inbounds double, ptr %sma_buffer_ptr, i64 0
  %sma_vload = load <4 x double>, ptr %sma_chunk_ptr, align 32
  %sma_vsum = fadd <4 x double> zeroinitializer, %sma_vload
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
  %sma_lane = extractelement <4 x double> %sma_vsum14, i64 0
  %sma_lane_add = fadd double 0.000000e+00, %sma_lane
  %sma_lane15 = extractelement <4 x double> %sma_vsum14, i64 1
  %sma_lane_add16 = fadd double %sma_lane_add, %sma_lane15
  %sma_lane17 = extractelement <4 x double> %sma_vsum14, i64 2
  %sma_lane_add18 = fadd double %sma_lane_add16, %sma_lane17
  %sma_lane19 = extractelement <4 x double> %sma_vsum14, i64 3
  %sma_lane_add20 = fadd double %sma_lane_add18, %sma_lane19
  %sma_simd_mean = fdiv double %sma_lane_add20, 2.000000e+01
  br label %sma_merge

sma_nan:                                          ; preds = %entry
  br label %sma_merge

sma_merge:                                        ; preds = %sma_simd, %sma_nan
  %sma_out = phi double [ %sma_simd_mean, %sma_simd ], [ 0x7FF8000000000000, %sma_nan ]
  %out_ptr = getelementptr double, ptr %outputs, i64 0
  store double %sma_out, ptr %out_ptr, align 8
  %mid_sum21 = fadd double %bid, %ask
  %mid22 = fmul double %mid_sum21, 5.000000e-01
  %ema = call double @jit_rt_ema_alpha(ptr %ctx, i64 2, double %mid22, double 1.250000e-01, i64 15)
  %out_ptr23 = getelementptr double, ptr %outputs, i64 1
  store double %ema, ptr %out_ptr23, align 8
  %mid_sum24 = fadd double %bid, %ask
  %mid25 = fmul double %mid_sum24, 5.000000e-01
  %lag = call double @jit_rt_lag(ptr %ctx, i64 3, double %mid25, i64 7)
  %out_ptr26 = getelementptr double, ptr %outputs, i64 2
  store double %lag, ptr %out_ptr26, align 8
  %mid_sum27 = fadd double %bid, %ask
  %mid28 = fmul double %mid_sum27, 5.000000e-01
  %sma_full34 = call i1 @jit_rt_sma_prepare(ptr %ctx, i64 4, double %mid28, i64 20, ptr %sma_buffer_ptr_addr32, ptr %sma_size_addr33)
  br i1 %sma_full34, label %sma_simd29, label %sma_nan30

sma_simd29:                                       ; preds = %sma_merge
  %sma_buffer_ptr35 = load ptr, ptr %sma_buffer_ptr_addr32, align 8
  %sma_chunk_ptr36 = getelementptr inbounds double, ptr %sma_buffer_ptr35, i64 0
  %sma_vload37 = load <4 x double>, ptr %sma_chunk_ptr36, align 32
  %sma_vsum38 = fadd <4 x double> zeroinitializer, %sma_vload37
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
  %sma_lane51 = extractelement <4 x double> %sma_vsum50, i64 0
  %sma_lane_add52 = fadd double 0.000000e+00, %sma_lane51
  %sma_lane53 = extractelement <4 x double> %sma_vsum50, i64 1
  %sma_lane_add54 = fadd double %sma_lane_add52, %sma_lane53
  %sma_lane55 = extractelement <4 x double> %sma_vsum50, i64 2
  %sma_lane_add56 = fadd double %sma_lane_add54, %sma_lane55
  %sma_lane57 = extractelement <4 x double> %sma_vsum50, i64 3
  %sma_lane_add58 = fadd double %sma_lane_add56, %sma_lane57
  %sma_simd_mean59 = fdiv double %sma_lane_add58, 2.000000e+01
  br label %sma_merge31

sma_nan30:                                        ; preds = %sma_merge
  br label %sma_merge31

sma_merge31:                                      ; preds = %sma_simd29, %sma_nan30
  %sma_out60 = phi double [ %sma_simd_mean59, %sma_simd29 ], [ 0x7FF8000000000000, %sma_nan30 ]
  %mid_sum61 = fadd double %bid, %ask
  %mid62 = fmul double %mid_sum61, 5.000000e-01
  %ema63 = call double @jit_rt_ema_alpha(ptr %ctx, i64 5, double %mid62, double 1.250000e-01, i64 15)
  %sub = fsub double %sma_out60, %ema63
  %mid_sum64 = fadd double %bid, %ask
  %mid65 = fmul double %mid_sum64, 5.000000e-01
  %lag66 = call double @jit_rt_lag(ptr %ctx, i64 6, double %mid65, i64 7)
  %add = fadd double %sub, %lag66
  %out_ptr67 = getelementptr double, ptr %outputs, i64 3
  store double %add, ptr %out_ptr67, align 8
  ret void
}

declare ptr @jit_rt_symbol_ctx(ptr, i32)

declare double @jit_rt_mid(ptr, i64)

declare double @jit_rt_bid(ptr, i64)

declare double @jit_rt_ask(ptr, i64)

declare double @jit_rt_spread(ptr, i64)

declare double @jit_rt_ema(ptr, i64, double, i64)

declare double @jit_rt_ema_alpha(ptr, i64, double, double, i64)

declare double @jit_rt_sma(ptr, i64, double, i64)

declare i1 @jit_rt_sma_prepare(ptr, i64, double, i64, ptr, ptr)

declare double @jit_rt_rolling_std(ptr, i64, double, i64)

declare double @jit_rt_zscore(ptr, i64, double, i64)

declare double @jit_rt_rolling_min(ptr, i64, double, i64)

declare double @jit_rt_rolling_max(ptr, i64, double, i64)

declare double @jit_rt_vwap(ptr, ptr, i64, i64, i64)

declare double @jit_rt_lag(ptr, i64, double, i64)

declare double @jit_rt_cross_above(ptr, i64, double, double)

declare double @jit_rt_cross_below(ptr, i64, double, double)

declare ptr @jit_rt_sma_lowered_base(ptr)

declare ptr @jit_rt_ema_lowered_base(ptr)

declare ptr @jit_rt_lag_lowered_base(ptr)
