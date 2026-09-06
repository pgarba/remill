# Flat Lifting — ABI v2 (By-Value Registers)

## Overview

Flat lifting transforms x86-64 machine code into LLVM IR where architectural
registers flow as **SSA values** instead of pointer-dereferenced struct fields.
This enables LLVM's optimizer to constant-fold, eliminate dead code, and
prove properties about the lifted code.

## Three Lifting Modes

| Flag | Mode | Register Args | State |
|------|------|---------------|-------|
| *(none)* | Original | `State *` (one pointer) | Struct in memory |
| `--flat` | Flat (struct) | 52 × `ptr` (in-out pointers) | STATE_LOCAL + SROA |
| `--flat-ssa` | Flat SSA (by-value) | 52 × by-value (`i64`/`i8`/`<2 x i64>`) | REG_* allocas → SSA |

## ABI v2: By-Value Register Args

The `--flat-ssa` mode passes registers **by value** as integers:

```llvm
define ptr @sub_0(
  ptr %PC, ptr %memory, ptr %NEXT_PC,          ; threaded between blocks
  i64 %RAX, i64 %RBX, ..., i64 %RIP,           ; 17 GPRs
  i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE,  ; 4 seg bases
  i8 %CF, i8 %PF, i8 %AF, i8 %ZF, i8 %SF, i8 %DF, i8 %OF,  ; 7 flags
  i64 %MM0, ..., i64 %MM7,                      ; 8 MMX
  <2 x i64> %XMM0, ..., <2 x i64> %XMM15)      ; 16 XMM
```

### How It Works

1. **Entry**: By-value args are stored into local `REG_*` allocas
2. **ISEL calls**: The `_flat` ISEL wrapper receives the `REG_*` allocas as
   pointers (it still uses the State struct internally)
3. **ISEL body**: Inlined wrapper materializes a local `State`, runs the
   original ISEL, writes results back to the `REG_*` allocas
4. **Exit**: Updated values are loaded from `REG_*` allocas and passed
   by-value to `__remill_flat_jump`
5. **Optimization**: SROA/mem2reg promotes `REG_*` allocas to SSA values;
   O3 eliminates remaining loads/stores

### Register Type Mapping

| Index | Register | LLVM Type |
|-------|----------|-----------|
| 0-16 | RAX-R15, RIP | `i64` |
| 17-20 | SS/GS/CS/FS_BASE | `i64` |
| 21-27 | CF, PF, AF, ZF, SF, DF, OF | `i8` |
| 28-35 | MM0-MM7 | `i64` |
| 36-51 | XMM0-XMM15 | `<2 x i64>` |

## Build Pipeline

### ISEL Bitcode Generation (`flat_gen`)

```
amd64.bc (original ISEL)
  │
  ├─ 1. Generate _flat wrappers (one per ISEL opcode)
  │     - Wrapper takes: (ptr mem, ptr reg_0..reg_51, operands...)
  │     - Body: alloca State, state_load, call ISEL, state_store, ret mem
  │
  ├─ 2. AlwaysInlinerPass
  │     - Inlines __remill_flat_state_load/store (alwaysinline, 53 args)
  │     - SROA can now see through them
  │
  ├─ 3. ScalarizeFlatFunction (inliner + mem2reg + SROA)
  │     - Scalarizes the local State in each wrapper
  │     - Eliminates most loads/stores
  │
  └─ 4. Optimization passes (instcombine + simplifycfg + mem2reg + SROA)
        - Further reduces wrapper size
        - Eliminates dead code, combines instructions
  │
  ▼
amd64_flat.bc (1589 optimized _flat wrappers)
```

### Lifting Pipeline (`remill-lift --flat-ssa`)

```
Machine code bytes
  │
  ├─ 1. Disassemble + decode
  ├─ 2. Lift each instruction (inline _flat ISEL wrapper)
  ├─ 3. Thread PC/NEXT_PC between blocks
  ├─ 4. FixZextPtrToPtrToInt (post-processing)
  ├─ 5. Strip unused declarations
  └─ 6. Output IR
  │
  ▼
lifted_code.ll (by-value register args, inlined ISEL bodies)
```

## Benchmark: xstrtoumax (334 instructions, 1328 bytes)

### Pre-Optimization (as lifted)

| Metric | original | `--flat-ssa` |
|--------|----------|--------------|
| IR lines | 2,138 | 22,230 |
| allocas | 9 | 62 |
| loads | 545 | 9,437 |
| stores | 462 | 8,343 |

### After clang -O3

| Metric | original | `--flat-ssa` | **ssa/orig** |
|--------|----------|--------------|--------------|
| IR lines | 878 | **367** | **-58%** |
| allocas | 2 | **0** | -100% |
| loads | 171 | **8** | -95% |
| stores | 75 | **22** | -71% |
| calls | 153 | **46** | -70% |

### ISEL Optimization Impact

| Step | Lines | Reduction |
|------|-------|-----------|
| No ISEL opt (old) | 166,132 | — |
| + instcombine/simplifycfg/SROA | 22,230 | -86.6% |
| + clang -O3 on output | 367 | -98.3% |

## Key Files

| File | Purpose |
|------|---------|
| `include/remill/BC/ABI.h` | Flat ABI constants + type helpers |
| `lib/Arch/X86/Arch.cpp` | Function type, init/finish (REG_* allocas) |
| `lib/BC/InstructionLifter.cpp` | LoadRegAddress → REG_* alloca lookup |
| `lib/BC/Util.cpp` | FixZextPtrToPtrToInt, ScalarizeFlatFunction |
| `tools/flat-gen/flat_gen.cpp` | ISEL wrapper generator + optimization pipeline |
| `include/remill/Arch/X86/Runtime/FlatState.h` | X86FlatState + jump declaration |
| `lib/Arch/X86/Runtime/FlatState.cpp` | state_load/store + jump implementation |
| `bin/lift/Lift.cpp` | --flat / --flat-ssa flags, output post-processing |

## Known Limitations

1. **LLVM 21 assertion bug**: SROA/inliner crashes on branch-containing
   functions (`CmpInst::getFlippedStrictnessPredicate`). Workaround: skip
   optimization for branch functions.
2. **X87 registers**: Functions using ST(0-7) will FATAL in flat mode
   (not in the 52-register ABI).
3. **`--flat` mode broken**: The by-value ABI change broke the old `--flat`
   mode's internal load logic (`load i64, i64 %RSP`). Needs a fix.
4. **REG_* allocas escape**: The 52 allocas are passed as pointers to the
   ISEL wrappers, preventing mem2reg from promoting them in the lift tool.
   O3 (which inlines the wrappers) eliminates them completely.

## Future Work

- [ ] Fix `--flat` mode for by-value ABI
- [ ] Run full O2 pipeline in the lift tool (avoid requiring clang -O3)
- [ ] Fix LLVM 21 assertion bug (upstream patch or workaround)
- [ ] Extend ABI for X87 ST(0-7) if needed
- [ ] FlatRegs custom calling convention (eliminate 52-arg overhead)
- [ ] Saturn runtime integration (call flat-lifted functions directly)

## Branch

`flat_lifting_v2` on `https://github.com/pgarba/remill`

Key commits:
- `3ca52f3`: Fix --flat-ssa ptr-arithmetic bug (AlwaysInlinerPass)
- `dd5961f`: Flat ABI v2: by-value register args
- `0c249c3`: __remill_flat_jump takes by-value args
- `0ca83d6`: Restore --flat mode with REG_* allocas
- `9db4561`: Fix FlatRegIsFlag/FlatRegIsXMM index ranges
