# Plan: Unflatten pass — from 398 flat block functions to one function with a real CFG

## Status: M0–M5 complete ✅

Implemented in-tree (`lib/BC/Unflatten.cpp` + `include/remill/BC/Unflatten.h`,
`--unflatten` / `--unflatten_opt` in `bin/lift/Lift.cpp`, `tools/flat_edges.py`).
Full quotearg result (see M3): **one function `@quotearg_unflat`, 880 BBs,
639 static `br` edges, 23,880 phi nodes, 261 dynamic
`@__remill_dynamic_dispatch` exits, 0 residual `flat_pc_*` / `@sub_*` block
calls, 0 `unreachable`; llvm-as-clean.** A 3-scenario self-contained smoke test
(`tests/Unflatten/`: jmp, jcc, jcc+optimize) is registered as the
`unflatten_smoke` ctest and passes.

**Dynamic exits use a switch-based re-entry join (D5), not `unreachable`.**
Every dynamic / off-manifest exit calls `@__remill_dynamic_dispatch` and
branches to a shared `dyn_join` block, which threads the dispatch's updated
state through 60 phis and `switch`es on the dynamic target PC to *every*
a lifted block (a conservative, sound model of dynamic re-entry: a call/indjmp
could resume at any block), defaulting to `ret ptr %memory`. Because every
block is reachable from the join, **`opt -O3` preserves the real CFG**: 880 BBs
→ 525 BBs / 371 bodies / 214 loop back-edges / the 396-case switch intact,
verifier-clean (O3 only folds the dead state-phi arithmetic, 27,781 → 3,367
arith/mem, and merges 28 trivially-mergeable blocks). Without the join the
same `-O3` collapses the function to a 3-BB `ret` (the off-manifest blocks are
dead from the entry under the no-op stub). The join's re-entry is a *model* —
the real Saturn runtime branches to the actual resolved continuation instead.

## Goal

Transform the merged flat-ABI module (one function per machine basic block,
63-arg by-value register file, control flow externalized through
`NEXT_PC` + `__remill_flat_jump` dispatch) into a **single LLVM function**
with a genuine control-flow graph:

- one basic block per machine block (~398 for quotearg)
- real `br` / conditional `br` edges for the 369 verified static exits
- real loops (backward `br` edges, e.g. `ja 0x3a2`, the `0x1d0` hub)
- 63-wide phi merges at every join point
- dynamic exits (indirect calls, PLT, devirtualized targets, unresolved
  branches) as mid-function calls + state-phi joins, *not* `unreachable`

Target artifact for quotearg: one `define` containing the full 8,448-byte
function's control flow, dynamic edges resolved to in-function blocks where
the target is statically known.

## Inputs (all on disk, all verified)

| Input | Source | Status |
|---|---|---|
| Merged+linked module | `qa_linked2.ll` style (`--link_blocks` output, post RSP/RBP fix `9332378`) | 398 defines, exits as tail calls / dispatch |
| Per-block bytes | `qa_blocks/b_%.5x.ll.bytes` + `manifest.txt` | 397 blocks |
| Machine edge table | objdump of `quotearg_full.txt` (decoder = audit script) | 369/369 static edges verified |
| Disassembly | `qa_disasm.txt` | for spot checks |

## Key design decisions

### D1. The 63-value exit vector is the seam

Every flat block function's exit call carries the full architectural state
as its operand vector. That is exactly the data an SSA merge needs:

```
for block B, exit E (one per machine successor):
    state_out(E) = <the 63 call operands of E's tail call>
```

- Linked exits (`tail call @sub_X`): operands already in block-function form
  (ptr RSP/RBP since `9332378`).
- Dispatch exits (`tail call @__remill_flat_jump`): operands in stub form
  (i64 RSP/RBP via `ptrtoint`) — normalize by replacing each with the
  `ptrtoint`'s source. **Both forms normalize to the block-function
  convention (ptr RSP/RBP)**, which becomes the single function's ABI.
