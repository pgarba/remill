# Plan: Flat ABI v2 — By-Value Register Args

## Problem

The current flat ABI passes registers as `ptr` (pointers to values). With opaque
pointers, this causes invalid IR when the inlined ISEL does arithmetic on the
pointer directly:

```llvm
%57 = sub ptr %RSP, i64 %11      ; INVALID: arithmetic on ptr
%58 = and ptr %R13, i64 4294967295  ; INVALID
```

Workarounds (zext→ptrtoint fix, compare inlining, ptr-arithmetic patching) are
fragile and incomplete.

## Solution

Change the flat ABI to pass registers **by value** as integers:

```llvm
; OLD (pointer args):
define ptr @sub_0(ptr %PC, ptr %memory, ptr %NEXT_PC,
                  ptr %RAX, ptr %RBX, ..., ptr %XMM15)

; NEW (by-value args):
define void @sub_0(i64 %pc, ptr %memory, i64 %next_pc,
                   i64 %RAX, i64 %RBX, ..., i8 %CF, ..., <2 x i64> %XMM0, ...)
```

The ISEL reads register values directly (no load). New values are accumulated
as local SSA. At block exit, the function returns/stores the final values.

## Changes Required

### Phase 1: ABI Definition

1. **`include/remill/BC/ABI.h`**
   - Change `kFlatNumRegs` comment: args are now by-value
   - Add `kFlatRegValueTypes[]` — maps register index → LLVM type:
     - 0-19 (GPRs + seg): `i64`
     - 20-26 (flags): `i8`
     - 27-34 (MMX): `i64`
     - 35-51 (XMM): `<2 x i64>`

2. **`lib/Arch/Arch.cpp`** (`CreateLiftedFunction`)
   - Change register arg types from `ptr` to the appropriate integer type
   - PC/NEXT_PC become `i64` (not `ptr`)
   - Memory stays `ptr`

### Phase 2: Lifter Changes

