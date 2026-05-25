; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr %market, ptr %arena, i32 %symbol_id, ptr %outputs) {
entry:
  %ctx = call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %ema_base = call ptr @jit_rt_ema_lowered_base(ptr %ctx)
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
  %ema_state_ptr = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 1
  %ema_val_pp = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr, i32 0, i32 0
  %ema_init_pp = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr, i32 0, i32 1
  %ema_prev = load double, ptr %ema_val_pp, align 8
  %ema_ax = fmul double 0x3FC745D1745D1746, %mid
  %ema_bp = fmul double 0x3FEA2E8BA2E8BA2E, %ema_prev
  %ema_blended = fadd double %ema_ax, %ema_bp
  %ema_init = load i64, ptr %ema_init_pp, align 8
  %ema_is_init = icmp ne i64 %ema_init, 0
  %ema_new = select i1 %ema_is_init, double %ema_blended, double %mid
  store double %ema_new, ptr %ema_val_pp, align 8
  store i64 1, ptr %ema_init_pp, align 8
  %out_ptr = getelementptr double, ptr %outputs, i64 0
  store double %ema_new, ptr %out_ptr, align 8
  %mid_sum3 = fadd double %bid, %ask
  %mid4 = fmul double %mid_sum3, 5.000000e-01
  %ema_state_ptr5 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 2
  %ema_val_pp6 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr5, i32 0, i32 0
  %ema_init_pp7 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr5, i32 0, i32 1
  %ema_prev8 = load double, ptr %ema_val_pp6, align 8
  %ema_ax9 = fmul double 0x3FA0C9714FBCDA3B, %mid4
  %ema_bp10 = fmul double 0x3FEEF368EB04325C, %ema_prev8
  %ema_blended11 = fadd double %ema_ax9, %ema_bp10
  %ema_init12 = load i64, ptr %ema_init_pp7, align 8
  %ema_is_init13 = icmp ne i64 %ema_init12, 0
  %ema_new14 = select i1 %ema_is_init13, double %ema_blended11, double %mid4
  store double %ema_new14, ptr %ema_val_pp6, align 8
  store i64 1, ptr %ema_init_pp7, align 8
  %out_ptr15 = getelementptr double, ptr %outputs, i64 1
  store double %ema_new14, ptr %out_ptr15, align 8
  %mid_sum16 = fadd double %bid, %ask
  %mid17 = fmul double %mid_sum16, 5.000000e-01
  %rstd = call double @jit_rt_rolling_std(ptr %ctx, i64 3, double %mid17, i64 30)
  %out_ptr18 = getelementptr double, ptr %outputs, i64 2
  store double %rstd, ptr %out_ptr18, align 8
  %mid_sum19 = fadd double %bid, %ask
  %mid20 = fmul double %mid_sum19, 5.000000e-01
  %ema_state_ptr21 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 4
  %ema_val_pp22 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr21, i32 0, i32 0
  %ema_init_pp23 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr21, i32 0, i32 1
  %ema_prev24 = load double, ptr %ema_val_pp22, align 8
  %ema_ax25 = fmul double 0x3FC745D1745D1746, %mid20
  %ema_bp26 = fmul double 0x3FEA2E8BA2E8BA2E, %ema_prev24
  %ema_blended27 = fadd double %ema_ax25, %ema_bp26
  %ema_init28 = load i64, ptr %ema_init_pp23, align 8
  %ema_is_init29 = icmp ne i64 %ema_init28, 0
  %ema_new30 = select i1 %ema_is_init29, double %ema_blended27, double %mid20
  store double %ema_new30, ptr %ema_val_pp22, align 8
  store i64 1, ptr %ema_init_pp23, align 8
  %mid_sum31 = fadd double %bid, %ask
  %mid32 = fmul double %mid_sum31, 5.000000e-01
  %ema_state_ptr33 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 5
  %ema_val_pp34 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr33, i32 0, i32 0
  %ema_init_pp35 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr33, i32 0, i32 1
  %ema_prev36 = load double, ptr %ema_val_pp34, align 8
  %ema_ax37 = fmul double 0x3FA0C9714FBCDA3B, %mid32
  %ema_bp38 = fmul double 0x3FEEF368EB04325C, %ema_prev36
  %ema_blended39 = fadd double %ema_ax37, %ema_bp38
  %ema_init40 = load i64, ptr %ema_init_pp35, align 8
  %ema_is_init41 = icmp ne i64 %ema_init40, 0
  %ema_new42 = select i1 %ema_is_init41, double %ema_blended39, double %mid32
  store double %ema_new42, ptr %ema_val_pp34, align 8
  store i64 1, ptr %ema_init_pp35, align 8
  %sub = fsub double %ema_new30, %ema_new42
  %out_ptr43 = getelementptr double, ptr %outputs, i64 3
  store double %sub, ptr %out_ptr43, align 8
  %mid_sum44 = fadd double %bid, %ask
  %mid45 = fmul double %mid_sum44, 5.000000e-01
  %ema_state_ptr46 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 6
  %ema_val_pp47 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr46, i32 0, i32 0
  %ema_init_pp48 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr46, i32 0, i32 1
  %ema_prev49 = load double, ptr %ema_val_pp47, align 8
  %ema_ax50 = fmul double 0x3FC745D1745D1746, %mid45
  %ema_bp51 = fmul double 0x3FEA2E8BA2E8BA2E, %ema_prev49
  %ema_blended52 = fadd double %ema_ax50, %ema_bp51
  %ema_init53 = load i64, ptr %ema_init_pp48, align 8
  %ema_is_init54 = icmp ne i64 %ema_init53, 0
  %ema_new55 = select i1 %ema_is_init54, double %ema_blended52, double %mid45
  store double %ema_new55, ptr %ema_val_pp47, align 8
  store i64 1, ptr %ema_init_pp48, align 8
  %mid_sum56 = fadd double %bid, %ask
  %mid57 = fmul double %mid_sum56, 5.000000e-01
  %ema_state_ptr58 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 7
  %ema_val_pp59 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr58, i32 0, i32 0
  %ema_init_pp60 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr58, i32 0, i32 1
  %ema_prev61 = load double, ptr %ema_val_pp59, align 8
  %ema_ax62 = fmul double 0x3FA0C9714FBCDA3B, %mid57
  %ema_bp63 = fmul double 0x3FEEF368EB04325C, %ema_prev61
  %ema_blended64 = fadd double %ema_ax62, %ema_bp63
  %ema_init65 = load i64, ptr %ema_init_pp60, align 8
  %ema_is_init66 = icmp ne i64 %ema_init65, 0
  %ema_new67 = select i1 %ema_is_init66, double %ema_blended64, double %mid57
  store double %ema_new67, ptr %ema_val_pp59, align 8
  store i64 1, ptr %ema_init_pp60, align 8
  %gt = fcmp ogt double %ema_new55, %ema_new67
  %gt_f = uitofp i1 %gt to double
  %mid_sum68 = fadd double %bid, %ask
  %mid69 = fmul double %mid_sum68, 5.000000e-01
  %rstd70 = call double @jit_rt_rolling_std(ptr %ctx, i64 8, double %mid69, i64 30)
  %gt71 = fcmp ogt double %rstd70, 0.000000e+00
  %gt_f72 = uitofp i1 %gt71 to double
  %and_l = fcmp une double %gt_f, 0.000000e+00
  %and_r = fcmp une double %gt_f72, 0.000000e+00
  %and = and i1 %and_l, %and_r
  %and_f = uitofp i1 %and to double
  %ifcond = fcmp une double %and_f, 0.000000e+00
  br i1 %ifcond, label %then, label %else

then:                                             ; preds = %entry
  %mid_sum73 = fadd double %bid, %ask
  %mid74 = fmul double %mid_sum73, 5.000000e-01
  %ema_state_ptr75 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 9
  %ema_val_pp76 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr75, i32 0, i32 0
  %ema_init_pp77 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr75, i32 0, i32 1
  %ema_prev78 = load double, ptr %ema_val_pp76, align 8
  %ema_ax79 = fmul double 0x3FC745D1745D1746, %mid74
  %ema_bp80 = fmul double 0x3FEA2E8BA2E8BA2E, %ema_prev78
  %ema_blended81 = fadd double %ema_ax79, %ema_bp80
  %ema_init82 = load i64, ptr %ema_init_pp77, align 8
  %ema_is_init83 = icmp ne i64 %ema_init82, 0
  %ema_new84 = select i1 %ema_is_init83, double %ema_blended81, double %mid74
  store double %ema_new84, ptr %ema_val_pp76, align 8
  store i64 1, ptr %ema_init_pp77, align 8
  %mid_sum85 = fadd double %bid, %ask
  %mid86 = fmul double %mid_sum85, 5.000000e-01
  %ema_state_ptr87 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 10
  %ema_val_pp88 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr87, i32 0, i32 0
  %ema_init_pp89 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr87, i32 0, i32 1
  %ema_prev90 = load double, ptr %ema_val_pp88, align 8
  %ema_ax91 = fmul double 0x3FA0C9714FBCDA3B, %mid86
  %ema_bp92 = fmul double 0x3FEEF368EB04325C, %ema_prev90
  %ema_blended93 = fadd double %ema_ax91, %ema_bp92
  %ema_init94 = load i64, ptr %ema_init_pp89, align 8
  %ema_is_init95 = icmp ne i64 %ema_init94, 0
  %ema_new96 = select i1 %ema_is_init95, double %ema_blended93, double %mid86
  store double %ema_new96, ptr %ema_val_pp88, align 8
  store i64 1, ptr %ema_init_pp89, align 8
  %sub97 = fsub double %ema_new84, %ema_new96
  %mid_sum98 = fadd double %bid, %ask
  %mid99 = fmul double %mid_sum98, 5.000000e-01
  %rstd100 = call double @jit_rt_rolling_std(ptr %ctx, i64 11, double %mid99, i64 30)
  %div = fdiv double %sub97, %rstd100
  br label %ifcont

else:                                             ; preds = %entry
  br label %ifcont

ifcont:                                           ; preds = %else, %then
  %iftmp = phi double [ %div, %then ], [ 0.000000e+00, %else ]
  %out_ptr101 = getelementptr double, ptr %outputs, i64 4
  store double %iftmp, ptr %out_ptr101, align 8
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
