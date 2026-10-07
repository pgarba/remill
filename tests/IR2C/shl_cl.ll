; ModuleID = '/tmp/sice-lift-251237/lift.ll'
source_filename = "lifted_code"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu-elf"

%union.vec128_t = type { %struct.uint128v1_t }
%struct.uint128v1_t = type { [1 x i128] }

define ptr @sub_401040(ptr %PC, ptr noalias %memory, ptr initializes((0, 8)) %NEXT_PC, i64 %RAX, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, ptr noalias %RSP, ptr noalias %RBP, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 %CF, i8 %PF, i8 %AF, i8 %ZF, i8 %SF, i8 %DF, i8 %OF, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7) local_unnamed_addr {
  %pc_init = load i64, ptr %PC, align 8
  %1 = add i64 %pc_init, 3
  store i64 %1, ptr %NEXT_PC, align 8
  %2 = and i64 %RCX, 63
  switch i64 %2, label %8 [
    i64 0, label %_ZN12_GLOBAL__N_13SHLI3RnWImE2RnImLb1EES4_EEP6MemoryS6_R5StateT_T0_T1__flat.exit
    i64 1, label %3
  ]

3:                                                ; preds = %0
  %4 = shl i64 %RAX, 1
  %5 = xor i64 %4, %RAX
  %6 = icmp slt i64 %5, 0
  %7 = tail call zeroext i8 @__remill_undefined_8() #3
  br label %14

8:                                                ; preds = %0
  %9 = add nsw i64 %2, -1
  %10 = shl i64 %RAX, %9
  %11 = tail call zeroext i8 @__remill_undefined_8() #3
  %12 = icmp ne i8 %11, 0
  %13 = shl i64 %10, 1
  br label %14

14:                                               ; preds = %8, %3
  %15 = phi i8 [ %11, %8 ], [ %7, %3 ]
  %16 = phi i64 [ %10, %8 ], [ %RAX, %3 ]
  %17 = phi i1 [ %12, %8 ], [ %6, %3 ]
  %18 = phi i64 [ %13, %8 ], [ %4, %3 ]
  %19 = lshr i64 %16, 63
  %20 = trunc nuw nsw i64 %19 to i8
  %21 = trunc i64 %18 to i8
  %22 = tail call range(i8 0, 8) i8 @llvm.ctpop.i8(i8 %21)
  %23 = and i8 %22, 1
  %24 = xor i8 %23, 1
  %25 = icmp eq i64 %18, 0
  %26 = zext i1 %25 to i8
  %27 = lshr i64 %18, 63
  %28 = trunc nuw nsw i64 %27 to i8
  %29 = zext i1 %17 to i8
  br label %_ZN12_GLOBAL__N_13SHLI3RnWImE2RnImLb1EES4_EEP6MemoryS6_R5StateT_T0_T1__flat.exit

_ZN12_GLOBAL__N_13SHLI3RnWImE2RnImLb1EES4_EEP6MemoryS6_R5StateT_T0_T1__flat.exit: ; preds = %14, %0
  %STATE_LOCAL.sroa.32.0.i = phi i8 [ %20, %14 ], [ %CF, %0 ]
  %STATE_LOCAL.sroa.35.0.i = phi i8 [ %24, %14 ], [ %PF, %0 ]
  %STATE_LOCAL.sroa.38.0.i = phi i8 [ %15, %14 ], [ %AF, %0 ]
  %STATE_LOCAL.sroa.41.0.i = phi i8 [ %26, %14 ], [ %ZF, %0 ]
  %STATE_LOCAL.sroa.44.0.i = phi i8 [ %28, %14 ], [ %SF, %0 ]
  %STATE_LOCAL.sroa.49.0.i = phi i8 [ %29, %14 ], [ %OF, %0 ]
  store i64 %1, ptr %PC, align 8
  %rsp_i64 = ptrtoint ptr %RSP to i64
  %rsp_i648 = ptrtoint ptr %RBP to i64
  %30 = tail call ptr @__remill_flat_jump(ptr nonnull %PC, ptr %memory, ptr nonnull %NEXT_PC, i64 %RAX, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, i64 %rsp_i64, i64 %rsp_i648, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 %STATE_LOCAL.sroa.32.0.i, i8 %STATE_LOCAL.sroa.35.0.i, i8 %STATE_LOCAL.sroa.38.0.i, i8 %STATE_LOCAL.sroa.41.0.i, i8 %STATE_LOCAL.sroa.44.0.i, i8 %DF, i8 %STATE_LOCAL.sroa.49.0.i, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7)
  ret ptr %30
}

; Function Attrs: mustprogress noduplicate nofree noinline nosync nounwind optnone willreturn memory(none)
declare zeroext i8 @__remill_undefined_8() local_unnamed_addr #0

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i8 @llvm.ctpop.i8(i8) #1

; Function Attrs: mustprogress noinline nounwind optnone
declare dso_local ptr @__remill_flat_jump(ptr noundef, ptr noundef, ptr noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef) local_unnamed_addr #2

attributes #0 = { mustprogress noduplicate nofree noinline nosync nounwind optnone willreturn memory(none) "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "tune-cpu"="generic" }
attributes #1 = { mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #2 = { mustprogress noinline nounwind optnone "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "tune-cpu"="generic" }
attributes #3 = { alwaysinline nobuiltin nounwind willreturn memory(none) "no-builtins" }
