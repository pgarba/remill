# Flat Lifting Design Document

## Overview

Flat lifting eliminates the `State` struct indirection from lifted code. Instead of
loading/storing registers through a pointer to a large struct, each register is a
direct pointer argument to the lifted function. This produces smaller, faster IR
that is already in SSA form.

Two modes are available:

| Flag | Mode | State access | Post-pass | ISEL source |
|------|------|--------------|-----------|-------------|
| `--flat` | Struct-pointer | STATE_LOCAL alloca + GEPs | SROA | `amd64.bc` |
| `--flat-ssa` | Pure-SSA | Pointer args directly | Inline only | `amd64_flat.bc` |
| *(none)* | Original | Function arg + GEPs | None | `amd64.bc` |

## Flat ABI

All flat modes share the same 54-argument function signature:

```
Memory *F(addr_t *pc, Memory *memory, addr_t *next_pc,
          ptr reg_0, ptr reg_1, ..., ptr reg_50)
```

| Index | Name | Type | Description |
|-------|------|------|-------------|
| 0 | `PC` | `addr_t *` | Current program counter (read/write) |
| 1 | `memory` | `Memory *` | Memory pointer (by value) |
| 2 | `NEXT_PC` | `addr_t *` | Next program counter (read/write) |
| 3–19 | `RAX`…`RIP` | `ptr` | 17 GPR pointers |
| 20–22 | `SS_BASE`…`CS_BASE` | `ptr` | 3 segment base pointers |
| 23–29 | `CF`…`OF` | `ptr` | 7 flag pointers (1 byte each) |
| 30–37 | `MM0`…`MM7` | `ptr` | 8 MMX register pointers |
| 38–53 | `XMM0`…`XMM15` | `ptr` | 16 XMM register pointers |

The caller allocates 51 storage slots, passes their addresses, and reads back the
modified values after the call. The lifted function owns the stack (RSP is in-out).

## Mode 1: `--flat` (Struct-Pointer)

### Flow

```
Entry:
  alloca State STATE_LOCAL
  __remill_flat_state_load(&STATE_LOCAL, *RAX, *RBX, ..., *XMM15)
  ...lift instructions (ISEL uses GEPs into STATE_LOCAL)...
Exit:
  __remill_flat_state_store(*RAX, *RBX, ..., *XMM15, &STATE_LOCAL)
  tail call __remill_flat_jump(*PC, memory, *NEXT_PC, regs...)
Post-lift:
  SROA scalarizes STATE_LOCAL (94% success rate)
```

### Characteristics
- Uses original `amd64.bc` ISEL functions (no build-time transform)
- SROA eliminates the struct for most functions
- 14% of functions retain the alloca (pointer escape to helpers)
- Smallest output (~58 lines for 3-instr block)

## Mode 2: `--flat-ssa` (Pure-SSA)

### Flow

```
Entry:
  *NEXT_PC = *PC
  MEMORY alloca = memory arg
  segment base allocas = 0
  ...lift instructions (call _flat ISEL wrappers directly)...
Exit:
  tail call __remill_flat_jump(*PC, memory, *NEXT_PC, regs...)
Post-lift:
  Inline all _flat ISEL calls into the lifted function
```

### Characteristics
- Uses pre-transformed `amd64_flat.bc` (1589 `_flat` wrappers from `flat_gen`)
- No State struct, no SROA, no flat_state_load/store
- Register access = pointer arg (zero indirection)
- Output is self-contained (ISEL inlined)
- Run `opt -passes='mem2reg,sroa,instcombine'` for further optimization

### Register Resolution

In pure-SSA mode, `LoadRegAddress` maps register names to pointer args:

```
"PC"       → arg 0
"NEXT_PC"  → arg 2
"MEMORY"   → MEMORY alloca (entry block)
"RAX"      → arg 3  (kFlatFirstRegArgNum + 0)
"EAX"/"AX" → arg 3  (sub-register alias)
"RSP"      → arg 9  (kFlatFirstRegArgNum + 6)
"CF"       → arg 26 (kFlatFirstRegArgNum + 23)
"XMM0"     → arg 41 (kFlatFirstRegArgNum + 38)
```

