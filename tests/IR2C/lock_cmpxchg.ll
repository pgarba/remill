; ModuleID = '/tmp/sice-lift-251237/lift.mem.ll'
source_filename = "lifted_code"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu-elf"

%union.vec128_t = type { %struct.uint128v1_t }
%struct.uint128v1_t = type { [1 x i128] }

define ptr @sub_401040(ptr %PC, ptr noalias %memory, ptr initializes((0, 8)) %NEXT_PC, i64 %RAX, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, ptr noalias %RSP, ptr noalias %RBP, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 %CF, i8 %PF, i8 %AF, i8 %ZF, i8 %SF, i8 %DF, i8 %OF, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7) local_unnamed_addr {
_ZN12_GLOBAL__N_111CMPXCHG_EAXI3MnWIjE2MnIjE2RnIjLb1EEEEP6MemoryS8_R5StateT_T0_T1__flat.exit:
  %pc_init = load i64, ptr %PC, align 8
  %0 = add i64 %pc_init, 4
  store i64 %0, ptr %NEXT_PC, align 8
  fence seq_cst
  %1 = trunc i64 %RCX to i32
  %STATE_LOCAL.sroa.61.2216.extract.trunc.i = trunc i64 %RAX to i32
  %p.i = inttoptr i64 %RDI to ptr
  %r.i = cmpxchg ptr %p.i, i32 %STATE_LOCAL.sroa.61.2216.extract.trunc.i, i32 %1 seq_cst seq_cst, align 4
  %o.i = extractvalue { i32, i1 } %r.i, 0
  %2 = extractvalue { i32, i1 } %r.i, 1
  %3 = zext i32 %o.i to i64
  %STATE_LOCAL.sroa.61.0.i = select i1 %2, i64 %RAX, i64 %3
  fence seq_cst
  %4 = sub i32 %STATE_LOCAL.sroa.61.2216.extract.trunc.i, %o.i
  %5 = xor i32 %4, %STATE_LOCAL.sroa.61.2216.extract.trunc.i
  %6 = lshr i32 %5, 31
  %7 = xor i32 %o.i, %STATE_LOCAL.sroa.61.2216.extract.trunc.i
  %8 = lshr i32 %7, 31
  %9 = add nuw nsw i32 %6, %8
  %10 = icmp eq i32 %9, 2
  %11 = zext i1 %10 to i8
  %12 = lshr i32 %4, 31
  %13 = trunc nuw nsw i32 %12 to i8
  %14 = xor i32 %5, %o.i
  %15 = trunc i32 %14 to i8
  %16 = lshr i8 %15, 4
  %17 = and i8 %16, 1
  %18 = trunc i32 %4 to i8
  %19 = tail call range(i8 0, 9) i8 @llvm.ctpop.i8(i8 %18)
  %20 = and i8 %19, 1
  %21 = xor i8 %20, 1
  %22 = icmp ugt i32 %o.i, %STATE_LOCAL.sroa.61.2216.extract.trunc.i
  %23 = zext i1 %22 to i8
  %24 = zext i1 %2 to i8
  %25 = load i64, ptr %NEXT_PC, align 8
  store i64 %25, ptr %PC, align 8
  %rsp_i64 = ptrtoint ptr %RSP to i64
  %rsp_i648 = ptrtoint ptr %RBP to i64
  %26 = tail call ptr @__remill_flat_jump(ptr nonnull %PC, ptr %memory, ptr nonnull %NEXT_PC, i64 %STATE_LOCAL.sroa.61.0.i, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, i64 %rsp_i64, i64 %rsp_i648, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 %23, i8 %21, i8 %17, i8 %24, i8 %13, i8 %DF, i8 %11, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7)
  ret ptr %26
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i8 @llvm.ctpop.i8(i8) #0

; Function Attrs: mustprogress noinline nounwind optnone
declare dso_local ptr @__remill_flat_jump(ptr noundef, ptr noundef, ptr noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef) local_unnamed_addr #1

attributes #0 = { mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #1 = { mustprogress noinline nounwind optnone "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "tune-cpu"="generic" }
