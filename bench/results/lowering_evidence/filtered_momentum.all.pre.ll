; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr %market, ptr %arena, i32 %symbol_id, ptr %outputs) {
entry:
  %ctx = call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %0 = getelementptr inbounds { ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr }, ptr %ctx, i32 0, i32 3
  %rstd_base = load ptr, ptr %0, align 8
  %1 = getelementptr inbounds { ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr }, ptr %ctx, i32 0, i32 1
  %ema_base = load ptr, ptr %1, align 8
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
  %rstd_state_ptr = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 3
  %rstd_buf_pp = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_state_ptr, i32 0, i32 0
  %rstd_head_pp = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_state_ptr, i32 0, i32 2
  %rstd_count_pp = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_state_ptr, i32 0, i32 3
  %rstd_sum_pp = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_state_ptr, i32 0, i32 4
  %rstd_sumsq_pp = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_state_ptr, i32 0, i32 6
  %rstd_buf = load ptr, ptr %rstd_buf_pp, align 8
  %rstd_head = load i64, ptr %rstd_head_pp, align 8
  %rstd_slot = getelementptr inbounds double, ptr %rstd_buf, i64 %rstd_head
  %rstd_old = load double, ptr %rstd_slot, align 8
  %rstd_sum_ld = load x86_fp80, ptr %rstd_sum_pp, align 16
  %rstd_sumsq_ld = load x86_fp80, ptr %rstd_sumsq_pp, align 16
  %rstd_sum = fptrunc x86_fp80 %rstd_sum_ld to double
  %rstd_sumsq = fptrunc x86_fp80 %rstd_sumsq_ld to double
  %rstd_x2 = fmul double %mid17, %mid17
  %rstd_count = load i64, ptr %rstd_count_pp, align 8
  %rstd_was_full = icmp eq i64 %rstd_count, 30
  %rstd_old_or_zero = select i1 %rstd_was_full, double %rstd_old, double 0.000000e+00
  %rstd_old2 = fmul double %rstd_old_or_zero, %rstd_old_or_zero
  %rstd_sum_plus_x = fadd double %rstd_sum, %mid17
  %rstd_sum_new = fsub double %rstd_sum_plus_x, %rstd_old_or_zero
  %rstd_sumsq_plus_x2 = fadd double %rstd_sumsq, %rstd_x2
  %rstd_sumsq_new = fsub double %rstd_sumsq_plus_x2, %rstd_old2
  %rstd_count_plus = add i64 %rstd_count, 1
  %rstd_count_new = select i1 %rstd_was_full, i64 %rstd_count, i64 %rstd_count_plus
  store i64 %rstd_count_new, ptr %rstd_count_pp, align 8
  store double %mid17, ptr %rstd_slot, align 8
  %rstd_head_plus = add i64 %rstd_head, 1
  %rstd_head_new = urem i64 %rstd_head_plus, 30
  store i64 %rstd_head_new, ptr %rstd_head_pp, align 8
  %2 = fpext double %rstd_sum_new to x86_fp80
  store x86_fp80 %2, ptr %rstd_sum_pp, align 16
  %3 = fpext double %rstd_sumsq_new to x86_fp80
  store x86_fp80 %3, ptr %rstd_sumsq_pp, align 16
  %rstd_mean = fdiv double %rstd_sum_new, 3.000000e+01
  %rstd_mean_sq = fmul double %rstd_mean, %rstd_mean
  %rstd_corr = fmul double 3.000000e+01, %rstd_mean_sq
  %rstd_ss = fsub double %rstd_sumsq_new, %rstd_corr
  %rstd_var = fdiv double %rstd_ss, 2.900000e+01
  %4 = fcmp olt double %rstd_var, 0.000000e+00
  %rstd_var_clamped = select i1 %4, double 0.000000e+00, double %rstd_var
  %rstd_std = call double @llvm.sqrt.f64(double %rstd_var_clamped)
  %rstd_is_full = icmp eq i64 %rstd_count_new, 30
  %rstd_out = select i1 %rstd_is_full, double %rstd_std, double 0x7FF8000000000000
  %out_ptr18 = getelementptr double, ptr %outputs, i64 2
  store double %rstd_out, ptr %out_ptr18, align 8
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
  %rstd_state_ptr70 = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 8
  %rstd_buf_pp71 = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_state_ptr70, i32 0, i32 0
  %rstd_head_pp72 = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_state_ptr70, i32 0, i32 2
  %rstd_count_pp73 = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_state_ptr70, i32 0, i32 3
  %rstd_sum_pp74 = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_state_ptr70, i32 0, i32 4
  %rstd_sumsq_pp75 = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_state_ptr70, i32 0, i32 6
  %rstd_buf76 = load ptr, ptr %rstd_buf_pp71, align 8
  %rstd_head77 = load i64, ptr %rstd_head_pp72, align 8
  %rstd_slot78 = getelementptr inbounds double, ptr %rstd_buf76, i64 %rstd_head77
  %rstd_old79 = load double, ptr %rstd_slot78, align 8
  %rstd_sum_ld80 = load x86_fp80, ptr %rstd_sum_pp74, align 16
  %rstd_sumsq_ld81 = load x86_fp80, ptr %rstd_sumsq_pp75, align 16
  %rstd_sum82 = fptrunc x86_fp80 %rstd_sum_ld80 to double
  %rstd_sumsq83 = fptrunc x86_fp80 %rstd_sumsq_ld81 to double
  %rstd_x284 = fmul double %mid69, %mid69
  %rstd_count85 = load i64, ptr %rstd_count_pp73, align 8
  %rstd_was_full86 = icmp eq i64 %rstd_count85, 30
  %rstd_old_or_zero87 = select i1 %rstd_was_full86, double %rstd_old79, double 0.000000e+00
  %rstd_old288 = fmul double %rstd_old_or_zero87, %rstd_old_or_zero87
  %rstd_sum_plus_x89 = fadd double %rstd_sum82, %mid69
  %rstd_sum_new90 = fsub double %rstd_sum_plus_x89, %rstd_old_or_zero87
  %rstd_sumsq_plus_x291 = fadd double %rstd_sumsq83, %rstd_x284
  %rstd_sumsq_new92 = fsub double %rstd_sumsq_plus_x291, %rstd_old288
  %rstd_count_plus93 = add i64 %rstd_count85, 1
  %rstd_count_new94 = select i1 %rstd_was_full86, i64 %rstd_count85, i64 %rstd_count_plus93
  store i64 %rstd_count_new94, ptr %rstd_count_pp73, align 8
  store double %mid69, ptr %rstd_slot78, align 8
  %rstd_head_plus95 = add i64 %rstd_head77, 1
  %rstd_head_new96 = urem i64 %rstd_head_plus95, 30
  store i64 %rstd_head_new96, ptr %rstd_head_pp72, align 8
  %5 = fpext double %rstd_sum_new90 to x86_fp80
  store x86_fp80 %5, ptr %rstd_sum_pp74, align 16
  %6 = fpext double %rstd_sumsq_new92 to x86_fp80
  store x86_fp80 %6, ptr %rstd_sumsq_pp75, align 16
  %rstd_mean97 = fdiv double %rstd_sum_new90, 3.000000e+01
  %rstd_mean_sq98 = fmul double %rstd_mean97, %rstd_mean97
  %rstd_corr99 = fmul double 3.000000e+01, %rstd_mean_sq98
  %rstd_ss100 = fsub double %rstd_sumsq_new92, %rstd_corr99
  %rstd_var101 = fdiv double %rstd_ss100, 2.900000e+01
  %7 = fcmp olt double %rstd_var101, 0.000000e+00
  %rstd_var_clamped102 = select i1 %7, double 0.000000e+00, double %rstd_var101
  %rstd_std103 = call double @llvm.sqrt.f64(double %rstd_var_clamped102)
  %rstd_is_full104 = icmp eq i64 %rstd_count_new94, 30
  %rstd_out105 = select i1 %rstd_is_full104, double %rstd_std103, double 0x7FF8000000000000
  %gt106 = fcmp ogt double %rstd_out105, 0.000000e+00
  %gt_f107 = uitofp i1 %gt106 to double
  %and_l = fcmp une double %gt_f, 0.000000e+00
  %and_r = fcmp une double %gt_f107, 0.000000e+00
  %and = and i1 %and_l, %and_r
  %and_f = uitofp i1 %and to double
  %ifcond = fcmp une double %and_f, 0.000000e+00
  br i1 %ifcond, label %then, label %else

then:                                             ; preds = %entry
  %sub108 = fsub double %ema_new55, %ema_new67
  %div = fdiv double %sub108, %rstd_out105
  br label %ifcont

else:                                             ; preds = %entry
  br label %ifcont

ifcont:                                           ; preds = %else, %then
  %iftmp = phi double [ %div, %then ], [ 0.000000e+00, %else ]
  %out_ptr109 = getelementptr double, ptr %outputs, i64 4
  store double %iftmp, ptr %out_ptr109, align 8
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

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare double @llvm.sqrt.f64(double) #0

attributes #0 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }
