# Plan: ISEL GEP-based RSP Access (Flat ABI)

## Problem

In the flat ABI, RSP is a `ptr`. The current flow:

```
state_load:  state->gpr.rsp.qword = ptrtoint(RSP)     // ptr → i64
ISEL:        state->gpr.rsp.qword ± C                  // i64 arithmetic
state_store: RSP_new = inttoptr(state->gpr.rsp.qword)  // i64 → ptr
```

After SROA + instcombine in the lift tool, the `inttoptr(ptrtoint(RSP) ± C)` chain
gets simplified (GEP chains collapse), breaking RSP tracking and preventing
store-load forwarding / dead store elimination.

We can convert `inttoptr(ptrtoint(X) ± C)` → `GEP(X, C)` in the bitcode
(2992 patterns converted), but the lift tool's instcombine × 3 re-simplifies
the GEP chains, breaking the tracking again.

**Goal:** Make the ISEL use GEPs directly for RSP, so the pointer chain is
never broken by optimization.

## Is It Possible?

**Yes.** The ISEL accesses RSP through `Read(REG_XSP)` / `Write(REG_XSP, val)`
macros, which expand to `state->gpr.rsp.qword` (i64). If we change the RSP
field to a pointer type and update the access patterns, the ISEL generates
GEPs natively.

## Approach Options

### Option A: Change `State::gpr.rsp` to pointer type

**What:** Change the RSP field in the State struct from `addr_t` (i64) to
`void *` (ptr). All ISEL code that does `Read(REG_XSP) ± C` becomes pointer
arithmetic, which LLVM compiles to GEPs.

**Changes:**
1. `include/remill/Arch/X86/Runtime/State.h` — change `gpr.rsp` type
2. `lib/Arch/X86/Runtime/Instructions.cpp` — `PopFromStack`, `PushToStack`
3. All ISEL opcodes that directly access `Read(REG_XSP)` / `Write(REG_XSP)`
4. `state_load` / `state_store` — remove ptrtoint/inttoptr for RSP
5. Rebuild bitcode

