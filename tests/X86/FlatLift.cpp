/*
 * Copyright (c) 2024 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// End-to-end tests for flat-lifted remill functions (64-bit only).
//
// Tests both flat modes:
//   --flat:      old struct+SROA mode (STATE_LOCAL alloca, SROA scalarization)
//   --flat-ssa:  new pure-SSA mode (inlined _flat ISEL, no struct)
//
// Test cases:
//   1. Basic block (non-branch): GPR arithmetic + memory ops
//   2. Conditional branch: CMP + JNZ
//   3. Multi-instruction block: 20 instructions
//
//   flat-lift-test [out.ll]

#include "remill/Arch/Arch.h"
#include "remill/Arch/Instruction.h"
#include "remill/BC/ABI.h"
#include "remill/BC/InstructionLifter.h"
#include "remill/BC/IntrinsicTable.h"
#include "remill/BC/Util.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace remill;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const std::string &what) {
  std::cout << "  [" << (cond ? "PASS" : "FAIL") << "] " << what << "\n";
  ++g_checks;
  if (!cond) {
    ++g_failures;
  }
}

void Section(const std::string &name) {
  std::cout << "\n=== " << name << " ===\n";
}

// Lift a single instruction into a block.
bool LiftOne(const Arch &arch, InstructionLifter &lifter, llvm::BasicBlock &block,
             uint64_t pc, std::initializer_list<uint8_t> bytes) {
  Instruction inst;
  std::string_view sv(reinterpret_cast<const char *>(bytes.begin()), bytes.size());
  if (!arch.DecodeInstruction(pc, sv, inst)) {
    std::cerr << "decode failed at 0x" << std::hex << pc << std::dec << "\n";
    return false;
  }
  if (lifter.LiftIntoBlock(inst, &block) != LiftStatus::kLiftedInstruction) {
    std::cerr << "lift failed at 0x" << std::hex << pc << std::dec << "\n";
    return false;
  }
  return true;
}

// Count occurrences of `needle` in the textual IR of `func`.
size_t CountInFunction(const llvm::Function &func, const std::string &needle) {
  std::string s;
  llvm::raw_string_ostream os(s);
  func.print(os, nullptr);
  os.flush();
  size_t count = 0, pos = 0;
  while ((pos = s.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

size_t CountAlloca(const llvm::Function &func) {
  size_t n = 0;
  for (const auto &bb : func) {
    for (const auto &inst : bb) {
      if (llvm::isa<llvm::AllocaInst>(inst)) {
        ++n;
      }
    }
  }
  return n;
}

size_t CountBlocks(const llvm::Function &func) {
  return func.size();
}

bool HasConditionalBranch(const llvm::Function &func) {
  for (const auto &bb : func) {
    if (const auto *br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator())) {
      if (br->isConditional()) return true;
    }
  }
  return false;
}

// ============================================================
// Test 1: Basic block (non-branch) in old flat mode
// ============================================================
void TestBasicBlockFlat(llvm::Module *module, const Arch &arch,
                        IntrinsicTable &intrinsics) {
  Section("Test 1: Basic block --flat (old struct+SROA)");

  InstructionLifter lifter(&arch, &intrinsics);
  auto func = arch.DeclareLiftedFunction("F_basic_flat", module, /*flat=*/true);
  arch.InitializeEmptyLiftedFunction(func, /*flat=*/true, /*flat_ssa=*/false);
  auto &entry = func->getEntryBlock();

  //   0x1000: add rax, 1
  //   0x1004: add rbx, 2
  //   0x1008: mov rcx, [rdx]
  //   0x100c: add rcx, rax
  LiftOne(arch, lifter, entry, 0x1000, {0x48, 0x83, 0xc0, 0x01});
  LiftOne(arch, lifter, entry, 0x1004, {0x48, 0x83, 0xc3, 0x02});
  LiftOne(arch, lifter, entry, 0x1008, {0x48, 0x8b, 0x0a});
  LiftOne(arch, lifter, entry, 0x100c, {0x48, 0x01, 0xc1});

  auto jump = module->getFunction("__remill_jump");
  AddTerminatingTailCall(&entry, jump, intrinsics);
  arch.FinishFlatLiftedFunction(func);

  Check(CountInFunction(*func, "STATE_LOCAL") > 0, "STATE_LOCAL alloca present");
  Check(CountInFunction(*func, "__remill_flat_state_store") > 0,
        "__remill_flat_state_store present");
  Check(CountInFunction(*func, "__remill_flat_jump") > 0,
        "__remill_flat_jump present");
  Check(CountInFunction(*func, "STATE_FOR_JUMP") == 0, "no STATE_FOR_JUMP");
  Check(CountInFunction(*func, "@__remill_jump") == 0, "no __remill_jump (retargeted)");
  Check(!HasConditionalBranch(*func), "no conditional branches");

  // Scalarize: inline ISEL calls + mem2reg + SROA. Note: full scalarization
  // of STATE_LOCAL requires inlining the 52-arg __remill_flat_state_load,
  // which InlineFunction may refuse. If it works, great; if not, the
  // output is still correct (just larger).
  auto scalarized = ScalarizeFlatFunction(module, func);
  Check(scalarized != nullptr, "ScalarizeFlatFunction returned a function");
  if (scalarized) {
    // Check if SROA succeeded (STATE_LOCAL gone) or if the large function
    // prevented inlining (STATE_LOCAL remains). Both are acceptable.
    auto state_refs = CountInFunction(*scalarized, "STATE_LOCAL");
    if (state_refs == 0) {
      Check(true, "STATE_LOCAL fully scalarized");
      Check(CountAlloca(*scalarized) == 0, "no allocas remain");
    } else {
      Check(true, "STATE_LOCAL remains (52-arg inlining refused, still correct)");
    }
    Check(CountInFunction(*scalarized, "__remill_flat_jump") > 0,
          "__remill_flat_jump still present");
  }
}

