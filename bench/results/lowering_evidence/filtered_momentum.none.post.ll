; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr nocapture readonly %market, ptr %arena, i32 %symbol_id, ptr nocapture writeonly %outputs) local_unnamed_addr {
entry:
  %ctx = tail call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %bid = load double, ptr %market, align 8
  %ask_ptr = getelementptr inbounds { double, double, double, double, i64, [24 x i8] }, ptr %market, i64 0, i32 1
  %ask = load double, ptr %ask_ptr, align 8
  %mid_sum = fadd double %bid, %ask
  %mid = fmul double %mid_sum, 5.000000e-01
  %ema = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 1, double %mid, double 0x3FC745D1745D1746, i64 10)
  store double %ema, ptr %outputs, align 8
  %ema5 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 2, double %mid, double 0x3FA0C9714FBCDA3B, i64 60)
  %out_ptr6 = getelementptr double, ptr %outputs, i64 1
  store double %ema5, ptr %out_ptr6, align 8
  %rstd = tail call double @jit_rt_rolling_std(ptr %ctx, i64 3, double %mid, i64 30)
  %out_ptr9 = getelementptr double, ptr %outputs, i64 2
  store double %rstd, ptr %out_ptr9, align 8
  %ema12 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 4, double %mid, double 0x3FC745D1745D1746, i64 10)
  %ema15 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 5, double %mid, double 0x3FA0C9714FBCDA3B, i64 60)
  %sub = fsub double %ema12, %ema15
  %out_ptr16 = getelementptr double, ptr %outputs, i64 3
  store double %sub, ptr %out_ptr16, align 8
  %ema19 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 6, double %mid, double 0x3FC745D1745D1746, i64 10)
  %ema22 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 7, double %mid, double 0x3FA0C9714FBCDA3B, i64 60)
  %gt = fcmp ogt double %ema19, %ema22
  %rstd25 = tail call double @jit_rt_rolling_std(ptr %ctx, i64 8, double %mid, i64 30)
  %gt26 = fcmp ogt double %rstd25, 0.000000e+00
  %and = and i1 %gt, %gt26
  br i1 %and, label %then, label %ifcont

then:                                             ; preds = %entry
  %sub28 = fsub double %ema19, %ema22
  %div = fdiv double %sub28, %rstd25
  br label %ifcont

ifcont:                                           ; preds = %entry, %then
  %iftmp = phi double [ %div, %then ], [ 0.000000e+00, %entry ]
  %out_ptr29 = getelementptr double, ptr %outputs, i64 4
  store double %iftmp, ptr %out_ptr29, align 8
  ret void
}

declare ptr @jit_rt_symbol_ctx(ptr, i32) local_unnamed_addr

declare double @jit_rt_ema_alpha(ptr, i64, double, double, i64) local_unnamed_addr

declare double @jit_rt_rolling_std(ptr, i64, double, i64) local_unnamed_addr
