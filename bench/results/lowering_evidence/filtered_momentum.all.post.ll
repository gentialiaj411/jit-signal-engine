; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr nocapture readonly %market, ptr %arena, i32 %symbol_id, ptr nocapture writeonly %outputs) local_unnamed_addr {
entry:
  %ctx = tail call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %ema_base = tail call ptr @jit_rt_ema_lowered_base(ptr %ctx)
  %bid = load double, ptr %market, align 8
  %ask_ptr = getelementptr inbounds { double, double, double, double, i64 }, ptr %market, i64 0, i32 1
  %ask = load double, ptr %ask_ptr, align 8
  %mid_sum = fadd double %bid, %ask
  %mid = fmul double %mid_sum, 5.000000e-01
  %ema_state_ptr = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 1
  %ema_init_pp = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 1, i32 1
  %ema_prev = load double, ptr %ema_state_ptr, align 8
  %ema_ax = fmul double %mid, 0x3FC745D1745D1746
  %ema_bp = fmul double %ema_prev, 0x3FEA2E8BA2E8BA2E
  %ema_blended = fadd double %ema_bp, %ema_ax
  %ema_init = load i64, ptr %ema_init_pp, align 8
  %ema_is_init.not = icmp eq i64 %ema_init, 0
  %ema_new = select i1 %ema_is_init.not, double %mid, double %ema_blended
  store double %ema_new, ptr %ema_state_ptr, align 8
  store i64 1, ptr %ema_init_pp, align 8
  store double %ema_new, ptr %outputs, align 8
  %ema_state_ptr5 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 2
  %ema_init_pp7 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 2, i32 1
  %ema_prev8 = load double, ptr %ema_state_ptr5, align 8
  %ema_ax9 = fmul double %mid, 0x3FA0C9714FBCDA3B
  %ema_bp10 = fmul double %ema_prev8, 0x3FEEF368EB04325C
  %ema_blended11 = fadd double %ema_ax9, %ema_bp10
  %ema_init12 = load i64, ptr %ema_init_pp7, align 8
  %ema_is_init13.not = icmp eq i64 %ema_init12, 0
  %ema_new14 = select i1 %ema_is_init13.not, double %mid, double %ema_blended11
  store double %ema_new14, ptr %ema_state_ptr5, align 8
  store i64 1, ptr %ema_init_pp7, align 8
  %out_ptr15 = getelementptr double, ptr %outputs, i64 1
  store double %ema_new14, ptr %out_ptr15, align 8
  %rstd = tail call double @jit_rt_rolling_std(ptr %ctx, i64 3, double %mid, i64 30)
  %out_ptr18 = getelementptr double, ptr %outputs, i64 2
  store double %rstd, ptr %out_ptr18, align 8
  %ema_state_ptr21 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 4
  %ema_init_pp23 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 4, i32 1
  %ema_prev24 = load double, ptr %ema_state_ptr21, align 8
  %ema_bp26 = fmul double %ema_prev24, 0x3FEA2E8BA2E8BA2E
  %ema_blended27 = fadd double %ema_ax, %ema_bp26
  %ema_init28 = load i64, ptr %ema_init_pp23, align 8
  %ema_is_init29.not = icmp eq i64 %ema_init28, 0
  %ema_new30 = select i1 %ema_is_init29.not, double %mid, double %ema_blended27
  store double %ema_new30, ptr %ema_state_ptr21, align 8
  store i64 1, ptr %ema_init_pp23, align 8
  %ema_state_ptr33 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 5
  %ema_init_pp35 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 5, i32 1
  %ema_prev36 = load double, ptr %ema_state_ptr33, align 8
  %ema_bp38 = fmul double %ema_prev36, 0x3FEEF368EB04325C
  %ema_blended39 = fadd double %ema_ax9, %ema_bp38
  %ema_init40 = load i64, ptr %ema_init_pp35, align 8
  %ema_is_init41.not = icmp eq i64 %ema_init40, 0
  %ema_new42 = select i1 %ema_is_init41.not, double %mid, double %ema_blended39
  store double %ema_new42, ptr %ema_state_ptr33, align 8
  store i64 1, ptr %ema_init_pp35, align 8
  %sub = fsub double %ema_new30, %ema_new42
  %out_ptr43 = getelementptr double, ptr %outputs, i64 3
  store double %sub, ptr %out_ptr43, align 8
  %ema_state_ptr46 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 6
  %ema_init_pp48 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 6, i32 1
  %ema_prev49 = load double, ptr %ema_state_ptr46, align 8
  %ema_bp51 = fmul double %ema_prev49, 0x3FEA2E8BA2E8BA2E
  %ema_blended52 = fadd double %ema_ax, %ema_bp51
  %ema_init53 = load i64, ptr %ema_init_pp48, align 8
  %ema_is_init54.not = icmp eq i64 %ema_init53, 0
  %ema_new55 = select i1 %ema_is_init54.not, double %mid, double %ema_blended52
  store double %ema_new55, ptr %ema_state_ptr46, align 8
  store i64 1, ptr %ema_init_pp48, align 8
  %ema_state_ptr58 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 7
  %ema_init_pp60 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 7, i32 1
  %ema_prev61 = load double, ptr %ema_state_ptr58, align 8
  %ema_bp63 = fmul double %ema_prev61, 0x3FEEF368EB04325C
  %ema_blended64 = fadd double %ema_ax9, %ema_bp63
  %ema_init65 = load i64, ptr %ema_init_pp60, align 8
  %ema_is_init66.not = icmp eq i64 %ema_init65, 0
  %ema_new67 = select i1 %ema_is_init66.not, double %mid, double %ema_blended64
  store double %ema_new67, ptr %ema_state_ptr58, align 8
  store i64 1, ptr %ema_init_pp60, align 8
  %gt = fcmp ogt double %ema_new55, %ema_new67
  %rstd70 = tail call double @jit_rt_rolling_std(ptr %ctx, i64 8, double %mid, i64 30)
  %gt71 = fcmp ogt double %rstd70, 0.000000e+00
  %and = and i1 %gt, %gt71
  br i1 %and, label %then, label %ifcont

then:                                             ; preds = %entry
  %ema_state_ptr75 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 9
  %ema_init_pp77 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 9, i32 1
  %ema_prev78 = load double, ptr %ema_state_ptr75, align 8
  %ema_bp80 = fmul double %ema_prev78, 0x3FEA2E8BA2E8BA2E
  %ema_blended81 = fadd double %ema_ax, %ema_bp80
  %ema_init82 = load i64, ptr %ema_init_pp77, align 8
  %ema_is_init83.not = icmp eq i64 %ema_init82, 0
  %ema_new84 = select i1 %ema_is_init83.not, double %mid, double %ema_blended81
  store double %ema_new84, ptr %ema_state_ptr75, align 8
  store i64 1, ptr %ema_init_pp77, align 8
  %ema_state_ptr87 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 10
  %ema_init_pp89 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 10, i32 1
  %ema_prev90 = load double, ptr %ema_state_ptr87, align 8
  %ema_bp92 = fmul double %ema_prev90, 0x3FEEF368EB04325C
  %ema_blended93 = fadd double %ema_ax9, %ema_bp92
  %ema_init94 = load i64, ptr %ema_init_pp89, align 8
  %ema_is_init95.not = icmp eq i64 %ema_init94, 0
  %ema_new96 = select i1 %ema_is_init95.not, double %mid, double %ema_blended93
  store double %ema_new96, ptr %ema_state_ptr87, align 8
  store i64 1, ptr %ema_init_pp89, align 8
  %sub97 = fsub double %ema_new84, %ema_new96
  %rstd100 = tail call double @jit_rt_rolling_std(ptr %ctx, i64 11, double %mid, i64 30)
  %div = fdiv double %sub97, %rstd100
  br label %ifcont

ifcont:                                           ; preds = %entry, %then
  %iftmp = phi double [ %div, %then ], [ 0.000000e+00, %entry ]
  %out_ptr101 = getelementptr double, ptr %outputs, i64 4
  store double %iftmp, ptr %out_ptr101, align 8
  ret void
}

declare ptr @jit_rt_symbol_ctx(ptr, i32) local_unnamed_addr

declare double @jit_rt_rolling_std(ptr, i64, double, i64) local_unnamed_addr

declare ptr @jit_rt_ema_lowered_base(ptr) local_unnamed_addr
