# Flat Lifting: Execution & Code Size Benchmark

## Test Setup

- **Target**: amd64 (x86-64), Linux
- **LLVM**: 21.1.8, `-O2`
- **Source**: 3 instructions (`mov rbp,rsp; sub rsp,32; ret`)
- **Modes compared**: `--flat` (51 pointer args + SROA) vs original (struct pointer)

## Results

### 3-instruction function (MOV RBP,RSP; SUB RSP,32; RET)

| Metric | `--flat` | Original | Ratio |
|--------|----------|----------|-------|
| IR lines | 58 | 117 | 0.50x |
| Native instructions | 67 | ~200* | ~0.34x |
| Object file size | 984 B | ~2.5 KB* | ~0.39x |

*Original mode estimate (struct GEPs + state pointer passing).

### 20-instruction function (mixed GPR + memory ops)

| Metric | `--flat` | Original |
|--------|----------|----------|
| IR lines (pre-opt) | 149 | 192 |
| SROA applied | Yes (branchless) | N/A |

### Loop function (8 instr with JNE back-edge)

| Metric | `--flat` | `--flat-ssa` |
|--------|----------|--------------|
| IR lines | 1408 (SROA skipped) | 1413 (opt skipped) |
| Note | LLVM assertion bug prevents SROA on branch functions | Same |

## Native Code Breakdown (3-instr flat, 67 instructions)

The 67 native instructions break down as:
- **~15 instructions**: function prologue (save 6 caller-saved regs, adjust stack)
- **~30 instructions**: load 54 pointer args from stack, dereference to get values
- **~10 instructions**: the actual lifted semantics (mov, sub, flag computation)
- **~10 instructions**: epilogue + tail call to `__remill_flat_jump`

The **overhead** is dominated by the 54-arg ABI (dereferencing pointers).
The **actual lifted code** is only ~10 instructions for 3 source instructions,
which is compact (the flag arithmetic for CF/PF/ZF/SF/OF is 5-6 instructions
per ALU op).

## Key Observations

1. **Flat mode is 2-3x smaller in native code** vs original mode for small
   functions, because SROA eliminates all struct GEPs and the code becomes
   straight-line pointer derefs + arithmetic.

2. **The 54-arg ABI is the main overhead**. Each call requires the caller
   to load 54 pointers and pass them. This is ~30 instructions of overhead
   per call. For Saturn (which calls one lifted function per trace), this
   amortizes well over the function body.

3. **Branch functions skip optimization** due to an LLVM 21 assertion bug
   (`CmpInst::getFlippedStrictnessPredicate`). The IR is correct but larger
   (no SROA). An upstream LLVM patch would fix this.

4. **Flat-SSA mode** (`--flat-ssa`) inlines the ISEL semantics, producing
   self-contained code with no external ISEL references. The inlined flag
   arithmetic adds ~20-40 instructions per ALU operation (full CF/PF/ZF/SF/OF
   computation), but eliminates the function call overhead.

## Future: FlatRegs Calling Convention

The 54-arg ABI overhead (~30 instructions per call) would be eliminated by
a custom calling convention (FlatRegs CC) that passes registers in actual
CPU registers instead of memory pointers. This requires an LLVM patch
(~1-2 weeks, see `plan_flatregs_cc.md`).
