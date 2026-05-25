; ModuleID = 'jit_signal_program_module'
source_filename = "jit_signal_program_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define void @signal_program_func_1(ptr %market, ptr %arena, i32 %symbol_id, ptr %outputs) {
entry:
  %ctx = call ptr @jit_rt_symbol_ctx(ptr %arena, i32 %symbol_id)
  %lag_base = call ptr @jit_rt_lag_lowered_base(ptr %ctx)
  %ema_base = call ptr @jit_rt_ema_lowered_base(ptr %ctx)
  %sma_base = call ptr @jit_rt_sma_lowered_base(ptr %ctx)
  %instruments_ptr = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr, i64 0, i64 0
  %bid_ptr = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr, i32 0, i32 0
  %bid = load double, ptr %bid_ptr, align 8
  %instruments_ptr1 = getelementptr inbounds { [1024 x { double, double, double, double, i64 }], i64 }, ptr %market, i32 0, i32 0
  %instrument_ptr2 = getelementptr inbounds [1024 x { double, double, double, double, i64 }], ptr %instruments_ptr1, i64 0, i64 0
  %ask_ptr = getelementptr inbounds { double, double, double, double, i64 }, ptr %instrument_ptr2, i32 0, i32 1
  %ask = load double, ptr %ask_ptr, align 8
  %mid_sum = fadd double %bid, %ask
  %mid = fmul double %mid_sum, 5.000000e-01
  %sma_state_ptr = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_base, i64 1
  %sma_buf_pp = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_state_ptr, i32 0, i32 0
  %sma_sum_pp = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_state_ptr, i32 0, i32 1
  %sma_head_pp = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_state_ptr, i32 0, i32 2
  %sma_count_pp = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_state_ptr, i32 0, i32 3
  %sma_buf = load ptr, ptr %sma_buf_pp, align 8
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
  %sma_count_new = select i1 %sma_was_full, i64 %sma_count, i64 %sma_count_plus_one
  store double %mid, ptr %sma_slot, align 8
  %sma_head_plus = add i64 %sma_head, 1
  %sma_head_new = urem i64 %sma_head_plus, 20
  store double %sma_sum_new, ptr %sma_sum_pp, align 8
  store i64 %sma_head_new, ptr %sma_head_pp, align 8
  store i64 %sma_count_new, ptr %sma_count_pp, align 8
  %sma_is_full = icmp eq i64 %sma_count_new, 20
  %sma_mean = fdiv double %sma_sum_new, 2.000000e+01
  %sma_out = select i1 %sma_is_full, double %sma_mean, double 0x7FF8000000000000
  %out_ptr = getelementptr double, ptr %outputs, i64 0
  store double %sma_out, ptr %out_ptr, align 8
  %mid_sum3 = fadd double %bid, %ask
  %mid4 = fmul double %mid_sum3, 5.000000e-01
  %ema_state_ptr = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 2
  %ema_val_pp = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr, i32 0, i32 0
  %ema_init_pp = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr, i32 0, i32 1
  %ema_prev = load double, ptr %ema_val_pp, align 8
  %ema_init = load i64, ptr %ema_init_pp, align 8
  %ema_is_init = icmp ne i64 %ema_init, 0
  %ema_ax = fmul double 1.250000e-01, %mid4
  %ema_bp = fmul double 8.750000e-01, %ema_prev
  %ema_blended = fadd double %ema_ax, %ema_bp
  %ema_new = select i1 %ema_is_init, double %ema_blended, double %mid4
  store double %ema_new, ptr %ema_val_pp, align 8
  store i64 1, ptr %ema_init_pp, align 8
  %out_ptr5 = getelementptr double, ptr %outputs, i64 1
  store double %ema_new, ptr %out_ptr5, align 8
  %mid_sum6 = fadd double %bid, %ask
  %mid7 = fmul double %mid_sum6, 5.000000e-01
  %lag_state_ptr = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_base, i64 3
  %lag_buf_pp = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_state_ptr, i32 0, i32 0
  %lag_head_pp = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_state_ptr, i32 0, i32 1
  %lag_count_pp = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_state_ptr, i32 0, i32 2
  %lag_buf = load ptr, ptr %lag_buf_pp, align 8
  %lag_head = load i64, ptr %lag_head_pp, align 8
  %lag_count = load i64, ptr %lag_count_pp, align 8
  %lag_is_full = icmp eq i64 %lag_count, 7
  %lag_slot = getelementptr inbounds double, ptr %lag_buf, i64 %lag_head
  %lag_lagged_buf = load double, ptr %lag_slot, align 8
  %lag_lagged = select i1 %lag_is_full, double %lag_lagged_buf, double 0x7FF8000000000000
  store double %mid7, ptr %lag_slot, align 8
  %lag_head_plus = add i64 %lag_head, 1
  %lag_head_new = urem i64 %lag_head_plus, 7
  store i64 %lag_head_new, ptr %lag_head_pp, align 8
  %lag_count_plus = add i64 %lag_count, 1
  %lag_count_new = select i1 %lag_is_full, i64 %lag_count, i64 %lag_count_plus
  store i64 %lag_count_new, ptr %lag_count_pp, align 8
  %out_ptr8 = getelementptr double, ptr %outputs, i64 2
  store double %lag_lagged, ptr %out_ptr8, align 8
  %mid_sum9 = fadd double %bid, %ask
  %mid10 = fmul double %mid_sum9, 5.000000e-01
  %sma_state_ptr11 = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_base, i64 4
  %sma_buf_pp12 = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_state_ptr11, i32 0, i32 0
  %sma_sum_pp13 = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_state_ptr11, i32 0, i32 1
  %sma_head_pp14 = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_state_ptr11, i32 0, i32 2
  %sma_count_pp15 = getelementptr inbounds { ptr, double, i64, i64, i64 }, ptr %sma_state_ptr11, i32 0, i32 3
  %sma_buf16 = load ptr, ptr %sma_buf_pp12, align 8
  %sma_sum17 = load double, ptr %sma_sum_pp13, align 8
  %sma_head18 = load i64, ptr %sma_head_pp14, align 8
  %sma_count19 = load i64, ptr %sma_count_pp15, align 8
  %sma_was_full20 = icmp eq i64 %sma_count19, 20
  %sma_slot21 = getelementptr inbounds double, ptr %sma_buf16, i64 %sma_head18
  %sma_old22 = load double, ptr %sma_slot21, align 8
  %sma_old_or_zero23 = select i1 %sma_was_full20, double %sma_old22, double 0.000000e+00
  %sma_sum_plus_x24 = fadd double %sma_sum17, %mid10
  %sma_sum_new25 = fsub double %sma_sum_plus_x24, %sma_old_or_zero23
  %sma_count_plus_one26 = add i64 %sma_count19, 1
  %sma_count_new27 = select i1 %sma_was_full20, i64 %sma_count19, i64 %sma_count_plus_one26
  store double %mid10, ptr %sma_slot21, align 8
  %sma_head_plus28 = add i64 %sma_head18, 1
  %sma_head_new29 = urem i64 %sma_head_plus28, 20
  store double %sma_sum_new25, ptr %sma_sum_pp13, align 8
  store i64 %sma_head_new29, ptr %sma_head_pp14, align 8
  store i64 %sma_count_new27, ptr %sma_count_pp15, align 8
  %sma_is_full30 = icmp eq i64 %sma_count_new27, 20
  %sma_mean31 = fdiv double %sma_sum_new25, 2.000000e+01
  %sma_out32 = select i1 %sma_is_full30, double %sma_mean31, double 0x7FF8000000000000
  %mid_sum33 = fadd double %bid, %ask
  %mid34 = fmul double %mid_sum33, 5.000000e-01
  %ema_state_ptr35 = getelementptr inbounds { double, i64 }, ptr %ema_base, i64 5
  %ema_val_pp36 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr35, i32 0, i32 0
  %ema_init_pp37 = getelementptr inbounds { double, i64 }, ptr %ema_state_ptr35, i32 0, i32 1
  %ema_prev38 = load double, ptr %ema_val_pp36, align 8
  %ema_init39 = load i64, ptr %ema_init_pp37, align 8
  %ema_is_init40 = icmp ne i64 %ema_init39, 0
  %ema_ax41 = fmul double 1.250000e-01, %mid34
  %ema_bp42 = fmul double 8.750000e-01, %ema_prev38
  %ema_blended43 = fadd double %ema_ax41, %ema_bp42
  %ema_new44 = select i1 %ema_is_init40, double %ema_blended43, double %mid34
  store double %ema_new44, ptr %ema_val_pp36, align 8
  store i64 1, ptr %ema_init_pp37, align 8
  %sub = fsub double %sma_out32, %ema_new44
  %mid_sum45 = fadd double %bid, %ask
  %mid46 = fmul double %mid_sum45, 5.000000e-01
  %lag_state_ptr47 = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_base, i64 6
  %lag_buf_pp48 = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_state_ptr47, i32 0, i32 0
  %lag_head_pp49 = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_state_ptr47, i32 0, i32 1
  %lag_count_pp50 = getelementptr inbounds { ptr, i64, i64, i64 }, ptr %lag_state_ptr47, i32 0, i32 2
  %lag_buf51 = load ptr, ptr %lag_buf_pp48, align 8
  %lag_head52 = load i64, ptr %lag_head_pp49, align 8
  %lag_count53 = load i64, ptr %lag_count_pp50, align 8
  %lag_is_full54 = icmp eq i64 %lag_count53, 7
  %lag_slot55 = getelementptr inbounds double, ptr %lag_buf51, i64 %lag_head52
  %lag_lagged_buf56 = load double, ptr %lag_slot55, align 8
  %lag_lagged57 = select i1 %lag_is_full54, double %lag_lagged_buf56, double 0x7FF8000000000000
  store double %mid46, ptr %lag_slot55, align 8
  %lag_head_plus58 = add i64 %lag_head52, 1
  %lag_head_new59 = urem i64 %lag_head_plus58, 7
  store i64 %lag_head_new59, ptr %lag_head_pp49, align 8
  %lag_count_plus60 = add i64 %lag_count53, 1
  %lag_count_new61 = select i1 %lag_is_full54, i64 %lag_count53, i64 %lag_count_plus60
  store i64 %lag_count_new61, ptr %lag_count_pp50, align 8
  %add = fadd double %sub, %lag_lagged57
  %out_ptr62 = getelementptr double, ptr %outputs, i64 3
  store double %add, ptr %out_ptr62, align 8
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

declare ptr @jit_rt_sma_lowered_base(ptr)

declare ptr @jit_rt_ema_lowered_base(ptr)

declare ptr @jit_rt_lag_lowered_base(ptr)
