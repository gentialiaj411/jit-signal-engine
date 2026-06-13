; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr %market, ptr %arena, i32 %symbol_id, ptr %outputs) {
entry:
  %ctx = call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %instruments_ptr = getelementptr inbounds { [1024 x { double, double, double, double, i64, [24 x i8] }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %instruments_ptr, i64 0, i64 0
  %bid_ptr = getelementptr inbounds { double, double, double, double, i64, [24 x i8] }, ptr %instrument_ptr, i32 0, i32 0
  %bid = load double, ptr %bid_ptr, align 8
  %instruments_ptr1 = getelementptr inbounds { [1024 x { double, double, double, double, i64, [24 x i8] }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr2 = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %instruments_ptr1, i64 0, i64 0
  %ask_ptr = getelementptr inbounds { double, double, double, double, i64, [24 x i8] }, ptr %instrument_ptr2, i32 0, i32 1
  %ask = load double, ptr %ask_ptr, align 8
  %mid_sum = fadd double %bid, %ask
  %mid = fmul double %mid_sum, 5.000000e-01
  %ema = call double @jit_rt_ema_alpha(ptr %ctx, i64 1, double %mid, double 0x3FC745D1745D1746, i64 10)
  %out_ptr = getelementptr double, ptr %outputs, i64 0
  store double %ema, ptr %out_ptr, align 8
  %mid_sum3 = fadd double %bid, %ask
  %mid4 = fmul double %mid_sum3, 5.000000e-01
  %ema5 = call double @jit_rt_ema_alpha(ptr %ctx, i64 2, double %mid4, double 0x3FA0C9714FBCDA3B, i64 60)
  %out_ptr6 = getelementptr double, ptr %outputs, i64 1
  store double %ema5, ptr %out_ptr6, align 8
  %mid_sum7 = fadd double %bid, %ask
  %mid8 = fmul double %mid_sum7, 5.000000e-01
  %rstd = call double @jit_rt_rolling_std(ptr %ctx, i64 3, double %mid8, i64 30)
  %out_ptr9 = getelementptr double, ptr %outputs, i64 2
  store double %rstd, ptr %out_ptr9, align 8
  %mid_sum10 = fadd double %bid, %ask
  %mid11 = fmul double %mid_sum10, 5.000000e-01
  %ema12 = call double @jit_rt_ema_alpha(ptr %ctx, i64 4, double %mid11, double 0x3FC745D1745D1746, i64 10)
  %mid_sum13 = fadd double %bid, %ask
  %mid14 = fmul double %mid_sum13, 5.000000e-01
  %ema15 = call double @jit_rt_ema_alpha(ptr %ctx, i64 5, double %mid14, double 0x3FA0C9714FBCDA3B, i64 60)
  %sub = fsub double %ema12, %ema15
  %out_ptr16 = getelementptr double, ptr %outputs, i64 3
  store double %sub, ptr %out_ptr16, align 8
  %mid_sum17 = fadd double %bid, %ask
  %mid18 = fmul double %mid_sum17, 5.000000e-01
  %ema19 = call double @jit_rt_ema_alpha(ptr %ctx, i64 6, double %mid18, double 0x3FC745D1745D1746, i64 10)
  %mid_sum20 = fadd double %bid, %ask
  %mid21 = fmul double %mid_sum20, 5.000000e-01
  %ema22 = call double @jit_rt_ema_alpha(ptr %ctx, i64 7, double %mid21, double 0x3FA0C9714FBCDA3B, i64 60)
  %gt = fcmp ogt double %ema19, %ema22
  %gt_f = uitofp i1 %gt to double
  %mid_sum23 = fadd double %bid, %ask
  %mid24 = fmul double %mid_sum23, 5.000000e-01
  %rstd25 = call double @jit_rt_rolling_std(ptr %ctx, i64 8, double %mid24, i64 30)
  %gt26 = fcmp ogt double %rstd25, 0.000000e+00
  %gt_f27 = uitofp i1 %gt26 to double
  %and_l = fcmp une double %gt_f, 0.000000e+00
  %and_r = fcmp une double %gt_f27, 0.000000e+00
  %and = and i1 %and_l, %and_r
  %and_f = uitofp i1 %and to double
  %ifcond = fcmp une double %and_f, 0.000000e+00
  br i1 %ifcond, label %then, label %else

then:                                             ; preds = %entry
  %sub28 = fsub double %ema19, %ema22
  %div = fdiv double %sub28, %rstd25
  br label %ifcont

else:                                             ; preds = %entry
  br label %ifcont

ifcont:                                           ; preds = %else, %then
  %iftmp = phi double [ %div, %then ], [ 0.000000e+00, %else ]
  %out_ptr29 = getelementptr double, ptr %outputs, i64 4
  store double %iftmp, ptr %out_ptr29, align 8
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

declare double @jit_rt_rolling_corr(ptr, i64, double, double, i64)

declare double @jit_rt_rolling_beta(ptr, i64, double, double, i64)

declare double @jit_rt_kalman1d(ptr, i64, double, double, double)
