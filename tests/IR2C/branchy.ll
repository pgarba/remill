; remill-ir2c differential tests: plain functions with control flow. Their C
; is compiled and run against this IR on random inputs (run_ir2c.py).

; the sum of 0 .. n-1, n = a & 255: a loop with two phis
define i64 @loop_sum(i64 %a) {
entry:
  %n = and i64 %a, 255
  %z = icmp eq i64 %n, 0
  br i1 %z, label %done, label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i1, %loop ]
  %s = phi i64 [ 0, %entry ], [ %s1, %loop ]
  %s1 = add i64 %s, %i
  %i1 = add i64 %i, 1
  %c = icmp ult i64 %i1, %n
  br i1 %c, label %loop, label %done
done:
  %r = phi i64 [ 0, %entry ], [ %s1, %loop ]
  ret i64 %r
}

; a diamond: a < b (signed) ? a * 3 : b - a
define i64 @diamond(i64 %a, i64 %b) {
entry:
  %lt = icmp slt i64 %a, %b
  br i1 %lt, label %then, label %else
then:
  %m = mul i64 %a, 3
  br label %join
else:
  %d = sub i64 %b, %a
  br label %join
join:
  %r = phi i64 [ %m, %then ], [ %d, %else ]
  ret i64 %r
}

; two phis that swap every iteration: their copies must be parallel
define i64 @swap(i64 %a, i64 %b) {
entry:
  %k = and i64 %a, 63
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i1, %loop ]
  %x = phi i64 [ %a, %entry ], [ %y, %loop ]
  %y = phi i64 [ %b, %entry ], [ %x, %loop ]
  %i1 = add i64 %i, 1
  %c = icmp ult i64 %i, %k
  br i1 %c, label %loop, label %done
done:
  %r = sub i64 %x, %y
  ret i64 %r
}

; 128-bit arithmetic: the high half of a * b
define i64 @mulhi(i64 %a, i64 %b) {
  %a128 = zext i64 %a to i128
  %b128 = zext i64 %b to i128
  %p = mul i128 %a128, %b128
  %h = lshr i128 %p, 64
  %r = trunc i128 %h to i64
  ret i64 %r
}

; a switch with a phi at the join
define i64 @sw(i64 %a) {
entry:
  %k = and i64 %a, 3
  switch i64 %k, label %d [ i64 0, label %c0
                            i64 1, label %c1 ]
c0:
  br label %j
c1:
  br label %j
d:
  br label %j
j:
  %r = phi i64 [ 100, %c0 ], [ 200, %c1 ], [ %a, %d ]
  ret i64 %r
}
