; ModuleID = 'lifted_code'
source_filename = "lifted_code"
target triple = "x86_64--"

%union.vec128_t = type { %struct.uint128v1_t }
%struct.uint128v1_t = type { [1 x i128] }

define ptr @sub_10b2(ptr %PC, ptr noalias %memory, ptr %NEXT_PC, i64 %RAX, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, ptr noalias %RSP, ptr noalias %RBP, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 %CF, i8 %PF, i8 %AF, i8 %ZF, i8 %SF, i8 %DF, i8 %OF, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7) {
  %pc_init = load i64, ptr %PC, align 4
  store i64 %pc_init, ptr %NEXT_PC, align 4
  %REG_XMM0 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM0, ptr %REG_XMM0, align 16
  %REG_XMM1 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM1, ptr %REG_XMM1, align 16
  %REG_XMM2 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM2, ptr %REG_XMM2, align 16
  %REG_XMM3 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM3, ptr %REG_XMM3, align 16
  %REG_XMM4 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM4, ptr %REG_XMM4, align 16
  %REG_XMM5 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM5, ptr %REG_XMM5, align 16
  %REG_XMM6 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM6, ptr %REG_XMM6, align 16
  %REG_XMM7 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM7, ptr %REG_XMM7, align 16
  %REG_XMM8 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM8, ptr %REG_XMM8, align 16
  %REG_XMM9 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM9, ptr %REG_XMM9, align 16
  %REG_XMM10 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM10, ptr %REG_XMM10, align 16
  %REG_XMM11 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM11, ptr %REG_XMM11, align 16
  %REG_XMM12 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM12, ptr %REG_XMM12, align 16
  %REG_XMM13 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM13, ptr %REG_XMM13, align 16
  %REG_XMM14 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM14, ptr %REG_XMM14, align 16
  %REG_XMM15 = alloca <2 x i64>, align 16
  store <2 x i64> %XMM15, ptr %REG_XMM15, align 16
  %1 = load i64, ptr %NEXT_PC, align 4
  store i64 %1, ptr %PC, align 4
  %2 = add i64 %1, 5
  store i64 %2, ptr %NEXT_PC, align 4
  %3 = load i128, ptr %REG_XMM0, align 1
  %4 = load i128, ptr %REG_XMM1, align 1
  %5 = load i128, ptr %REG_XMM2, align 1
  %6 = load i128, ptr %REG_XMM3, align 1
  %7 = load i128, ptr %REG_XMM4, align 1
  %8 = load i128, ptr %REG_XMM5, align 1
  %9 = load i128, ptr %REG_XMM6, align 1
  %10 = load i128, ptr %REG_XMM7, align 1
  %11 = load i128, ptr %REG_XMM8, align 1
  %12 = load i128, ptr %REG_XMM9, align 1
  %13 = load i128, ptr %REG_XMM10, align 1
  %14 = load i128, ptr %REG_XMM11, align 1
  %15 = load i128, ptr %REG_XMM12, align 1
  %16 = load i128, ptr %REG_XMM13, align 1
  %17 = load i128, ptr %REG_XMM14, align 1
  %18 = load i128, ptr %REG_XMM15, align 1
  %19 = trunc i64 %RAX to i32
  %20 = and i32 %19, -1543503872
  %21 = icmp eq i32 %20, 0
  %22 = zext i1 %21 to i8
  %23 = lshr i32 %19, 31
  %24 = trunc nuw nsw i32 %23 to i8
  %25 = call zeroext i8 @__remill_undefined_8() #2
  store i128 %3, ptr %REG_XMM0, align 1
  store i128 %4, ptr %REG_XMM1, align 1
  store i128 %5, ptr %REG_XMM2, align 1
  store i128 %6, ptr %REG_XMM3, align 1
  store i128 %7, ptr %REG_XMM4, align 1
  store i128 %8, ptr %REG_XMM5, align 1
  store i128 %9, ptr %REG_XMM6, align 1
  store i128 %10, ptr %REG_XMM7, align 1
  store i128 %11, ptr %REG_XMM8, align 1
  store i128 %12, ptr %REG_XMM9, align 1
  store i128 %13, ptr %REG_XMM10, align 1
  store i128 %14, ptr %REG_XMM11, align 1
  store i128 %15, ptr %REG_XMM12, align 1
  store i128 %16, ptr %REG_XMM13, align 1
  store i128 %17, ptr %REG_XMM14, align 1
  store i128 %18, ptr %REG_XMM15, align 1
  %26 = load i64, ptr %NEXT_PC, align 4
  store i64 %26, ptr %PC, align 4
  %27 = add i64 %26, 6
  store i64 %27, ptr %NEXT_PC, align 4
  %28 = add i64 %26, -3175
  %29 = load i128, ptr %REG_XMM0, align 1
  %30 = load i128, ptr %REG_XMM1, align 1
  %31 = load i128, ptr %REG_XMM2, align 1
  %32 = load i128, ptr %REG_XMM3, align 1
  %33 = load i128, ptr %REG_XMM4, align 1
  %34 = load i128, ptr %REG_XMM5, align 1
  %35 = load i128, ptr %REG_XMM6, align 1
  %36 = load i128, ptr %REG_XMM7, align 1
  %37 = load i128, ptr %REG_XMM8, align 1
  %38 = load i128, ptr %REG_XMM9, align 1
  %39 = load i128, ptr %REG_XMM10, align 1
  %40 = load i128, ptr %REG_XMM11, align 1
  %41 = load i128, ptr %REG_XMM12, align 1
  %42 = load i128, ptr %REG_XMM13, align 1
  %43 = load i128, ptr %REG_XMM14, align 1
  %44 = load i128, ptr %REG_XMM15, align 1
  %45 = select i1 %21, i64 %28, i64 %27
  store i64 %45, ptr %NEXT_PC, align 8
  store i128 %29, ptr %REG_XMM0, align 1
  store i128 %30, ptr %REG_XMM1, align 1
  store i128 %31, ptr %REG_XMM2, align 1
  store i128 %32, ptr %REG_XMM3, align 1
  store i128 %33, ptr %REG_XMM4, align 1
  store i128 %34, ptr %REG_XMM5, align 1
  store i128 %35, ptr %REG_XMM6, align 1
  store i128 %36, ptr %REG_XMM7, align 1
  store i128 %37, ptr %REG_XMM8, align 1
  store i128 %38, ptr %REG_XMM9, align 1
  store i128 %39, ptr %REG_XMM10, align 1
  store i128 %40, ptr %REG_XMM11, align 1
  store i128 %41, ptr %REG_XMM12, align 1
  store i128 %42, ptr %REG_XMM13, align 1
  store i128 %43, ptr %REG_XMM14, align 1
  store i128 %44, ptr %REG_XMM15, align 1
  br i1 %21, label %46, label %49

common.ret:                                       ; preds = %49, %46
  %common.ret.op = phi ptr [ %48, %46 ], [ %51, %49 ]
  ret ptr %common.ret.op

46:                                               ; preds = %0
  %47 = load i64, ptr %NEXT_PC, align 4
  store i64 %47, ptr %PC, align 4
  %rsp_i64 = ptrtoint ptr %RSP to i64
  %rsp_i648 = ptrtoint ptr %RBP to i64
  %reg_out37 = load <2 x i64>, ptr %REG_XMM0, align 16
  %reg_out38 = load <2 x i64>, ptr %REG_XMM1, align 16
  %reg_out39 = load <2 x i64>, ptr %REG_XMM2, align 16
  %reg_out40 = load <2 x i64>, ptr %REG_XMM3, align 16
  %reg_out41 = load <2 x i64>, ptr %REG_XMM4, align 16
  %reg_out42 = load <2 x i64>, ptr %REG_XMM5, align 16
  %reg_out43 = load <2 x i64>, ptr %REG_XMM6, align 16
  %reg_out44 = load <2 x i64>, ptr %REG_XMM7, align 16
  %reg_out45 = load <2 x i64>, ptr %REG_XMM8, align 16
  %reg_out46 = load <2 x i64>, ptr %REG_XMM9, align 16
  %reg_out47 = load <2 x i64>, ptr %REG_XMM10, align 16
  %reg_out48 = load <2 x i64>, ptr %REG_XMM11, align 16
  %reg_out49 = load <2 x i64>, ptr %REG_XMM12, align 16
  %reg_out50 = load <2 x i64>, ptr %REG_XMM13, align 16
  %reg_out51 = load <2 x i64>, ptr %REG_XMM14, align 16
  %reg_out52 = load <2 x i64>, ptr %REG_XMM15, align 16
  %48 = tail call ptr @__remill_flat_jump(ptr nonnull %PC, ptr %memory, ptr nonnull %NEXT_PC, i64 %RAX, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, i64 %rsp_i64, i64 %rsp_i648, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 0, i8 1, i8 %25, i8 %22, i8 %24, i8 %DF, i8 0, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %reg_out37, <2 x i64> %reg_out38, <2 x i64> %reg_out39, <2 x i64> %reg_out40, <2 x i64> %reg_out41, <2 x i64> %reg_out42, <2 x i64> %reg_out43, <2 x i64> %reg_out44, <2 x i64> %reg_out45, <2 x i64> %reg_out46, <2 x i64> %reg_out47, <2 x i64> %reg_out48, <2 x i64> %reg_out49, <2 x i64> %reg_out50, <2 x i64> %reg_out51, <2 x i64> %reg_out52, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7)
  br label %common.ret

49:                                               ; preds = %0
  %50 = load i64, ptr %NEXT_PC, align 4
  store i64 %50, ptr %PC, align 4
  %rsp_i6468 = ptrtoint ptr %RSP to i64
  %rsp_i6470 = ptrtoint ptr %RBP to i64
  %reg_out99 = load <2 x i64>, ptr %REG_XMM0, align 16
  %reg_out100 = load <2 x i64>, ptr %REG_XMM1, align 16
  %reg_out101 = load <2 x i64>, ptr %REG_XMM2, align 16
  %reg_out102 = load <2 x i64>, ptr %REG_XMM3, align 16
  %reg_out103 = load <2 x i64>, ptr %REG_XMM4, align 16
  %reg_out104 = load <2 x i64>, ptr %REG_XMM5, align 16
  %reg_out105 = load <2 x i64>, ptr %REG_XMM6, align 16
  %reg_out106 = load <2 x i64>, ptr %REG_XMM7, align 16
  %reg_out107 = load <2 x i64>, ptr %REG_XMM8, align 16
  %reg_out108 = load <2 x i64>, ptr %REG_XMM9, align 16
  %reg_out109 = load <2 x i64>, ptr %REG_XMM10, align 16
  %reg_out110 = load <2 x i64>, ptr %REG_XMM11, align 16
  %reg_out111 = load <2 x i64>, ptr %REG_XMM12, align 16
  %reg_out112 = load <2 x i64>, ptr %REG_XMM13, align 16
  %reg_out113 = load <2 x i64>, ptr %REG_XMM14, align 16
  %reg_out114 = load <2 x i64>, ptr %REG_XMM15, align 16
  %51 = tail call ptr @__remill_flat_jump(ptr nonnull %PC, ptr %memory, ptr nonnull %NEXT_PC, i64 %RAX, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, i64 %rsp_i6468, i64 %rsp_i6470, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 0, i8 1, i8 %25, i8 %22, i8 %24, i8 %DF, i8 0, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %reg_out99, <2 x i64> %reg_out100, <2 x i64> %reg_out101, <2 x i64> %reg_out102, <2 x i64> %reg_out103, <2 x i64> %reg_out104, <2 x i64> %reg_out105, <2 x i64> %reg_out106, <2 x i64> %reg_out107, <2 x i64> %reg_out108, <2 x i64> %reg_out109, <2 x i64> %reg_out110, <2 x i64> %reg_out111, <2 x i64> %reg_out112, <2 x i64> %reg_out113, <2 x i64> %reg_out114, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7)
  br label %common.ret
}

; Function Attrs: mustprogress noduplicate nofree noinline nosync nounwind optnone readnone willreturn memory(none)
declare zeroext i8 @__remill_undefined_8() #0

; Function Attrs: mustprogress noinline nounwind optnone
declare dso_local ptr @__remill_flat_jump(ptr noundef, ptr noundef, ptr noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef) #1

attributes #0 = { mustprogress noduplicate nofree noinline nosync nounwind optnone readnone willreturn memory(none) "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "tune-cpu"="generic" }
attributes #1 = { mustprogress noinline nounwind optnone "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "tune-cpu"="generic" }
attributes #2 = { alwaysinline nobuiltin nounwind willreturn memory(none) "no-builtins" }
