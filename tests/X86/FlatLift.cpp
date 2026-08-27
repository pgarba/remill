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

// End-to-end test for flat-lifted remill functions (64-bit only).
//
// Lifts a representative basic block in FLAT mode, then verifies:
//
//   1. Pre-scalarize IR invariants (checked in-process):
//        - STATE_LOCAL alloca present (the scalarizable local State)
//        - __remill_flat_state_store present (exit writes regs back to pointers)
//        - __remill_flat_jump present (exit passes pointer args directly)
//        - NO STATE_FOR_JUMP alloca (the old struct-rebuild path is gone)
//        - NO __remill_jump call (retargeted to __remill_flat_jump)
//
//   2. Post-scalarize invariants (checked in-process via ScalarizeFlatFunction,
//      which runs inliner + mem2reg + SROA through the new pass manager):
//        - STATE_LOCAL fully scalarized (0 references remain)
//        - no allocas left in the function
//        - no memcpy
//        - __remill_flat_jump still present
//
// The scalarization is done in-process (no external `opt`), so the test is
// self-contained.
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
#include <llvm/Support/raw_ostream.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace remill;

namespace {

int g_failures = 0;

void Check(bool cond, const std::string &what) {
  std::cout << "  [" << (cond ? "PASS" : "FAIL") << "] " << what << "\n";
  if (!cond) {
    ++g_failures;
  }
}

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
  InstructionLifter lifter(arch.get(), &intrinsics);

  auto func = arch->DeclareLiftedFunction("F_e2e", module.get(), /*flat=*/true);
  arch->InitializeEmptyLiftedFunction(func, /*flat=*/true);
  auto &entry = func->getEntryBlock();

  // A representative basic block: GPR arithmetic, a memory load, and a
  // register-to-register add. All independent state modifications (no
  // branches).
  //   0x1000: add rax, 1
  //   0x1004: add rbx, 2
  //   0x1008: mov rcx, [rdx]
  //   0x100c: add rcx, rax
  if (!LiftOne(*arch, lifter, entry, 0x1000, {0x48, 0x83, 0xc0, 0x01})) {
    return 1;
  }
  if (!LiftOne(*arch, lifter, entry, 0x1004, {0x48, 0x83, 0xc3, 0x02})) {
    return 1;
  }
  if (!LiftOne(*arch, lifter, entry, 0x1008, {0x48, 0x8b, 0x0a})) {  // mov rcx,[rdx]
    return 1;
  }
  if (!LiftOne(*arch, lifter, entry, 0x100c, {0x48, 0x01, 0xc1})) {  // add rcx,rax
    return 1;
  }

  auto jump = module->getFunction("__remill_jump");
  if (!jump) {
    std::cerr << "FATAL: missing __remill_jump\n";
    return 1;
  }
  AddTerminatingTailCall(&entry, jump, intrinsics);
  arch->FinishFlatLiftedFunction(func);

  std::cout << "=== Pre-scalarize IR invariants ===\n";
  Check(CountInFunction(*func, "STATE_LOCAL") > 0, "STATE_LOCAL alloca present");
  Check(CountInFunction(*func, "__remill_flat_state_store") > 0,
        "__remill_flat_state_store present (exit)");
  Check(CountInFunction(*func, "__remill_flat_jump") > 0,
        "__remill_flat_jump present (exit)");
  Check(CountInFunction(*func, "STATE_FOR_JUMP") == 0, "no STATE_FOR_JUMP (old path gone)");
  Check(CountInFunction(*func, "@__remill_jump") == 0, "no __remill_jump call (retargeted)");

  // Scalarize the flat function in-process (inliner + mem2reg + SROA).
  auto scalarized = ScalarizeFlatFunction(module.get(), func);
  Check(scalarized != nullptr, "ScalarizeFlatFunction returned a function");
  if (!scalarized) {
    return 1;
  }

  std::cout << "\n=== Post-scalarize invariants (in-process SROA) ===\n";
  Check(CountInFunction(*scalarized, "STATE_LOCAL") == 0, "STATE_LOCAL scalarized (0 refs)");
  Check(CountAlloca(*scalarized) == 0, "no allocas remain");
  Check(CountInFunction(*scalarized, "memcpy") == 0, "no memcpy");
  Check(CountInFunction(*scalarized, "__remill_flat_jump") > 0,
        "__remill_flat_jump still present");

  // Save the scalarized IR.
  {
    std::error_code ec;
    llvm::raw_fd_ostream out(outpath, ec);
    module->print(out, nullptr);
    out.close();
  }
  std::cout << "\nSaved scalarized IR to " << outpath << "\n";

  std::cout << "\n" << (g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED")
            << " (" << g_failures << " failure(s))\n";
  return g_failures == 0 ? 0 : 1;
}
