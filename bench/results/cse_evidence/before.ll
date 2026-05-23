; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr %market, ptr %ctx, ptr %outputs) {
entry:
  %instruments_ptr = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr, i64 0, i64 0
  %mid_bid_ptr = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr, i32 0, i32 0
  %mid_bid = load double, ptr %mid_bid_ptr, align 8
  %instruments_ptr1 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr2 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr1, i64 0, i64 0
  %mid_ask_ptr = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr2, i32 0, i32 1
  %mid_ask = load double, ptr %mid_ask_ptr, align 8
  %mid_sum = fadd double %mid_bid, %mid_ask
  %mid = fmul double %mid_sum, 5.000000e-01
  %ema = call double @jit_rt_ema_alpha(ptr %ctx, i64 1, double %mid, double 0x3FC745D1745D1746, i64 10)
  %out_ptr = getelementptr double, ptr %outputs, i64 0
  store double %ema, ptr %out_ptr, align 8
  %instruments_ptr3 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr4 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr3, i64 0, i64 0
  %mid_bid_ptr5 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr4, i32 0, i32 0
  %mid_bid6 = load double, ptr %mid_bid_ptr5, align 8
  %instruments_ptr7 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr8 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr7, i64 0, i64 0
  %mid_ask_ptr9 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr8, i32 0, i32 1
  %mid_ask10 = load double, ptr %mid_ask_ptr9, align 8
  %mid_sum11 = fadd double %mid_bid6, %mid_ask10
  %mid12 = fmul double %mid_sum11, 5.000000e-01
  %ema13 = call double @jit_rt_ema_alpha(ptr %ctx, i64 1, double %mid12, double 0x3FA0C9714FBCDA3B, i64 60)
  %out_ptr14 = getelementptr double, ptr %outputs, i64 1
  store double %ema13, ptr %out_ptr14, align 8
  %instruments_ptr15 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr16 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr15, i64 0, i64 0
  %mid_bid_ptr17 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr16, i32 0, i32 0
  %mid_bid18 = load double, ptr %mid_bid_ptr17, align 8
  %instruments_ptr19 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr20 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr19, i64 0, i64 0
  %mid_ask_ptr21 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr20, i32 0, i32 1
  %mid_ask22 = load double, ptr %mid_ask_ptr21, align 8
  %mid_sum23 = fadd double %mid_bid18, %mid_ask22
  %mid24 = fmul double %mid_sum23, 5.000000e-01
  %rstd = call double @jit_rt_rolling_std(ptr %ctx, i64 1, double %mid24, i64 30)
  %out_ptr25 = getelementptr double, ptr %outputs, i64 2
  store double %rstd, ptr %out_ptr25, align 8
  %instruments_ptr26 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr27 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr26, i64 0, i64 0
  %mid_bid_ptr28 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr27, i32 0, i32 0
  %mid_bid29 = load double, ptr %mid_bid_ptr28, align 8
  %instruments_ptr30 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr31 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr30, i64 0, i64 0
  %mid_ask_ptr32 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr31, i32 0, i32 1
  %mid_ask33 = load double, ptr %mid_ask_ptr32, align 8
  %mid_sum34 = fadd double %mid_bid29, %mid_ask33
  %mid35 = fmul double %mid_sum34, 5.000000e-01
  %ema36 = call double @jit_rt_ema_alpha(ptr %ctx, i64 1, double %mid35, double 0x3FC745D1745D1746, i64 10)
  %instruments_ptr37 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr38 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr37, i64 0, i64 0
  %mid_bid_ptr39 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr38, i32 0, i32 0
  %mid_bid40 = load double, ptr %mid_bid_ptr39, align 8
  %instruments_ptr41 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr42 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr41, i64 0, i64 0
  %mid_ask_ptr43 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr42, i32 0, i32 1
  %mid_ask44 = load double, ptr %mid_ask_ptr43, align 8
  %mid_sum45 = fadd double %mid_bid40, %mid_ask44
  %mid46 = fmul double %mid_sum45, 5.000000e-01
  %ema47 = call double @jit_rt_ema_alpha(ptr %ctx, i64 2, double %mid46, double 0x3FA0C9714FBCDA3B, i64 60)
  %sub = fsub double %ema36, %ema47
  %out_ptr48 = getelementptr double, ptr %outputs, i64 3
  store double %sub, ptr %out_ptr48, align 8
  %instruments_ptr49 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr50 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr49, i64 0, i64 0
  %mid_bid_ptr51 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr50, i32 0, i32 0
  %mid_bid52 = load double, ptr %mid_bid_ptr51, align 8
  %instruments_ptr53 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr54 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr53, i64 0, i64 0
  %mid_ask_ptr55 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr54, i32 0, i32 1
  %mid_ask56 = load double, ptr %mid_ask_ptr55, align 8
  %mid_sum57 = fadd double %mid_bid52, %mid_ask56
  %mid58 = fmul double %mid_sum57, 5.000000e-01
  %ema59 = call double @jit_rt_ema_alpha(ptr %ctx, i64 1, double %mid58, double 0x3FC745D1745D1746, i64 10)
  %instruments_ptr60 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr61 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr60, i64 0, i64 0
  %mid_bid_ptr62 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr61, i32 0, i32 0
  %mid_bid63 = load double, ptr %mid_bid_ptr62, align 8
  %instruments_ptr64 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr65 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr64, i64 0, i64 0
  %mid_ask_ptr66 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr65, i32 0, i32 1
  %mid_ask67 = load double, ptr %mid_ask_ptr66, align 8
  %mid_sum68 = fadd double %mid_bid63, %mid_ask67
  %mid69 = fmul double %mid_sum68, 5.000000e-01
  %ema70 = call double @jit_rt_ema_alpha(ptr %ctx, i64 2, double %mid69, double 0x3FA0C9714FBCDA3B, i64 60)
  %gt = fcmp ogt double %ema59, %ema70
  %gt_f = uitofp i1 %gt to double
  %instruments_ptr71 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr72 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr71, i64 0, i64 0
  %mid_bid_ptr73 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr72, i32 0, i32 0
  %mid_bid74 = load double, ptr %mid_bid_ptr73, align 8
  %instruments_ptr75 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr76 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr75, i64 0, i64 0
  %mid_ask_ptr77 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr76, i32 0, i32 1
  %mid_ask78 = load double, ptr %mid_ask_ptr77, align 8
  %mid_sum79 = fadd double %mid_bid74, %mid_ask78
  %mid80 = fmul double %mid_sum79, 5.000000e-01
  %rstd81 = call double @jit_rt_rolling_std(ptr %ctx, i64 3, double %mid80, i64 30)
  %gt82 = fcmp ogt double %rstd81, 0.000000e+00
  %gt_f83 = uitofp i1 %gt82 to double
  %and_l = fcmp une double %gt_f, 0.000000e+00
  %and_r = fcmp une double %gt_f83, 0.000000e+00
  %and = and i1 %and_l, %and_r
  %and_f = uitofp i1 %and to double
  %ifcond = fcmp une double %and_f, 0.000000e+00
  br i1 %ifcond, label %then, label %else

then:                                             ; preds = %entry
  %instruments_ptr84 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr85 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr84, i64 0, i64 0
  %mid_bid_ptr86 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr85, i32 0, i32 0
  %mid_bid87 = load double, ptr %mid_bid_ptr86, align 8
  %instruments_ptr88 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr89 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr88, i64 0, i64 0
  %mid_ask_ptr90 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr89, i32 0, i32 1
  %mid_ask91 = load double, ptr %mid_ask_ptr90, align 8
  %mid_sum92 = fadd double %mid_bid87, %mid_ask91
  %mid93 = fmul double %mid_sum92, 5.000000e-01
  %ema94 = call double @jit_rt_ema_alpha(ptr %ctx, i64 4, double %mid93, double 0x3FC745D1745D1746, i64 10)
  %instruments_ptr95 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr96 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr95, i64 0, i64 0
  %mid_bid_ptr97 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr96, i32 0, i32 0
  %mid_bid98 = load double, ptr %mid_bid_ptr97, align 8
  %instruments_ptr99 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr100 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr99, i64 0, i64 0
  %mid_ask_ptr101 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr100, i32 0, i32 1
  %mid_ask102 = load double, ptr %mid_ask_ptr101, align 8
  %mid_sum103 = fadd double %mid_bid98, %mid_ask102
  %mid104 = fmul double %mid_sum103, 5.000000e-01
  %ema105 = call double @jit_rt_ema_alpha(ptr %ctx, i64 5, double %mid104, double 0x3FA0C9714FBCDA3B, i64 60)
  %sub106 = fsub double %ema94, %ema105
  %instruments_ptr107 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr108 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr107, i64 0, i64 0
  %mid_bid_ptr109 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr108, i32 0, i32 0
  %mid_bid110 = load double, ptr %mid_bid_ptr109, align 8
  %instruments_ptr111 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr112 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr111, i64 0, i64 0
  %mid_ask_ptr113 = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr112, i32 0, i32 1
  %mid_ask114 = load double, ptr %mid_ask_ptr113, align 8
  %mid_sum115 = fadd double %mid_bid110, %mid_ask114
  %mid116 = fmul double %mid_sum115, 5.000000e-01
  %rstd117 = call double @jit_rt_rolling_std(ptr %ctx, i64 6, double %mid116, i64 30)
  %div = fdiv double %sub106, %rstd117
  br label %ifcont

else:                                             ; preds = %entry
  br label %ifcont

ifcont:                                           ; preds = %else, %then
  %iftmp = phi double [ %div, %then ], [ 0.000000e+00, %else ]
  %out_ptr118 = getelementptr double, ptr %outputs, i64 4
  store double %iftmp, ptr %out_ptr118, align 8
  ret void
}

declare double @jit_rt_mid(ptr, i64)

declare double @jit_rt_bid(ptr, i64)

declare double @jit_rt_ask(ptr, i64)

declare double @jit_rt_spread(ptr, i64)

declare double @jit_rt_ema(ptr, i64, double, i64)

declare double @jit_rt_ema_alpha(ptr, i64, double, double, i64)

declare double @jit_rt_sma(ptr, i64, double, i64)

declare double @jit_rt_rolling_std(ptr, i64, double, i64)

declare double @jit_rt_zscore(ptr, i64, double, i64)

declare double @jit_rt_rolling_min(ptr, i64, double, i64)

declare double @jit_rt_rolling_max(ptr, i64, double, i64)

declare double @jit_rt_vwap(ptr, ptr, i64, i64, i64)

declare double @jit_rt_lag(ptr, i64, double, i64)

declare double @jit_rt_cross_above(ptr, i64, double, double)

declare double @jit_rt_cross_below(ptr, i64, double, double)
