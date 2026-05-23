; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr nocapture readonly %market, ptr %ctx, ptr nocapture writeonly %outputs) local_unnamed_addr {
entry:
  %mid_bid = load double, ptr %market, align 8
  %mid_ask_ptr = getelementptr inbounds { double, double, double, double, i64 }, ptr %market, i64 0, i32 1
  %mid_ask = load double, ptr %mid_ask_ptr, align 8
  %mid_sum = fadd double %mid_bid, %mid_ask
  %mid = fmul double %mid_sum, 5.000000e-01
  %ema = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 1, double %mid, double 0x3FC745D1745D1746, i64 10)
  store double %ema, ptr %outputs, align 8
  %mid_bid6 = load double, ptr %market, align 8
  %mid_ask10 = load double, ptr %mid_ask_ptr, align 8
  %mid_sum11 = fadd double %mid_bid6, %mid_ask10
  %mid12 = fmul double %mid_sum11, 5.000000e-01
  %ema13 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 1, double %mid12, double 0x3FA0C9714FBCDA3B, i64 60)
  %out_ptr14 = getelementptr double, ptr %outputs, i64 1
  store double %ema13, ptr %out_ptr14, align 8
  %mid_bid18 = load double, ptr %market, align 8
  %mid_ask22 = load double, ptr %mid_ask_ptr, align 8
  %mid_sum23 = fadd double %mid_bid18, %mid_ask22
  %mid24 = fmul double %mid_sum23, 5.000000e-01
  %rstd = tail call double @jit_rt_rolling_std(ptr %ctx, i64 1, double %mid24, i64 30)
  %out_ptr25 = getelementptr double, ptr %outputs, i64 2
  store double %rstd, ptr %out_ptr25, align 8
  %mid_bid29 = load double, ptr %market, align 8
  %mid_ask33 = load double, ptr %mid_ask_ptr, align 8
  %mid_sum34 = fadd double %mid_bid29, %mid_ask33
  %mid35 = fmul double %mid_sum34, 5.000000e-01
  %ema36 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 1, double %mid35, double 0x3FC745D1745D1746, i64 10)
  %mid_bid40 = load double, ptr %market, align 8
  %mid_ask44 = load double, ptr %mid_ask_ptr, align 8
  %mid_sum45 = fadd double %mid_bid40, %mid_ask44
  %mid46 = fmul double %mid_sum45, 5.000000e-01
  %ema47 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 2, double %mid46, double 0x3FA0C9714FBCDA3B, i64 60)
  %sub = fsub double %ema36, %ema47
  %out_ptr48 = getelementptr double, ptr %outputs, i64 3
  store double %sub, ptr %out_ptr48, align 8
  %mid_bid52 = load double, ptr %market, align 8
  %mid_ask56 = load double, ptr %mid_ask_ptr, align 8
  %mid_sum57 = fadd double %mid_bid52, %mid_ask56
  %mid58 = fmul double %mid_sum57, 5.000000e-01
  %ema59 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 1, double %mid58, double 0x3FC745D1745D1746, i64 10)
  %mid_bid63 = load double, ptr %market, align 8
  %mid_ask67 = load double, ptr %mid_ask_ptr, align 8
  %mid_sum68 = fadd double %mid_bid63, %mid_ask67
  %mid69 = fmul double %mid_sum68, 5.000000e-01
  %ema70 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 2, double %mid69, double 0x3FA0C9714FBCDA3B, i64 60)
  %gt = fcmp ogt double %ema59, %ema70
  %mid_bid74 = load double, ptr %market, align 8
  %mid_ask78 = load double, ptr %mid_ask_ptr, align 8
  %mid_sum79 = fadd double %mid_bid74, %mid_ask78
  %mid80 = fmul double %mid_sum79, 5.000000e-01
  %rstd81 = tail call double @jit_rt_rolling_std(ptr %ctx, i64 3, double %mid80, i64 30)
  %gt82 = fcmp ogt double %rstd81, 0.000000e+00
  %and = and i1 %gt, %gt82
  br i1 %and, label %then, label %ifcont

then:                                             ; preds = %entry
  %mid_bid87 = load double, ptr %market, align 8
  %mid_ask91 = load double, ptr %mid_ask_ptr, align 8
  %mid_sum92 = fadd double %mid_bid87, %mid_ask91
  %mid93 = fmul double %mid_sum92, 5.000000e-01
  %ema94 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 4, double %mid93, double 0x3FC745D1745D1746, i64 10)
  %mid_bid98 = load double, ptr %market, align 8
  %mid_ask102 = load double, ptr %mid_ask_ptr, align 8
  %mid_sum103 = fadd double %mid_bid98, %mid_ask102
  %mid104 = fmul double %mid_sum103, 5.000000e-01
  %ema105 = tail call double @jit_rt_ema_alpha(ptr %ctx, i64 5, double %mid104, double 0x3FA0C9714FBCDA3B, i64 60)
  %sub106 = fsub double %ema94, %ema105
  %mid_bid110 = load double, ptr %market, align 8
  %mid_ask114 = load double, ptr %mid_ask_ptr, align 8
  %mid_sum115 = fadd double %mid_bid110, %mid_ask114
  %mid116 = fmul double %mid_sum115, 5.000000e-01
  %rstd117 = tail call double @jit_rt_rolling_std(ptr %ctx, i64 6, double %mid116, i64 30)
  %div = fdiv double %sub106, %rstd117
  br label %ifcont

ifcont:                                           ; preds = %entry, %then
  %iftmp = phi double [ %div, %then ], [ 0.000000e+00, %entry ]
  %out_ptr118 = getelementptr double, ptr %outputs, i64 4
  store double %iftmp, ptr %out_ptr118, align 8
  ret void
}

declare double @jit_rt_ema_alpha(ptr, i64, double, double, i64) local_unnamed_addr

declare double @jit_rt_rolling_std(ptr, i64, double, i64) local_unnamed_addr
