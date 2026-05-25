; ModuleID = 'jit_signal_program_vec_module'
source_filename = "jit_signal_program_vec_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(read, argmem: readwrite, inaccessiblemem: none)
define void @signal_program_vec4_func_1(ptr nocapture readonly %per_lane_market, ptr nocapture readnone %arena, i32 %base_symbol, ptr nocapture writeonly %outputs) local_unnamed_addr #0 {
entry:
  %bid_mst = load ptr, ptr %per_lane_market, align 8
  %instrument_ptr = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %bid_mst, i64 0, i64 1
  %bid_scalar = load double, ptr %instrument_ptr, align 8
  %bid_lane0 = insertelement <4 x double> poison, double %bid_scalar, i64 0
  %bid_slot_ptr1 = getelementptr inbounds ptr, ptr %per_lane_market, i64 1
  %bid_mst2 = load ptr, ptr %bid_slot_ptr1, align 8
  %instrument_ptr4 = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %bid_mst2, i64 0, i64 1
  %bid_scalar6 = load double, ptr %instrument_ptr4, align 8
  %bid_lane1 = insertelement <4 x double> %bid_lane0, double %bid_scalar6, i64 1
  %bid_slot_ptr7 = getelementptr inbounds ptr, ptr %per_lane_market, i64 2
  %bid_mst8 = load ptr, ptr %bid_slot_ptr7, align 8
  %instrument_ptr10 = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %bid_mst8, i64 0, i64 1
  %bid_scalar12 = load double, ptr %instrument_ptr10, align 8
  %bid_lane2 = insertelement <4 x double> %bid_lane1, double %bid_scalar12, i64 2
  %bid_slot_ptr13 = getelementptr inbounds ptr, ptr %per_lane_market, i64 3
  %bid_mst14 = load ptr, ptr %bid_slot_ptr13, align 8
  %instrument_ptr16 = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %bid_mst14, i64 0, i64 1
  %bid_scalar18 = load double, ptr %instrument_ptr16, align 8
  %bid_lane3 = insertelement <4 x double> %bid_lane2, double %bid_scalar18, i64 3
  %ask_field_ptr = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %bid_mst, i64 0, i64 1, i32 1
  %ask_scalar = load double, ptr %ask_field_ptr, align 8
  %ask_lane0 = insertelement <4 x double> poison, double %ask_scalar, i64 0
  %ask_field_ptr25 = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %bid_mst2, i64 0, i64 1, i32 1
  %ask_scalar26 = load double, ptr %ask_field_ptr25, align 8
  %ask_lane1 = insertelement <4 x double> %ask_lane0, double %ask_scalar26, i64 1
  %ask_field_ptr31 = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %bid_mst8, i64 0, i64 1, i32 1
  %ask_scalar32 = load double, ptr %ask_field_ptr31, align 8
  %ask_lane2 = insertelement <4 x double> %ask_lane1, double %ask_scalar32, i64 2
  %ask_field_ptr37 = getelementptr inbounds [1024 x { double, double, double, double, i64, [24 x i8] }], ptr %bid_mst14, i64 0, i64 1, i32 1
  %ask_scalar38 = load double, ptr %ask_field_ptr37, align 8
  %ask_lane3 = insertelement <4 x double> %ask_lane2, double %ask_scalar38, i64 3
  %bid_scalar44 = load double, ptr %bid_mst, align 8
  %bid_lane045 = insertelement <4 x double> poison, double %bid_scalar44, i64 0
  %bid_scalar51 = load double, ptr %bid_mst2, align 8
  %bid_lane152 = insertelement <4 x double> %bid_lane045, double %bid_scalar51, i64 1
  %bid_scalar58 = load double, ptr %bid_mst8, align 8
  %bid_lane259 = insertelement <4 x double> %bid_lane152, double %bid_scalar58, i64 2
  %bid_scalar65 = load double, ptr %bid_mst14, align 8
  %bid_lane366 = insertelement <4 x double> %bid_lane259, double %bid_scalar65, i64 3
  %ask_field_ptr71 = getelementptr inbounds { double, double, double, double, i64, [24 x i8] }, ptr %bid_mst, i64 0, i32 1
  %ask_scalar72 = load double, ptr %ask_field_ptr71, align 8
  %ask_lane073 = insertelement <4 x double> poison, double %ask_scalar72, i64 0
  %ask_field_ptr78 = getelementptr inbounds { double, double, double, double, i64, [24 x i8] }, ptr %bid_mst2, i64 0, i32 1
  %ask_scalar79 = load double, ptr %ask_field_ptr78, align 8
  %ask_lane180 = insertelement <4 x double> %ask_lane073, double %ask_scalar79, i64 1
  %ask_field_ptr85 = getelementptr inbounds { double, double, double, double, i64, [24 x i8] }, ptr %bid_mst8, i64 0, i32 1
  %ask_scalar86 = load double, ptr %ask_field_ptr85, align 8
  %ask_lane287 = insertelement <4 x double> %ask_lane180, double %ask_scalar86, i64 2
  %ask_field_ptr92 = getelementptr inbounds { double, double, double, double, i64, [24 x i8] }, ptr %bid_mst14, i64 0, i32 1
  %ask_scalar93 = load double, ptr %ask_field_ptr92, align 8
  %ask_lane394 = insertelement <4 x double> %ask_lane287, double %ask_scalar93, i64 3
  %mid_sum = fadd <4 x double> %bid_lane3, %ask_lane3
  %mid = fmul <4 x double> %mid_sum, <double 5.000000e-01, double 5.000000e-01, double 5.000000e-01, double 5.000000e-01>
  %mid_sum95 = fadd <4 x double> %bid_lane366, %ask_lane394
  %mid96 = fmul <4 x double> %mid_sum95, <double 5.000000e-01, double 5.000000e-01, double 5.000000e-01, double 5.000000e-01>
  %sub = fsub <4 x double> %mid, %mid96
  store <4 x double> %sub, ptr %outputs, align 8
  %add = fadd <4 x double> %mid, %mid96
  %out_ptr_sig1 = getelementptr inbounds double, ptr %outputs, i64 4
  store <4 x double> %add, ptr %out_ptr_sig1, align 8
  %sub101 = fsub <4 x double> %ask_lane3, %bid_lane3
  %mul = fmul <4 x double> %sub101, <double 5.000000e-01, double 5.000000e-01, double 5.000000e-01, double 5.000000e-01>
  %sub102 = fsub <4 x double> %ask_lane394, %bid_lane366
  %mul103 = fmul <4 x double> %sub102, <double 5.000000e-01, double 5.000000e-01, double 5.000000e-01, double 5.000000e-01>
  %add104 = fadd <4 x double> %mul, %mul103
  %out_ptr_sig2 = getelementptr inbounds double, ptr %outputs, i64 8
  store <4 x double> %add104, ptr %out_ptr_sig2, align 8
  %mul115 = fmul <4 x double> %sub, %add
  %sub121 = fsub <4 x double> %mul115, %add104
  %out_ptr_sig3 = getelementptr inbounds double, ptr %outputs, i64 12
  store <4 x double> %sub121, ptr %out_ptr_sig3, align 8
  %add144 = fadd <4 x double> %sub, %sub121
  %sub167 = fsub <4 x double> %sub121, %add
  %mul168 = fmul <4 x double> %add144, %sub167
  %out_ptr_sig4 = getelementptr inbounds double, ptr %outputs, i64 16
  store <4 x double> %mul168, ptr %out_ptr_sig4, align 8
  %abs = tail call <4 x double> @llvm.fabs.v4f64(<4 x double> %sub121)
  %abs233 = tail call <4 x double> @llvm.fabs.v4f64(<4 x double> %mul168)
  %sub234 = fsub <4 x double> %abs, %abs233
  %out_ptr_sig5 = getelementptr inbounds double, ptr %outputs, i64 20
  store <4 x double> %sub234, ptr %out_ptr_sig5, align 8
  %mul282 = fmul <4 x double> %mul168, <double 2.500000e-01, double 2.500000e-01, double 2.500000e-01, double 2.500000e-01>
  %mul350 = fmul <4 x double> %sub234, <double 7.500000e-01, double 7.500000e-01, double 7.500000e-01, double 7.500000e-01>
  %add351 = fadd <4 x double> %mul282, %mul350
  %out_ptr_sig6 = getelementptr inbounds double, ptr %outputs, i64 24
  store <4 x double> %add351, ptr %out_ptr_sig6, align 8
  %add474 = fadd <4 x double> %sub, %add351
  %add597 = fadd <4 x double> %add, %add351
  %sub598 = fsub <4 x double> %add474, %add597
  %out_ptr_sig7 = getelementptr inbounds double, ptr %outputs, i64 28
  store <4 x double> %sub598, ptr %out_ptr_sig7, align 8
  %mul846 = fmul <4 x double> %sub598, <double 5.000000e-01, double 5.000000e-01, double 5.000000e-01, double 5.000000e-01>
  %sub857 = fsub <4 x double> %sub, %add
  %mul858 = fmul <4 x double> %sub857, <double 5.000000e-01, double 5.000000e-01, double 5.000000e-01, double 5.000000e-01>
  %add859 = fadd <4 x double> %mul858, %mul846
  %out_ptr_sig8 = getelementptr inbounds double, ptr %outputs, i64 32
  store <4 x double> %add859, ptr %out_ptr_sig8, align 8
  ret void
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare <4 x double> @llvm.fabs.v4f64(<4 x double>) #1

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(read, argmem: readwrite, inaccessiblemem: none) }
attributes #1 = { mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none) }
