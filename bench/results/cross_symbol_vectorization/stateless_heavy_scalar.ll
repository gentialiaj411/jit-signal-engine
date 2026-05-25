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
  %mid_sum = fadd double %bid, %ask
  %mid = fmul double %mid_sum, 5.000000e-01
  %mid_sum11 = fadd double %bid6, %ask10
  %mid12 = fmul double %mid_sum11, 5.000000e-01
  %sub = fsub double %mid, %mid12
  store double %sub, ptr %outputs, align 8
  %add = fadd double %mid, %mid12
  %out_ptr17 = getelementptr double, ptr %outputs, i64 1
  store double %add, ptr %out_ptr17, align 8
  %sub18 = fsub double %ask, %bid
  %mul = fmul double %sub18, 5.000000e-01
  %sub19 = fsub double %ask10, %bid6
  %mul20 = fmul double %sub19, 5.000000e-01
  %add21 = fadd double %mul, %mul20
  %out_ptr22 = getelementptr double, ptr %outputs, i64 2
  store double %add21, ptr %out_ptr22, align 8
  %mul33 = fmul double %sub, %add
  %sub39 = fsub double %mul33, %add21
  %out_ptr40 = getelementptr double, ptr %outputs, i64 3
  store double %sub39, ptr %out_ptr40, align 8
  %add63 = fadd double %sub, %sub39
  %sub86 = fsub double %sub39, %add
  %mul87 = fmul double %add63, %sub86
  %out_ptr88 = getelementptr double, ptr %outputs, i64 4
  store double %mul87, ptr %out_ptr88, align 8
  %abs = tail call double @llvm.fabs.f64(double %sub39)
  %abs153 = tail call double @llvm.fabs.f64(double %mul87)
  %sub154 = fsub double %abs, %abs153
  %out_ptr155 = getelementptr double, ptr %outputs, i64 5
  store double %sub154, ptr %out_ptr155, align 8
  %mul203 = fmul double %mul87, 2.500000e-01
  %mul271 = fmul double %sub154, 7.500000e-01
  %add272 = fadd double %mul203, %mul271
  %out_ptr273 = getelementptr double, ptr %outputs, i64 6
  store double %add272, ptr %out_ptr273, align 8
  %add396 = fadd double %sub, %add272
  %add519 = fadd double %add, %add272
  %sub520 = fsub double %add396, %add519
  %out_ptr521 = getelementptr double, ptr %outputs, i64 7
  store double %sub520, ptr %out_ptr521, align 8
  %mul769 = fmul double %sub520, 5.000000e-01
  %sub780 = fsub double %sub, %add
  %mul781 = fmul double %sub780, 5.000000e-01
  %add782 = fadd double %mul781, %mul769
  %out_ptr783 = getelementptr double, ptr %outputs, i64 8
  store double %add782, ptr %out_ptr783, align 8
  ret void
}

declare ptr @jit_rt_symbol_ctx(ptr, i32) local_unnamed_addr

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare double @llvm.fabs.f64(double) #0

attributes #0 = { mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none) }
