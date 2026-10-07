; Floating point and vectors (XMM): plain functions, run against the IR in
; the differential test. Vectors are bit blobs in C (unsigned __int128).

; scalar double math, an fcmp-select, a conversion
define double @fparith(double %a, double %b, i64 %c) {
  %m = fmul double %a, %b
  %ci = sitofp i64 %c to double
  %s = fadd double %m, %ci
  %d = fsub double %a, 2.5
  %q = fdiv double %s, %d
  %n = fneg double %q
  %lt = fcmp olt double %a, %b
  %r = select i1 %lt, double %n, double %q
  ret double %r
}

; every fcmp predicate, one bit each
define i32 @fcmps(double %a, double %b) {
  %p1 = fcmp oeq double %a, %b
  %p2 = fcmp ogt double %a, %b
  %p3 = fcmp oge double %a, %b
  %p4 = fcmp olt double %a, %b
  %p5 = fcmp ole double %a, %b
  %p6 = fcmp one double %a, %b
  %p7 = fcmp ord double %a, %b
  %p8 = fcmp ueq double %a, %b
  %p9 = fcmp ugt double %a, %b
  %p10 = fcmp uge double %a, %b
  %p11 = fcmp ult double %a, %b
  %p12 = fcmp ule double %a, %b
  %p13 = fcmp une double %a, %b
  %p14 = fcmp uno double %a, %b
  %z1 = zext i1 %p1 to i32
  %z2 = zext i1 %p2 to i32
  %z3 = zext i1 %p3 to i32
  %z4 = zext i1 %p4 to i32
  %z5 = zext i1 %p5 to i32
  %z6 = zext i1 %p6 to i32
  %z7 = zext i1 %p7 to i32
  %z8 = zext i1 %p8 to i32
  %z9 = zext i1 %p9 to i32
  %z10 = zext i1 %p10 to i32
  %z11 = zext i1 %p11 to i32
  %z12 = zext i1 %p12 to i32
  %z13 = zext i1 %p13 to i32
  %z14 = zext i1 %p14 to i32
  %s2 = shl i32 %z2, 1
  %s3 = shl i32 %z3, 2
  %s4 = shl i32 %z4, 3
  %s5 = shl i32 %z5, 4
  %s6 = shl i32 %z6, 5
  %s7 = shl i32 %z7, 6
  %s8 = shl i32 %z8, 7
  %s9 = shl i32 %z9, 8
  %s10 = shl i32 %z10, 9
  %s11 = shl i32 %z11, 10
  %s12 = shl i32 %z12, 11
  %s13 = shl i32 %z13, 12
  %s14 = shl i32 %z14, 13
  %o1 = or i32 %z1, %s2
  %o2 = or i32 %o1, %s3
  %o3 = or i32 %o2, %s4
  %o4 = or i32 %o3, %s5
  %o5 = or i32 %o4, %s6
  %o6 = or i32 %o5, %s7
  %o7 = or i32 %o6, %s8
  %o8 = or i32 %o7, %s9
  %o9 = or i32 %o8, %s10
  %o10 = or i32 %o9, %s11
  %o11 = or i32 %o10, %s12
  %o12 = or i32 %o11, %s13
  %o13 = or i32 %o12, %s14
  ret i32 %o13
}

; conversions both ways, float and double, and the math intrinsics
define i64 @conv(double %a, float %f) {
  %i = fptosi double %a to i32
  %e = fpext float %f to double
  %t = fptrunc double %a to float
  %ab = call double @llvm.fabs.f64(double %a)
  %sq = call double @llvm.sqrt.f64(double %ab)
  %fl = call float @llvm.floor.f32(float %t)
  %mx = call double @llvm.maxnum.f64(double %e, double %sq)
  %u = fptoui double %ab to i64
  %bm = bitcast double %mx to i64
  %bf = bitcast float %fl to i32
  %zf = zext i32 %bf to i64
  %zi = zext i32 %i to i64
  %x1 = xor i64 %bm, %zf
  %x2 = add i64 %x1, %zi
  %x3 = mul i64 %x2, %u
  ret i64 %x3
}

; what remill makes of mulsd xmm0, xmm1: the low lane as a double, the high
; lane kept; then lanes through a shuffle and lane-wise integer math
define i64 @sse(i64 %a, i64 %b, i64 %hi) {
  %v0 = insertelement <2 x i64> poison, i64 %a, i64 0
  %v = insertelement <2 x i64> %v0, i64 %hi, i64 1
  %vd = bitcast <2 x i64> %v to <2 x double>
  %lo = extractelement <2 x double> %vd, i64 0
  %bd = bitcast i64 %b to double
  %p = fmul double %lo, %bd
  %r = insertelement <2 x double> %vd, double %p, i64 0
  %ri = bitcast <2 x double> %r to <4 x i32>
  %sh = shufflevector <4 x i32> %ri, <4 x i32> zeroinitializer, <4 x i32> <i32 3, i32 2, i32 5, i32 0>
  %sum = add <4 x i32> %sh, <i32 1, i32 -1, i32 7, i32 16>
  %x = xor <4 x i32> %sum, %ri
  %xi = bitcast <4 x i32> %x to <2 x i64>
  %l0 = extractelement <2 x i64> %xi, i64 0
  %l1 = extractelement <2 x i64> %xi, i64 1
  %m = mul i64 %l1, 3
  %res = add i64 %l0, %m
  ret i64 %res
}

declare double @llvm.fabs.f64(double)
declare double @llvm.sqrt.f64(double)
declare float @llvm.floor.f32(float)
declare double @llvm.maxnum.f64(double, double)