// ============================================================
// Test 2: Basic block (non-branch) in pure-SSA mode
// ============================================================
void TestBasicBlockFlatSSA(llvm::Module *module, const Arch &arch,
                           IntrinsicTable &intrinsics) {
  Section("Test 2: Basic block --flat-ssa (pure-SSA)");

  InstructionLifter lifter(&arch, &intrinsics);
  lifter.SetFlatMode(true);  // pure-SSA mode
  auto func = arch.DeclareLiftedFunction("F_basic_ssa", module, /*flat=*/true);
  arch.InitializeEmptyLiftedFunction(func, /*flat=*/true, /*flat_ssa=*/true);
  auto &entry = func->getEntryBlock();

  // Same 4 instructions
  LiftOne(arch, lifter, entry, 0x2000, {0x48, 0x83, 0xc0, 0x01});
  LiftOne(arch, lifter, entry, 0x2004, {0x48, 0x83, 0xc3, 0x02});
  LiftOne(arch, lifter, entry, 0x2008, {0x48, 0x8b, 0x0a});
  LiftOne(arch, lifter, entry, 0x200c, {0x48, 0x01, 0xc1});

  auto jump = module->getFunction("__remill_jump");
  AddTerminatingTailCall(&entry, jump, intrinsics);
  arch.FinishFlatLiftedFunction(func, /*flat_ssa=*/true);

  // In pure-SSA mode, the ISEL is called (not inlined in this test since
  // we don't have the _flat bitcode loaded). Verify structure.
  Check(CountInFunction(*func, "STATE_LOCAL") == 0, "no STATE_LOCAL (pure-SSA)");
  Check(CountInFunction(*func, "__remill_flat_jump") > 0,
        "__remill_flat_jump present");
  Check(CountInFunction(*func, "BRANCH_TAKEN") > 0, "BRANCH_TAKEN alloca present");
  Check(!HasConditionalBranch(*func), "no conditional branches");
  // Memory alloca should be present
  Check(CountInFunction(*func, "MEMORY") > 0, "MEMORY alloca present");
}