3. **`lib/BC/InstructionLifter.cpp`**
   - `LoadRegAddress` → rename to `LoadRegValue`: returns the arg directly
     (it's already an integer, no load needed)
   - `LoadRegValue` → return `NthArgument(func, kFlatFirstRegArgNum + idx)`
     directly (the value IS the arg)
   - Remove all `zext ptr` / `ptrtoint` / load-through-pointer logic
   - `LiftOperand`: pass the value directly (no pointer)
   - Memory access: still uses `ptr %memory` (unchanged)

4. **`lib/BC/TraceLifter.cpp`**
   - Block-to-block value passing: instead of threading pointers, pass
     integer values between blocks (phi nodes for branches)
   - Branch handling: `BRANCH_TAKEN` stays as `i8` by-value

5. **`lib/BC/Util.cpp`**
   - `FindVarInFunction`: remove pointer-arg lookup (args are values now)
   - `LoadBranchTaken`: return the i8 arg directly
   - `FixZextPtrToPtrToInt`: can be removed (no more ptr arithmetic)
   - `ScalarizeFlatFunction` / `OptimizeFlatSSAFunction`: simplified
     (no SROA needed, values are already SSA)

### Phase 3: ISEL Interface

6. **`tools/flat-gen/flat_gen.cpp`** — regenerate `_flat` wrappers
   - `_flat` ISEL signature: `(Memory *mem, i64 rax, i64 rbx, ..., <2 x i64> xmm0, ...)`
   - Returns: struct of updated values OR writes to output params
   - **Key decision**: how does the ISEL return updated values?
     - Option A: Return a 52-field struct (SROA will scalarize it)
     - Option B: Take output pointer params in addition to input value params
     - Option C: The ISEL body already computes new values as SSA; the
       wrapper extracts them from the local State and returns them

   **Recommendation: Option A** (return struct). The `_flat` wrapper:
   1. Takes by-value register args
   2. Builds a local `State` from the args (52 stores)
   3. Calls the original ISEL (which modifies the local State)
   4. Reads updated values from the State (52 loads)
   5. Returns them as a struct
   6. SROA scalarizes the struct back to SSA values

   After SROA + inlining, the 52 stores + 52 loads cancel out and the
   register values flow as pure SSA.

7. **`include/remill/Arch/X86/Runtime/FlatState.h`**
   - `__remill_flat_jump`: change to by-value args
     ```c
     Memory *__remill_flat_jump(i64 pc, Memory *memory, i64 next_pc,
                                i64 rax, i64 rbx, ..., <2 x i64> xmm0, ...);
     ```
   - The jump function stores values into the caller's state array

8. **`lib/Arch/X86/Runtime/FlatState.cpp`**
   - Update `__remill_flat_jump` implementation
   - `__remill_flat_state_load/store`: can be removed (no more State materialization)

### Phase 4: Arch / Block Threading

9. **`lib/Arch/X86/Arch.cpp`**
   - `InitializeFlatLiftedFunction`: remove STATE_LOCAL alloca, remove
     segment base allocas (they're now by-value args)
   - `FinishFlatLiftedFunctionImpl`: build the return struct from the
     current SSA values, tail-call `__remill_flat_jump`
   - `kFlatRegs` array: add value types

10. **Block-to-block passing (TraceLifter)**
    - In the original design, blocks are separate functions that call each
      other. With by-value args, the caller passes the updated register
      values as arguments to the next block function.
    - For branches: both successors receive the same values (phi nodes
      handle the merge).

### Phase 5: Cleanup

11. **Remove workarounds**
    - `FixZextPtrToPtrToInt` — no longer needed
    - `__remill_compare_*` inlining — may still be needed (verify)
    - ptr-arithmetic fix loop — no longer needed
    - `use_empty()` / `parent()` workarounds in `ScalarizeFlatFunction`

12. **Update tests** (`tests/X86/FlatLift.cpp`)
    - Update expected signatures (i64 args instead of ptr)
    - Add test: verify no `load` from register args (they're values)
    - Add test: verify no `ptr`-typed arithmetic

13. **Update CTest** — ensure all 33+ checks still pass

### Phase 6: Regenerate & Verify

14. **Regenerate `amd64_flat.bc`** with new `_flat` wrappers
15. **Lift real-world functions** (xstrtoumax, human_readable, quotearg)
16. **Verify output compiles** with `clang -c`
17. **Verify no invalid IR** (no `zext ptr`, no `sub ptr`, no `and ptr`)
18. **Benchmark** code size vs old pointer-arg mode

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| PC/NEXT_PC type | `i64` by-value | Consistent with other regs |
| Memory type | `ptr` (unchanged) | Must be a pointer (heap object) |
| ISEL return | Struct (Option A) | SROA scalarizes; clean interface |
| BRANCH_TAKEN | `i8` by-value | It's a flag |
| Block calling | By-value args between blocks | No pointer threading |
| State materialization | Only inside `_flat` wrapper | Not in lifted code |

## Risk Assessment

- **High risk**: Block-to-block value passing (TraceLifter changes). Currently
  blocks pass a state pointer; switching to 52 by-value args per call is a
  significant interface change.
- **Medium risk**: `_flat` ISEL regeneration. The wrapper must correctly
  materialize + dematerialize the State struct.
- **Low risk**: Lifter changes (LoadRegValue simplification).

## Estimated Effort

- Phase 1-2 (ABI + lifter): 1-2 days
- Phase 3 (ISEL + FlatState): 2-3 days
- Phase 4 (Arch + block threading): 2-3 days
- Phase 5-6 (cleanup + verify): 1 day
- **Total: ~1-2 weeks**

## Status (in progress)

- [x] Plan written
- [ ] Phase 1: ABI.h by-value types
- [ ] Phase 2: Lifter changes (LoadRegValue)
- [ ] Phase 3: flat_gen.cpp by-value wrappers + struct return
- [ ] Phase 4: Arch.cpp function type + init/finish
- [ ] Phase 5: FlatState jump by-value
- [ ] Phase 6: TraceLifter block threading
- [ ] Phase 7: Remove workarounds, verify

## Root Cause Analysis (updated)

The `--flat-ssa` ptr-arithmetic bug (`sub ptr %RSP, i64 16`) occurs because:
1. `_flat` wrapper calls external `__remill_flat_state_load` (no body in module)
2. SROA scalarizes the local `State` alloca based on the inlined ISEL's GEPs
3. SROA splits the State into individual allocas
4. The external `state_load` can't write to the split allocas (opaque call)
5. Result: ISEL's register accesses become the pointer args directly (unloaded)

**Fix**: by-value ABI eliminates the pointer indirection entirely. The ISEL
reads integer values directly. No State materialization in the lifted code.
