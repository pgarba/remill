# Flat Mode Benchmark

## Test Function
42-instruction branchless arithmetic function (add, sub, imul, xor, or, and, lea, mov).
Source: `/tmp/bench_branchless.c` — compiled with `clang -O2`, machine code extracted.

## IR Size (post-SROA, ISEL inlined)

| Metric | Old Mode | Flat Mode | Change |
|--------|----------|-----------|--------|
| IR instructions | 1,086 | 63 | **-94%** |
| IR loads | 79 | 4 | **-95%** |
| IR stores | 43 | 15 | **-65%** |
| IR size (bytes) | 12,597 | 4,058 | **-68%** |
| Allocas | 0 | 0 | — |

## Native Code Size (llc -mtriple=x86_64)

| Metric | Old Mode | Flat Mode | Change |
|--------|----------|-----------|--------|
| Assembly lines | 404 | 161 | **-60%** |
| Assembly size (bytes) | 11,427 | 4,624 | **-60%** |
| Machine instructions | 378 | 135 | **-64%** |

## Lifting Speed

| Metric | Old Mode | Flat Mode | Change |
|--------|----------|-----------|--------|
| Time per lift (42 instrs) | 44.86 ms | 44.99 ms | +0.3% |
| Lifts per second | 22.3 | 22.2 | — |

**Note:** Lifting speed is dominated by bitcode parsing (~44 ms). The actual
IR construction (decode + LiftIntoBlock + flat init/finish) adds <0.1 ms.
Flat mode lifting speed is effectively identical to old mode.

## Why Flat Mode is Smaller

**Old mode** per instruction:
1. GEP into State struct to get register address
2. Load operand(s) from State
3. Call ISEL function (or inlined body)
4. Store result back to State
5. Update PC in State
6. Compute 6 flags (CF, PF, AF, ZF, SF, OF)
7. Store 6 flags to State

**Flat mode** per instruction (after SROA):
1. (Entry) Load 4 input registers from pointer args
2. Pure SSA arithmetic (add, mul, sub, xor, or, and)
3. (Exit) Store changed registers to pointer args
4. (Exit) Compute flags for final result only
5. Tail call `__remill_flat_jump`

The 51-register pointer argument tax is **per-block** (not per-instruction),
so it amortizes to zero for blocks with many instructions.

## Artifacts
- `/tmp/bench_old_final.ll` — old mode IR (self-contained, 1289 lines)
- `/tmp/bench_flat_final.ll` — flat mode IR (self-contained, 266 lines)
- `/tmp/bench_branchless.c` — source C function
- `/tmp/bench_instrs.h` — extracted machine code (42 instructions)
- `/tmp/bench_flat_vs_old` — benchmark binary
