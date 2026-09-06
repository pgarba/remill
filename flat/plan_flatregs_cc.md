# FlatRegs Calling Convention — Implementation Plan

## Goal

Define a custom LLVM calling convention (`FlatRegs`) for x86-64 that maps
virtual registers directly to physical registers (in-out mode), eliminating
stack frames, prologues, epilogues, and pointer indirection in lifted code.

## Why

Current flat mode passes 51 pointer args → load/store through pointers → SROA.
The custom CC eliminates all of that:

```
  Current:  51 ptr args → 51 loads → compute → 51 stores → SROA → regalloc
  Target:   32 reg args (in-out) → compute → done (regalloc is identity)
```

Lifted code becomes:
```asm
  ; no push rbp, no sub rsp, N, no epilogue
  sub rsp, 16           ; lifted instruction
  mov [rsp], rdi        ; lifted instruction
  add rax, rsi          ; lifted instruction
  ret                   ; lifted instruction
```

## Phase 1: LLVM Backend — Define the CC

### 1.1 Add CC ID

File: `llvm/IR/CallingConv.h`

```cpp
enum CallingConvID : unsigned short {
  ...
  // First available for custom use:
  FlatRegs = 200,  // x86-64: all GPRs+XMMs in-out, no frame
};
```

### 1.2 Implement CC in X86 backend

File: `llvm/lib/Target/X86/X86CallingConvention.cpp`

Register mapping (32 in-out registers):
```
  arg 0  → RAX     (in-out)
  arg 1  → RBX     (in-out)
  arg 2  → RCX     (in-out)
  arg 3  → RDX     (in-out)
  arg 4  → RSI     (in-out)
  arg 5  → RDI     (in-out)
  arg 6  → RBP     (in-out)
  arg 7  → RSP     (in-out)   ← key: no stack recovery
  arg 8  → R8      (in-out)
  arg 9  → R9      (in-out)
  arg 10 → R10     (in-out)
  arg 11 → R11     (in-out)
  arg 12 → R12     (in-out)
  arg 13 → R13     (in-out)
  arg 14 → R14     (in-out)
  arg 15 → R15     (in-out)
  arg 16 → XMM0    (in-out)
  arg 17 → XMM1    (in-out)
  ...
  arg 31 → XMM15   (in-out)
```

Key methods to implement:

```cpp
// 1. getCallPSAreas: assign each arg to its physical register
//    Mark as Volatile=true (in-out: callee modifies, caller sees change)
void getCallPSAreas(CallingConv CC, ...) const {
  if (CC == CallingConv::FlatRegs) {
    for (unsigned i = 0; i < NumArgs; ++i) {
      MCRegUnit Reg = FlatRegMap[i];
      Areas.push_back({Reg, 0, /*Volatile=*/true});
    }
    // No stack areas
    // No return value area (implicit: registers ARE the result)
  }
}

// 2. getCalleeSavedRegs: return nullptr (save nothing)
//    All registers are in-out; no callee-saved set.

// 3. getStackAlignment: return 16 (minimal, no frame)

// 4. getReturnArea: no return register (or RAX for i64 return)

// 5. Mark function as frameless:
//    - No frame pointer
//    - No stack reallocation
//    - Prologue/epilogue: empty
```

### 1.3 Handle RSP specially

RSP is the stack pointer. The compiler normally uses it for frame management.
With FlatRegs CC:

- The function is implicitly `naked` (no compiler-managed frame)
- RSP is an in-out argument: the lifted code owns the stack
- The compiler must NOT allocate stack space (no spills)
- If the compiler needs a temp, it uses a GPR (16 available)

Implementation:
```cpp
// In X86FrameLowering or X86ISel:
// If CC == FlatRegs:
//   - Skip prologue/epilogue generation
//   - Skip stack frame allocation
//   - RSP is a live-in/live-out register (like any other arg)
//   - No red zone, no alignment fixups
```

### 1.4 Unused registers → undef/zero

Registers not used by the lifted function:
- Entry: contain garbage (or zero, if we want deterministic behavior)
- The CC doesn't need to do anything special — the allocator simply
  doesn't assign them. They're not part of the function's live state.
- Optionally: emit `xor reg, reg` for determinism (debug builds)

