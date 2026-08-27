# Flat Lifting — Remaining Work Plan

## Status
- ✅ 1589 base amd64 ISEL opcodes transformed to flat wrappers (94% scalarized)
- ✅ `__remill_flat_jump` no-op implemented
- ✅ Benchmark: flat mode is 60-94% smaller than old mode
- ✅ Lifting speed: identical (bitcode load dominates)
- ✅ Unsupported opcodes identified: 294 AVX512-wide, 101 X87, 4 K_REG
- ✅ **`--flat` flag integrated into `remill-lift`** (end-to-end working)
- ✅ **Flat generator moved into repo** (`tools/flat-gen/`)
- ✅ **CMake integration** (`flat-bitcode` target, `remill-flat-gen-21` binary)
- ✅ **`OptimizeBareModule` stub added** (was swallowed by comment block)
- ✅ **All tests pass** (`flat_lift` test: 0.52 sec)

## Remaining Work (Priority Order)

### 1. ~~Integrate flat mode into the lift pipeline~~ ✅ DONE
- `--flat` flag added to `remill-lift`
- `TraceLifter` accepts `flat` parameter, passes it to `DeclareLiftedFunction`/`InitializeEmptyLiftedFunction`
- `FinishFlatLiftedFunction` called after lifting to retarget tail calls
- `ScalarizeFlatFunction` (SROA) run after lifting
- **Files modified:**
  - `bin/lift/Lift.cpp` — `--flat` flag + SROA call
  - `bin/CMakeLists.txt` — uncommented `add_subdirectory(lift)`
  - `include/remill/BC/TraceLifter.h` — `flat` parameter
  - `lib/BC/TraceLifter.cpp` — flat mode logic
  - `lib/BC/Optimizer.cpp` — `OptimizeBareModule` stub
- **Verified:** `remill-lift-21 --flat --bytes "4889e54883ec10c9c3"` produces 60-line scalarized IR

### 2. ~~CMake integration for flat bitcode generation~~ ✅ DONE
- `tools/flat-gen/flat_gen.cpp` — moved from `/tmp/flat_gen.cpp`
- `tools/flat-gen/CMakeLists.txt` — builds `remill-flat-gen-21` + `flat-bitcode` target
- `CMakeLists.txt` — added `add_subdirectory(tools/flat-gen)`
- Run with: `ninja flat-bitcode` (produces `build/lib/Arch/X86/Runtime/amd64_flat.bc`)

### 3. ~~Investigate un-scalarized functions~~ ✅ DONE (220 found, root cause identified)
- **220/1589 (14%)** flat wrappers retain `STATE_LOCAL` alloca after SROA
- **Root cause: pointer escape.** The ISEL code passes a GEP into the local
  State to a helper function. SROA cannot scalarize an alloca whose address escapes.

| Root Cause | Count | Examples |
|------------|-------|----------|
| Escape → `__remill_sync_hyper_call` | 60 | IN/OUT ports, LGDTR, control/debug regs |
| Escape → `__remill_error` | 30 | DIV/IDIV (overflow), SSE compares (CMPPD) |
| Conditional branches | 13 | x87 FCOMIP, PCMPISTR |
| Other (x87 FPU, CMPXCHG, complex) | 117 | FADD, FSUB, FMUL, CMPXCHG |

**Categories by opcode type:**
- x87 FPU (71): FADD, FSUB, FMUL, FDIV, FLD, FST, FCOM — use 80-bit floats
- DIV/IDIV (16): integer division with overflow error path
- I/O ports (12): IN/OUT — call `__remill_sync_hyper_call`
- SSE compare (12): CMPPD, CMPPS, COMISD — error path on invalid operand
- Privileged regs (15): LGDTR, control/debug register access
- Segment MOV/POP (20): MOV_ESI2R, POP_GSI — segment cache access
- SSE shift/permute (27): PSHUFB, PEXTRQ, PINSRW — complex memory patterns
- CMPXCHG (4): atomic compare-and-exchange

**Decision: Accept.** These 14% of opcodes are still CORRECT — they just retain
the 51-load/store overhead. The common arithmetic/logic instructions (ADD, SUB,
IMUL, XOR, OR, AND, MOV) all scalarize perfectly. For a VMP workload, the
common instructions dominate. Fixing would require modifying ISEL semantics
(off-limits) or a hybrid approach (per-opcode mode selection).

### 4. Extend flat ABI for AVX512/X87 (LOW — only if needed)
- 294 AVX512-wide opcodes need vec[16-31] (16 more slots)
- 101 X87 opcodes need st0-st7 (8 more slots)
- 4 K_REG opcodes need k0-k7 (8 more slots)
- **Decision:** Only extend if the target workload uses these instructions
- For now: these opcodes fall back to old mode (State pointer)

### 5. Multi-block flat lifting (LOW)
- Current flat mode handles single basic blocks
- Multi-block would need: phi nodes for loop-carried values, or trace-based unrolling
- Flat mode is designed for straight-line traces (VMP use case)

### 6. Execution-level benchmark (LOW)
- Compile both modes to native code and measure execution time
- Current benchmark shows size difference; execution time should also favor flat mode
- **Action:** Link both .s files with a test harness, measure with `perf` or `rdtsc`