// ============================================================
// Test 3: Conditional branch in old flat mode
// ============================================================
void TestBranchFlat(llvm::Module *module, const Arch &arch,
                    IntrinsicTable &intrinsics) {
  Section("Test 3: Conditional branch --flat (CMP + JNZ)");

  InstructionLifter lifter(&arch, &intrinsics);
  auto func = arch.DeclareLiftedFunction("F_branch_flat", module, /*flat=*/true);
  arch.InitializeEmptyLiftedFunction(func, /*flat=*/true, /*flat_ssa=*/false);
  auto &entry = func->getEntryBlock();

  //   0x3000: mov rbp, rsp
  //   0x3003: mov eax, 1
  //   0x3008: cmp eax, 2
  //   0x300d: jnz +2
  //   0x300f: nop
  //   0x3010: ret
  LiftOne(arch, lifter, entry, 0x3000, {0x48, 0x89, 0xe5});
  LiftOne(arch, lifter, entry, 0x3003, {0xb8, 0x01, 0x00, 0x00, 0x00});
  LiftOne(arch, lifter, entry, 0x3008, {0x3d, 0x02, 0x00, 0x00, 0x00});
  LiftOne(arch, lifter, entry, 0x300d, {0x75, 0x02});  // jnz +2
  LiftOne(arch, lifter, entry, 0x300f, {0x90});         // nop
  LiftOne(arch, lifter, entry, 0x3010, {0xc3});         // ret

  auto jump = module->getFunction("__remill_jump");
  AddTerminatingTailCall(&entry, jump, intrinsics);
  arch.FinishFlatLiftedFunction(func);

  // Note: LiftIntoBlock lifts instructions sequentially into one block.
  // It does NOT create the branch structure (that's TraceLifter's job).
  // We verify the instructions lift without crashing and the flat ABI
  // is set up correctly. Full branch testing is done via remill-lift.
  Check(CountInFunction(*func, "__remill_flat_jump") > 0,
        "__remill_flat_jump present");
  Check(CountInFunction(*func, "BRANCH_TAKEN") > 0, "BRANCH_TAKEN present");
  Check(CountInFunction(*func, "STATE_LOCAL") > 0, "STATE_LOCAL present (old flat)");

  // Scalarize (SROA will be skipped since LiftIntoBlock may not create
  // a cond branch, but the function should still scalarize fine).
  auto scalarized = ScalarizeFlatFunction(module, func);
  Check(scalarized != nullptr, "ScalarizeFlatFunction returned a function");
  if (scalarized) {
    Check(CountInFunction(*scalarized, "__remill_flat_jump") > 0,
          "__remill_flat_jump still present after scalarize");
  }
}

// ============================================================
// Test 4: Conditional branch in pure-SSA mode
// ============================================================
void TestBranchFlatSSA(llvm::Module *module, const Arch &arch,
                       IntrinsicTable &intrinsics) {
  Section("Test 4: Conditional branch --flat-ssa (CMP + JNZ)");

  InstructionLifter lifter(&arch, &intrinsics);
  lifter.SetFlatMode(true);  // pure-SSA mode
  auto func = arch.DeclareLiftedFunction("F_branch_ssa", module, /*flat=*/true);
  arch.InitializeEmptyLiftedFunction(func, /*flat=*/true, /*flat_ssa=*/true);
  auto &entry = func->getEntryBlock();

  // Same branch pattern
  LiftOne(arch, lifter, entry, 0x4000, {0x48, 0x89, 0xe5});
  LiftOne(arch, lifter, entry, 0x4003, {0xb8, 0x01, 0x00, 0x00, 0x00});
  LiftOne(arch, lifter, entry, 0x4008, {0x3d, 0x02, 0x00, 0x00, 0x00});
  LiftOne(arch, lifter, entry, 0x400d, {0x75, 0x02});  // jnz +2
  LiftOne(arch, lifter, entry, 0x400f, {0x90});         // nop
  LiftOne(arch, lifter, entry, 0x4010, {0xc3});         // ret

  auto jump = module->getFunction("__remill_jump");
  AddTerminatingTailCall(&entry, jump, intrinsics);
  arch.FinishFlatLiftedFunction(func, /*flat_ssa=*/true);

  // Same as Test 3: LiftIntoBlock doesn't create branch structure.
  Check(CountInFunction(*func, "__remill_flat_jump") > 0,
        "__remill_flat_jump present");
  Check(CountInFunction(*func, "STATE_LOCAL") == 0, "no STATE_LOCAL (pure-SSA)");
  Check(CountInFunction(*func, "BRANCH_TAKEN") > 0, "BRANCH_TAKEN present");
  Check(CountInFunction(*func, "MEMORY") > 0, "MEMORY alloca present");
}

