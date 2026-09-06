# Flat Lifting — ABI v2 (By-Value Registers, 60-Register)

## Overview

Flat lifting transforms x86-64 machine code into LLVM IR where architectural
registers flow as **SSA values** instead of pointer-dereferenced struct fields.
This enables LLVM's optimizer to constant-fold, eliminate dead code, and
prove properties about the lifted code.

## Lifting Modes

| Flag | Mode | Register Args | State |
|------|------|---------------|-------|
| *(none)* | Original | `State *` (one pointer) | Struct in memory |
| `--flat` | Flat (by-value SSA) | 60 × by-value (`i64`/`i8`/`<2 x i64>`/`i128`) | REG_* allocas → SSA |
| `--flat-ssa` | Alias for `--flat` | (same) | (same) |

The old struct-pointer flat mode has been removed. `--flat` is now the
by-value SSA mode.

## ABI: 60 By-Value Register Args

The `--flat` mode passes registers **by value** as integers:

```llvm
define ptr @sub_0(
  ptr %PC, ptr %memory, ptr %NEXT_PC,              ; threaded between blocks
  i64 %RAX, i64 %RBX, ..., i64 %RIP,               ; 17 GPRs
  i64 %SS_BASE, i64 %GS_BASE, i64 %CS_BASE, i64 %FS_BASE,  ; 4 seg bases
  i8 %CF, i8 %PF, i8 %AF, i8 %ZF, i8 %SF, i8 %DF, i8 %OF,  ; 7 flags
  i64 %MM0, ..., i64 %MM7,                          ; 8 MMX
  <2 x i64> %XMM0, ..., <2 x i64> %XMM15,          ; 16 XMM
  i128 %ST0, ..., i128 %ST7)                       ; 8 X87 ST
```

### How It Works

1. **Entry**: By-value args are stored into local `REG_*` allocas
2. **ISEL calls**: The `_flat` ISEL wrapper receives the 60 `REG_*` allocas
   as pointers (it uses a local State struct internally)
3. **ISEL body**: Inlined wrapper materializes a local `State` via
   `state_load`, runs the original ISEL, writes results back via `state_store`
4. **Exit**: Updated values are loaded from `REG_*` allocas and passed
   by-value to `__remill_flat_jump`
5. **Optimization**: The lift tool runs 3× (mem2reg + SROA + instcombine +
   simplifycfg + DCE). External O3 further eliminates remaining overhead.

### Register Type Mapping

| Index | Register | LLVM Type | Count |
|-------|----------|-----------|-------|
| 0-16 | RAX-R15, RIP | `i64` | 17 |
| 17-20 | SS/GS/CS/FS_BASE | `i64` | 4 |
| 21-27 | CF, PF, AF, ZF, SF, DF, OF | `i8` | 7 |
| 28-35 | MM0-MM7 | `i64` | 8 |
| 36-51 | XMM0-XMM15 | `<2 x i64>` | 16 |
| 52-59 | ST0-ST7 | `i128` | 8 |
| **Total** | | | **60** |

## Build Pipeline

### ISEL Bitcode Generation (`flat_gen`)

```
amd64.bc (original ISEL)
  │
  ├─ 1. Generate _flat wrappers (one per ISEL opcode)
  │     - Wrapper takes: (ptr mem, ptr reg_0..reg_59, operands...)
  │     - Body: alloca State, state_load, call ISEL, state_store, ret mem
  │
  ├─ 2. AlwaysInlinerPass
  │     - Inlines __remill_flat_state_load/store (alwaysinline, 61 args)
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

### Lifting Pipeline (`remill-lift --flat`)

```
Machine code bytes
  │
  ├─ 1. Disassemble + decode
  ├─ 2. Lift each instruction (inline _flat ISEL wrapper)
  ├─ 3. Thread PC/NEXT_PC between blocks
  ├─ 4. OptimizeFlatSSAFunction: 3× (mem2reg + SROA + instcombine
  │    + simplifycfg + DCE). Branch functions skip SROA (LLVM bug).
  ├─ 5. FixZextPtrToPtrToInt (post-processing)
  ├─ 6. Strip unused declarations
  └─ 7. Output IR
  │
  ▼
