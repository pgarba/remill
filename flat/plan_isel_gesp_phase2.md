# Plan: GEP-based RSP/RBP Access in ISEL (Flat ABI)

## Goal

Make the ISEL generate GEPs natively for RSP/RBP stack access, eliminating
the `ptrtoint`/`inttoptr` round-trip. This enables the optimizer to properly
track stack slots and eliminate dead push/pop stores.

**Target:** quotearg O3 → 0-5 stores (down from 17). Simple test → 0 stores.

## Current State (Working Baseline)

- `--flat` mode: by-value SSA, 60 registers, RSP/RBP as `ptr`
- 0 memory model calls (RSP-as-ptr works)
- 31 GEPs on quotearg, 17 stores after O3
- All 38 CTest checks pass
- Branch: `flat_lifting_v2`, latest commit: `3d64467`
- **Uncommitted changes from this session** (verified with `git diff`):
  - `include/remill/Arch/X86/Runtime/State.h` — added `char *rsp_ptr; char *rbp_ptr;` to `X86State` + updated `static_assert` (+16 bytes)
  - `include/remill/Arch/X86/Runtime/FlatState.h` — jump signature: `addr_t rsp, addr_t rbp`
  - `include/remill/BC/ABI.h` — `kFlatRBPIndex`, `FlatRegIsRBP()`, `FlatRegIsStackPtr()`
  - `lib/Arch/X86/Arch.cpp` — `FlatRegIsStackPtr(i)` → `ptr`; `ptrtoint` in jump call
  - `lib/Arch/X86/Runtime/FlatState.cpp` — `state_load`: `void **rsp`/`void **rbp`, sets `state->rsp_ptr`/`state->rbp_ptr`; `state_store`: reads from `state->rsp_ptr`/`state->rbp_ptr`
  - `lib/BC/Util.cpp` — `EliminateStackMemoryAccesses`, `ForwardStackSlotAccesses` passes; removed `InstCombinePass` from `OptimizeFlatSSAFunction` loop
  - `tools/flat-gen/flat_gen.cpp` — **REVERTED to original** (no GEP conversion)

**Build:** `cd build && ninja remill-lift-21`
**Test:** `cd build/tests/X86 && ctest -R flat_lift`
**Lift test:** `./build/bin/lift/remill-lift-21 --arch amd64 --flat --bytes 554889e55dc3 --ir_out /tmp/test.ll`
**Quotearg:** bytes in `/tmp/quotearg_full.txt`

## What Was Tried and Why It Failed

### Attempt 1: GEP conversion in `flat_gen.cpp` (post-SROA)

Added a pass that converts `inttoptr(add(ptrtoint(X), C))` → `GEP(X, C)`
in the bitcode after SROA.

**Why it failed:** The `ScalarizeFlatFunction` pass (mem2reg + SROA) eliminates
the `inttoptr` from the inlined `state_store` before the GEP conversion sees it.
The `ret` ends up using the memory pointer from `__remill_write_memory` instead
of the RSP pointer. 0 patterns found.

### Attempt 2: GEP conversion in `flat_gen.cpp` (pre-optimization)

Moved the GEP conversion before the optimization passes.

**Why it failed:** Before SROA, the `inttoptr` is inside the `state_store`
function (separate function, not inlined into the wrapper). The pattern
`inttoptr(add(ptrtoint(X), C))` doesn't exist in a single function.

### Attempt 3: ISEL source changes with `REMILL_FLAT_ABI`

Modified `PUSH.cpp`, `POP.cpp`, `CALL_RET.cpp`, `Instructions.cpp` to use
`char *rsp_ptr` for RSP arithmetic in `#ifdef REMILL_FLAT_ABI` blocks.
Added `char *rsp_ptr; char *rbp_ptr;` to `X86State`.

**Why it failed:** Build errors in `Operators.h`:
```
static assertion failed: 'sizeof(RnW<unsigned short>) <= sizeof(unsigned short)'
cannot convert 'RnW<unsigned short>' to 'unsigned short'
```
The `#ifdef REMILL_FLAT_ABI` blocks inside **template functions**
(`PushToStack<T>`, `PopFromStack<T>`) use `char *` for RSP, which conflicts
with the template type system. The errors are in `Operators.h` (template
metaprogramming), not in the ISEL functions themselves.

