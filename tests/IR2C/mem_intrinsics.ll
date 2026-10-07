; memset / memcpy / memmove (opt merges adjacent stores and copies into
; these), and intrinsics without an effect, which print nothing
define void @mem(i64 %d, i64 %s, i64 %n) {
  %dp = inttoptr i64 %d to ptr
  %sp = inttoptr i64 %s to ptr
  call void @llvm.experimental.noalias.scope.decl(metadata !0)
  call void @llvm.memset.p0.i64(ptr %dp, i8 0, i64 32, i1 false)
  %d2 = getelementptr i8, ptr %dp, i64 64
  call void @llvm.memcpy.p0.p0.i64(ptr %d2, ptr %sp, i64 16, i1 false)
  call void @llvm.memmove.p0.p0.i64(ptr %dp, ptr %sp, i64 %n, i1 false)
  ret void
}

declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare void @llvm.experimental.noalias.scope.decl(metadata)

!0 = !{!1}
!1 = distinct !{!1, !2, !"scope"}
!2 = distinct !{!2, !"domain"}
