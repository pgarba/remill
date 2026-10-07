; !LIFT trace of sample: 25 step(s) from 0x4011b9, seeded with the live registers
; lifted by the remill port's flat lifter, optimized by opt, saved 2026-10-08 00:29:13
;
; seed  flags  = 0x44 (ZF PF)
; seed  rax    = 0x0000000000000000
; seed  rcx    = 0x0000000000403df0
; seed  rdx    = 0x0000000000000024
; seed  rbx    = 0x0000000000000000
; seed  rsi    = 0x0000000000000024
; seed  rdi    = 0x0000000000402004
; seed  r8     = 0x00007ffff7e3e6a0
; seed  r9     = 0x00007ffff7e3ffe0
; seed  r10    = 0x00007fffffffd3c0
; seed  r11    = 0x00007ffff7ffca90
; seed  r12    = 0x0000000000000001
; seed  r13    = 0x0000000000403df0
; seed  r14    = 0x00007ffff7ffd000
; seed  r15    = 0x00007fffffffd4c8
; 0x4011b9  call   0x401030 <printf@plt>  [call: not lifted; rsp-=8, return address 0x4011be stored]
; 0x401030  jmp    QWORD PTR [rip+0x2fca]        # 0x404000 <printf@got.plt>  [jmp: not lifted]
; 0x401036  push   0x0
; 0x40103b  jmp    0x401020  [jmp: not lifted]
; 0x401020  push   QWORD PTR [rip+0x2fca]        # 0x403ff0
; 0x401026  jmp    QWORD PTR [rip+0x2fcc]        # 0x403ff8  [jmp: not lifted]
; 0x7ffff7fd3cf0  endbr64
; 0x7ffff7fd3cf4  push   rbx
; 0x7ffff7fd3cf5  mov    rbx, rsp
; 0x7ffff7fd3cf8  and    rsp, 0xffffffffffffffc0
; ---- segment 1 at 0x7ffff7fd3cfc: 0x7ffff7ffcc90 is out of RIP-relative reach of the previous one
; 0x7ffff7fd3cfc  sub    rsp, QWORD PTR [rip+0x28f8d]        # 0x7ffff7ffcc90 <_rtld_global_ro+464>
; 0x7ffff7fd3d03  mov    QWORD PTR [rsp], rax
; 0x7ffff7fd3d07  mov    QWORD PTR [rsp+0x8], rcx
; 0x7ffff7fd3d0c  mov    QWORD PTR [rsp+0x10], rdx
; 0x7ffff7fd3d11  mov    QWORD PTR [rsp+0x18], rsi
; 0x7ffff7fd3d16  mov    QWORD PTR [rsp+0x20], rdi
; 0x7ffff7fd3d1b  mov    QWORD PTR [rsp+0x28], r8
; 0x7ffff7fd3d20  mov    QWORD PTR [rsp+0x30], r9
; 0x7ffff7fd3d25  mov    eax, 0x800ee
; 0x7ffff7fd3d2a  xor    edx, edx
; 0x7ffff7fd3d2c  mov    QWORD PTR [rsp+0x250], rdx
; 0x7ffff7fd3d34  mov    QWORD PTR [rsp+0x258], rdx
; 0x7ffff7fd3d3c  mov    QWORD PTR [rsp+0x260], rdx
; 0x7ffff7fd3d44  mov    QWORD PTR [rsp+0x268], rdx
; 0x7ffff7fd3d4c  mov    QWORD PTR [rsp+0x270], rdx

; ModuleID = '/tmp/sice-lift-274994/lift.in.ll'
source_filename = "llvm-link"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu-elf"

%union.vec128_t.0 = type { %struct.uint128v1_t.1 }
%struct.uint128v1_t.1 = type { [1 x i128] }

