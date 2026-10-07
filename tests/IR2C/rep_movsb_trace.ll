; !LIFT trace of repstr: 13 step(s) from 0x401149, seeded with the live registers
; lifted by the remill port's flat lifter, optimized by opt, saved 2026-10-07 23:08:22
;
; seed  flags  = 0x44 (ZF PF)
; seed  rax    = 0x00007ffff7e45e28
; seed  rcx    = 0x0000000000403df0
; seed  rdx    = 0x00007fffffffd4c8
; seed  rbx    = 0x0000000000000000
; seed  rsi    = 0x00007fffffffd4b8
; seed  rdi    = 0x0000000000000001
; seed  r8     = 0x00007ffff7e3e6a0
; seed  r9     = 0x00007ffff7e3ffe0
; seed  r10    = 0x00007fffffffd3c0
; seed  r11    = 0x00007ffff7ffca90
; seed  r12    = 0x0000000000000001
; seed  r13    = 0x0000000000403df0
; seed  r14    = 0x00007ffff7ffd000
; seed  r15    = 0x00007fffffffd4c8
; 0x401149  push   rbp
; 0x40114a  mov    rbp, rsp
; 0x40114d  lea    rsi, [rip+0x2ecc]        # 0x404020 <src>
; 0x401154  lea    rdi, [rip+0x2f05]        # 0x404060 <dst>
; 0x40115b  mov    ecx, 0x4
; 0x401160  rep    movsb  [one rep iteration]
; 0x401160  (rep)  [rcx-=1]
; 0x401160  rep    movsb  [one rep iteration]
; 0x401160  (rep)  [rcx-=1]
; 0x401160  rep    movsb  [one rep iteration]
; 0x401160  (rep)  [rcx-=1]
; 0x401160  rep    movsb  [one rep iteration]
; 0x401160  (rep)  [rcx-=1]

; ModuleID = '/tmp/sice-lift-252616/lift.mem.ll'
source_filename = "lifted_code"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu-elf"

%union.vec128_t = type { %struct.uint128v1_t }
%struct.uint128v1_t = type { [1 x i128] }

define ptr @lift(ptr initializes((0, 8)) %PC, ptr noalias %memory, ptr initializes((0, 8)) %NEXT_PC, i64 "sice.seed"="140737352326696" %RAX.in, i64 "sice.seed"="0" %RBX.in, i64 "sice.seed"="4210160" %RCX.in, i64 "sice.seed"="140737488344264" %RDX.in, i64 "sice.seed"="140737488344248" %RSI.in, i64 "sice.seed"="1" %RDI.in, ptr noalias initializes((-8, 0)) %RSP, ptr noalias %RBP, i64 "sice.seed"="140737352296096" %R8.in, i64 "sice.seed"="140737352302560" %R9.in, i64 "sice.seed"="140737488344000" %R10.in, i64 "sice.seed"="140737354123920" %R11.in, i64 "sice.seed"="1" %R12.in, i64 "sice.seed"="4210160" %R13.in, i64 "sice.seed"="140737354125312" %R14.in, i64 "sice.seed"="140737488344264" %R15.in, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 "sice.seed"="0" %CF.in, i8 "sice.seed"="1" %PF.in, i8 "sice.seed"="0" %AF.in, i8 "sice.seed"="1" %ZF.in, i8 "sice.seed"="0" %SF.in, i8 "sice.seed"="0" %DF.in, i8 "sice.seed"="0" %OF.in, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7) local_unnamed_addr {
  %1 = ptrtoint ptr %RBP to i64
  %rsp_slot = getelementptr i8, ptr %RSP, i64 -8
  store i64 %1, ptr %rsp_slot, align 8
  store i64 4198752, ptr %PC, align 8
  store i64 4198753, ptr %NEXT_PC, align 8
  %stack_load = load i8, ptr inttoptr (i64 4210720 to ptr), align 32
  store i8 %stack_load, ptr inttoptr (i64 4210784 to ptr), align 32
  %2 = load i64, ptr %NEXT_PC, align 8
  %3 = add i64 %2, 19
  store i64 %3, ptr %NEXT_PC, align 8
  %v.i = load i8, ptr inttoptr (i64 4210721 to ptr), align 1
  store i8 %v.i, ptr inttoptr (i64 4210785 to ptr), align 1
  %v.i1 = load i8, ptr inttoptr (i64 4210722 to ptr), align 2
  store i8 %v.i1, ptr inttoptr (i64 4210786 to ptr), align 2
  %v.i2 = load i8, ptr inttoptr (i64 4210723 to ptr), align 1
  store i8 %v.i2, ptr inttoptr (i64 4210787 to ptr), align 1
  store i64 %3, ptr %PC, align 8
  %rsp_i64 = ptrtoint ptr %rsp_slot to i64
  %4 = tail call ptr @__remill_flat_jump(ptr nonnull %PC, ptr %memory, ptr nonnull %NEXT_PC, i64 140737352326696, i64 0, i64 0, i64 140737488344264, i64 4210724, i64 4210788, i64 %rsp_i64, i64 %rsp_i64, i64 140737352296096, i64 140737352302560, i64 140737488344000, i64 140737354123920, i64 1, i64 4210160, i64 140737354125312, i64 140737488344264, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 0, i8 1, i8 0, i8 1, i8 0, i8 0, i8 0, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7)
  ret ptr %4
}

; Function Attrs: mustprogress noinline nounwind optnone
declare dso_local ptr @__remill_flat_jump(ptr noundef, ptr noundef, ptr noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef) local_unnamed_addr #0

attributes #0 = { mustprogress noinline nounwind optnone "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "tune-cpu"="generic" }