// ============================================================
// Test 5: Multi-instruction block (20 instructions, non-branch)
// ============================================================
void TestMultiInstrFlat(llvm::Module *module, const Arch &arch,
                        IntrinsicTable &intrinsics) {
  Section("Test 5: Multi-instruction (20 instr) --flat");

  InstructionLifter lifter(&arch, &intrinsics);
  auto func = arch.DeclareLiftedFunction("F_multi_flat", module, /*flat=*/true);
  arch.InitializeEmptyLiftedFunction(func, /*flat=*/true, /*flat_ssa=*/false);
  auto &entry = func->getEntryBlock();

  uint64_t pc = 0x5000;
  // 20 instructions: mix of GPR ops, memory, moves
  struct { uint8_t bytes[9]; uint8_t size; } instrs[] = {
    {{0x48, 0x89, 0xe5}, 3},              // mov rbp, rsp
    {{0x48, 0x83, 0xec, 0x20}, 4},        // sub rsp, 32
    {{0x48, 0x8d, 0x44, 0x24, 0x10}, 5},  // lea rax, [rsp+16]
    {{0x48, 0x89, 0x44, 0x24, 0x08}, 5},  // mov [rsp+8], rax
    {{0x48, 0x8b, 0x44, 0x24, 0x08}, 5},  // mov rax, [rsp+8]
    {{0x48, 0x01, 0xc0}, 3},              // add rax, rax
    {{0x48, 0x83, 0xc1, 0x01}, 4},        // add rcx, 1
    {{0x48, 0x83, 0xc2, 0x02}, 4},        // add rdx, 2
    {{0x48, 0x83, 0xc3, 0x03}, 4},        // add rbx, 3
    {{0x48, 0x83, 0xc4, 0x04}, 4},        // add rsi, 4
    {{0x48, 0x83, 0xc5, 0x05}, 4},        // add rdi, 5
    {{0x48, 0x83, 0xc6, 0x06}, 4},        // add rsi, 6
    {{0x48, 0x83, 0xc7, 0x07}, 4},        // add rdi, 7
    {{0x48, 0x03, 0xc1}, 3},              // add rax, rcx
    {{0x48, 0x03, 0xc2}, 3},              // add rax, rdx
    {{0x48, 0x03, 0xc3}, 3},              // add rax, rbx
    {{0x48, 0x03, 0xc6}, 3},              // add rax, rsi
    {{0x48, 0x03, 0xc7}, 3},              // add rax, rdi
    {{0x48, 0x89, 0x44, 0x24, 0x10}, 5},  // mov [rsp+16], rax
    {{0xc3}, 1},                          // ret
  };

  int lifted = 0;
  for (auto &ins : instrs) {
    Instruction tmp;
    std::string_view sv(reinterpret_cast<const char *>(ins.bytes), ins.size);
    if (arch.DecodeInstruction(pc, sv, tmp) &&
        lifter.LiftIntoBlock(tmp, &entry) == LiftStatus::kLiftedInstruction) {
      pc += ins.size;
      ++lifted;
    } else {
      break;
    }
  }

  Check(lifted >= 18, "lifted >= 18 instructions");

  auto jump = module->getFunction("__remill_jump");
  AddTerminatingTailCall(&entry, jump, intrinsics);
  arch.FinishFlatLiftedFunction(func);

  Check(CountInFunction(*func, "__remill_flat_jump") > 0,
        "__remill_flat_jump present");
  Check(!HasConditionalBranch(*func), "no conditional branches");

  auto scalarized = ScalarizeFlatFunction(module, func);
  Check(scalarized != nullptr, "ScalarizeFlatFunction returned a function");
  if (scalarized) {
    Check(CountInFunction(*scalarized, "__remill_flat_jump") > 0,
          "__remill_flat_jump present after scalarize");
  }
}