**Pros:**
- Clean, no special cases
- GEPs are generated natively by the compiler
- No post-processing needed
- instcombine preserves GEPs (it doesn't collapse `GEP(GEP(X,-8),8)` → `X`
  unless it can prove the offset is 0, which it can't for non-constant chains)

**Cons:**
- Changes State struct layout (breaks ABI with non-flat path)
- Need to update ALL ISEL code that touches RSP
- `UAdd(old_xsp, 8)` needs to become GEP arithmetic
- Memory reads at RSP: `Read(ReadPtr<T>(old_xsp))` needs the pointer, not i64

**Effort:** ~3-5 days

### Option B: Add parallel `rsp_ptr` field (union)

**What:** Add a `void *rsp_ptr` field alongside `gpr.rsp.qword`. In the flat
ABI, the ISEL uses `rsp_ptr`; in the non-flat path, it uses `gpr.rsp.qword`.

**Changes:**
1. `State.h` — add `rsp_ptr` field
2. `Read(REG_XSP)` macro — return `rsp_ptr` in flat mode
3. `Write(REG_XSP, val)` — write to `rsp_ptr` in flat mode
4. ISEL helpers — use pointer arithmetic in flat mode
5. `state_load` / `state_store` — set `rsp_ptr` directly (no cast)

**Pros:**
- Doesn't break non-flat path
- Can be done incrementally

**Cons:**
- Two sources of truth (must keep in sync)
- More complex macros
- Still need to update all ISEL RSP access

**Effort:** ~4-6 days

### Option C: Intercept at the `Read`/`Write` macro level (RECOMMENDED)

**What:** Keep the State struct unchanged. Modify the `Read(REG_XSP)` and
`Write(REG_XSP, val)` macros to use GEPs when in flat ABI mode. The ISEL
code doesn't change — the macros handle the conversion.

**Key insight:** The ISEL doesn't directly access `state->gpr.rsp.qword`.
It uses `Read(REG_XSP)` and `Write(REG_XSP, val)` macros. If we make these
macros return/accept a pointer (in flat mode), the ISEL's i64 arithmetic
(`old_xsp + 8`) becomes pointer arithmetic, which LLVM compiles to GEPs.

**Changes:**

1. **`include/remill/Arch/X86/Runtime/State.h`**
   - No changes needed. `gpr.rsp.qword` stays i64.

2. **`include/remill/BC/ABI.h`** (or a new `FlatRegs.h`)
   - Add `FlatRspRead(state)` → returns `(void *)(uintptr_t)state->gpr.rsp.qword`
   - Add `FlatRspWrite(state, ptr)` → `state->gpr.rsp.qword = (addr_t)(uintptr_t)ptr`

3. **`lib/Arch/X86/Runtime/Instructions.cpp`**
   - `PopFromStack`: Replace `Read(REG_XSP)` / `Write(REG_XSP, ...)` with
     GEP-based pointer arithmetic:
     ```c
     // Before:
     addr_t old_xsp = Read(REG_XSP);
     addr_t new_xsp = UAdd(old_xsp, op_size);
     T val = Read(ReadPtr<T>(old_xsp _IF_32BIT(REG_SS_BASE)));
     Write(REG_XSP, new_xsp);

     // After (flat mode):
     void *old_xsp = (void *)(uintptr_t)Read(REG_XSP);
     void *new_xsp = (void *)((char *)old_xsp + op_size);  // GEP
     T val = *reinterpret_cast<T *>(old_xsp);  // direct load
     Write(REG_XSP, (addr_t)(uintptr_t)new_xsp);
     ```
   - BUT: This still uses i64 internally. The GEP is only in the pointer cast.

4. **The real fix: Make `Read(REG_XSP)` return a `void *` in flat mode.**
   - This requires a **template specialization** or **preprocessor conditional**:
     ```c
     #ifdef REMILL_FLAT_ABI
     #define Read_RSP() ((void *)(uintptr_t)state->gpr.rsp.qword)
     #define Write_RSP(ptr) (state->gpr.rsp.qword = (addr_t)(uintptr_t)(ptr))
     #else
     #define Read_RSP() (state->gpr.rsp.qword)
     #define Write_RSP(val) (state->gpr.rsp.qword = (val))
     #endif
     ```
   - Then modify `PopFromStack`, `PushToStack`, and all direct RSP accesses
     to use `Read_RSP()` / `Write_RSP()` instead of `Read(REG_XSP)`.

**Pros:**
- Minimal changes to State struct
- Can be done incrementally (helper by helper)
- Non-flat path unchanged
- GEPs generated natively by the compiler

**Cons:**
- Preprocessor conditional is ugly
- Still need to update all ISEL RSP access points
- The `Read(REG_XSP)` macro is used in ~50+ places

**Effort:** ~2-3 days

### Option D: Post-process ISEL bitcode (NO ISEL CHANGES)

**What:** Don't modify the ISEL at all. In `flat_gen`, after SROA, run a
pass that:
1. Finds all `Read(REG_XSP)` / `Write(REG_XSP)` patterns (i64 load/store
   to `state->gpr.rsp.qword`)
2. Replaces them with GEP-based pointer operations
3. The GEPs are in the bitcode and survive the lift tool's optimization

**Changes:**
1. `tools/flat-gen/flat_gen.cpp` — add RSP GEP conversion pass (already
   partially implemented: 2992 patterns converted)
2. `lib/BC/Util.cpp` — modify lift tool to NOT run instcombine on
   RSP-derived GEPs (or re-run GEP normalization after instcombine)

**Pros:**
- No ISEL changes
- Already 80% implemented (2992 patterns converted in bitcode)

**Cons:**
- The lift tool's instcombine still breaks GEP chains
- Need to either disable instcombine for RSP GEPs or re-normalize after
- Fragile: depends on the ISEL's IR pattern not changing

**Effort:** ~1-2 days (to fix the instcombine issue)

## Recommendation

**Option C** (macro-level interception) is the best balance of effort and
result. It:
- Generates GEPs natively (no post-processing needed)
- Doesn't change the State struct
- Is incremental (can do helpers first, then opcodes)
- Preserves the non-flat path

## Implementation Plan (Option C)

### Phase 1: RSP pointer helpers in flat_gen wrappers (~0.5 day)

Instead of modifying the ISEL, modify the **flat_gen wrapper generation** to
handle RSP with GEPs:

1. In `flat_gen.cpp`, when generating the `_flat` wrapper:
   - After inlining the ISEL, find all `load i64, STATE[2312]` (RSP read)
     and `store i64, STATE[2312]` (RSP write) patterns
   - Replace the RSP read with `ptrtoint(RSP_arg)` (the pointer from the
     wrapper argument)
   - Replace the RSP write with a GEP: `GEP(RSP_arg, offset)` where offset
     is computed from the i64 arithmetic
   - This is what our current GEP conversion pass does (2992 patterns)

2. In the lift tool (`OptimizeFlatSSAFunction`):
   - After instcombine, re-run the GEP normalization to fix any GEP chains
     that were simplified
   - Or: skip instcombine for basic blocks that contain RSP GEPs

### Phase 2: Prevent instcombine from breaking RSP GEPs (~1 day)

Option A: Add `!llvm.loop` metadata or a custom attribute to RSP GEPs that
prevents simplification.

Option B: After the optimization loop, re-run a GEP normalization pass that:
1. Finds all GEPs from RSP/RBP
2. Computes the total offset by following the GEP chain
3. Replaces the chain with a single `GEP(RSP, total_offset)`
4. This restores the canonical form that `ForwardStackSlotAccesses` expects

Option C (simplest): Don't run instcombine in the lift tool. Only run
mem2reg + SROA + DCE. The GEPs from the bitcode are preserved. The IR is
slightly larger but correct.

### Phase 3: Store-load forwarding with canonical GEPs (~0.5 day)

With RSP GEPs preserved, the `ForwardStackSlotAccesses` pass can:
1. Match stores and loads by GEP offset
2. Forward values (eliminate push/pop pairs)
3. DCE dead stores

### Phase 4: Test + benchmark (~0.5 day)

- CTest: verify all 38 checks pass
- Simple test: `push rbp; mov rbp,rsp; pop rbp; ret` → 0 stores
- Quotearg: expect 0-5 stores (down from 17)
- O3: verify native code size

## Effort Estimate

| Phase | Time | Risk |
|-------|------|------|
| Phase 1 (GEP normalization in flat_gen) | 0.5 day | Low (already 80% done) |
| Phase 2 (preserve GEPs through lift) | 1 day | Medium |
| Phase 3 (store-load forwarding) | 0.5 day | Low |
| Phase 4 (test + benchmark) | 0.5 day | Low |
| **Total** | **2-3 days** | **Low-Medium** |

## Risk: Full ISEL Modification (Option A/C)

If we go the full route (modify ISEL to use GEPs natively):

| Task | Time | Risk |
|------|------|------|
| Change State struct RSP to ptr | 0.5 day | Medium (ABI break) |
| Update Read/Write macros | 0.5 day | Medium |
| Update PopFromStack/PushToStack | 0.5 day | Low |
| Update all ISEL RSP accesses (~50 places) | 1-2 days | Medium |
| Rebuild bitcode + fix errors | 1 day | Medium |
| Test (all opcodes) | 1 day | High |
| **Total** | **4-6 days** | **Medium-High** |

## Conclusion

The **pragmatic path** (Option D + Phase 2C) is:
1. Keep the 2992 GEP conversions in the bitcode (already done)
2. In the lift tool, skip instcombine OR re-normalize GEPs after it
3. Run `ForwardStackSlotAccesses` with canonical GEPs
4. Result: 0-5 stores for quotearg (down from 17)

This is **2-3 days** of work and achieves 95% of the benefit without touching
the ISEL.

The **full ISEL modification** (Option A/C) is **4-6 days** and achieves 100%
of the benefit but with higher risk.
