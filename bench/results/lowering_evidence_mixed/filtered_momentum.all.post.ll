; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr nocapture readonly %market, ptr %arena, i32 %symbol_id, ptr nocapture writeonly %outputs) local_unnamed_addr {
entry:
  %ctx = tail call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %lag_base = tail call ptr @jit_rt_lag_lowered_base(ptr %ctx)
  %ema_base = tail call ptr @jit_rt_ema_lowered_base(ptr %ctx)
  %sma_base = tail call ptr @jit_rt_sma_lowered_base(ptr %ctx)
  %bid = load double, ptr %market, align 8
  %ask_ptr = getelementptr inbounds { double, double, double, double, i64 }, ptr %market, i64 0, i32 1
  %ask = load double, ptr %ask_ptr, align 8
  %mid_sum = fadd double %bid, %ask
  %mid = fmul double %mid_sum, 5.000000e-01
  %sma_state_ptr = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_base, i64 1
  %sma_sum_pp = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_base, i64 1, i32 1
  %sma_head_pp = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_base, i64 1, i32 2
  %sma_count_pp = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_base, i64 1, i32 3
  %sma_buf = load ptr, ptr %sma_state_ptr, align 8
  %sma_sum = load double, ptr %sma_sum_pp, align 8
  %sma_head = load i64, ptr %sma_head_pp, align 8
  %sma_count = load i64, ptr %sma_count_pp, align 8
  %sma_was_full = icmp eq i64 %sma_count, 20
  %sma_slot = getelementptr inbounds double, ptr %sma_buf, i64 %sma_head
  %sma_old = load double, ptr %sma_slot, align 8
  %sma_old_or_zero = select i1 %sma_was_full, double %sma_old, double 0.000000e+00
  %sma_sum_plus_x = fadd double %sma_sum, %mid
  %sma_sum_new = fsub double %sma_sum_plus_x, %sma_old_or_zero
  %sma_count_plus_one = add i64 %sma_count, 1
  %sma_count_new = select i1 %sma_was_full, i64 20, i64 %sma_count_plus_one
  store double %mid, ptr %sma_slot, align 8
  %sma_head_plus = add i64 %sma_head, 1
  %sma_head_new = urem i64 %sma_head_plus, 20
  store double %sma_sum_new, ptr %sma_sum_pp, align 8
  store i64 %sma_head_new, ptr %sma_head_pp, align 8
  store i64 %sma_count_new, ptr %sma_count_pp, align 8
  %sma_is_full = icmp eq i64 %sma_count_new, 20
  %sma_mean = fdiv double %sma_sum_new, 2.000000e+01
  %sma_out = select i1 %sma_is_full, double %sma_mean, double 0x7FF8000000000000
  store double %sma_out, ptr %outputs, align 8
  %ema_state_ptr = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 2
  %ema_init_pp = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 2, i32 1
  %ema_prev = load double, ptr %ema_state_ptr, align 8
  %ema_init = load i64, ptr %ema_init_pp, align 8
  %ema_is_init.not = icmp eq i64 %ema_init, 0
  %ema_ax = fmul double %mid, 1.250000e-01
  %ema_bp = fmul double %ema_prev, 8.750000e-01
  %ema_blended = fadd double %ema_ax, %ema_bp
  %ema_new = select i1 %ema_is_init.not, double %mid, double %ema_blended
  store double %ema_new, ptr %ema_state_ptr, align 8
  store i64 1, ptr %ema_init_pp, align 8
  %out_ptr5 = getelementptr double, ptr %outputs, i64 1
  store double %ema_new, ptr %out_ptr5, align 8
  %lag_state_ptr = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_base, i64 3
  %lag_head_pp = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_base, i64 3, i32 1
  %lag_count_pp = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_base, i64 3, i32 2
  %lag_buf = load ptr, ptr %lag_state_ptr, align 8
  %lag_head = load i64, ptr %lag_head_pp, align 8
  %lag_count = load i64, ptr %lag_count_pp, align 8
  %lag_is_full = icmp eq i64 %lag_count, 7
  %lag_slot = getelementptr inbounds double, ptr %lag_buf, i64 %lag_head
  %lag_lagged_buf = load double, ptr %lag_slot, align 8
  %lag_lagged = select i1 %lag_is_full, double %lag_lagged_buf, double 0x7FF8000000000000
  store double %mid, ptr %lag_slot, align 8
  %lag_head_plus = add i64 %lag_head, 1
  %lag_head_new = urem i64 %lag_head_plus, 7
  store i64 %lag_head_new, ptr %lag_head_pp, align 8
  %lag_count_plus = add i64 %lag_count, 1
  %lag_count_new = select i1 %lag_is_full, i64 7, i64 %lag_count_plus
  store i64 %lag_count_new, ptr %lag_count_pp, align 8
  %out_ptr8 = getelementptr double, ptr %outputs, i64 2
  store double %lag_lagged, ptr %out_ptr8, align 8
  %sma_state_ptr11 = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_base, i64 4
  %sma_sum_pp13 = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_base, i64 4, i32 1
  %sma_head_pp14 = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_base, i64 4, i32 2
  %sma_count_pp15 = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_base, i64 4, i32 3
  %sma_buf16 = load ptr, ptr %sma_state_ptr11, align 8
  %sma_sum17 = load double, ptr %sma_sum_pp13, align 8
  %sma_head18 = load i64, ptr %sma_head_pp14, align 8
  %sma_count19 = load i64, ptr %sma_count_pp15, align 8
  %sma_was_full20 = icmp eq i64 %sma_count19, 20
  %sma_slot21 = getelementptr inbounds double, ptr %sma_buf16, i64 %sma_head18
  %sma_old22 = load double, ptr %sma_slot21, align 8
  %sma_old_or_zero23 = select i1 %sma_was_full20, double %sma_old22, double 0.000000e+00
  %sma_sum_plus_x24 = fadd double %mid, %sma_sum17
  %sma_sum_new25 = fsub double %sma_sum_plus_x24, %sma_old_or_zero23
  %sma_count_plus_one26 = add i64 %sma_count19, 1
  %sma_count_new27 = select i1 %sma_was_full20, i64 20, i64 %sma_count_plus_one26
  store double %mid, ptr %sma_slot21, align 8
  %sma_head_plus28 = add i64 %sma_head18, 1
  %sma_head_new29 = urem i64 %sma_head_plus28, 20
  store double %sma_sum_new25, ptr %sma_sum_pp13, align 8
  store i64 %sma_head_new29, ptr %sma_head_pp14, align 8
  store i64 %sma_count_new27, ptr %sma_count_pp15, align 8
  %sma_is_full30 = icmp eq i64 %sma_count_new27, 20
  %sma_mean31 = fdiv double %sma_sum_new25, 2.000000e+01
  %sma_out32 = select i1 %sma_is_full30, double %sma_mean31, double 0x7FF8000000000000
  %ema_state_ptr35 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 5
  %ema_init_pp37 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 5, i32 1
  %ema_prev38 = load double, ptr %ema_state_ptr35, align 8
  %ema_init39 = load i64, ptr %ema_init_pp37, align 8
  %ema_is_init40.not = icmp eq i64 %ema_init39, 0
  %ema_bp42 = fmul double %ema_prev38, 8.750000e-01
  %ema_blended43 = fadd double %ema_ax, %ema_bp42
  %ema_new44 = select i1 %ema_is_init40.not, double %mid, double %ema_blended43
  store double %ema_new44, ptr %ema_state_ptr35, align 8
  store i64 1, ptr %ema_init_pp37, align 8
  %sub = fsub double %sma_out32, %ema_new44
  %lag_state_ptr47 = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_base, i64 6
  %lag_head_pp49 = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_base, i64 6, i32 1
  %lag_count_pp50 = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_base, i64 6, i32 2
  %lag_buf51 = load ptr, ptr %lag_state_ptr47, align 8
  %lag_head52 = load i64, ptr %lag_head_pp49, align 8
  %lag_count53 = load i64, ptr %lag_count_pp50, align 8
  %lag_is_full54 = icmp eq i64 %lag_count53, 7
  %lag_slot55 = getelementptr inbounds double, ptr %lag_buf51, i64 %lag_head52
  %lag_lagged_buf56 = load double, ptr %lag_slot55, align 8
  %lag_lagged57 = select i1 %lag_is_full54, double %lag_lagged_buf56, double 0x7FF8000000000000
  store double %mid, ptr %lag_slot55, align 8
  %lag_head_plus58 = add i64 %lag_head52, 1
  %lag_head_new59 = urem i64 %lag_head_plus58, 7
  store i64 %lag_head_new59, ptr %lag_head_pp49, align 8
  %lag_count_plus60 = add i64 %lag_count53, 1
  %lag_count_new61 = select i1 %lag_is_full54, i64 7, i64 %lag_count_plus60
  store i64 %lag_count_new61, ptr %lag_count_pp50, align 8
  %add = fadd double %sub, %lag_lagged57
  %out_ptr62 = getelementptr double, ptr %outputs, i64 3
  store double %add, ptr %out_ptr62, align 8
  ret void
}

declare ptr @jit_rt_symbol_ctx(ptr, i32) local_unnamed_addr

declare ptr @jit_rt_sma_lowered_base(ptr) local_unnamed_addr

declare ptr @jit_rt_ema_lowered_base(ptr) local_unnamed_addr

declare ptr @jit_rt_lag_lowered_base(ptr) local_unnamed_addr
