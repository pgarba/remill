; ModuleID = 'lifted_code'
source_filename = "lifted_code"
target triple = "x86_64--"

%union.vec128_t = type { %struct.uint128v1_t }
%struct.uint128v1_t = type { [1 x i128] }

define ptr @sub_1bdc(ptr %PC, ptr noalias %memory, ptr %NEXT_PC, i64 %RAX, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, ptr noalias %RSP, ptr noalias %RBP, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 %CF, i8 %PF, i8 %AF, i8 %ZF, i8 %SF, i8 %DF, i8 %OF, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7) {
  %pc_init = load i64, ptr %PC, align 4
  store i64 %pc_init, ptr %NEXT_PC, align 4
  store i64 %pc_init, ptr %PC, align 4
  %1 = add i64 %pc_init, 5
  store i64 %1, ptr %NEXT_PC, align 4
  %2 = add i64 %pc_init, -6009
  store i64 %2, ptr %NEXT_PC, align 8
  store i64 %2, ptr %PC, align 4
  %rsp_i64 = ptrtoint ptr %RSP to i64
  %rsp_i648 = ptrtoint ptr %RBP to i64
  %3 = tail call ptr @__remill_flat_jump(ptr nonnull %PC, ptr %memory, ptr nonnull %NEXT_PC, i64 %RAX, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, i64 %rsp_i64, i64 %rsp_i648, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %2, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 %CF, i8 %PF, i8 %AF, i8 %ZF, i8 %SF, i8 %DF, i8 %OF, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7)
  ret ptr %3
}

; Function Attrs: mustprogress noinline nounwind optnone
declare dso_local ptr @__remill_flat_jump(ptr noundef, ptr noundef, ptr noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef) #0

attributes #0 = { mustprogress noinline nounwind optnone "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "tune-cpu"="generic" }
