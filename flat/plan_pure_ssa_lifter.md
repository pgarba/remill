# Pure SSA Lifter (No State Struct) — Implementation Plan

## Goal

Remove the STATE_LOCAL alloca + flat_state_load/store + GEPs from the flat
lifting pipeline. The 51 pointer args ARE the register addresses. Load/store
through them directly. No struct. No SROA needed.

## Current Flow (with struct)

```
func(pc, mem, *rax, *rbx, ..., *xmm15) {
  %STATE_LOCAL = alloca i8, 2048              ; 2 KB struct on stack
  call @__remill_flat_state_load(%STATE_LOCAL, %rax, %rbx, ...)  ; load all 51 regs INTO struct
  ; ...
  %rax_ptr = GEP %STATE_LOCAL, offset_of_rax  ; GEP into struct
  %rax_val = load %rax_ptr                    ; load from struct
  %result = add %rax_val, %rbx_val
  store %result, %rax_ptr                     ; store to struct
  ; ...
  call @__remill_flat_state_store(%rax, %rbx, ..., %STATE_LOCAL)  ; load all 51 regs OUT of struct
  tail call @__remill_flat_jump(...)
}
```

Problem: GEPs into an alloca → SROA must scalarize it. 220/1589 fail.

## Target Flow (pure SSA, no struct)

```
func(pc, mem, *rax, *rbx, ..., *xmm15) {
  %rax_val = load i64, ptr %rax               ; direct load from pointer arg
  %rbx_val = load i64, ptr %rbx               ; direct load from pointer arg
  %result = add %rax_val, %rbx_val            ; pure SSA arithmetic
  store %result, ptr %rax                     ; direct store to pointer arg
  tail call @__remill_flat_jump(...)
}
```

No alloca. No GEPs. No flat_state_load/store. No SROA. The pointer args
are the register addresses, period.

## What Changes

### 1. `TraceLifter` — `InitializeEmptyLiftedFunction`

**Current (flat mode):**
```cpp
// Create STATE_LOCAL alloca
auto *state_alloca = ir.CreateAlloca(state_type, nullptr, "STATE_LOCAL");
// Call __remill_flat_state_load to populate it
ir.CreateCall(flat_state_load_func, {state_alloca, arg0, arg1, ...});
```

**New:**
```cpp
// Do nothing. The pointer args are already the register addresses.
// No alloca. No state_load.
```

### 2. `InstructionLifter` — `GetStatePointer`

**Current (flat mode):**
```cpp
// Find STATE_LOCAL alloca in entry block
for (auto &inst : func->getEntryBlock()) {
  if (auto alloca = dyn_cast<AllocaInst>(&inst)) {
    if (alloca->getName() == "STATE_LOCAL") return alloca;
  }
}
```

**New:**
```cpp
// Return nullptr (or a sentinel). In pure-SSA mode there is no state pointer.
// LoadRegAddress will use the pointer args directly.
return nullptr;
```

### 3. `InstructionLifter` — `LoadRegAddress`

**Current:**
```cpp
// state_ptr is the STATE_LOCAL alloca
// reg->AddressOf(state_ptr, ir) → GEP into struct
reg_ptr = reg->AddressOf(state_ptr, ir);
```

**New:**
```cpp
// state_ptr is nullptr → look up the pointer arg directly
// The register name maps to the Nth pointer argument
// "RAX" → arg 2, "RBX" → arg 3, ..., "XMM15" → arg 52
auto *ptr_arg = NthArgument(func, kFlatFirstRegArgNum + reg_index);
reg_ptr = ptr_arg;  // it's already a pointer to the register
```

This is the core change. Instead of GEP into a struct, just return the
function argument that corresponds to that register.

### 4. `InstructionLifter` — `LoadRegValue`

**Current:**
```cpp
auto [ptr, ptr_ty] = LoadRegAddress(block, state_ptr, reg_name);
return new LoadInst(ptr_ty, ptr, "", block);  // load from GEP
```

**New:**
```cpp
// Same code, but LoadRegAddress now returns the pointer arg directly.
// LoadInst loads from the pointer arg. No GEP involved.
auto [ptr, ptr_ty] = LoadRegAddress(block, state_ptr, reg_name);
return new LoadInst(ptr_ty, ptr, "", block);  // load from pointer arg
```

No change needed here! The load still happens, but from the pointer arg
directly instead of from a GEP.

### 5. `Arch` — `FinishFlatLiftedFunction`

**Current:**
```cpp
// Before each tail call to __remill_flat_jump:
//   1. Call __remill_flat_state_store(args, STATE_LOCAL)
//   2. Retarget tail call to __remill_flat_jump
```