Sub-register aliases (EAX→RAX, EDX→RDX, etc.) all map to the same pointer arg.
The ISEL accesses the correct width through the pointer.

### Unsupported Registers

Registers not in the 51-slot flat ABI (X87 stack, AVX512 ZMM/KMASK, privileged
CR registers) are not resolvable. In pure-SSA mode, `LoadRegAddress` returns
nullptr for these, producing invalid IR. The `_flat` ISEL wrappers handle X87
internally (via their own State alloca), so X87 instructions work when called
through the wrapper even though the lifter can't resolve X87 register operands.

## Build-Time Transform: `flat_gen`

`tools/flat-gen/flat_gen.cpp` produces `amd64_flat.bc` from `amd64.bc`:

1. For each ISEL function `F(Memory*, State*, operands...)`:
   - Create `F_flat(Memory*, 51 reg ptrs, operands...)`
   - Alloca State, call `__remill_flat_state_load`
   - Call original `F` with the State pointer
   - Call `__remill_flat_state_store` to write back
   - `ret`
2. Inline the original `F` into `F_flat` (via `InlineFunction`)
3. Run SROA over the module (scalarizes the State alloca in most wrappers)

Result: 1589 `_flat` functions, 94% scalarized. The remaining 6% still have the
State alloca but are functionally correct.

## Key Files

| File | Role |
|------|------|
| `include/remill/BC/ABI.h` | Flat ABI constants (`kFlatPCArgNum`, `kFlatNumRegs`, etc.) |
| `lib/Arch/Arch.cpp` | `DeclareLiftedFunction` (names args), `InitializeEmptyLiftedFunction` |
| `lib/Arch/X86/Arch.cpp` | `InitializeFlatLiftedFunction`, `FinishFlatLiftedFunctionImpl`, `kFlatRegs` |
| `lib/BC/InstructionLifter.cpp` | `LoadRegAddress` (flat-mode pointer arg lookup), `LiftIntoBlock` (flat ISEL call) |
| `lib/BC/InstructionLifter.h` | `flat_reg_index` map, `SetFlatMode`, `InitFlatRegMap` |
| `lib/BC/TraceLifter.cpp` | `state_ptr` selection, `InitializeEmptyLiftedFunction` call |
| `lib/BC/Util.cpp` | `FindVarInFunction` (checks args), `LoadStatePointer`, `ScalarizeFlatFunction` |
| `bin/lift/Lift.cpp` | `--flat` / `--flat-ssa` flags, bitcode loading, inlining, SROA |
| `tools/flat-gen/flat_gen.cpp` | Build-time ISEL transform |
| `lib/Arch/X86/Runtime/FlatState.cpp` | `__remill_flat_state_load/store`, `__remill_flat_jump` |

## Performance

Benchmark (42-instruction branchless function, post-optimization):

| Metric | Original | `--flat` (SROA) | `--flat-ssa` (opt) |
|--------|----------|-----------------|---------------------|
| IR instructions | 1,086 | 63 | ~63 (after opt) |
| IR size | 12,597 B | 4,058 B | ~4,000 B (after opt) |
| Native code lines | 404 | 161 | ~161 (after opt) |
| Lift speed | 44.86 ms | 44.99 ms | ~45 ms |

The pure-SSA mode matches the struct+SROA mode in output size after `opt`, but
doesn't require the SROA pass at lift time.

## Future: FlatRegs Calling Convention

The long-term goal is a custom LLVM calling convention (`FlatRegs = 200`) that
maps the 51 pointer args directly to physical registers (in-out), eliminating
the stack frame entirely. See `flat/plan_flatregs_cc.md` for the implementation
plan.

With FlatRegs CC:
- No prologue/epilogue (registers are in-out)
- Tail call between flat functions = plain `jmp`
- RSP is in-out (lifted code owns the stack)
- LLVM can keep values in registers across ISEL calls

This requires patching LLVM's X86 backend (~1–2 weeks).