**Key insight:** The `REMILL_FLAT_ABI` define changes the code path inside
templates, and the `char *` type in the RSP access conflicts with the
`RnW`/`MnW` template types in `Operators.h`. The exact mechanism is unclear
but the fix is to avoid putting type-changing code inside templates.

## What Needs To Be Done

### Phase 1: Fix the ISEL template conflict (~2-4 hours)

The `#ifdef REMILL_FLAT_ABI` blocks must NOT be inside template functions.
Instead, use a **helper function** that is selected at compile time:

**File: `lib/Arch/X86/Runtime/Instructions.cpp`**

Add non-template helper functions (outside any template):

```cpp
#ifdef REMILL_FLAT_ABI
// Non-template helpers for flat ABI RSP/RBP pointer access.
// These are NOT templates, so they don't conflict with RnW/MnW.
inline char *FlatRspGet() { return state.rsp_ptr; }
inline void FlatRspSet(char *p) {
    state.rsp_ptr = p;
    state.gpr.rsp.qword = (addr_t)(uintptr_t)p;
}
inline char *FlatRbpGet() { return state.rbp_ptr; }
inline void FlatRbpSet(char *p) {
    state.rbp_ptr = p;
    state.gpr.rbp.qword = (addr_t)(uintptr_t)p;
}
#endif
```

Wait — `state` is a local variable in each ISEL function, not a global.
The helpers need to take `State &state` as a parameter:

```cpp
#ifdef REMILL_FLAT_ABI
inline char *FlatRspGet(State &s) { return s.rsp_ptr; }
inline void FlatRspSet(State &s, char *p) {
    s.rsp_ptr = p;
    s.gpr.rsp.qword = (addr_t)(uintptr_t)p;
}
inline char *FlatRbpGet(State &s) { return s.rbp_ptr; }
inline void FlatRbpSet(State &s, char *p) {
    s.rbp_ptr = p;
    s.gpr.rbp.qword = (addr_t)(uintptr_t)p;
}
#endif
```

**Then in the template functions**, call the helpers instead of inlining
the `char *` code:

```cpp
template <typename T>
DEF_HELPER(PopFromStack)->T {
  addr_t op_size = TruncTo<addr_t>(sizeof(T));
#ifdef REMILL_FLAT_ABI
  char *old_xsp = FlatRspGet(state);
  T val = *reinterpret_cast<T *>(old_xsp);
  FlatRspSet(state, old_xsp + op_size);
#else
  addr_t old_xsp = Read(REG_XSP);
  addr_t new_xsp = UAdd(old_xsp, op_size);
  T val = Read(ReadPtr<T>(old_xsp _IF_32BIT(REG_SS_BASE)));
  Write(REG_XSP, new_xsp);
#endif
  return val;
}
```

**Key rule:** The `#ifdef REMILL_FLAT_ABI` block must only call
non-template helper functions. No `char *` variables or pointer arithmetic
directly in the template body. This avoids the `RnW`/`MnW` conflict.

**Files to modify:**

1. **`include/remill/Arch/X86/Runtime/State.h`**
   - Already done: `char *rsp_ptr; char *rbp_ptr;` added to `X86State`
   - Already done: `static_assert` updated (+16)

2. **`lib/Arch/X86/Runtime/FlatState.cpp`**
   - Already done: `state_load` sets `state->rsp_ptr`/`state->rbp_ptr`
   - Already done: `state_store` reads from `state->rsp_ptr`/`state->rbp_ptr`

3. **`lib/Arch/X86/Runtime/Instructions.cpp`**
   - Add `FlatRspGet/FlatRspSet/FlatRbpGet/FlatRbpSet` helper functions
   - Modify `PopFromStack<T>` to use helpers in `#ifdef REMILL_FLAT_ABI`
   - Do NOT put `char *` variables directly in the template body

4. **`lib/Arch/X86/Semantics/PUSH.cpp`**
   - Modify `PushToStack<T>` to use helpers in `#ifdef REMILL_FLAT_ABI`
   - Pattern: `char *new_xsp = FlatRspGet(state) - op_size;`
   - Store: `*reinterpret_cast<T *>(new_xsp) = val;`
   - Update: `FlatRspSet(state, new_xsp);`