define ptr @lift(ptr initializes((0, 8)) %PC, ptr noalias %memory, ptr initializes((0, 8)) %NEXT_PC, i64 "sice.seed"="0" %RAX.in, i64 "sice.seed"="0" %RBX.in, i64 "sice.seed"="4210160" %RCX.in, i64 "sice.seed"="36" %RDX.in, i64 "sice.seed"="36" %RSI.in, i64 "sice.seed"="4202500" %RDI.in, ptr noalias initializes((-32, 0)) %RSP, ptr noalias %RBP, i64 "sice.seed"="140737352296096" %R8.in, i64 "sice.seed"="140737352302560" %R9.in, i64 "sice.seed"="140737488344000" %R10.in, i64 "sice.seed"="140737354123920" %R11.in, i64 "sice.seed"="1" %R12.in, i64 "sice.seed"="4210160" %R13.in, i64 "sice.seed"="140737354125312" %R14.in, i64 "sice.seed"="140737488344264" %R15.in, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 "sice.seed"="0" %CF.in, i8 "sice.seed"="1" %PF.in, i8 "sice.seed"="0" %AF.in, i8 "sice.seed"="1" %ZF.in, i8 "sice.seed"="0" %SF.in, i8 "sice.seed"="0" %DF.in, i8 "sice.seed"="0" %OF.in, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7) local_unnamed_addr {
  %rsp_slot204 = getelementptr i8, ptr %RSP, i64 -8
  store i32 4198846, ptr %rsp_slot204, align 4
  %rsp_slot206 = getelementptr i8, ptr %RSP, i64 -4
  store i32 0, ptr %rsp_slot206, align 4
  %rsp_slot208 = getelementptr i8, ptr %RSP, i64 -16
  store i64 0, ptr %rsp_slot208, align 8
  store i64 4198866, ptr %PC, align 8
  store i64 4198872, ptr %NEXT_PC, align 8
  %stack_load = load i64, ptr inttoptr (i64 4210672 to ptr), align 16
  %rsp_slot205 = getelementptr i8, ptr %RSP, i64 -24
  store i64 %stack_load, ptr %rsp_slot205, align 8
  %rsp_slot207 = getelementptr i8, ptr %RSP, i64 -32
  store i64 0, ptr %rsp_slot207, align 8
  %1 = ptrtoint ptr %rsp_slot207 to i64
  %2 = and i64 %1, -64
  %sice.sp0 = inttoptr i64 %2 to ptr
  tail call void @llvm.experimental.noalias.scope.decl(metadata !0)
  store i64 140737353956604, ptr %PC, align 8, !noalias !3
  store i64 140737353956611, ptr %NEXT_PC, align 8, !noalias !3
  %stack_load.i = load i64, ptr inttoptr (i64 140737354124432 to ptr), align 16, !noalias !3
  %neg_off.i = sub i64 0, %stack_load.i
  %rsp_next.i = getelementptr inbounds i8, ptr %sice.sp0, i64 %neg_off.i
  store i64 0, ptr %rsp_next.i, align 8, !alias.scope !0, !noalias !6
  %stack_ptr62.i = getelementptr inbounds nuw i8, ptr %rsp_next.i, i64 8
  store i64 4210160, ptr %stack_ptr62.i, align 8, !alias.scope !0, !noalias !6
  %stack_ptr63.i = getelementptr inbounds nuw i8, ptr %rsp_next.i, i64 16
  store i64 36, ptr %stack_ptr63.i, align 8, !alias.scope !0, !noalias !6
  %stack_ptr64.i = getelementptr inbounds nuw i8, ptr %rsp_next.i, i64 24
  store i64 36, ptr %stack_ptr64.i, align 8, !alias.scope !0, !noalias !6
  %stack_ptr65.i = getelementptr inbounds nuw i8, ptr %rsp_next.i, i64 32
  store i64 4202500, ptr %stack_ptr65.i, align 8, !alias.scope !0, !noalias !6
  %stack_ptr66.i = getelementptr inbounds nuw i8, ptr %rsp_next.i, i64 40
  store i64 140737352296096, ptr %stack_ptr66.i, align 8, !alias.scope !0, !noalias !6
  %stack_ptr67.i = getelementptr inbounds nuw i8, ptr %rsp_next.i, i64 48
  store i64 140737352302560, ptr %stack_ptr67.i, align 8, !alias.scope !0, !noalias !6
  %stack_ptr68.i = getelementptr inbounds nuw i8, ptr %rsp_next.i, i64 592
  tail call void @llvm.memset.p0.i64(ptr noundef nonnull align 8 dereferenceable(32) %stack_ptr68.i, i8 0, i64 32, i1 false)
  store i64 140737353956692, ptr %NEXT_PC, align 8, !noalias !3
  %stack_ptr72.i = getelementptr inbounds nuw i8, ptr %rsp_next.i, i64 624
  store i64 0, ptr %stack_ptr72.i, align 8, !alias.scope !0, !noalias !6
  %3 = tail call zeroext i8 @__remill_undefined_8() #4
  store i64 140737353956692, ptr %PC, align 8, !noalias !3
  %rsp_i64.i = ptrtoint ptr %rsp_next.i to i64
  %rsp_i648.i = ptrtoint ptr %RBP to i64
  %4 = tail call ptr @__remill_flat_jump(ptr nonnull %PC, ptr %memory, ptr nonnull %NEXT_PC, i64 524526, i64 %1, i64 4210160, i64 0, i64 36, i64 4202500, i64 %rsp_i64.i, i64 %rsp_i648.i, i64 140737352296096, i64 140737352302560, i64 140737488344000, i64 140737354123920, i64 1, i64 4210160, i64 140737354125312, i64 140737488344264, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 0, i8 1, i8 %3, i8 1, i8 0, i8 0, i8 0, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7)
  ret ptr %4
}

; Function Attrs: mustprogress noduplicate nofree noinline nosync nounwind optnone willreturn memory(none)
declare zeroext i8 @__remill_undefined_8() local_unnamed_addr #0

; Function Attrs: mustprogress noinline nounwind optnone
declare dso_local ptr @__remill_flat_jump(ptr noundef, ptr noundef, ptr noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, ptr noundef byval(%union.vec128_t.0) align 8, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef) local_unnamed_addr #1

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: readwrite)
declare void @llvm.experimental.noalias.scope.decl(metadata) #2

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: write)
declare void @llvm.memset.p0.i64(ptr writeonly captures(none), i8, i64, i1 immarg) #3

attributes #0 = { mustprogress noduplicate nofree noinline nosync nounwind optnone willreturn memory(none) "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "tune-cpu"="generic" }
attributes #1 = { mustprogress noinline nounwind optnone "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "tune-cpu"="generic" }
attributes #2 = { mustprogress nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: readwrite) }
attributes #3 = { nocallback nofree nounwind willreturn memory(argmem: write) }
attributes #4 = { alwaysinline nobuiltin nounwind willreturn memory(none) "no-builtins" }

!0 = !{!1}
!1 = distinct !{!1, !2, !"sice_seg_1: %RSP"}
!2 = distinct !{!2, !"sice_seg_1"}
!3 = !{!4, !1, !5}
!4 = distinct !{!4, !2, !"sice_seg_1: %memory"}
!5 = distinct !{!5, !2, !"sice_seg_1: %RBP"}
!6 = !{!4, !5}
