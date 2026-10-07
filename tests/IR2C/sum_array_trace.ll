; !LIFT trace of sample: 107 step(s) from 0x401192, seeded with the live registers
; lifted by the remill port's flat lifter, optimized by opt, saved 2026-10-07 22:49:20
;
; seed  flags  = 0x44 (ZF PF)
; seed  rax    = 0x00007ffff7e45e28
; seed  rcx    = 0x0000000000403df0
; seed  rdx    = 0x00007fffffffd4d8
; seed  rbx    = 0x0000000000000000
; seed  rsi    = 0x00007fffffffd4c8
; seed  rdi    = 0x0000000000000001
; seed  r8     = 0x00007ffff7e3e6a0
; seed  r9     = 0x00007ffff7e3ffe0
; seed  r10    = 0x00007fffffffd3d0
; seed  r11    = 0x00007ffff7ffca90
; seed  r12    = 0x0000000000000001
; seed  r13    = 0x0000000000403df0
; seed  r14    = 0x00007ffff7ffd000
; seed  r15    = 0x00007fffffffd4d8
; 0x401192  lea    rax, [rip+0x2ea7]        # 0x404040 <data>
; 0x401199  mov    esi, 0x8
; 0x40119e  mov    rdi, rax
; 0x4011a1  call   0x401149 <sum_array>  [call: not lifted; rsp-=8, return address 0x4011a6 stored]
; 0x401149  push   rbp
; 0x40114a  mov    rbp, rsp
; 0x40114d  mov    QWORD PTR [rbp-0x18], rdi
; 0x401151  mov    DWORD PTR [rbp-0x1c], esi
; 0x401154  mov    DWORD PTR [rbp-0x8], 0x0
; 0x40115b  mov    DWORD PTR [rbp-0x4], 0x0
; 0x401162  jmp    0x401181 <sum_array+56>  [jmp: not lifted]
; 0x401181  mov    eax, DWORD PTR [rbp-0x4]
; 0x401184  cmp    eax, DWORD PTR [rbp-0x1c]
; 0x401187  jl     0x401164 <sum_array+27>  [branch: not lifted]
; 0x401164  mov    eax, DWORD PTR [rbp-0x4]
; 0x401167  cdqe
; 0x401169  lea    rdx, [rax*4]
; 0x401171  mov    rax, QWORD PTR [rbp-0x18]
; 0x401175  add    rax, rdx
; 0x401178  mov    eax, DWORD PTR [rax]
; 0x40117a  add    DWORD PTR [rbp-0x8], eax
; 0x40117d  add    DWORD PTR [rbp-0x4], 0x1
; 0x401181  mov    eax, DWORD PTR [rbp-0x4]
; 0x401184  cmp    eax, DWORD PTR [rbp-0x1c]
; 0x401187  jl     0x401164 <sum_array+27>  [branch: not lifted]
; 0x401164  mov    eax, DWORD PTR [rbp-0x4]
; 0x401167  cdqe
; 0x401169  lea    rdx, [rax*4]
; 0x401171  mov    rax, QWORD PTR [rbp-0x18]
; 0x401175  add    rax, rdx
; 0x401178  mov    eax, DWORD PTR [rax]
; 0x40117a  add    DWORD PTR [rbp-0x8], eax
; 0x40117d  add    DWORD PTR [rbp-0x4], 0x1
; 0x401181  mov    eax, DWORD PTR [rbp-0x4]
; 0x401184  cmp    eax, DWORD PTR [rbp-0x1c]
; 0x401187  jl     0x401164 <sum_array+27>  [branch: not lifted]
; 0x401164  mov    eax, DWORD PTR [rbp-0x4]
; 0x401167  cdqe
; 0x401169  lea    rdx, [rax*4]
; 0x401171  mov    rax, QWORD PTR [rbp-0x18]
; 0x401175  add    rax, rdx
; 0x401178  mov    eax, DWORD PTR [rax]
; 0x40117a  add    DWORD PTR [rbp-0x8], eax
; 0x40117d  add    DWORD PTR [rbp-0x4], 0x1
; 0x401181  mov    eax, DWORD PTR [rbp-0x4]
; 0x401184  cmp    eax, DWORD PTR [rbp-0x1c]
; 0x401187  jl     0x401164 <sum_array+27>  [branch: not lifted]
; 0x401164  mov    eax, DWORD PTR [rbp-0x4]
; 0x401167  cdqe
; 0x401169  lea    rdx, [rax*4]
; 0x401171  mov    rax, QWORD PTR [rbp-0x18]
; 0x401175  add    rax, rdx
; 0x401178  mov    eax, DWORD PTR [rax]
; 0x40117a  add    DWORD PTR [rbp-0x8], eax
; 0x40117d  add    DWORD PTR [rbp-0x4], 0x1
; 0x401181  mov    eax, DWORD PTR [rbp-0x4]
; 0x401184  cmp    eax, DWORD PTR [rbp-0x1c]
; 0x401187  jl     0x401164 <sum_array+27>  [branch: not lifted]
; 0x401164  mov    eax, DWORD PTR [rbp-0x4]
; 0x401167  cdqe
; 0x401169  lea    rdx, [rax*4]
; 0x401171  mov    rax, QWORD PTR [rbp-0x18]
; 0x401175  add    rax, rdx
; 0x401178  mov    eax, DWORD PTR [rax]
; 0x40117a  add    DWORD PTR [rbp-0x8], eax
; 0x40117d  add    DWORD PTR [rbp-0x4], 0x1
; 0x401181  mov    eax, DWORD PTR [rbp-0x4]
; 0x401184  cmp    eax, DWORD PTR [rbp-0x1c]
; 0x401187  jl     0x401164 <sum_array+27>  [branch: not lifted]
; 0x401164  mov    eax, DWORD PTR [rbp-0x4]
; 0x401167  cdqe
; 0x401169  lea    rdx, [rax*4]
; 0x401171  mov    rax, QWORD PTR [rbp-0x18]
; 0x401175  add    rax, rdx
; 0x401178  mov    eax, DWORD PTR [rax]
; 0x40117a  add    DWORD PTR [rbp-0x8], eax
; 0x40117d  add    DWORD PTR [rbp-0x4], 0x1
; 0x401181  mov    eax, DWORD PTR [rbp-0x4]
; 0x401184  cmp    eax, DWORD PTR [rbp-0x1c]
; 0x401187  jl     0x401164 <sum_array+27>  [branch: not lifted]
; 0x401164  mov    eax, DWORD PTR [rbp-0x4]
; 0x401167  cdqe
; 0x401169  lea    rdx, [rax*4]
; 0x401171  mov    rax, QWORD PTR [rbp-0x18]
; 0x401175  add    rax, rdx
; 0x401178  mov    eax, DWORD PTR [rax]
; 0x40117a  add    DWORD PTR [rbp-0x8], eax
; 0x40117d  add    DWORD PTR [rbp-0x4], 0x1
; 0x401181  mov    eax, DWORD PTR [rbp-0x4]
; 0x401184  cmp    eax, DWORD PTR [rbp-0x1c]
; 0x401187  jl     0x401164 <sum_array+27>  [branch: not lifted]
; 0x401164  mov    eax, DWORD PTR [rbp-0x4]
; 0x401167  cdqe
; 0x401169  lea    rdx, [rax*4]
; 0x401171  mov    rax, QWORD PTR [rbp-0x18]
; 0x401175  add    rax, rdx
; 0x401178  mov    eax, DWORD PTR [rax]
; 0x40117a  add    DWORD PTR [rbp-0x8], eax
; 0x40117d  add    DWORD PTR [rbp-0x4], 0x1
; 0x401181  mov    eax, DWORD PTR [rbp-0x4]
; 0x401184  cmp    eax, DWORD PTR [rbp-0x1c]
; 0x401187  jl     0x401164 <sum_array+27>  [branch: not lifted]
; 0x401189  mov    eax, DWORD PTR [rbp-0x8]
; 0x40118c  pop    rbp
; 0x40118d  ret  [ret: not lifted; rsp+=8]
; 0x4011a6  mov    edx, eax
; 0x4011a8  lea    rax, [rip+0xe55]        # 0x402004