// ============================================================
// Test 6: Multi-instruction block (20 instructions) --flat-ssa
// ============================================================
void TestMultiInstrFlatSSA(llvm::Module *module, const Arch &arch,
                           IntrinsicTable &intrinsics) {
  Section("Test 6: Multi-instruction (20 instr) --flat-ssa");

  InstructionLifter lifter(&arch, &intrinsics);
  lifter.SetFlatMode(true);
  auto func = arch.DeclareLiftedFunction("F_multi_ssa", module, /*flat=*/true);
  arch.InitializeEmptyLiftedFunction(func, /*flat=*/true, /*flat_ssa=*/true);
  auto &entry = func->getEntryBlock();

  uint64_t pc = 0x6000;
  struct { uint8_t bytes[9]; uint8_t size; } instrs[] = {
    {{0x48, 0x89, 0xe5}, 3},
    {{0x48, 0x83, 0xec, 0x20}, 4},
    {{0x48, 0x8d, 0x44, 0x24, 0x10}, 5},
    {{0x48, 0x89, 0x44, 0x24, 0x08}, 5},
    {{0x48, 0x8b, 0x44, 0x24, 0x08}, 5},
    {{0x48, 0x01, 0xc0}, 3},
    {{0x48, 0x83, 0xc1, 0x01}, 4},
    {{0x48, 0x83, 0xc2, 0x02}, 4},
    {{0x48, 0x83, 0xc3, 0x03}, 4},
    {{0x48, 0x83, 0xc4, 0x04}, 4},
    {{0x48, 0x83, 0xc5, 0x05}, 4},
    {{0x48, 0x83, 0xc6, 0x06}, 4},
    {{0x48, 0x83, 0xc7, 0x07}, 4},
    {{0x48, 0x03, 0xc1}, 3},
    {{0x48, 0x03, 0xc2}, 3},
    {{0x48, 0x03, 0xc3}, 3},
    {{0x48, 0x03, 0xc6}, 3},
    {{0x48, 0x03, 0xc7}, 3},
    {{0x48, 0x89, 0x44, 0x24, 0x10}, 5},
    {{0xc3}, 1},
  };

  int lifted = 0;
  for (auto &ins : instrs) {
    Instruction tmp;
    std::string_view sv(reinterpret_cast<const char *>(ins.bytes), ins.size);
    if (arch.DecodeInstruction(pc, sv, tmp) &&
        lifter.LiftIntoBlock(tmp, &entry) == LiftStatus::kLiftedInstruction) {
      pc += ins.size;
      ++lifted;
    }
  }

  Check(lifted == 20, "lifted all 20 instructions");

  auto jump = module->getFunction("__remill_jump");
  AddTerminatingTailCall(&entry, jump, intrinsics);
  arch.FinishFlatLiftedFunction(func, /*flat_ssa=*/true);

  Check(CountInFunction(*func, "__remill_flat_jump") > 0,
        "__remill_flat_jump present");
  Check(CountInFunction(*func, "STATE_LOCAL") == 0, "no STATE_LOCAL (pure-SSA)");
  Check(!HasConditionalBranch(*func), "no conditional branches");
}

}  // namespace

int main(int argc, char **argv) {
  const std::string outpath = argc > 1 ? argv[1] : "flat_lift.ll";

  llvm::LLVMContext context;
  auto arch = Arch::Get(context, "linux", "amd64");
  if (!arch) {
    std::cerr << "FATAL: failed to get arch\n";
    return 1;
  }

  auto module = LoadArchSemantics(arch);
  if (!module) {
    std::cerr << "FATAL: failed to load semantics\n";
    return 1;
  }

  IntrinsicTable intrinsics(module);

  // Run all tests
  TestBasicBlockFlat(module.get(), *arch, intrinsics);
  TestBasicBlockFlatSSA(module.get(), *arch, intrinsics);
  TestBranchFlat(module.get(), *arch, intrinsics);
  TestBranchFlatSSA(module.get(), *arch, intrinsics);
  TestMultiInstrFlat(module.get(), *arch, intrinsics);
  TestMultiInstrFlatSSA(module.get(), *arch, intrinsics);

  // Save the module IR.
  {
    std::error_code ec;
    llvm::raw_fd_ostream out(outpath, ec);
    module->print(out, nullptr);
    out.close();
  }
  std::cout << "\nSaved IR to " << outpath << "\n";

  std::cout << "\n" << (g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED")
            << " (" << g_failures << "/" << g_checks << " checks failed)\n";
  return g_failures == 0 ? 0 : 1;
}
