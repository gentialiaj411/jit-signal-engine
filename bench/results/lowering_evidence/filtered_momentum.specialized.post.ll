; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_warm_func_1(ptr nocapture readonly %market, ptr %arena, i32 %symbol_id, ptr nocapture writeonly %outputs) local_unnamed_addr {
entry:
  %ctx = tail call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %ema_base = tail call ptr @jit_rt_ema_lowered_base(ptr %ctx)
  %bid = load double, ptr %market, align 8
  %ask_ptr = getelementptr inbounds { double, double, double, double, i64 }, ptr %market, i64 0, i32 1
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
  %rstd = tail call double @jit_rt_rolling_std(ptr %ctx, i64 3, double %mid, i64 30)
  %out_ptr15 = getelementptr double, ptr %outputs, i64 2
  store double %rstd, ptr %out_ptr15, align 8
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
  %rstd55 = tail call double @jit_rt_rolling_std(ptr %ctx, i64 8, double %mid, i64 30)
  %gt56 = fcmp ogt double %rstd55, 0.000000e+00
  %and = and i1 %gt, %gt56
  br i1 %and, label %then, label %ifcont

then:                                             ; preds = %entry
  %ema_state_ptr60 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 9
  %ema_init_pp62 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 9, i32 1
  %ema_prev63 = load double, ptr %ema_state_ptr60, align 8
  %ema_bp65 = fmul double %ema_prev63, 0x3FEA2E8BA2E8BA2E
  %ema_blended66 = fadd double %ema_ax, %ema_bp65
  %ema_init = load i64, ptr %ema_init_pp62, align 8
  %ema_is_init.not = icmp eq i64 %ema_init, 0
  %ema_new = select i1 %ema_is_init.not, double %mid, double %ema_blended66
  store double %ema_new, ptr %ema_state_ptr60, align 8
  store i64 1, ptr %ema_init_pp62, align 8
  %ema_state_ptr69 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 10
  %ema_init_pp71 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 10, i32 1
  %ema_prev72 = load double, ptr %ema_state_ptr69, align 8
  %ema_bp74 = fmul double %ema_prev72, 0x3FEEF368EB04325C
  %ema_blended75 = fadd double %ema_ax9, %ema_bp74
  %ema_init76 = load i64, ptr %ema_init_pp71, align 8
  %ema_is_init77.not = icmp eq i64 %ema_init76, 0
  %ema_new78 = select i1 %ema_is_init77.not, double %mid, double %ema_blended75
  store double %ema_new78, ptr %ema_state_ptr69, align 8
  store i64 1, ptr %ema_init_pp71, align 8
  %sub79 = fsub double %ema_new, %ema_new78
  %rstd82 = tail call double @jit_rt_rolling_std(ptr %ctx, i64 11, double %mid, i64 30)
  %div = fdiv double %sub79, %rstd82
  br label %ifcont

ifcont:                                           ; preds = %entry, %then
  %iftmp = phi double [ %div, %then ], [ 0.000000e+00, %entry ]
  %out_ptr83 = getelementptr double, ptr %outputs, i64 4
  store double %iftmp, ptr %out_ptr83, align 8
  ret void
}

declare ptr @jit_rt_symbol_ctx(ptr, i32) local_unnamed_addr

declare double @jit_rt_rolling_std(ptr, i64, double, i64) local_unnamed_addr

declare ptr @jit_rt_ema_lowered_base(ptr) local_unnamed_addr
