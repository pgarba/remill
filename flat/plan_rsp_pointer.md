# Plan: RSP as Pointer (Stack Slot Elimination)

## Goal

Eliminate `__remill_read/write_memory` calls for all RSP-relative memory
accesses by making RSP a `ptr` in the flat ABI. The Saturn runtime allocates
the stack buffer and passes a pointer; the lifted function uses it directly.

## Current State

```llvm
; RSP is i64 in the flat ABI
define ptr @sub_0(..., i64 %RSP, ...) {
  ; Every stack access goes through the memory model:
  %addr = add i64 %RSP, -8
  %v = call @__remill_read_memory_64(%memory, %addr)
}
```

## Target State

```llvm
; RSP is ptr in the flat ABI
define ptr @sub_0(..., ptr %RSP, ...) {
  ; Stack accesses are direct pointer operations:
  %v = load i64, ptr %RSP, align 8
  %RSP.1 = getelementptr i8, ptr %RSP, i64 8

  ; Loop buffers work for free:
  %idx = mul i64 %RCX, 8
  %ptr = getelementptr i8, ptr %RSP, i64 %idx
  store i64 %RAX, ptr %ptr
}
```

## Changes Required

### 1. ABI Change (`include/remill/BC/ABI.h`)

- Add a helper: `FlatRegIsRSP(idx)` → `idx == 6` (RSP is at index 6)
- RSP type changes from `i64` to `ptr`
- All other registers unchanged

### 2. Function Type (`lib/Arch/X86/Arch.cpp`)

- `FlatLiftedFunctionType`: RSP arg type → `ptr` instead of `i64`
- `kFlatRegs`: RSP entry unchanged (name "rsp")

### 3. REG_RSP Alloca (`lib/Arch/X86/Arch.cpp`)

- `InitializeFlatLiftedFunction`: REG_RSP alloca type → `ptr`
- Entry: `store ptr %RSP_arg, ptr %REG_RSP` (was: `store i64`)

### 4. state_load/store (`lib/Arch/X86/Runtime/FlatState.cpp`)

- `state_load`: RSP param type → `ptr`
  ```c
  state->gpr.rsp.qword = (addr_t)(uintptr_t)*rsp;  // ptr → i64
  ```
- `state_store`: RSP param type → `ptr`
  ```c
  *rsp = (ptr)(uintptr_t)state->gpr.rsp.qword;    // i64 → ptr
  ```

### 5. Jump Function (`lib/Arch/X86/Runtime/FlatState.h/cpp`)

- `__remill_flat_jump`: RSP param → `ptr` (was `addr_t`)
- Implementation: cast to `addr_t` for storage

### 6. ISEL Call (`lib/BC/InstructionLifter.cpp`)

- REG_RSP alloca is now `ptr` type
- ISEL wrapper still expects `addr_t *` (pointer to i64) for RSP
- **Bridge**: Create a local `i64` alloca, `ptrtoint` the RSP pointer into it,
  pass that to the ISEL wrapper
- OR: Change the ISEL wrapper's RSP param to `ptr` type (cleaner, requires
  regenerating flat bitcode)

### 7. Post-Processing Pass: `StackAccessElimination` (NEW)

In `lib/BC/Util.cpp`, add a pass that:

1. Finds all `__remill_read_memory_*` / `__remill_write_memory_*` calls
2. Checks if the address operand is derived from the RSP pointer:
   - `ptr %RSP` directly
   - `getelementptr i8, ptr %RSP, i64 <const>`
   - `getelementptr i8, ptr %RSP, i64 <reg>` (variable offset)
   - Any GEP chain starting from REG_RSP
3. Replaces with direct load/store:
   ```llvm
   ; Before:
   %addr = getelementptr i8, ptr %RSP, i64 -8
   %v = call @__remill_read_memory_64(%mem, ptrtoint %addr)
   
   ; After:
   %v = load i64, ptr %addr, align 8
   ```
4. For writes:
   ```llvm
   ; Before:
   call @__remill_write_memory_64(%mem, %addr, %val)
   
   ; After:
   store i64 %val, ptr %addr, align 8
   ```

