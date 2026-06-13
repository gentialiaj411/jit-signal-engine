; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr nocapture readonly %market, ptr %arena, i32 %symbol_id, ptr nocapture writeonly %outputs) local_unnamed_addr {
entry:
  %ctx = tail call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %instrument_ptr = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %market, i64 0, i64 1
  %bid = load double, ptr %instrument_ptr, align 8
  %ask_ptr = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %market, i64 0, i64 1, i32 1
  %ask = load double, ptr %ask_ptr, align 8
  %bid6 = load double, ptr %market, align 8
  %ask_ptr9 = getelementptr inbounds { double, double, double, double, i64, [24 x i8] }, ptr %market, i64 0, i32 1
  %ask10 = load double, ptr %ask_ptr9, align 8
  %mid_sum = fadd double %bid6, %ask10
  %mid = fmul double %mid_sum, 5.000000e-01
  store double %mid, ptr %outputs, align 8
  %mid_sum11 = fadd double %bid, %ask
  %mid12 = fmul double %mid_sum11, 5.000000e-01
  %out_ptr13 = getelementptr double, ptr %outputs, i64 1
  store double %mid12, ptr %out_ptr13, align 8
  %sub = fsub double %ask10, %bid6
  %out_ptr14 = getelementptr double, ptr %outputs, i64 2
  store double %sub, ptr %out_ptr14, align 8
  %sub15 = fsub double %ask, %bid
  %out_ptr16 = getelementptr double, ptr %outputs, i64 3
  store double %sub15, ptr %out_ptr16, align 8
  %mul = fmul double %mid, %mid
  %mul25 = fmul double %mid12, %mid12
  %add = fadd double %mul25, %mul
  %mul28 = fmul double %sub, %sub
  %add29 = fadd double %mul28, %add
  %mul32 = fmul double %sub15, %sub15
  %add33 = fadd double %mul32, %add29
  %out_ptr34 = getelementptr double, ptr %outputs, i64 4
  store double %add33, ptr %out_ptr34, align 8
  %mul39 = fmul double %mid12, %mid
  %mul42 = fmul double %sub15, %sub
  %sub43 = fsub double %mul39, %mul42
  %sub48 = fsub double %mid, %mid12
  %sub51 = fsub double %sub, %sub15
  %mul52 = fmul double %sub51, %sub48
  %add53 = fadd double %sub43, %mul52
  %out_ptr54 = getelementptr double, ptr %outputs, i64 5
  store double %add53, ptr %out_ptr54, align 8
  %add74 = fadd double %add33, 1.000000e+00
  %sqrt = tail call double @llvm.sqrt.f64(double %add74)
  %mul113 = fmul double %add53, %add53
  %add114 = fadd double %mul113, 1.000000e+00
  %sqrt115 = tail call double @llvm.sqrt.f64(double %add114)
  %sub116 = fsub double %sqrt, %sqrt115
  %out_ptr117 = getelementptr double, ptr %outputs, i64 6
  store double %sub116, ptr %out_ptr117, align 8
  %add122 = fadd double %mid12, %mid
  %mul128 = fmul double %add122, %add122
  %mul139 = fmul double %sub48, %sub48
  %sub140 = fsub double %mul128, %mul139
  %out_ptr141 = getelementptr double, ptr %outputs, i64 7
  store double %sub140, ptr %out_ptr141, align 8
  %add165 = fadd double %sub140, 1.000000e+00
  %sqrt166 = tail call double @llvm.sqrt.f64(double %add165)
  %abs = tail call double @llvm.fabs.f64(double %add53)
  %add186 = fadd double %abs, 1.000000e+00
  %sqrt187 = tail call double @llvm.sqrt.f64(double %add186)
  %add188 = fadd double %sqrt166, %sqrt187
  %out_ptr189 = getelementptr double, ptr %outputs, i64 8
  store double %add188, ptr %out_ptr189, align 8
  %mul272 = fmul double %add33, %sub116
  %mul340 = fmul double %add53, %add188
  %sub341 = fsub double %mul272, %mul340
  %add365 = fadd double %sub140, %sub341
  %out_ptr366 = getelementptr double, ptr %outputs, i64 9
  store double %add365, ptr %out_ptr366, align 8
  %mul478 = fmul double %sub116, %add188
  %mul521 = fmul double %sub140, %add33
  %add522 = fadd double %mul521, %mul478
  %sub542 = fsub double %add522, %add53
  %out_ptr543 = getelementptr double, ptr %outputs, i64 10
  store double %sub542, ptr %out_ptr543, align 8
  %abs720 = tail call double @llvm.fabs.f64(double %add365)
  %add721 = fadd double %abs720, 1.000000e+00
  %sqrt722 = tail call double @llvm.sqrt.f64(double %add721)
  %abs899 = tail call double @llvm.fabs.f64(double %sub542)
  %add900 = fadd double %abs899, 1.000000e+00
  %sqrt901 = tail call double @llvm.sqrt.f64(double %add900)
  %add902 = fadd double %sqrt722, %sqrt901
  %out_ptr903 = getelementptr double, ptr %outputs, i64 11
  store double %add902, ptr %out_ptr903, align 8
  %mul1256 = fmul double %add365, %sub542
  %mul1975 = fmul double %add902, %add902
  %sub1976 = fsub double %mul1256, %mul1975
  %out_ptr1977 = getelementptr double, ptr %outputs, i64 12
  store double %sub1976, ptr %out_ptr1977, align 8
  %abs3051 = tail call double @llvm.fabs.f64(double %sub1976)
  %add3052 = fadd double %abs3051, 1.000000e+00
  %sqrt3053 = tail call double @llvm.sqrt.f64(double %add3052)
  %mul3054 = fmul double %sqrt3053, 5.000000e-01
  %sub3590 = fsub double %add902, %add365
  %abs3591 = tail call double @llvm.fabs.f64(double %sub3590)
  %mul3592 = fmul double %abs3591, 2.500000e-01
  %add3593 = fadd double %mul3592, %mul3054
  %out_ptr3594 = getelementptr double, ptr %outputs, i64 13
  store double %add3593, ptr %out_ptr3594, align 8
  %abs3948 = tail call double @llvm.fabs.f64(double %mul1256)
  %add3949 = fadd double %abs3948, 1.000000e+00
  %sqrt3950 = tail call double @llvm.sqrt.f64(double %add3949)
  %mul5025 = fmul double %abs3051, 1.250000e-01
  %sub5026 = fsub double %sqrt3950, %mul5025
  %out_ptr5027 = getelementptr double, ptr %outputs, i64 14
  store double %sub5026, ptr %out_ptr5027, align 8
  %sub8076 = fsub double %add3593, %sub5026
  %mul8077 = fmul double %sub8076, 5.000000e-01
  %add8430 = fadd double %add365, %sub542
  %mul8431 = fmul double %add8430, 1.250000e-01
  %add8432 = fadd double %mul8431, %mul8077
  %out_ptr8433 = getelementptr double, ptr %outputs, i64 15
  store double %add8432, ptr %out_ptr8433, align 8
  ret void
}

declare ptr @jit_rt_symbol_ctx(ptr, i32) local_unnamed_addr

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare double @llvm.sqrt.f64(double) #0

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare double @llvm.fabs.f64(double) #0

attributes #0 = { mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none) }
