# Fresh-session prompt — flat-lift `ret` result propagation

Paste the text below (between the --- lines) to a new session.

---

You are continuing remill flat-lifting work in `/home/adam/saturn_21_1/saturn/build_remill/remill`
(branch `flat_lifting_v2`, commit `c3a9c9f`). Read `flat/HANDOFF_ret_result.md` first — it has the
full context, the repro command, and where to look.

**The task:** The op1 register-destination clobber bug is fixed and committed
(`RemoveStateStoreClobbers` in `lib/BC/Util.cpp`; op1 now keeps its three `shl i32`). The
remaining problem is that we cannot observe a `ret`-terminated flat-lift function's **return value**
end-to-end. Specifically, the whole-function `--flat` lift of op1's `test` produces a single function
`sub_1170` (an instruction-execution loop over PC/NEXT_PC) whose only exit is one
`tail call ptr @__remill_flat_jump(...)` + `ret ptr`. That dispatch passes the **input** RAX
(arg3 = `%RAX`, unchanged) in the RAX slot — not a computed return value — even though the `test`
C function returns its result in RAX. A C harness stubbing `@__remill_flat_jump` therefore sees
`RAX=0` (the input), so op1(3,7) is not observable as 10.

**Investigate and determine which is true:**
1. The dispatch *should* carry the computed register file, and the lift is failing to propagate the
   final RAX into it (a real lift bug). Find where the accumulated result should reach RAX and why
   the dispatch's arg3 is still the input `%RAX`.
2. `ret` in the flat model hands the register file to `@__remill_dynamic_dispatch` (the dynamic
   re-entry join), and that's the missing boundary. **Note: `@__remill_dynamic_dispatch` has no
   implementation anywhere in the tree** — if the ret is routed through it, that is the gap.

**Where to look:** `/tmp/op1_clean.ll` (regenerate with the command in the handoff doc; block 138 is
the dispatch), `bin/lift/Lift.cpp` (single-function flat-lift assembly + terminal dispatch emission,
~line 493 inlining loop), `tools/flat-gen/flat_gen.cpp` (how each `_flat` wrapper models `ret` and
whether the ret's destination write reaches the register file passed to the dispatch), and the arch
flat register map (`kFlatRegs` / `REG_*` slots in the arch layer + `Definitions.h`).

**Success looks like:** a clear answer to (1) vs (2) above, and — if (1) — a fix in the lift/flat-gen
so the computed RAX reaches the dispatch, with op1(3,7) then observable as 10 through a linked C
harness. If (2), document exactly what `@__remill_dynamic_dispatch` must do and (optionally) add a
minimal C stub so the harness can link and the result can be read.

**Build:** `cd /home/adam/saturn_21_1/saturn/build_remill/remill/build && cmake --build . --target remill-lift-21`.
This is a custom LLVM fork — see the handoff doc and prior commits for API notes (e.g. `StringRef`
uses `starts_with`, `verifyModule` is structural-only, no `SimplifyCFG` in the flat-opt pipeline).