; ModuleID = '/tmp/sice-lift-239167/lift.mem.ll'
source_filename = "lifted_code"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu-elf"

%union.vec128_t = type { %struct.uint128v1_t }
%struct.uint128v1_t = type { [1 x i128] }

define ptr @lift(ptr initializes((0, 8)) %PC, ptr noalias %memory, ptr initializes((0, 8)) %NEXT_PC, i64 "sice.seed"="140737352326696" %RAX.in, i64 "sice.seed"="0" %RBX.in, i64 "sice.seed"="4210160" %RCX.in, i64 "sice.seed"="140737488344280" %RDX.in, i64 "sice.seed"="140737488344264" %RSI.in, i64 "sice.seed"="1" %RDI.in, ptr noalias initializes((-44, -32), (-24, 0)) %RSP, ptr noalias %RBP, i64 "sice.seed"="140737352296096" %R8.in, i64 "sice.seed"="140737352302560" %R9.in, i64 "sice.seed"="140737488344016" %R10.in, i64 "sice.seed"="140737354123920" %R11.in, i64 "sice.seed"="1" %R12.in, i64 "sice.seed"="4210160" %R13.in, i64 "sice.seed"="140737354125312" %R14.in, i64 "sice.seed"="140737488344280" %R15.in, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 "sice.seed"="0" %CF.in, i8 "sice.seed"="1" %PF.in, i8 "sice.seed"="0" %AF.in, i8 "sice.seed"="1" %ZF.in, i8 "sice.seed"="0" %SF.in, i8 "sice.seed"="0" %DF.in, i8 "sice.seed"="0" %OF.in, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7) local_unnamed_addr {
  %rsp_slot2060 = getelementptr i8, ptr %RSP, i64 -8
  store i32 4198822, ptr %rsp_slot2060, align 4
  %rsp_slot2070 = getelementptr i8, ptr %RSP, i64 -4
  store i32 0, ptr %rsp_slot2070, align 4
  %1 = ptrtoint ptr %RBP to i64
  %rsp_slot2055 = getelementptr i8, ptr %RSP, i64 -16
  store i64 %1, ptr %rsp_slot2055, align 8
  %rsp_slot2034 = getelementptr i8, ptr %RSP, i64 -40
  store i64 4210752, ptr %rsp_slot2034, align 8
  %rsp_slot2084 = getelementptr i8, ptr %RSP, i64 -44
  store i32 8, ptr %rsp_slot2084, align 4
  %rsp_slot2077 = getelementptr i8, ptr %RSP, i64 -24
  %rsp_slot2052 = getelementptr i8, ptr %RSP, i64 -20
  %v.i = load i32, ptr inttoptr (i64 4210752 to ptr), align 64
  %v.i1 = load i32, ptr inttoptr (i64 4210756 to ptr), align 4
  %2 = add i32 %v.i1, %v.i
  %v.i2 = load i32, ptr inttoptr (i64 4210760 to ptr), align 8
  %3 = add i32 %2, %v.i2
  %v.i3 = load i32, ptr inttoptr (i64 4210764 to ptr), align 4
  %4 = add i32 %3, %v.i3
  %v.i4 = load i32, ptr inttoptr (i64 4210768 to ptr), align 16
  %5 = add i32 %4, %v.i4
  %v.i5 = load i32, ptr inttoptr (i64 4210772 to ptr), align 4
  %6 = add i32 %5, %v.i5
  %v.i6 = load i32, ptr inttoptr (i64 4210776 to ptr), align 8
  %7 = add i32 %6, %v.i6
  %v.i7 = load i32, ptr inttoptr (i64 4210780 to ptr), align 4
  %8 = add i32 %7, %v.i7
  store i32 %8, ptr %rsp_slot2077, align 4
  store i32 8, ptr %rsp_slot2052, align 4
  store i64 4199166, ptr %NEXT_PC, align 8
  %9 = zext i32 %8 to i64
  store i64 4199166, ptr %PC, align 8
  %rsp_i64 = ptrtoint ptr %RSP to i64
  %10 = tail call ptr @__remill_flat_jump(ptr nonnull %PC, ptr %memory, ptr nonnull %NEXT_PC, i64 4202500, i64 0, i64 4210160, i64 %9, i64 8, i64 4210752, i64 %rsp_i64, i64 %1, i64 140737352296096, i64 140737352302560, i64 140737488344016, i64 140737354123920, i64 1, i64 4210160, i64 140737354125312, i64 140737488344280, i64 %RIP, i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE, i8 0, i8 1, i8 0, i8 1, i8 0, i8 0, i8 0, i64 %MM0, i64 %MM1, i64 %MM2, i64 %MM3, i64 %MM4, i64 %MM5, i64 %MM6, i64 %MM7, <2 x i64> %XMM0, <2 x i64> %XMM1, <2 x i64> %XMM2, <2 x i64> %XMM3, <2 x i64> %XMM4, <2 x i64> %XMM5, <2 x i64> %XMM6, <2 x i64> %XMM7, <2 x i64> %XMM8, <2 x i64> %XMM9, <2 x i64> %XMM10, <2 x i64> %XMM11, <2 x i64> %XMM12, <2 x i64> %XMM13, <2 x i64> %XMM14, <2 x i64> %XMM15, i128 %ST0, i128 %ST1, i128 %ST2, i128 %ST3, i128 %ST4, i128 %ST5, i128 %ST6, i128 %ST7)
  ret ptr %10
}

; Function Attrs: mustprogress noinline nounwind optnone
declare dso_local ptr @__remill_flat_jump(ptr noundef, ptr noundef, ptr noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i8 noundef zeroext, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, ptr noundef byval(%union.vec128_t) align 8, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef, i64 noundef) local_unnamed_addr #0

attributes #0 = { mustprogress noinline nounwind optnone "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "tune-cpu"="generic" }