**New:**
```cpp
// Before each tail call to __remill_flat_jump:
//   1. (No state_store needed — regs are already in the pointer args)
//   2. Retarget tail call to __remill_flat_jump
```

Remove the `flat_state_store` call. The stores already happened inline
(during instruction lifting). The pointer args already have the final values.

### 6. `Lift.cpp` — Remove SROA call

**Current:**
```cpp
if (flat) {
  ScalarizeFlatFunction(module.get(), func);  // SROA
}
```

**New:**
```cpp
// No SROA needed. No struct to scalarize.
// Optionally: run mem2reg to promote any remaining allocas
// (but there shouldn't be any)
```

### 7. `FlatState.cpp` — Remove (or keep for old mode)

`__remill_flat_state_load` and `__remill_flat_state_store` are no longer
called by the lifter. They can be:
- Removed (if nothing else uses them)
- Kept (for the old flat mode / debugging)

## Register → Argument Index Mapping

```
  arg 0:  pc (addr_t)
  arg 1:  memory (Memory *)
  arg 2:  RAX      (addr_t *)
  arg 3:  RBX      (addr_t *)
  arg 4:  RCX      (addr_t *)
  arg 5:  RDX      (addr_t *)
  arg 6:  RSI      (addr_t *)
  arg 7:  RDI      (addr_t *)
  arg 8:  R8       (addr_t *)
  arg 9:  R9       (addr_t *)
  arg 10: R10      (addr_t *)
  arg 11: R11      (addr_t *)
  arg 12: R12      (addr_t *)
  arg 13: R13      (addr_t *)
  arg 14: R14      (addr_t *)
  arg 15: R15      (addr_t *)
  arg 16: RSP      (addr_t *)
  arg 17: RBP      (addr_t *)
  arg 18: RIP      (addr_t *)
  arg 19: SS_BASE  (addr_t *)
  arg 20: CS_BASE  (addr_t *)
  arg 21: GS_BASE  (addr_t *)
  arg 22: CF       (uint8_t *)
  arg 23: PF       (uint8_t *)
  arg 24: AF       (uint8_t *)
  arg 25: ZF       (uint8_t *)
  arg 26: SF       (uint8_t *)
  arg 27: OF       (uint8_t *)
  arg 28: DF       (uint8_t *)
  arg 29: XMM0     (vec128_t *)
  ...
  arg 44: XMM15    (vec128_t *)
```

The `Register::AddressOf` method currently does:
```cpp
// GEP: state_ptr + offset_of_this_register_in_struct
Value *AddressOf(Value *state_ptr, IRBuilder &ir) {
  return ir.CreateGEP(state_type, state_ptr, {0, field_index});
}
```

New behavior (when state_ptr is nullptr / pure-SSA mode):
```cpp
// Just return the pointer argument directly
// The register index maps to arg (kFlatFirstRegArgNum + index)
Value *AddressOf(Value *state_ptr, IRBuilder &ir) {
  if (state_ptr == nullptr) {
    // Pure SSA mode: return the function arg
    return parent_func->getArg(kFlatFirstRegArgNum + reg_index);
  }
  // Old mode: GEP into struct
  return ir.CreateGEP(state_type, state_ptr, {0, field_index});
}
```

## Files to Modify

| File | Change |
|------|--------|
| `lib/BC/TraceLifter.cpp` | Remove STATE_LOCAL alloca + flat_state_load in flat mode |
| `lib/BC/InstructionLifter.cpp` | `LoadRegAddress`: if state_ptr is null, return pointer arg |
| `lib/Arch/X86/Arch.cpp` | `FinishFlatLiftedFunction`: remove flat_state_store call |
| `lib/BC/InstructionLifter.cpp` | `GetStatePointer`: return nullptr in pure-SSA mode |
| `bin/lift/Lift.cpp` | Remove `ScalarizeFlatFunction` call (or make it optional) |
| `include/remill/Arch/X86/Register.h` | `AddressOf`: handle nullptr state_ptr → return arg |
| `lib/Arch/X86/Register.cpp` | Same |

## What Stays The Same

- Function signature: 53 args (pc, mem, 51 ptrs) — unchanged
- `__remill_flat_jump`: still the tail call target — unchanged
- ISEL semantics functions: unchanged (they call LoadRegValue/StoreRegValue)
- Old mode: unchanged (state struct still works)
- The `--flat` flag: same flag, just different internal behavior

## What We Lose

