# Handoff: flat-lift `ret` result propagation

## RESOLVED (commit `8668485` + test `f317ebc` on `flat_lifting_v2`)

The `ret` result is now observable end-to-end: the op1 test function lifts and
executes to `RAX == 10` for `op1(3,7)`, matching the reference, and a
live-`jne` function returns the correct value on both branch arms. A permanent
end-to-end execution regression test (`tests/FlatRet`, ctest `flat_ret`) guards
it and is verified to FAIL on the pre-fix code.

Two bugs were the root cause (both in `lib/BC/Util.cpp`):

1. **`RemoveStateStoreClobbers` deleted by position** ("the last store to each
   `REG_*` slot"). But scalarization orders the dst (computed) store and the
   `state_store` snapshot (pre-instruction) store differently per opcode — for
   SHL the snapshot is last, for ADD the dst store is last — so the position rule
   kept the computed value for SHL but deleted it for ADD. Fixed to be
   value-based: a store is a snapshot round-trip iff its value is a load from the
   SAME slot, and only such stores are removed (and only when a computed store to
   the slot also exists).

2. **`FixZextPtrToPtrToInt` inlined `__remill_compare_neq(x)` as `!x`**, but the
   real runtime intrinsic is the identity (`tests/X86/Run.cpp`: `return result`).
   The polarity is already in the argument (JNZ uses
   `compare_neq(BNot(FLAG_ZF))`). Inlining it as a NOT inverted every JNZ/JE-style
   conditional branch — in BOTH the flat path and the general path (the `!x`
   came from commit `4e82196`). Fixed to inline all `__remill_compare_*` as the
   identity, matching the runtime.

Verification: `ctest -R "flat|unflatten"` → flat_lift, unflatten_smoke, flat_ret
all pass. The `flat_ret` test lifts fixed byte blobs (op1 + br3), links each
against a C harness stubbing `__remill_flat_jump`, drives the function, and
checks the captured RAX against a reference.

---

Status snapshot as of commit `c3a9c9f` on `flat_lifting_v2` (historical).

## What is DONE and committed

- **op1 register-destination clobber fix** (`RemoveStateStoreClobbers` in
  `lib/BC/Util.cpp`, run inside `OptimizeFlatSSAFunction` right after the first
  `EliminateStackMemoryAccesses`). This deletes the inlined `state_store`'s
  trailing snapshot write to any `REG_*` slot the ISEL body also wrote, so the
  body's destination store survives DCE. Verified: op1 now retains its three real
  `shl i32` (was 0).
- **SROA PreserveCFG** for branched flat functions (kept per decision): in the
  `OptimizeFlatSSAFunction` mem2reg/SROA/DCE loop, branched functions now get
  `SROAPass(SROAOptions::PreserveCFG)` instead of skipping SROA entirely, which
  avoids the LLVM-21 `ModifyCFG` assertion while still scalarizing.
- All debug instrumentation (`LIFT_DEBUG_DUMP*`, `OPT_STAGE_A/B/C`) stripped.
  Working tree is clean.

## ~~The OPEN problem: `ret` return value is not observable end-to-end~~ — RESOLVED

> The answer to the core question below is **(a)**: the dispatch is supposed to
> carry the computed register file, and the lift was failing to propagate the
> final RAX into it (bug #1 above), compounded by the inverted JNZ (bug #2).
> `ret` does NOT route through `@__remill_dynamic_dispatch`; the single trailing
> `@__remill_flat_jump` dispatch is the boundary, and its RAX argument now holds
> the computed result. `@__remill_dynamic_dispatch` remains a separate, unrelated
> runtime work item.

(Historical) Goal: confirm `op1(3,7) == 10` by actually executing the lifted
function and reading its return value. The open question at the time was how a
`ret`-terminated flat-lift function exposes its result:

### Facts established

1. The `--flat` lift of the whole `test` function produces a **single** function
   `sub_1170` — an instruction-execution loop over `PC`/`NEXT_PC` (not a
   per-instruction flat-ABI block). It takes the 63-arg flat register file as
   input (arg0=PC, arg1=memory, arg2=NEXT_PC, arg3=RAX, arg4=RBX, arg5=RCX,
   arg6=RDX, arg7=RSI, arg8=RDI, arg9=RSP, arg10=RBP, ...) and ends with **one**
   `tail call ptr @__remill_flat_jump(...)` followed by `ret ptr %145`.

2. The final dispatch (IR block 138, see `/tmp/op1_clean.ll` offset ~298) passes
   a **MIX** of input and computed values:
   - arg3 (RAX slot) = `%RAX` → the **input** RAX parameter (unmodified).
   - arg6 (RDX slot) = `%141` = `zext((trunc %RDX to i32) << 1)` → a **computed**
     value (the RDX<<1 term).
   - Most other slots are the input parameters; a few (RSP/RBP-derived) are
     recomputed.

3. A C harness that stubs `@__remill_flat_jump` (captures its args into globals)
   and drives `sub_1170` with RDI=a, RSI=b (RAX=RBX=RCX=RDX=0) runs clean
   (no segfault). The stub sees `RAX=0 RBX=0 RCX=0 RDX=0 RSI=7 RDI=3` — i.e. the
   **input** register file, **not** a computed return value. The RAX slot of the
   dispatch is 0 (the input), so "capture RAX from the dispatch stub" does **not**
   yield the function's return value.

### Core open question

> In the flat ABI, how is a function's `ret` result supposed to reach the caller?
> The single trailing `@__remill_flat_jump` dispatch carries the *input* RAX, not
> a computed one. Either:
>
> (a) the dispatch is *supposed* to carry the computed register file and the lift
>     is failing to propagate the final RAX into it (a real lift bug — most
>     likely, given the `test` function's return value lives in RAX), or
> (b) `ret` in the flat model is not "return RAX to caller" but "hand the
>     register file to the dynamic dispatcher", and the *real* runtime
>     (`@__remill_dynamic_dispatch`, which has **no** implementation anywhere in
>     the tree) is where the return value is extracted. In that case my stub is
>     intercepting the wrong boundary.

Note the distinction:
- `@__remill_flat_jump` — the per-block *static* dispatch (71-arg byval XMM
  signature). Present in the lifted output.
- `@__remill_dynamic_dispatch` — the dynamic re-entry join. **No implementation
  exists anywhere in the tree** (neither a C stub nor IR). If the `ret` is routed
  through it, that is the missing piece.

### What to check / where to look

- `/tmp/op1_clean.ll` (and regenerate with the command below) — the lifted IR.
  Block 138 is the dispatch. Trace where the loop's accumulated result *should*
  land (RAX?) and why the dispatch's arg3 is still the input `%RAX`.
- `bin/lift/Lift.cpp` — how the single-function flat lift is assembled and how
  the terminal `ret` block / dispatch is emitted (search for
  `__remill_flat_jump`, `MoveFunctionIntoModule`, the inlining loop ~line 493).
- `tools/flat-gen/flat_gen.cpp` — how each `_flat` wrapper models `ret` / its
  terminal dispatch, and whether the destination register write for a `ret`
  reaches the register file passed to the dispatch.
- `include/remill/Arch/Runtime/Definitions.h` + the arch layer — the flat register
  map (`kFlatRegs`, `REG_*` slots) and the intended `ret` semantics.
- Whether a `@__remill_dynamic_dispatch` C stub is expected by the flat ABI (it is
  currently absent; the link harness must provide whatever symbols the lifted IR
  references, or it won't link/run).

### Repro command (op1)

```
cd /home/adam/saturn_21_1/saturn/tests/op1
LIFT=/home/adam/saturn_21_1/saturn/build_remill/remill/build/bin/lift/remill-lift-21
X=$(xxd -p test.exe | tr -d '\n')          # test.exe is a 477-byte RAW .text, not ELF
$LIFT --arch amd64 --address 0x1050 --entry_address 0x1170 --flat --bytes "$X" \
      --ir_out /tmp/op1_clean.ll
grep -c 'shl i32' /tmp/op1_clean.ll        # expect 3 (the fix)
```
Expected `test(3,7) == 10` (original C semantics). The lifted run currently
cannot produce/observe that value.

### Harness artifacts (ephemeral, in /tmp — likely gone)

- `/tmp/op1_harness.c` — C stub for `@__remill_flat_jump` + `main()` driving
  `sub_1170`. Needs `@__remill_dynamic_dispatch` (and any other referenced
  runtime symbols) stubbed to link. Rebuild from scratch if missing.

## Related open items (NOT this problem, for context)

- `@__remill_dynamic_dispatch` has no implementation in-tree (runtime work item).
- `@__remill_flat_jump` static-signature mismatch (71 vs 63 args) in the ISEL.
- Missing-semantics gaps: `POPCNT_GPRv_GPRv_64`, `PMOVZXDQ` (document in
  `flat/unsupported_opcodes.md`).
- `gotos` loop-count discrepancy (2 remill vs 3 disasm) — characterized, not fixed.
- Pre-existing intermittent lift+link+serialize segfault (Heisenbug) — blocked on
  a reproducer.