### 1.5 Tail calls

With FlatRegs CC, a tail call to another FlatRegs function is a plain `jmp`:
- Same register mapping → no register shuffling
- No stack frame to adjust
- The callee's "arguments" are already in the right registers

```cpp
// In X86ISelLowering or X86InstrInfo:
// If CC == FlatRegs && tail call && callee CC == FlatRegs:
//   Emit JMP (not CALL)
//   No stack adjustment
//   No register moves
```

## Phase 2: Lifter — Emit FlatRegs Code

### 2.1 Remove the state struct

Current `LiftIntoBlock`:
```cpp
state_ptr = GetStatePointer(func);           // find STATE_LOCAL alloca
reg_ptr = LoadRegAddress(block, state_ptr, "RAX");  // GEP into struct
val = LoadRegValue(block, state_ptr, "RAX");        // load from struct
```

New `LiftIntoBlock` (FlatRegs mode):
```cpp
// Virtual register file: map<string, Value*>
// Seeded at function entry from the CC arguments
Value *rax = reg_file["RAX"];    // → CC arg 0 (physical RAX)
Value *rbx = reg_file["RBX"];    // → CC arg 1 (physical RBX)
// ...
Value *sum = ir.CreateAdd(rax, rbx);  // → add rax, rbx
reg_file["RAX"] = sum;               // update the map
```

No GEPs. No loads. No stores. No struct. Just SSA values.

### 2.2 Function entry

```cpp
// Declare function with FlatRegs CC
auto *func_type = FunctionType::get(ret_type, arg_types, false);
auto *func = Function::Create(func_type, ExternalLinkage, name, module);
func->setCallingConv(CallingConv::FlatRegs);

// Entry block: seed the register file from CC arguments
// arg 0 → RAX, arg 1 → RBX, etc.
auto &entry = func->getEntryBlock();
IRBuilder<> ir(&entry);

reg_file["RAX"] = &func->getArg(0);   // physical RAX
reg_file["RBX"] = &func->getArg(1);   // physical RBX
reg_file["RSP"] = &func->getArg(7);   // physical RSP
// ...
// Unused registers: undef
reg_file["R8"] = UndefValue::get(i64);  // if not used
```

### 2.3 Block merges (phi nodes)

When two blocks merge (branch), the register file needs phi nodes:

```cpp
// At merge block:
for (each reg in reg_file) {
  if (reg has different values from each incoming block) {
    reg_file[reg] = ir.CreatePHI(type, num_incoming);
    reg_file[reg]->addIncoming(val_from_block1, block1);
    reg_file[reg]->addIncoming(val_from_block2, block2);
  }
}
```

This is standard SSA construction. The register allocator handles the rest.

### 2.4 Function exit

```cpp
// Return: the value is already in RAX (CC arg 0)
// If the return value is in a different register:
//   reg_file["RAX"] = reg_file["RDX"];  // move to RAX
// ret
ir.CreateRet(reg_file["RAX"]);
```

### 2.5 Memory operations

Memory reads/writes use the memory pointer (passed as a regular arg or in a reserved register):

```cpp
// "mov rax, [rbx + 16]"
Value *addr = ir.CreateGEP(i8, mem_ptr, ir.CreateAdd(reg_file["RBX"], 16));
reg_file["RAX"] = ir.CreateLoad(i64, addr);
// → mov rax, [rbx + 16]  (allocator keeps RBX in physical RBX)
```

## Phase 3: Integration

### 3.1 Build the modified LLVM

```bash
cd /home/adam/saturn_21_1/saturn/deps/tob_libraries/llvm
# Apply patches to:
#   include/llvm/IR/CallingConv.h
#   lib/Target/X86/X86CallingConvention.cpp
#   lib/Target/X86/X86FrameLowering.cpp (skip prologue for FlatRegs)
#   lib/Target/X86/X86ISelLowering.cpp (tail call → jmp)
ninja llvm  # rebuild
```

### 3.2 Modify the lifter

```bash
cd /home/adam/saturn_21_1/saturn/build_remill/remill
# Modify:
#   lib/BC/InstructionLifter.cpp — FlatRegs mode (no state struct)
#   lib/Arch/X86/Arch.cpp — DeclareLiftedFunction with FlatRegs CC
#   bin/lift/Lift.cpp — --flat-cc flag
```