lifted_code.ll (by-value register args, optimized)
```

## Benchmarks

### xstrtoumax (334 instructions, 1328 bytes)

| Metric | original | `--flat` pre-O3 | `--flat` O3 | **O3 flat/orig** |
|--------|----------|-----------------|-------------|-----------------|
| IR lines | 2,138 | 5,492 | **366** | **-58%** |
| allocas | 9 | 0 | 0 | -100% |
| Native instr | 1,248 | — | **858** | **-31%** |

### quotearg_buffer_restyled (1995 instructions, 4352 bytes)

| Metric | original | `--flat` pre-O3 | `--flat` O3 | **O3 flat/orig** |
|--------|----------|-----------------|-------------|-----------------|
| IR lines | 730 | 1,824 | **136** | **-47%** |
| Native instr | 334 | — | **444** | +33% |

### ISEL Optimization Impact (xstrtoumax)

| Step | IR Lines | Reduction |
|------|----------|-----------|
| No ISEL opt (old) | 166,132 | — |
| + instcombine/simplifycfg/SROA in flat_gen | 5,492 | -96.7% |
| + clang -O3 on output | 366 | -99.8% |

### human_readable (1281 instructions, 5072 bytes)

| Mode | Result |
|------|--------|
| original | 14,973 lines |
| `--flat` | FATAL: X87 internal register `SIL` not in flat ABI |

## Key Files

| File | Purpose |
|------|---------|
| `include/remill/BC/ABI.h` | Flat ABI constants (60 regs) + type helpers |
| `lib/Arch/X86/Arch.cpp` | Function type, init/finish (REG_* allocas, jump) |
| `lib/BC/InstructionLifter.cpp` | LoadRegAddress → REG_* alloca lookup, ISEL call |
| `lib/BC/Util.cpp` | FixZextPtrToPtrToInt, OptimizeFlatSSAFunction |
| `tools/flat-gen/flat_gen.cpp` | ISEL wrapper generator (60-arg) + optimization pipeline |
| `include/remill/Arch/X86/Runtime/FlatState.h` | Jump declaration (60-arg) |
| `lib/Arch/X86/Runtime/FlatState.cpp` | state_load/store (60-arg) + jump implementation |
| `bin/lift/Lift.cpp` | --flat flag, output post-processing |
| `tests/X86/FlatLift.cpp` | CTest: 38 checks (by-value ABI, REG_* allocas) |

## Known Limitations

1. **LLVM 21 assertion bug**: SROA crashes on branch-containing functions
   (`CmpInst::getFlippedStrictnessPredicate`). Workaround: skip SROA for
   branch functions (still run instcombine/simplifycfg/DCE).
2. **X87 ST(0-7)**: In the ABI (i128 slots), state_load/store wired, but
   ISEL execution FATALs on internal FPU registers (`SIL`, `SWD`, `CWD`).
   Full X87 support needs these mapped into the flat ABI.
3. **Native code size**: For call-heavy functions (quotearg), the 60-arg
   ABI overhead makes flat native code 33% *larger* than original. For
   compute-heavy functions (xstrtoumax), flat is 31% smaller.
4. **REG_* allocas escape in lift tool**: The 60 allocas are passed as
   pointers to ISEL wrappers, preventing mem2reg in the lift tool.
   External O3 (which inlines the wrappers) eliminates them completely.

## Future Work

### High Priority
- [ ] **Stack slot elimination**: Fold `[RSP + const]` memory accesses into
      local variables using symbolic RSP tracking (K = RSP delta).
      Eliminates ~80% of `__remill_read/write_memory` calls.
- [ ] Fix LLVM 21 assertion bug (upstream patch or workaround) so SROA
      runs on branch functions

### Medium Priority
- [ ] Full X87 support: map internal FPU registers (SIL, SWD, CWD, FTW)
      into the flat ABI
- [ ] Saturn runtime integration (call flat-lifted functions directly)

### Low Priority
- [ ] FlatRegs custom calling convention (patch LLVM, eliminate 60-arg
      overhead) — see `plan_flatregs_cc.md`
- [ ] Upstream LLVM patch for `CmpInst::getFlippedStrictnessPredicate`

### Completed
- [x] Flat ABI v2: by-value register args (was: pointer args)
- [x] 60-register ABI (was: 52, added X87 ST0-ST7 as i128)
- [x] Removed old struct-pointer flat mode
- [x] Built-in optimization in lift tool (3× pass pipeline)
- [x] ISEL optimization in flat_gen (instcombine + simplifycfg + SROA)
- [x] `__remill_compare_*` inlining (identity/NOT elimination)
- [x] `FixZextPtrToPtrToInt` post-processing pass
- [x] Unused declaration stripping
- [x] CTest: 38 checks (by-value ABI, REG_* allocas, branch correctness)
- [x] End-to-end verification on real-world binaries
- [x] Blog post published

## Design Documents

| Document | Description |
|----------|-------------|
| `design.md` | Original flat lifting design (pointer args) |
| `plan.md` | Implementation plan and findings |
| `plan_flat_abi_v2.md` | By-value ABI design and migration plan |
| `plan_pure_ssa_lifter.md` | Pure SSA lifter design |
| `plan_flatregs_cc.md` | Custom calling convention proposal |
| `benchmark.md` | Pre-ABI-v2 benchmarks |
| `benchmark_execution.md` | Execution-level benchmarks |
| `unsupported_opcodes.md` | AVX512-wide, X87, K_REG opcodes |

## Stack Slot Elimination (Planned)

The next major optimization: fold RSP-relative memory accesses into local
variables.

**Approach**: Track RSP symbolically as `RSP_init + K` where K is a
compile-time constant delta. Map `[RSP + offset]` to a stack slot =
`K + offset`, backed by a local alloca.

```
push rax           → K -= 8,  slot[K] = RAX        (no memory call)
pop rbx            → rbx = slot[K], K += 8          (no memory call)
mov [rsp-16], rcx  → slot[K-16] = RCX              (no memory call)
add rsp, 32        → K += 32
call foo           → K -= 8 (return addr push)
```

**Expected impact**: Eliminates ~80% of `__remill_read/write_memory` calls
in typical functions. The remaining calls are for non-stack memory (heap,
globals, pointer-based accesses).

**Constraints**:
- Only `[RSP + const]` (no index registers)
- K tracked per-block (RSP is in the flat ABI)
- Functions with pointer args that might alias the stack need fallback

## Branch

`flat_lifting_v2` on `https://github.com/pgarba/remill`

### Commit History (latest)

| Commit | Description |
|--------|-------------|
| `c43ffa0` | Fix segfault: kRegNames array 52→60 in LoadRegAddress |
| `2b9e718` | Wire X87 ST(0-7) through ISEL wrapper, state_load/store, jump |
| `582279c` | Add X87 ST(0-7) to flat ABI (60 registers) |
| `a85edd2` | Optimize flat output in lift tool + extend CTest |
| `e715367` | Remove old flat mode: --flat is now by-value SSA only |
| `fe1d437` | Add flat/README.md documentation |
| `9db4561` | Fix FlatRegIsFlag/FlatRegIsXMM index ranges |
| `0ca83d6` | Restore --flat mode with REG_* allocas |
| `0c249c3` | __remill_flat_jump takes by-value args |
| `dd5961f` | Flat ABI v2: by-value register args |
| `3ca52f3` | Fix --flat-ssa ptr-arithmetic bug (AlwaysInlinerPass) |
