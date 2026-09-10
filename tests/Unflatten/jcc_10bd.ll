; ModuleID = 'lifted_code'
source_filename = "lifted_code"
target triple = "x86_64--"

%union.vec128_t = type { %struct.uint128v1_t }
%struct.uint128v1_t = type { [1 x i128] }

define ptr @sub_10bd(ptr %PC, ptr noalias %memory, ptr %NEXT_PC, i64 %RAX, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, ptr noalias %RSP, ptr noalias %RBP, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 %CF, i8 %PF, i8 %AF, i8 %ZF, i8 %SF, i8 %DF, i8 %OF, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7) {
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
  %rsp_slot = getelementptr i8, ptr %RSP, i64 108
  %stack_load = load i32, ptr %rsp_slot, align 4
  %19 = add i32 %stack_load, -2
  %20 = icmp ult i32 %stack_load, 2
  %21 = zext i1 %20 to i8
  %22 = trunc i32 %19 to i8
  %23 = call range(i8 0, 9) i8 @llvm.ctpop.i8(i8 %22)
  %24 = and i8 %23, 1
  %25 = xor i8 %24, 1
  %26 = xor i32 %19, %stack_load
  %27 = trunc i32 %26 to i8
  %28 = lshr i8 %27, 4
  %29 = and i8 %28, 1
  %30 = icmp eq i32 %stack_load, 2
  %31 = zext i1 %30 to i8
  %32 = lshr i32 %19, 31
  %33 = trunc nuw nsw i32 %32 to i8
  %34 = lshr i32 %stack_load, 31
  %35 = lshr i32 %26, 31
  %36 = add nuw nsw i32 %35, %34
  %37 = icmp eq i32 %36, 2
  %38 = zext i1 %37 to i8
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
  %39 = load i64, ptr %NEXT_PC, align 4
  store i64 %39, ptr %PC, align 4
  %40 = add i64 %39, 6
  store i64 %40, ptr %NEXT_PC, align 4
  %41 = add i64 %39, 430
  %42 = load i128, ptr %REG_XMM0, align 1
  %43 = load i128, ptr %REG_XMM1, align 1
  %44 = load i128, ptr %REG_XMM2, align 1
  %45 = load i128, ptr %REG_XMM3, align 1
  %46 = load i128, ptr %REG_XMM4, align 1
  %47 = load i128, ptr %REG_XMM5, align 1
  %48 = load i128, ptr %REG_XMM6, align 1
  %49 = load i128, ptr %REG_XMM7, align 1
  %50 = load i128, ptr %REG_XMM8, align 1
  %51 = load i128, ptr %REG_XMM9, align 1
  %52 = load i128, ptr %REG_XMM10, align 1
  %53 = load i128, ptr %REG_XMM11, align 1
  %54 = load i128, ptr %REG_XMM12, align 1
  %55 = load i128, ptr %REG_XMM13, align 1
  %56 = load i128, ptr %REG_XMM14, align 1
  %57 = load i128, ptr %REG_XMM15, align 1
  %58 = select i1 %30, i64 %41, i64 %40
  store i64 %58, ptr %NEXT_PC, align 8
  store i128 %42, ptr %REG_XMM0, align 1
  store i128 %43, ptr %REG_XMM1, align 1
  store i128 %44, ptr %REG_XMM2, align 1
  store i128 %45, ptr %REG_XMM3, align 1
  store i128 %46, ptr %REG_XMM4, align 1
  store i128 %47, ptr %REG_XMM5, align 1
  store i128 %48, ptr %REG_XMM6, align 1
  store i128 %49, ptr %REG_XMM7, align 1
  store i128 %50, ptr %REG_XMM8, align 1
  store i128 %51, ptr %REG_XMM9, align 1
  store i128 %52, ptr %REG_XMM10, align 1
  store i128 %53, ptr %REG_XMM11, align 1
  store i128 %54, ptr %REG_XMM12, align 1
  store i128 %55, ptr %REG_XMM13, align 1
  store i128 %56, ptr %REG_XMM14, align 1
  store i128 %57, ptr %REG_XMM15, align 1
  br i1 %30, label %59, label %62

common.ret:                                       ; preds = %62, %59
  %common.ret.op = phi ptr [ %61, %59 ], [ %64, %62 ]
  ret ptr %common.ret.op

59:                                               ; preds = %0
  %60 = load i64, ptr %NEXT_PC, align 4
  store i64 %60, ptr %PC, align 4
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
  %61 = tail call ptr @__remill_flat_jump(ptr nonnull %PC, ptr %memory, ptr nonnull %NEXT_PC, i64 %RAX, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, i64 %rsp_i64, i64 %rsp_i648, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 %21, i8 %25, i8 %29, i8 %31, i8 %33, i8 %DF, i8 %38, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %reg_out37, <2 x i64> %reg_out38, <2 x i64> %reg_out39, <2 x i64> %reg_out40, <2 x i64> %reg_out41, <2 x i64> %reg_out42, <2 x i64> %reg_out43, <2 x i64> %reg_out44, <2 x i64> %reg_out45, <2 x i64> %reg_out46, <2 x i64> %reg_out47, <2 x i64> %reg_out48, <2 x i64> %reg_out49, <2 x i64> %reg_out50, <2 x i64> %reg_out51, <2 x i64> %reg_out52, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7)
  br label %common.ret

62:                                               ; preds = %0
  %63 = load i64, ptr %NEXT_PC, align 4
  store i64 %63, ptr %PC, align 4
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
  %64 = tail call ptr @__remill_flat_jump(ptr nonnull %PC, ptr %memory, ptr nonnull %NEXT_PC, i64 %RAX, i64 %RBX, i64 %RCX, i64 %RDX, i64 %RSI, i64 %RDI, i64 %rsp_i6468, i64 %rsp_i6470, i64 %R8, i64 %R9, i64 %R10, i64 %R11, i64 %R12, i64 %R13, i64 %R14, i64 %R15, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 %21, i8 %25, i8 %29, i8 %31, i8 %33, i8 %DF, i8 %38, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %reg_out99, <2 x i64> %reg_out100, <2 x i64> %reg_out101, <2 x i64> %reg_out102, <2 x i64> %reg_out103, <2 x i64> %reg_out104, <2 x i64> %reg_out105, <2 x i64> %reg_out106, <2 x i64> %reg_out107, <2 x i64> %reg_out108, <2 x i64> %reg_out109, <2 x i64> %reg_out110, <2 x i64> %reg_out111, <2 x i64> %reg_out112, <2 x i64> %reg_out113, <2 x i64> %reg_out114, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7)
  br label %common.ret
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i8 @llvm.ctpop.i8(i8) #0

; Function Attrs: mustprogress noinline nounwind optnone
declare dso_local ptr @__remill_flat_jump(ptr noundef, ptr noundef, ptr noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef) #1

attributes #0 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #1 = { mustprogress noinline nounwind optnone "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "tune-cpu"="generic" }