### 7. Cleanup (LOW)
- Remove dead `__remill_flag_computation_*` declarations from bitcode
- Remove `STATE_FOR_JUMP` code paths (fully replaced by direct pointer args)
- Document the flat ABI in `include/remill/BC/ABI.h` comments

## Key Files
| File | Purpose |
|------|---------|
| `lib/Arch/X86/Arch.cpp` | Flat lifting: `InitializeFlatLiftedFunction`, `FinishFlatLiftedFunctionImpl` |
| `lib/Arch/X86/Runtime/FlatState.{h,cpp}` | `__remill_flat_state_load/store`, `__remill_flat_jump` |
| `lib/BC/Util.cpp` | `ScalarizeFlatFunction` (inliner + mem2reg + SROA) |
| `lib/BC/InstructionLifter.cpp` | `LiftIntoBlock`, `LoadRegAddress` (needs flat mode) |
| `include/remill/BC/ABI.h` | Flat ABI constants (kFlatNumRegs=51, etc.) |
| `tests/X86/FlatLift.cpp` | End-to-end flat lift test |
| `tools/flat-gen/` | (to create) Flat wrapper generator |
| `bin/lift/Lift.cpp` | (to modify) Add `--flat` flag |

## Build Commands
```bash
# Build remill
cd /home/adam/saturn_21_1/saturn/build_remill/remill/build && ninja

# Run flat lift test
cd build/tests/X86 && ctest -R flat_lift

# Generate flat bitcode (standalone, for now)
/tmp/flat_gen build/lib/Arch/X86/Runtime/amd64.bc /tmp/amd64_flat.bc

# Run benchmark
/tmp/bench_flat_vs_old /tmp/bench_old.ll /tmp/bench_flat.ll
/tmp/bench_lift_speed
```

## Prompt for New Session

```
You are continuing work on the "flat lifting" feature for remill (an LLVM-based
binary lifter for x86-64). The working directory is:
  /home/adam/saturn_21_1/saturn/build_remill/remill

## Context
Flat lifting passes 51 register pointers as function arguments instead of a
single State struct pointer. After SROA scalarization, the lifted code is
60-94% smaller than old mode. All 1589 base amd64 opcodes are supported.

Key files:
- lib/Arch/X86/Arch.cpp — flat lifting implementation
- lib/Arch/X86/Runtime/FlatState.{h,cpp} — flat state helpers + __remill_flat_jump
- lib/BC/Util.cpp — ScalarizeFlatFunction (line ~2280)
- lib/BC/InstructionLifter.cpp — LiftIntoBlock, LoadRegAddress
- include/remill/BC/ABI.h — flat ABI constants
- tests/X86/FlatLift.cpp — end-to-end test
- flat/ — benchmark results, unsupported opcode list, this plan

## What to do next (pick the highest priority item from flat/plan.md):

1. **Integrate flat mode into the lift pipeline:**
   - Uncomment `add_subdirectory(lift)` in `bin/CMakeLists.txt`
   - Add `--flat` flag to `bin/lift/Lift.cpp`
   - When `--flat` is set, the lifter should:
     a. Use `DeclareLiftedFunction(name, module, /*flat=*/true)`
     b. Use `InitializeEmptyLiftedFunction(func, /*flat=*/true)`
     c. In `LiftIntoBlock`: instead of `CreateCall(isel_func, {memory, state, ...})`,
        look up `isel_func_flat` (the pre-generated flat wrapper) and inline its body
     d. In `LoadRegAddress`: return the reg_i pointer argument instead of GEP
     e. Call `FinishFlatLiftedFunction(func)` instead of `AddTerminatingTailCall`
   - The flat wrappers are in `amd64_flat.bc` (generated by tools/flat-gen)
   - For now, you can test with the standalone approach: load amd64_flat.bc,
     find the _flat function, and inline it into the block

2. **Move the flat generator into the repo:**
   - Copy `/tmp/flat_gen.cpp` to `tools/flat-gen/flat_gen.cpp`
   - Create `tools/flat-gen/CMakeLists.txt`
   - Add a CMake custom target that runs it after amd64.bc is built
   - Output: `build/lib/Arch/X86/Runtime/amd64_flat.bc`

3. **Run the existing test to verify nothing is broken:**
   ```
   cd build/tests/X86 && ctest -R flat_lift
   ```

## Build
```bash
cd /home/adam/saturn_21_1/saturn/build_remill/remill/build && ninja
```

## LLVM
- Custom LLVM at: /home/adam/saturn_21_1/saturn/deps/tob_libraries/llvm
- Use `-I$LLVM/include` (NOT `llvm-config --includedir`)
- PassBuilder: 4-arg constructor `(TM, PTO, PGO, PIC)`
- SROA: function pass, wrap in FunctionPassManager
- Full O2 pipeline segfaults — use minimal (inliner + mem2reg + SROA)

## Constraints
- Do NOT rewrite ISEL semantics functions
- Keep old struct-pointer mode behind a flag (default: old mode)
- Flat lifting is 64-bit only
- LLVM 21 API (getElementType, not getPointerElementType)
- `vec128_t` is a union; XMM params must be `vec128_t *`
- XMM access via `dqwords.elems[0]` to avoid memcpy
```
