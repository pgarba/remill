; remill-ir2c test: a plain (non flat-ABI) function using the operations the
; lifted traces in this directory don't: narrow arithmetic, signed operations,
; rotates, min/max/abs, popcount, an overflow pair, select.
define i64 @ops(i64 %a, i64 %b, i8 %c, i32 %d) {
  %m = mul i8 %c, 3
  %s = shl i8 %m, 2
  %ext = zext i8 %s to i64
  %sx = sext i32 %d to i64
  %ash = ashr i64 %sx, 3
  %div = sdiv i64 %a, 7
  %rot = call i64 @llvm.fshl.i64(i64 %a, i64 %a, i64 13)
  %mn = call i64 @llvm.umin.i64(i64 %a, i64 %b)
  %mx = call i64 @llvm.smax.i64(i64 %a, i64 %b)
  %ab = call i64 @llvm.abs.i64(i64 %b, i1 false)
  %pc = call i64 @llvm.ctpop.i64(i64 %b)
  %ov = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %a, i64 %b)
  %sum = extractvalue { i64, i1 } %ov, 0
  %carry = extractvalue { i64, i1 } %ov, 1
  %cz = zext i1 %carry to i64
  %lt = icmp slt i64 %a, %b
  %sel = select i1 %lt, i64 %mn, i64 %mx
  %neg = add i64 %a, -8
  %not = xor i64 %b, -1
  %r1 = add i64 %ext, %ash
  %r2 = add i64 %r1, %div
  %r3 = xor i64 %r2, %rot
  %r4 = add i64 %r3, %sel
  %r5 = sub i64 %r4, %ab
  %r6 = add i64 %r5, %pc
  %r7 = add i64 %r6, %sum
  %r8 = add i64 %r7, %cz
  %r9 = and i64 %r8, %neg
  %r10 = or i64 %r9, %not
  ret i64 %r10
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
declare i64 @llvm.umin.i64(i64, i64)
declare i64 @llvm.smax.i64(i64, i64)
declare i64 @llvm.abs.i64(i64, i1)
declare i64 @llvm.ctpop.i64(i64)
declare { i64, i1 } @llvm.uadd.with.overflow.i64(i64, i64)