- SROA scalarization (don't need it)
- The 96% scalarization rate metric (irrelevant — no struct to scalarize)
- The 220 "un-scalarized" functions problem (goes away — no struct)

## What We Gain

- Simpler IR (no alloca, no GEPs into struct)
- No SROA dependency (no pipeline, no TM, no segfault risk)
- All 1589 opcodes work identically (no pointer escape issue)
- Smaller IR (no struct load/store of all 51 regs at entry/exit)
- Easier to debug (the IR is what the machine code looks like)

## Expected IR Output

```llvm
define ptr @sub_0(i64 %pc, ptr %memory, ptr %rax, ptr %rbx, ptr %rcx,
                  ptr %rdx, ptr %rsi, ptr %rdi, ptr %r8, ptr %r9,
                  ptr %r10, ptr %r11, ptr %r12, ptr %r13, ptr %r14,
                  ptr %r15, ptr %rsp, ptr %rbp, ptr %rip,
                  ptr %ss_base, ptr %cs_base, ptr %gs_base,
                  i8 %cf, i8 %pf, i8 %af, i8 %zf, i8 %sf, i8 %of, i8 %df,
                  <2 x i64> %xmm0, ..., <2 x i64> %xmm15) {
entry:
  %0 = load i64, ptr %rsp
  %1 = sub i64 %0, 16
  store i64 %1, ptr %rsp

  %2 = load i64, ptr %rbp
  store i64 %2, ptr %rsp       ; mov [rsp], rbp

  %3 = load i64, ptr %rcx
  store i64 %3, ptr %rax       ; mov rax, rcx (nop)

  tail call ptr @__remill_flat_jump(i64 %pc, ptr %memory,
      ptr %rax, ptr %rbx, ptr %rcx, ...)
}
```

Compare to current (with SROA):
- Same number of loads/stores
- No alloca
- No GEPs
- No flat_state_load/store
- No SROA pipeline needed

## Verification

```bash
# Lift with pure SSA mode
remill-lift-21 --flat --bytes "4889e54883ec10c9c3" --ir_out /tmp/pure_ssa.ll

# Verify: no alloca
grep -c "alloca" /tmp/pure_ssa.ll        # expect: 0

# Verify: no GEP into State
grep -c "getelementptr.*State" /tmp/pure_ssa.ll  # expect: 0

# Verify: no flat_state_load/store
grep -c "flat_state" /tmp/pure_ssa.ll    # expect: 0

# Verify: direct loads from pointer args
grep "load" /tmp/pure_ssa.ll             # should show: load i64, ptr %rsp

# Verify: still correct (same as before)
# Compare with: remill-lift-21 --flat --bytes "..." --ir_out /tmp/old_flat.ll
# Both should produce equivalent machine code
```

## Effort

| Task | Time |
|------|------|
| Modify `LoadRegAddress` to return pointer arg when state_ptr is null | 2h |
| Remove STATE_LOCAL alloca + flat_state_load from TraceLifter | 1h |
| Remove flat_state_store from FinishFlatLiftedFunction | 1h |
| Remove SROA call from Lift.cpp | 30m |
| Handle special regs (PC, NEXT_PC, MEMORY) that aren't pointer args | 2h |
| Test: single instruction, multi-instruction, branch | 2h |
| Test: all 1589 opcodes (run flat-gen equivalent) | 2h |
| Fix edge cases (x87, AVX512, K_REG) | 2h |
| **Total** | **~1 day** |

## Open Question: PC, NEXT_PC, MEMORY

These are currently stored in the state struct but aren't pointer args:
- `PC` → arg 0 (i64, by value)
- `NEXT_PC` → derived from PC + instr_length
- `MEMORY` → arg 1 (ptr, by value)

In pure-SSA mode:
- `LoadRegAddress(block, state_ptr, "PC")` → return arg 0 (or a local copy)
- `LoadRegAddress(block, state_ptr, "NEXT_PC")` → compute PC + len
- `LoadRegAddress(block, state_ptr, "MEMORY")` → return arg 1

These need special handling since they're by-value args, not pointer args.
Options:
1. Create a local alloca for PC/NEXT_PC (just 2 × 8 bytes, SROA-able trivially)
2. Pass them as pointer args too (add 2 more args: *pc, *next_pc)
3. Use SSA values directly (no pointer, just the value)

Option 3 is cleanest but requires changing how `LoadRegValue` works for PC.
Option 2 is simplest (just add 2 more pointer args to the ABI).

**Recommendation: Option 2** — add `*pc` and `*next_pc` as pointer args.
Keeps the "everything is a pointer" invariant. MEMORY is already a pointer arg.