**Pattern matching:**
- Track the REG_RSP pointer through the function
- Any GEP from REG_RSP (with any offset, including register-based) is a stack access
- The address must be a `ptr` type (GEP result)

### 8. flat_gen (`tools/flat-gen/flat_gen.cpp`)

- RSP param in wrappers: `addr_t *rsp` → `ptr rsp` (or keep as `ptr` to `i64`)
- state_load/store: RSP param type change
- Regenerate `amd64_flat.bc`

### 9. flat_gen state_load/store signature

The `_flat` wrapper passes REG_RSP to state_load. If REG_RSP is `ptr`:
- Option A: state_load takes `ptr rsp` and does `ptrtoint` internally
- Option B: The wrapper does `ptrtoint` before calling state_load (keeps state_load signature)

**Recommendation: Option A** (cleaner, state_load handles the cast)

### 10. CTest Updates

- Verify RSP arg is `ptr` type in function signature
- Verify no `__remill_read/write_memory` calls for RSP-derived addresses
- Verify `getelementptr` + `load/store` pattern for stack accesses

## Implementation Order

1. **ABI + function type**: Change RSP to `ptr` in the signature
2. **state_load/store**: Handle `ptr` RSP (cast to/from `addr_t`)
3. **Jump function**: Update RSP param type
4. **flat_gen**: Update wrapper generation, regenerate bitcode
5. **Post-processing pass**: `StackAccessElimination`
6. **Build + test**: Verify non-stack code still works
7. **Benchmark**: Measure reduction in memory calls
8. **CTest**: Add checks for the new pattern

## Risks

1. **RSP used as data pointer**: If code does `mov rax, [rsp]` where RSP
   points to heap data (not stack), the pointer dereference would read from
   the wrong place. **Mitigation**: The runtime ensures RSP points to the
   allocated stack buffer. If the original code uses RSP as a data pointer,
   the runtime must have allocated enough space.

2. **RSP modification by ISEL**: The ISEL computes `new_rsp = rsp + offset`
   as an i64. The state_store converts back to ptr. This works because the
   runtime's stack buffer is in the "simulated" address space.

3. **Alignment**: Stack accesses must be properly aligned. The runtime
   should align the stack buffer to 16 bytes.

4. **Bounds checking**: If the lifted code accesses beyond the allocated
   stack, it's undefined behavior. The runtime should allocate with
   sufficient margin (e.g., 2× the observed max depth).

## Expected Impact

For xstrtoumax (334 instr):
- Current: 22 `__remill_read/write_memory` calls (O3)
- Target: 0 stack memory calls, only heap/generic memory calls remain
- Native code: potentially 20-40% smaller (no memory model overhead for stack)

For quotearg (1995 instr):
- Current: many memory calls for stack operations
- Target: all RSP-relative accesses become direct load/store
- The 60-arg ABI overhead remains, but memory calls are eliminated

## Saturn Runtime Integration

The runtime already allocates a stack buffer for each lifted function.
With this change:
- The runtime passes the stack buffer pointer as the RSP argument
- No changes needed in the runtime (it already does this)
- The "wrong length" problem is solved: the runtime sizes the stack,
  the lifted function just uses the pointer

## File Changes Summary

| File | Change |
|------|--------|
| `include/remill/BC/ABI.h` | Add `FlatRegIsRSP()` helper |
| `lib/Arch/X86/Arch.cpp` | RSP arg type → ptr, REG_RSP alloca → ptr |
| `lib/Arch/X86/Runtime/FlatState.h` | Jump: RSP → ptr |
| `lib/Arch/X86/Runtime/FlatState.cpp` | state_load/store: RSP ptr→i64 cast |
| `lib/BC/InstructionLifter.cpp` | ISEL call: bridge ptr RSP to i64 |
| `lib/BC/Util.cpp` | New: StackAccessElimination pass |
| `tools/flat-gen/flat_gen.cpp` | Wrapper: RSP param → ptr |
| `tests/X86/FlatLift.cpp` | Verify ptr RSP, no mem calls for stack |

## Estimate

- **Core ABI + state_load/store**: ~4 hours
- **flat_gen + bitcode regen**: ~2 hours
- **StackAccessElimination pass**: ~4 hours
- **Testing + benchmarking**: ~2 hours
- **Total**: ~2 days