### 3.3 Test

```bash
# Lift a simple function
remill-lift-21 --flat-cc --bytes "4889e54883ec10c9c3" --ir_out out.ll

# Verify: no alloca, no GEP into state, no load/store through pointers
grep -c "alloca" out.ll        # expect: 0
grep -c "getelementptr.*State" out.ll  # expect: 0

# Compile to machine code
llc -mtriple=x86_64 out.ll -o out.s

# Verify: no push rbp, no sub rsp (except lifted instructions)
grep "push\|pop" out.s         # expect: 0 (or only lifted push/pop)
```

## Phase 4: Validation

### 4.1 Differential test

Lift the same function in three modes:
1. Old mode (state struct)
2. Current flat mode (51 pointer args + SROA)
3. New FlatRegs CC mode

Compare machine code output. All three must produce equivalent results.

### 4.2 Size comparison

```
  Metric              Old    Flat(ptr)    FlatRegs(CC)
  ──────────────────  ────   ───────────   ────────────
  IR instructions     1086    63            ~42 (1:1 with machine code)
  IR loads            79      4             0
  IR stores           43      15            0
  Machine instrs      378     135           ~42
  Prologue/epilogue   yes     yes           no
  Stack frame         yes     yes           no
```

### 4.3 Correctness

Run the lifted code against the native code on random inputs.
Compare all register values + memory after execution.

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| RSP handling breaks compiler assumptions | High | Test with `-mno-red-zone`, verify no implicit stack use |
| Register allocator fights the CC | Medium | CC-pinned regs are "reserved" — allocator can use them but must restore at exit |
| Inliner doesn't preserve CC semantics | Medium | Test: inline a FlatRegs call into a SysV caller |
| Debug info is wrong (regs don't match DWARF) | Low | Use `-g0` for VMP use case |
| LLVM upstream rejects the patch | Low | Keep as out-of-tree patch / fork |

## File Changes Summary

### LLVM (fork/patch)
| File | Change |
|------|--------|
| `include/llvm/IR/CallingConv.h` | Add `FlatRegs = 200` |
| `lib/Target/X86/X86CallingConvention.cpp` | Implement `getCallPSAreas` for FlatRegs |
| `lib/Target/X86/X86FrameLowering.cpp` | Skip prologue/epilogue for FlatRegs |
| `lib/Target/X86/X86ISelLowering.cpp` | Tail call → JMP for FlatRegs→FlatRegs |
| `lib/Target/X86/X86RegisterInfo.cpp` | Mark all 32 regs as volatile for FlatRegs |

### Remill (lifter)
| File | Change |
|------|--------|
| `lib/BC/InstructionLifter.cpp` | FlatRegs mode: reg_file map, no state struct |
| `lib/Arch/X86/Arch.cpp` | `DeclareLiftedFunction` with `CallingConv::FlatRegs` |
| `include/remill/BC/ABI.h` | `kFlatRegsCC` constant |
| `bin/lift/Lift.cpp` | `--flat-cc` flag |
| `tests/X86/FlatCC.cpp` | New test: lift + verify no frame |

## Effort Estimate

| Phase | Work | Time |
|-------|------|------|
| 1: LLVM CC | Patch 5 files, build, test with hand-written IR | 2-3 days |
| 2: Lifter | Reg_file map, remove state struct, phi nodes | 2-3 days |
| 3: Integration | Build, wire up, basic test | 1 day |
| 4: Validation | Differential test, size comparison, correctness | 1-2 days |
| **Total** | | **~1-2 weeks** |

## First Step (prototype)

Before patching LLVM, validate the concept with a hand-written IR file:

```llvm
; /tmp/flat_cc_test.ll
; Test: does llc respect a custom CC with in-out registers?

; For now, use "naked" + inline asm to simulate the CC:
define naked void @test() {
entry:
  ; Simulate: RAX, RBX, RSP are in-out
  ; No prologue, no epilogue
  ret void
}
```

Then check the machine code output for absence of prologue/epilogue.
If `naked` alone gives us what we need, the CC patch is simpler.