5. **`lib/Arch/X86/Semantics/POP.cpp`**
   - Modify `POP<D>` and `POP_MEM_XSP<D>` similarly
   - Skip segment register pops (POP_ES, POP_SS, etc.) for now

6. **`lib/Arch/X86/Semantics/CALL_RET.cpp`**
   - Modify `CALL`, `RET`, `RET_IMM` similarly
   - CALL: `char *next_sp = FlatRspGet(state) - ADDRESS_SIZE_BYTES;`
   - RET: `auto new_pc = *reinterpret_cast<addr_t *>(FlatRspGet(state));`
   - RET: `FlatRspSet(state, FlatRspGet(state) + ADDRESS_SIZE_BYTES);`

7. **`lib/Arch/X86/Semantics/MISC.cpp`**
   - LEAVE, ENTER: skip for now (rare instructions)

### Phase 2: Build the flat bitcode (~30 min)

**Option A: Separate CMake target (preferred)**

Add to `lib/Arch/X86/Runtime/CMakeLists.txt`:

```cmake
# Inside the if(CMAKE_SIZEOF_VOID_P EQUAL 8) block:
add_runtime_helper(amd64_flat_source 64 0 0 "REMILL_FLAT_ABI=1")
```

The `add_runtime_helper` function needs to accept an optional `extra_defs`
parameter (5th arg). Modify the function:

```cmake
function(add_runtime_helper target_name address_bit_size enable_avx enable_avx512 extra_defs)
  # ... existing code ...
  set(defs "HAS_FEATURE_AVX=${enable_avx}" "HAS_FEATURE_AVX512=${enable_avx512}")
  if(extra_defs)
    list(APPEND defs ${extra_defs})
  endif()
  add_runtime(${target_name}
    # ... use ${defs} instead of hardcoded definitions ...
  )
endfunction()
```

Update existing calls to pass `""` as 5th arg:
```cmake
add_runtime_helper(x86 32 0 0 "")
add_runtime_helper(amd64 64 0 0 "")
# etc.
```

Update `tools/flat-gen/CMakeLists.txt` to use `amd64_flat_source.bc`:
```cmake
set(BASE_BC_INPUT "${CMAKE_BINARY_DIR}/lib/Arch/X86/Runtime/amd64_flat_source.bc")
# DEPENDS ${REMILL_FLAT_GEN} amd64_flat_source
```

**Option B: Define `REMILL_FLAT_ABI` in the existing target**

Simpler but affects the non-flat path. Only safe if the `#ifdef` blocks
are properly guarded.

### Phase 3: Update `flat_gen.cpp` (~1 hour)

With the ISEL generating GEPs natively, the `flat_gen.cpp` changes are:

1. **Remove the GEP conversion pass** (no longer needed — GEPs are in the ISEL)
2. **Keep the `ScalarizeFlatFunction` loop** (scalarize all wrappers)
3. **Keep the optimization passes** (instcombine, simplifycfg, DCE)
4. **The `state_load`/`state_store` no longer need `ptrtoint`/`inttoptr` for RSP/RBP**
   — they use `state.rsp_ptr`/`state.rbp_ptr` directly

Actually, the `state_load`/`state_store` in `FlatState.cpp` still do:
```cpp
state->gpr.rsp.qword = (addr_t)(uintptr_t)*rsp;  // i64
state->rsp_ptr = (char *)*rsp;                    // pointer (new)
```

The `ptrtoint`/`inttoptr` are gone — the pointer is stored directly. The ISEL
uses `state.rsp_ptr` (pointer) for RSP arithmetic. The compiler generates GEPs.

### Phase 4: Lift tool changes (~30 min)

In `lib/BC/Util.cpp` (`OptimizeFlatSSAFunction`):

1. **InstCombine is already removed** from the optimization loop (done this session)
2. **`EliminateStackMemoryAccesses`** should still work (converts
   `__remill_read/write_memory` to GEP+load/store for non-RSP addresses)
3. **`ForwardStackSlotAccesses`** should now work because GEPs are preserved
   (no instcombine to break them)
4. **The `ptrtoint` at jump boundary** in `Arch.cpp` should still work
   (RSP/RBP are `ptr` in the function body, `i64` at the jump)

### Phase 5: Test (~1 hour)