- A machine block with a conditional branch has *two* exit stubs
  (taken/fall); the audit's select-arm logic maps each stub to its machine
  successor (reuse `FlatExitTargetPc`'s arm recovery).

### D2. New function ABI

```
define ptr @sub_0(ptr %memory,
                  i64 × 17 GPRs, i64 × 4 seg, i8 × 7 flags,
                  i64 × 8 MMX, <2 x i64> × 16 XMM, i128 × 8 X87,
                  ptr %RSP, ptr %RBP)
```

- Drop `PC` / `NEXT_PC` arguments: control flow is now real control flow;
  the PC is implicit in the CFG.
- Drop all `@flat_pc_N` globals.
- `RSP`/`RBP` stay ptr, threaded through phis like any other state value.

### D3. Body extraction + PC constant folding

Each block's straight-line body is the instructions before the first exit
stub. It references the envelope in exactly three ways, all removable:

1. **Per-instruction PC bookkeeping**
   (`load %NEXT_PC / add / store %NEXT_PC / store %PC` chains) → delete.
2. **Uses of the PC *value*** — only in the GOT-displacement pattern for
   calls: `%.cast = inttoptr <pc_value>; %stack_ptr = GEP %.cast, <disp>`.
   The audit's linear scan already computes the exact PC constant at every
   instruction; replace the value with that constant (e.g. 0x76 at the
   0x70 call site), keep the `inttoptr+GEP` (it is a runtime load of the
   PLT/GOT slot — still needed for devirtualization).
3. **Flag/stack computations** — keep as-is; they are plain dataflow.

Postcondition (unit-tested): zero references to `PC`, `NEXT_PC`,
`@flat_pc_*` in any extracted body.

### D4. Join points: 63-wide phi farms

For each block B and each state slot s:

```
%bb_B:
  %s.phi = phi <type> [ %arg_s,     %entry ]      ; if B == 0x0
                 [ %s.out(p), %bb_p ]              ; for each pred p
  <body of B, remapped>
  <exit per D5>
```

- Entry block 0x0 binds slots to the function arguments.
- ~100 join points × 63 slots ≈ 6,300 phi nodes. Size is not a concern.
- Blocks whose predecessor is not lifted (the 7 X87 DIL/SIL blocks) are
  excluded from the phi; edges into them go to the runtime join (D5).

### D5. Exit model

| Machine terminal | Emission |
|---|---|
| `jmp` / fallthrough to lifted block | `br label %bb_T` |
| `jcc` (both arms lifted) | `br i1 %cond, label %bb_taken, label %bb_fall` |
| `jcc` / `jmp` with an unlifted or off-function target | `switch` on the exit PC value: known lifted targets → their BBs; `default` → runtime join (below) |
| indirect call / PLT call / devirt call (`__remill_function_call`, 3-arg `@sub_8a80`-style) | keep the call **mid-block**, then a **state-phi join** (see D6), then `br` to the return-address block (always statically known) |
| `ret` (if any — quotearg has none in-function) | runtime join (return targets unknown statically) |

**Runtime join block** (one per function) — **implemented**:

Each dead-end dispatch stub calls the opaque external and branches to the
shared join; the join phi-merges the call's outputs (and the dynamic target)
and `switch`es on the target PC to *every* lifted block, defaulting to `ret`:

```
%dyn_join:                              ; 60 state phis + 1 target-PC phi,
  %t   = phi i64    [ %target, %stub_i ]    one incoming per dispatch stub
  %s   = phi <60>   [ %dispatch_out, %stub_i ]
  switch i64 %t, label %dyn_default [      ; re-entry: a call/indjmp could
    0x0, label %bb_0, ... 0x20c1, label %bb_20c1 ]  resume at any block
%dyn_default:
  ret ptr %memory
```

Because the join reaches every block, the full CFG is reachable from
`fn_entry` and survives `opt -O3` (see Status). The `switch` re-entry is a
*conservative model* of dynamic re-entry — sound (it keeps a superset of the
truly-reachable set), and it is exactly what the real Saturn runtime would
narrow to the actually-resolved continuation.

`@__remill_dynamic_dispatch` is an **opaque external** with a documented
contract (execute the out-of-function target / resolve the indirect target,
return the updated register file). The minimal runtime in this tree gets a
no-op stub returning the inputs unchanged; the real Saturn runtime
implements it. The 60 join phis thread that updated state into whichever
block the `switch` resumes in.

### D6. The indirect-call clobber problem (correctness-critical)

The flat model threads *pre-call* register values past external calls
(the runtime is supposed to update the state between blocks, but the
by-value SSA operands can't see that). In a single function this must be
modeled explicitly:

- After every dynamic call, **all registers the callee may clobber are
  unknown.** v1 conservatively treats the full 63-state as post-call
  outputs of the call (D5's join): the call's typed result feeds the
  continuation block's phis.
- This is exactly how remill's regular (non-flat) ABI models calls
  (`__remill_function_call` returns a state struct). We adopt the same
  semantics: the call returns the updated register file.
- Consequence: the minimal stub runtime must be extended to actually
  execute the target and fill the state (or the test harness must supply a
  per-test implementation). Documented as a runtime work item.

Note for quotearg: the code after the 0x70 call reloads what it needs from
the stack (`mov 0x44(%rsp),%eax`), so even a no-op stub gives correct
*control flow*; register-exactness needs the real runtime.

### D7. Metadata hygiene

- **Strip all** `!noalias`, `!alias.scope`, `!nofree`, `initializes(...)`
  on the merged function. The per-block alias scopes ("sub_76: %RSP") are
  *unsound* once blocks share one function: different blocks genuinely
  alias through the same stack. Keeping them would let the optimizer prove
  away cross-block stack traffic.
- Keep `poison` flag values (they are correct: flags not set by a block
  are undefined at its entry only when *all* preds leave them undefined —
  v1 keeps the conservative ISEL values; revisit after D4 phis are in).
- Keep `!prof`/none: not present.

### D8. Implementation location

In-process, in the lift tool (same pattern as `MergeFlatBlockModules` /
`LinkFlatBlockExits`, which is the proven path in this tree — the fork's
standalone opt has the broken verifier and simplifycfg):

```
include/remill/BC/Unflatten.h      : UnflattenModule(Module&, const EdgeTable&)
lib/BC/Unflatten.cpp               : the pass
bin/lift/Lift.cpp                  : --unflatten <merged.ll> [--edges <edges.json>] --ir_out out.ll
```

Edge table: v1 reads a **JSON sidecar** produced by the (existing) Python
audit decoder — avoids porting the x86 decoder to C++ up front. v2 may port
the decoder in-tree. JSON schema:

```json
{ "0x128a": { "end": 0x1293, "kind": "jcc",
              "taken": 0x18d7, "notaken": 0x1293, "lifted": [true, true] } }
```

## Phases & acceptance criteria

### M0 — Edge table (0.5 d)
Port the audit decoder to a standalone script emitting `edges.json` for the
whole function.
✅ 397 entries; 369 with ≥1 lifted static target; matches the audit's
   369/369 result byte-for-byte.

### M1 — Body extraction (1 d)
Per-block: split body/stubs, collect exit vectors (both stubs where
conditional), PC constant folding, delete bookkeeping.
✅ every body: 0 refs to PC/NEXT_PC/cells; every exit vector has 63
   normalized values; module still parses + passes the custom call checker
   (`check_calls.py` logic).

### M2 — Single-function emission on a real small test (1–2 d) ✅
Used real quotearg blocks (not a synthetic fixture) to avoid the 71-arg
`byval(%union.vec128_t)` XMM encoding that makes hand-crafted blocks
`@__remill_flat_jump`-type-invalid. Two fixtures in `tests/Unflatten/`:
  * jmp: `0x1bdc` -(static jmp)-> `0x463` -(off-manifest jmp)-> dispatch
  * jcc: `0x10b2` -(jcc)-> { taken: off-manifest dispatch,
                            notaken: `0x10bd` (static `br`) }
✅ `unflatten_smoke` ctest passes all three scenarios (jmp, jcc, jcc+optimize):
   1 function, 61-arg ABI, `fn_entry` → entry block, a 60-wide phi farm at
   every block BB, and an **edge validation** that every in-fixture static
   machine edge is emitted as a real `br` to the successor BB. The jcc arm
   mapping is verified (D9): `br i1 %c, label %taken, label %notaken`.
   Note: entry is the lowest-PC block (the pass assumes a real function's
   entry is its lowest PC — true for quotearg's `0x0`).

### M3 — Full quotearg (2–3 d) ✅
✅ 1 function `@quotearg_unflat`; 878 BBs (397 body + 480 jcc/jmp arm stubs +
   `fn_entry`); 639 `br` edges; 384 loop back-edges (spot-checked: `bb_318`
   has 9 back-edge preds — the main dispatch loop); 23,820 phi nodes
   (60 × 397 block BBs); 261 `@__remill_dynamic_dispatch` calls.
   **Edge validation: 397/408 static lifted machine edges appear as a `br`
   to the correct `%bb_<target>`; the 11 missing are all the 7 merged-jcc
   blocks (no static arms — runtime dispatch, expected v1 behavior).**
   0 residual `flat_pc_*` / `@sub_*` block calls; llvm-as-clean.
   The 237 `unreachable`s are the documented off-manifest cold targets
   (see the Status banner), not lifted X87 dead-edges.

### M4 — Dynamic call state model + joins (1–2 d) ✅
Two distinct dynamic-call patterns, handled per D6:
  * **Terminal dynamic call** (12 blocks, `call ptr
    @__remill_function_call(%PC, %disp, %memory)` as the block's last call):
    replaced with a `@__remill_dynamic_dispatch` call in the exit stub +
    `br` to the statically-known return-address BB.
  * **Mid-block devirt** (3 blocks, 3-arg `call ptr @sub_8a80(...)` in the
    body): the 63-arg `@sub_8a80` *declare* makes a 3-arg call type-invalid,
    so the call is **retargeted to `@__remill_function_call`** (3-arg) in
    `CloneInto` — type-valid, and the conservative full-state-clobber is
    modeled by the block continuing straight-line (quotearg reloads from the
    stack after the 0x70 call, so a no-op state stub gives correct control
    flow — see D6's quotearg note).
   `@sub_8a80.1` (isolated trampoline) is deleted; the now-unused 3-arg
   `@sub_8a80` declare is DCE'd.

### M5 — Optimization + differential check (1 d) ✅
In-process, behind `--unflatten_opt` (the plan's "simplifycfg-off" phrasing
means: **no** `SimplifyCFGPass`). Runs `InstCombinePass` + `DCEPass` on the
new function (a `FunctionPassManager` on the PassBuilder-registered FAM —
a bare manager segfaults, same as GlobalDCE). Verified on full quotearg:
**CFG structure is byte-identical before/after** (same 878 BBs, 639 edges,
384 back-edges, same per-BB terminators), body drops 109,484 → 3,508 lines
(96.8%). The dead per-instruction PC GEPs and constant PCs the flat envelope
carried fold away; dead (off-manifest/cold) block bodies empty out —
expected, since the static prefix is short. Covered by the `jcc_opt` smoke
scenario (static edge survives optimization).

## Risks & open questions

1. **RSP as phi-threaded pointer**: each block's stack GEPs use its *input*
   RSP; after merging, offsets from different blocks compose through the
   phis (verified for the 0x76 chain: machine `[rsp+0x8e]` @0x76 =
   `[rsp₀+86]`). Risk: an optimizer must not fold GEPs across a call that
   may change RSP (external calls can push/pop!). Mitigation: mark the
   dynamic-call join as clobbering RSP (full state already clobbered per
   D6 — consistent).
2. **X87 holes** (7 unliftable blocks): edges targeting them must route to
   the runtime join, and phis must not take their outputs. Edge table
   carries `lifted: false`. If any of them sits on the *static prefix*
   (it doesn't in quotearg), the prefix would end at a join — acceptable.
3. **Select-arm → stub mapping** already proven (audit); port the exact
   logic, don't reinvent.
4. **The 3-arg `@sub_8a80` devirt calls** collide in name with the 63-arg
   block declare (latent bug C). Unflatten renames the 3-arg dispatcher
   uses to a fresh `@__remill_call_<target>` external — resolves the
   collision in the output for free.
5. **Stub ABI skew (latent bug B)** disappears in the output: no
   `__remill_flat_jump` 63-vs-71 calls remain; only the new, consistently
   typed `@__remill_dynamic_dispatch` external.
6. **Scale**: ~6,300 phis + ~398 BBs is trivial for LLVM; the risk is book
   keeping in the pass, not performance.

## Out of scope

- Lifting the 7 X87 DIL/SIL blocks (needs X87 in the flat state).
- Fixing the ISEL↔`__remill_flat_jump` stub ABI (bug B) — moot for the
  unflattened output, still relevant for the flat pipeline.
- A new runtime: `@__remill_dynamic_dispatch` is declared, stubbed
  no-op, contract documented; real implementation is a Saturn-runtime task.

## Deliverables

1. `flat/plan_unflatten.md` (this file)
2. `edges.json` generator script (Python, in `tools/` or repo-local)
3. `lib/BC/Unflatten.cpp` + `include/remill/BC/Unflatten.h`
4. `--unflatten` (+ `--unflatten_opt`) mode in `bin/lift/Lift.cpp`
5. `/tmp/quotearg_unflattened.ll` — the single-function quotearg (4.8 MB,
   pre-opt; the `--unflatten_opt` build is 126 KB with the same CFG)
6. `tests/Unflatten/` — self-contained `unflatten_smoke` ctest on real
   quotearg blocks (jmp + jcc + optimize scenarios, with edge validation)
