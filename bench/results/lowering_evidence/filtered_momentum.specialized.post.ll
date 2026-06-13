; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_warm_func_1(ptr nocapture readonly %market, ptr %arena, i32 %symbol_id, ptr nocapture writeonly %outputs) local_unnamed_addr {
entry:
  %ctx = tail call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %0 = getelementptr inbounds { ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr }, ptr %ctx, i64 0, i32 3
  %rstd_base = load ptr, ptr %0, align 8
  %1 = getelementptr inbounds { ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr }, ptr %ctx, i64 0, i32 1
  %ema_base = load ptr, ptr %1, align 8
  %bid = load double, ptr %market, align 8
  %ask_ptr = getelementptr inbounds { double, double, double, double, i64, [24 x i8] }, ptr %market, i64 0, i32 1
  %ask = load double, ptr %ask_ptr, align 8
  %mid_sum = fadd double %bid, %ask
  %mid = fmul double %mid_sum, 5.000000e-01
  %ema_state_ptr = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 1
  %ema_prev = load double, ptr %ema_state_ptr, align 8
  %ema_ax = fmul double %mid, 0x3FC745D1745D1746
  %ema_bp = fmul double %ema_prev, 0x3FEA2E8BA2E8BA2E
  %ema_blended = fadd double %ema_bp, %ema_ax
  store double %ema_blended, ptr %ema_state_ptr, align 8
  store double %ema_blended, ptr %outputs, align 8
  %ema_state_ptr5 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 2
  %ema_prev8 = load double, ptr %ema_state_ptr5, align 8
  %ema_ax9 = fmul double %mid, 0x3FA0C9714FBCDA3B
  %ema_bp10 = fmul double %ema_prev8, 0x3FEEF368EB04325C
  %ema_blended11 = fadd double %ema_ax9, %ema_bp10
  store double %ema_blended11, ptr %ema_state_ptr5, align 8
  %out_ptr12 = getelementptr double, ptr %outputs, i64 1
  store double %ema_blended11, ptr %out_ptr12, align 8
  %rstd_state_ptr = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 3
  %rstd_head_pp = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 3, i32 2
  %rstd_count_pp = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 3, i32 3
  %rstd_sum_pp = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 3, i32 4
  %rstd_sumsq_pp = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 3, i32 6
  %rstd_buf = load ptr, ptr %rstd_state_ptr, align 8
  %rstd_head = load i64, ptr %rstd_head_pp, align 8
  %rstd_slot = getelementptr inbounds double, ptr %rstd_buf, i64 %rstd_head
  %rstd_old = load double, ptr %rstd_slot, align 8
  %rstd_sum_ld = load x86_fp80, ptr %rstd_sum_pp, align 16
  %rstd_sumsq_ld = load x86_fp80, ptr %rstd_sumsq_pp, align 16
  %rstd_sum = fptrunc x86_fp80 %rstd_sum_ld to double
  %rstd_sumsq = fptrunc x86_fp80 %rstd_sumsq_ld to double
  %rstd_x2 = fmul double %mid, %mid
  %rstd_count = load i64, ptr %rstd_count_pp, align 8
  %rstd_was_full = icmp eq i64 %rstd_count, 30
  %rstd_old_or_zero = select i1 %rstd_was_full, double %rstd_old, double 0.000000e+00
  %rstd_old2 = fmul double %rstd_old_or_zero, %rstd_old_or_zero
  %rstd_sum_plus_x = fadd double %mid, %rstd_sum
  %rstd_sum_new = fsub double %rstd_sum_plus_x, %rstd_old_or_zero
  %rstd_sumsq_plus_x2 = fadd double %rstd_x2, %rstd_sumsq
  %rstd_sumsq_new = fsub double %rstd_sumsq_plus_x2, %rstd_old2
  %rstd_count_plus = add i64 %rstd_count, 1
  %rstd_count_new = select i1 %rstd_was_full, i64 30, i64 %rstd_count_plus
  store i64 %rstd_count_new, ptr %rstd_count_pp, align 8
  store double %mid, ptr %rstd_slot, align 8
  %rstd_head_plus = add i64 %rstd_head, 1
  %rstd_head_new = urem i64 %rstd_head_plus, 30
  store i64 %rstd_head_new, ptr %rstd_head_pp, align 8
  %2 = fpext double %rstd_sum_new to x86_fp80
  store x86_fp80 %2, ptr %rstd_sum_pp, align 16
  %3 = fpext double %rstd_sumsq_new to x86_fp80
  store x86_fp80 %3, ptr %rstd_sumsq_pp, align 16
  %rstd_mean = fdiv double %rstd_sum_new, 3.000000e+01
  %rstd_mean_sq = fmul double %rstd_mean, %rstd_mean
  %rstd_corr = fmul double %rstd_mean_sq, 3.000000e+01
  %rstd_ss = fsub double %rstd_sumsq_new, %rstd_corr
  %rstd_var = fdiv double %rstd_ss, 2.900000e+01
  %4 = fcmp olt double %rstd_var, 0.000000e+00
  %rstd_var_clamped = select i1 %4, double 0.000000e+00, double %rstd_var
  %rstd_std = tail call double @llvm.sqrt.f64(double %rstd_var_clamped)
  %rstd_is_full = icmp eq i64 %rstd_count_new, 30
  %rstd_out = select i1 %rstd_is_full, double %rstd_std, double 0x7FF8000000000000
  %out_ptr15 = getelementptr double, ptr %outputs, i64 2
  store double %rstd_out, ptr %out_ptr15, align 8
  %ema_state_ptr18 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 4
  %ema_prev21 = load double, ptr %ema_state_ptr18, align 8
  %ema_bp23 = fmul double %ema_prev21, 0x3FEA2E8BA2E8BA2E
  %ema_blended24 = fadd double %ema_ax, %ema_bp23
  store double %ema_blended24, ptr %ema_state_ptr18, align 8
  %ema_state_ptr27 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 5
  %ema_prev30 = load double, ptr %ema_state_ptr27, align 8
  %ema_bp32 = fmul double %ema_prev30, 0x3FEEF368EB04325C
  %ema_blended33 = fadd double %ema_ax9, %ema_bp32
  store double %ema_blended33, ptr %ema_state_ptr27, align 8
  %sub = fsub double %ema_blended24, %ema_blended33
  %out_ptr34 = getelementptr double, ptr %outputs, i64 3
  store double %sub, ptr %out_ptr34, align 8
  %ema_state_ptr37 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 6
  %ema_prev40 = load double, ptr %ema_state_ptr37, align 8
  %ema_bp42 = fmul double %ema_prev40, 0x3FEA2E8BA2E8BA2E
  %ema_blended43 = fadd double %ema_ax, %ema_bp42
  store double %ema_blended43, ptr %ema_state_ptr37, align 8
  %ema_state_ptr46 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 7
  %ema_prev49 = load double, ptr %ema_state_ptr46, align 8
  %ema_bp51 = fmul double %ema_prev49, 0x3FEEF368EB04325C
  %ema_blended52 = fadd double %ema_ax9, %ema_bp51
  store double %ema_blended52, ptr %ema_state_ptr46, align 8
  %gt = fcmp ogt double %ema_blended43, %ema_blended52
  %rstd_state_ptr55 = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 8
  %rstd_head_pp57 = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 8, i32 2
  %rstd_count_pp58 = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 8, i32 3
  %rstd_sum_pp59 = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 8, i32 4
  %rstd_sumsq_pp60 = getelementptr inbounds { ptr, i64, i64, i64, x86_fp80, x86_fp80, x86_fp80, i64 }, ptr %rstd_base, i64 8, i32 6
  %rstd_buf61 = load ptr, ptr %rstd_state_ptr55, align 8
  %rstd_head62 = load i64, ptr %rstd_head_pp57, align 8
  %rstd_slot63 = getelementptr inbounds double, ptr %rstd_buf61, i64 %rstd_head62
  %rstd_old64 = load double, ptr %rstd_slot63, align 8
  %rstd_sum_ld65 = load x86_fp80, ptr %rstd_sum_pp59, align 16
  %rstd_sumsq_ld66 = load x86_fp80, ptr %rstd_sumsq_pp60, align 16
  %rstd_sum67 = fptrunc x86_fp80 %rstd_sum_ld65 to double
  %rstd_sumsq68 = fptrunc x86_fp80 %rstd_sumsq_ld66 to double
  %rstd_count70 = load i64, ptr %rstd_count_pp58, align 8
  %rstd_was_full71 = icmp eq i64 %rstd_count70, 30
  %rstd_old_or_zero72 = select i1 %rstd_was_full71, double %rstd_old64, double 0.000000e+00
  %rstd_old273 = fmul double %rstd_old_or_zero72, %rstd_old_or_zero72
  %rstd_sum_plus_x74 = fadd double %mid, %rstd_sum67
  %rstd_sum_new75 = fsub double %rstd_sum_plus_x74, %rstd_old_or_zero72
  %rstd_sumsq_plus_x276 = fadd double %rstd_x2, %rstd_sumsq68
  %rstd_sumsq_new77 = fsub double %rstd_sumsq_plus_x276, %rstd_old273
  %rstd_count_plus78 = add i64 %rstd_count70, 1
  %rstd_count_new79 = select i1 %rstd_was_full71, i64 30, i64 %rstd_count_plus78
  store i64 %rstd_count_new79, ptr %rstd_count_pp58, align 8
  store double %mid, ptr %rstd_slot63, align 8
  %rstd_head_plus80 = add i64 %rstd_head62, 1
  %rstd_head_new81 = urem i64 %rstd_head_plus80, 30
  store i64 %rstd_head_new81, ptr %rstd_head_pp57, align 8
  %5 = fpext double %rstd_sum_new75 to x86_fp80
  store x86_fp80 %5, ptr %rstd_sum_pp59, align 16
  %6 = fpext double %rstd_sumsq_new77 to x86_fp80
  store x86_fp80 %6, ptr %rstd_sumsq_pp60, align 16
  %rstd_mean82 = fdiv double %rstd_sum_new75, 3.000000e+01
  %rstd_mean_sq83 = fmul double %rstd_mean82, %rstd_mean82
  %rstd_corr84 = fmul double %rstd_mean_sq83, 3.000000e+01
  %rstd_ss85 = fsub double %rstd_sumsq_new77, %rstd_corr84
  %rstd_var86 = fdiv double %rstd_ss85, 2.900000e+01
  %7 = fcmp olt double %rstd_var86, 0.000000e+00
  %rstd_var_clamped87 = select i1 %7, double 0.000000e+00, double %rstd_var86
  %rstd_std88 = tail call double @llvm.sqrt.f64(double %rstd_var_clamped87)
  %rstd_is_full89 = icmp eq i64 %rstd_count_new79, 30
  %rstd_out90 = select i1 %rstd_is_full89, double %rstd_std88, double 0x7FF8000000000000
  %gt91 = fcmp ogt double %rstd_out90, 0.000000e+00
  %and = and i1 %gt, %gt91
  br i1 %and, label %then, label %ifcont

then:                                             ; preds = %entry
  %sub93 = fsub double %ema_blended43, %ema_blended52
  %div = fdiv double %sub93, %rstd_out90
  br label %ifcont

ifcont:                                           ; preds = %entry, %then
  %iftmp = phi double [ %div, %then ], [ 0.000000e+00, %entry ]
  %out_ptr94 = getelementptr double, ptr %outputs, i64 4
  store double %iftmp, ptr %out_ptr94, align 8
  ret void
}

declare ptr @jit_rt_symbol_ctx(ptr, i32) local_unnamed_addr

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare double @llvm.sqrt.f64(double) #0

attributes #0 = { mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none) }
