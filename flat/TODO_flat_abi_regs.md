# To-do: flat-ABI register slots not yet supported

Context: the flat lift (`--flat`) represents machine state as by-value function
arguments defined by `kFlatRegs` (`lib/Arch/X86/Arch.cpp`) and looked up by name
in `InitFlatRegMap` (`lib/BC/InstructionLifter.h`). Any register the arch decoder
can produce that has **no `kFlatRegs` slot** makes the flat lifter `LOG(FATAL)`
with `Register '<name>' is not in the flat ABI`.

The general-purpose register sub-registers (including the 8-bit `SIL/DIL/SPL/BPL`
and the R8–R15 `D/W/B` forms) were fixed — see commit
`3bb1e33` "Fix flat-lift GPR sub-register slot mapping". The remaining
unsupported register classes are below. Each needs the same three-part change:

1. Add the register(s) to `kFlatRegs[]` (+ `kFlatNumRegs`) in `Arch.cpp`.
2. Add a matching by-value argument to `FlatLiftedFunctionType` / the
   `__remill_flat_jump` dispatch signature.
3. Add the name(s) to `InitFlatRegMap` (`InstructionLifter.h`), mapping to the
   new slot index. Then re-verify with a targeted lift (see the fib/regtest
   pattern) and the `tests/Unflatten` smoke test.

## [ ] Segment-value registers: `SS DS ES CS GS FS` (6, `u16`)
- Arch defines them (`Arch.cpp:1516-1521`) but `kFlatRegs` only has the *base*
  addresses `ss_base/gs_base/cs_base/fs_base` (slots 17–20).
- Needed for code that *reads/writes the segment selector* (e.g. `mov %ax,%ss`,
  far-call `cs` save/restore). Segment-**relative** addressing already works via
  `FSBASE/GSBASE`.
- Low frequency in user code; ring/CR3/IRET paths are the main consumers.

## [ ] AVX-512 vector registers: `ZMM0..ZMM31` (32, `v512`)
- Arch defines them under `has_avx512` (`Arch.cpp:1537+`).
- `kFlatRegs` only carries 128-bit `XMM0..XMM15` (slots 36–51).
- Requires deciding the `v512` by-value encoding (e.g. `<8 x i64>`) and runtime
  storage. Larger ABI change (32 new args).

## [ ] AVX-512 opmask registers: `K0..K7` (8, `u64`)
- Arch defines them (`Arch.cpp:1693+`).
- Only needed together with `ZMM` (k-masked AVX-512 ops). Track with the ZMM item.

## Notes / non-issues
- `YMM` (AVX 256-bit): no separate `REG(YMM*)` in the arch table — 256-bit is
  handled through the `vec[]` structure; revisit only if a 256-bit-only workload
  FATALs.
- `BND0..BND3` (MPX bounds): not defined in the arch table — no action.
- Segment bases in 64-bit mode: arch defines only `GSBASE/FSBASE` (not
  `SSBASE/CSBASE/ESBASE`), though the flat map lists all four — harmless unused
  slots; no action.

## Fixed: memory-source register-write dst clobber (flat-gen)
Symptom: a scalar mul with a **memory source** (`imul -0x10(%rbp),%rax`) was
silently dropped in flat mode — the lifted IR had no `mul` at all, so `-O0`
multiply code miscomputed. Reg-reg (`imul %a,%b`) and vector (`pmuludq`) muls
were fine.

Root cause (`tools/flat-gen/flat_gen.cpp`): each opcode is wrapped as
`state_load(reg_ptrs -> local State)` → *inlined ISEL body* →
`state_store(local State -> reg_ptrs)`. The ISEL body writes a register-write
dst operand **directly to the register pointer** (bypassing the local State),
then `state_store` restores the State's (initial) copy of that same register
back to the pointer — clobbering the dst write. DCE then removed the now-dead
`mul` (its RAX result no longer reached the next instruction). Reg-reg muls
with constant operands happened to fold, masking the bug.

Fix: after emitting the `state_store` call, move any `store` to a
pointer-typed operand (a register-write dst) to **after** the `state_store`,
so the dst write is the final write to the register. Regenerated
`amd64_flat.bc` via the `flat-bitcode` target. Verified: mem-source mul
survives and threads to the consumer; reg-reg / folded cases and the
`tests/Unflatten` smoke test still pass.

## Acceptance (per item)
- A freestanding `-O2` test binary exercising the target registers lifts
  cleanly (no `FATAL`) and routes the ops to the correct `REG_*` slot.
- `python3 tests/Unflatten/run_smoke.py build/bin/lift/remill-lift-21
  tests/Unflatten` still passes (no regression).
