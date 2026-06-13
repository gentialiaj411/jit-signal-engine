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
  %mid_sum = fadd <4 x double> %bid_lane366, %ask_lane394
  %mid = fmul <4 x double> %mid_sum, <double 5.000000e-01, double 5.000000e-01, double 5.000000e-01, double 5.000000e-01>
  store <4 x double> %mid, ptr %outputs, align 8
  %mid_sum95 = fadd <4 x double> %bid_lane3, %ask_lane3
  %mid96 = fmul <4 x double> %mid_sum95, <double 5.000000e-01, double 5.000000e-01, double 5.000000e-01, double 5.000000e-01>
  %out_ptr_sig1 = getelementptr inbounds double, ptr %outputs, i64 4
  store <4 x double> %mid96, ptr %out_ptr_sig1, align 8
  %sub = fsub <4 x double> %ask_lane394, %bid_lane366
  %out_ptr_sig2 = getelementptr inbounds double, ptr %outputs, i64 8
  store <4 x double> %sub, ptr %out_ptr_sig2, align 8
  %sub97 = fsub <4 x double> %ask_lane3, %bid_lane3
  %out_ptr_sig3 = getelementptr inbounds double, ptr %outputs, i64 12
  store <4 x double> %sub97, ptr %out_ptr_sig3, align 8
  %mul = fmul <4 x double> %mid, %mid
  %mul106 = fmul <4 x double> %mid96, %mid96
  %add = fadd <4 x double> %mul106, %mul
  %mul109 = fmul <4 x double> %sub, %sub
  %add110 = fadd <4 x double> %mul109, %add
  %mul113 = fmul <4 x double> %sub97, %sub97
  %add114 = fadd <4 x double> %mul113, %add110
  %out_ptr_sig4 = getelementptr inbounds double, ptr %outputs, i64 16
  store <4 x double> %add114, ptr %out_ptr_sig4, align 8
  %mul119 = fmul <4 x double> %mid96, %mid
  %mul122 = fmul <4 x double> %sub97, %sub
  %sub123 = fsub <4 x double> %mul119, %mul122
  %sub128 = fsub <4 x double> %mid, %mid96
  %sub131 = fsub <4 x double> %sub, %sub97
  %mul132 = fmul <4 x double> %sub131, %sub128
  %add133 = fadd <4 x double> %sub123, %mul132
  %out_ptr_sig5 = getelementptr inbounds double, ptr %outputs, i64 20
  store <4 x double> %add133, ptr %out_ptr_sig5, align 8
  %add153 = fadd <4 x double> %add114, <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
  %sqrt = tail call <4 x double> @llvm.sqrt.v4f64(<4 x double> %add153)
  %mul192 = fmul <4 x double> %add133, %add133
  %add193 = fadd <4 x double> %mul192, <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
  %sqrt194 = tail call <4 x double> @llvm.sqrt.v4f64(<4 x double> %add193)
  %sub195 = fsub <4 x double> %sqrt, %sqrt194
  %out_ptr_sig6 = getelementptr inbounds double, ptr %outputs, i64 24
  store <4 x double> %sub195, ptr %out_ptr_sig6, align 8
  %add200 = fadd <4 x double> %mid96, %mid
  %mul206 = fmul <4 x double> %add200, %add200
  %mul217 = fmul <4 x double> %sub128, %sub128
  %sub218 = fsub <4 x double> %mul206, %mul217
  %out_ptr_sig7 = getelementptr inbounds double, ptr %outputs, i64 28
  store <4 x double> %sub218, ptr %out_ptr_sig7, align 8
  %add242 = fadd <4 x double> %sub218, <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
  %sqrt243 = tail call <4 x double> @llvm.sqrt.v4f64(<4 x double> %add242)
  %abs = tail call <4 x double> @llvm.fabs.v4f64(<4 x double> %add133)
  %add263 = fadd <4 x double> %abs, <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
  %sqrt264 = tail call <4 x double> @llvm.sqrt.v4f64(<4 x double> %add263)
  %add265 = fadd <4 x double> %sqrt243, %sqrt264
  %out_ptr_sig8 = getelementptr inbounds double, ptr %outputs, i64 32
  store <4 x double> %add265, ptr %out_ptr_sig8, align 8
  %mul348 = fmul <4 x double> %add114, %sub195
  %mul416 = fmul <4 x double> %add133, %add265
  %sub417 = fsub <4 x double> %mul348, %mul416
  %add441 = fadd <4 x double> %sub218, %sub417
  %out_ptr_sig9 = getelementptr inbounds double, ptr %outputs, i64 36
  store <4 x double> %add441, ptr %out_ptr_sig9, align 8
  %mul553 = fmul <4 x double> %sub195, %add265
  %mul596 = fmul <4 x double> %sub218, %add114
  %add597 = fadd <4 x double> %mul596, %mul553
  %sub617 = fsub <4 x double> %add597, %add133
  %out_ptr_sig10 = getelementptr inbounds double, ptr %outputs, i64 40
  store <4 x double> %sub617, ptr %out_ptr_sig10, align 8
  %abs794 = tail call <4 x double> @llvm.fabs.v4f64(<4 x double> %add441)
  %add795 = fadd <4 x double> %abs794, <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
  %sqrt796 = tail call <4 x double> @llvm.sqrt.v4f64(<4 x double> %add795)
  %abs973 = tail call <4 x double> @llvm.fabs.v4f64(<4 x double> %sub617)
  %add974 = fadd <4 x double> %abs973, <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
  %sqrt975 = tail call <4 x double> @llvm.sqrt.v4f64(<4 x double> %add974)
  %add976 = fadd <4 x double> %sqrt796, %sqrt975
  %out_ptr_sig11 = getelementptr inbounds double, ptr %outputs, i64 44
  store <4 x double> %add976, ptr %out_ptr_sig11, align 8
  %mul1329 = fmul <4 x double> %add441, %sub617
  %mul2048 = fmul <4 x double> %add976, %add976
  %sub2049 = fsub <4 x double> %mul1329, %mul2048
  %out_ptr_sig12 = getelementptr inbounds double, ptr %outputs, i64 48
  store <4 x double> %sub2049, ptr %out_ptr_sig12, align 8
  %abs3123 = tail call <4 x double> @llvm.fabs.v4f64(<4 x double> %sub2049)
  %add3124 = fadd <4 x double> %abs3123, <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
  %sqrt3125 = tail call <4 x double> @llvm.sqrt.v4f64(<4 x double> %add3124)
  %mul3126 = fmul <4 x double> %sqrt3125, <double 5.000000e-01, double 5.000000e-01, double 5.000000e-01, double 5.000000e-01>
  %sub3662 = fsub <4 x double> %add976, %add441
  %abs3663 = tail call <4 x double> @llvm.fabs.v4f64(<4 x double> %sub3662)
  %mul3664 = fmul <4 x double> %abs3663, <double 2.500000e-01, double 2.500000e-01, double 2.500000e-01, double 2.500000e-01>
  %add3665 = fadd <4 x double> %mul3664, %mul3126
  %out_ptr_sig13 = getelementptr inbounds double, ptr %outputs, i64 52
  store <4 x double> %add3665, ptr %out_ptr_sig13, align 8
  %abs4019 = tail call <4 x double> @llvm.fabs.v4f64(<4 x double> %mul1329)
  %add4020 = fadd <4 x double> %abs4019, <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
  %sqrt4021 = tail call <4 x double> @llvm.sqrt.v4f64(<4 x double> %add4020)
  %mul5096 = fmul <4 x double> %abs3123, <double 1.250000e-01, double 1.250000e-01, double 1.250000e-01, double 1.250000e-01>
  %sub5097 = fsub <4 x double> %sqrt4021, %mul5096
  %out_ptr_sig14 = getelementptr inbounds double, ptr %outputs, i64 56
  store <4 x double> %sub5097, ptr %out_ptr_sig14, align 8
  %sub8146 = fsub <4 x double> %add3665, %sub5097
  %mul8147 = fmul <4 x double> %sub8146, <double 5.000000e-01, double 5.000000e-01, double 5.000000e-01, double 5.000000e-01>
  %add8500 = fadd <4 x double> %add441, %sub617
  %mul8501 = fmul <4 x double> %add8500, <double 1.250000e-01, double 1.250000e-01, double 1.250000e-01, double 1.250000e-01>
  %add8502 = fadd <4 x double> %mul8501, %mul8147
  %out_ptr_sig15 = getelementptr inbounds double, ptr %outputs, i64 60
  store <4 x double> %add8502, ptr %out_ptr_sig15, align 8
  ret void
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare <4 x double> @llvm.sqrt.v4f64(<4 x double>) #1

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare <4 x double> @llvm.fabs.v4f64(<4 x double>) #1

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(read, argmem: readwrite, inaccessiblemem: none) }
attributes #1 = { mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none) }