1. **Build:** `cd build && cmake .. && ninja remill-lift-21`
2. **Regen flat bitcode:** `ninja flat-bitcode`
3. **CTest:** `cd build/tests/X86 && ctest -R flat_lift` (38 checks)
4. **Simple test:**
   ```
   ./build/bin/lift/remill-lift-21 --arch amd64 --flat \
     --bytes 554889e55dc3 --ir_out /tmp/gep_test.ll
   ```
   Expected: 0 stores (push/pop/call/ret all eliminated)
5. **Quotearg test:**
   ```
   BYTES=$(cat /tmp/quotearg_full.txt)
   ./build/bin/lift/remill-lift-21 --arch amd64 --flat \
     --bytes "$BYTES" --ir_out /tmp/quotearg_gep.ll
   ```
   Expected: 0-5 stores (down from 17)
6. **O3 test:**
   ```
   $LLVM/opt -O3 /tmp/quotearg_gep.ll -S -o /tmp/quotearg_gep_O3.ll
   grep -c 'store' /tmp/quotearg_gep_O3.ll
   ```
   Expected: 0-5 stores

### Phase 6: Fix regressions (~variable)

Likely issues:
- **Template errors:** If the `#ifdef` blocks still conflict with `RnW`/`MnW`,
  move ALL flat-specific code into non-template helper functions
- **`state.gpr.rsp.qword` sync:** The ISEL helpers update both `state.rsp_ptr`
  and `state.gpr.rsp.qword`. Make sure both are updated in every helper
- **32-bit mode:** The `#ifdef REMILL_FLAT_ABI` blocks should be inside
  `IF_64BIT(...)` guards (flat ABI is 64-bit only)
- **Segment register pops:** POP_ES/SS/DS/FS/GS still use `Read(REG_XSP)`
  (i64). They won't get GEPs but will still work (via `state.gpr.rsp.qword`)

## Risk Assessment

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Template errors persist | Medium | Move ALL flat code to non-template helpers |
| `state.gpr.rsp.qword` out of sync | Low | Always update both in helpers |
| SROA eliminates GEPs | Low | GEPs are "real" (from compiler), not post-processed |
| Instcombine still breaks GEPs | Medium | Already removed from lift tool pipeline |
| 32-bit mode broken | Low | Flat ABI is 64-bit only; guard with `IF_64BIT` |

## Effort Estimate

| Phase | Time |
|-------|------|
| Phase 1: Fix ISEL template conflict | 2-4 hours |
| Phase 2: Build flat bitcode | 30 min |
| Phase 3: Update flat_gen.cpp | 1 hour |
| Phase 4: Lift tool changes | 30 min |
| Phase 5: Test | 1 hour |
| Phase 6: Fix regressions | 1-4 hours |
| **Total** | **6-12 hours** |

## Key Files

| File | Change |
|------|--------|
| `include/remill/Arch/X86/Runtime/State.h` | `rsp_ptr`/`rbp_ptr` fields (DONE) |
| `lib/Arch/X86/Runtime/FlatState.cpp` | `state_load`/`state_store` use ptr (DONE) |
| `lib/Arch/X86/Runtime/Instructions.cpp` | Add helpers, modify `PopFromStack` |
| `lib/Arch/X86/Semantics/PUSH.cpp` | Modify `PushToStack` |
| `lib/Arch/X86/Semantics/POP.cpp` | Modify `POP`, `POP_MEM_XSP` |
| `lib/Arch/X86/Semantics/CALL_RET.cpp` | Modify `CALL`, `RET`, `RET_IMM` |
| `lib/Arch/X86/Runtime/CMakeLists.txt` | Add `amd64_flat_source` target |
| `tools/flat-gen/CMakeLists.txt` | Use `amd64_flat_source.bc` |
| `tools/flat-gen/flat_gen.cpp` | Remove GEP conversion (not needed) |
| `lib/BC/Util.cpp` | InstCombine removed from loop (DONE) |

## Critical Constraint

**The `#ifdef REMILL_FLAT_ABI` blocks must NOT contain `char *` variables
or pointer arithmetic directly in template function bodies.** All flat-specific
RSP/RBP access must go through non-template inline helper functions
(`FlatRspGet`, `FlatRspSet`, etc.) to avoid the `RnW`/`MnW` template
assertion failures in `Operators.h`.
