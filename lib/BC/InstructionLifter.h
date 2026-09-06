/*
 * Copyright (c) 2017 Trail of Bits, Inc.
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

#include "remill/BC/Logging.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <remill/BC/InstructionLifter.h>

#include <functional>
#include <ios>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "remill/Arch/Arch.h"
#include "remill/Arch/Instruction.h"
#include "remill/Arch/Name.h"
#include "remill/BC/ABI.h"
#include "remill/BC/Compat/DataLayout.h"
#include "remill/BC/IntrinsicTable.h"
#include "remill/BC/Util.h"
#include "remill/OS/OS.h"

namespace remill {

class InstructionLifter::Impl {
 public:
  Impl(const Arch *arch_, const IntrinsicTable *intrinsics_);

  // Architecture being used for lifting.
  const Arch *const arch;

  // Set of intrinsics.
  const IntrinsicTable *const intrinsics;

  // Machine word type for this architecture.
  llvm::Type *const word_type;

  // Type of the memory pointer.
  llvm::Type *const memory_ptr_type;

  // Cache of looked up registers inside of `last_func`.
  std::unordered_map<std::string, std::pair<llvm::Value *, llvm::Type *>>
      reg_ptr_cache;

  // Cached load of the `STATE` variable for `last_func`. This is the state
  // pointer the ISEL code operates on: the `state` argument in the classic
  // ABI, or the local `State` alloca in flat mode.
  llvm::Value *state_ptr{nullptr};

  // The function into which we're lifting. If This gets out of date, we
  // clear out `reg_ptr_cache`.
  llvm::Function *last_func{nullptr};

  llvm::Module *const module;
  llvm::Function *const invalid_instruction;
  llvm::Function *const unsupported_instruction;

  // Pure-SSA flat mode: pointer args ARE register addresses. No state struct.
  bool flat{false};

  // Cache of flat register name → argument index.
  std::unordered_map<std::string, size_t> flat_reg_index;

  // Build the flat register name → index mapping.
  // Register names are UPPERCASE (matching the Arch register names).
  void InitFlatRegMap() {
    // 17 GPRs (matches kFlatRegs order in Arch.cpp).
    const char *gpr_names[] = {"RAX","RBX","RCX","RDX","RSI","RDI","RSP","RBP",
                               "R8","R9","R10","R11","R12","R13","R14","R15","RIP"};
    for (size_t i = 0; i < 17; ++i) flat_reg_index[gpr_names[i]] = i;

    // Sub-register aliases: all sub-registers map to the same pointer arg
    // as their full-size parent (the ISEL accesses the correct width through
    // the pointer).
    // RAX (0): EAX, AX, AL, AH
    flat_reg_index["EAX"] = 0; flat_reg_index["AX"] = 0;
    flat_reg_index["AL"] = 0; flat_reg_index["AH"] = 0;
    // RBX (1): EBX, BX, BL, BH
    flat_reg_index["EBX"] = 1; flat_reg_index["BX"] = 1;
    flat_reg_index["BL"] = 1; flat_reg_index["BH"] = 1;
    // RCX (2): ECX, CX, CL, CH
    flat_reg_index["ECX"] = 2; flat_reg_index["CX"] = 2;
    flat_reg_index["CL"] = 2; flat_reg_index["CH"] = 2;
    // RDX (3): EDX, DX, DL, DH
    flat_reg_index["EDX"] = 3; flat_reg_index["DX"] = 3;
    flat_reg_index["DL"] = 3; flat_reg_index["DH"] = 3;
    // RSI (4): ESI, SI
    flat_reg_index["ESI"] = 4; flat_reg_index["SI"] = 4;
    // RDI (5): EDI, DI
    flat_reg_index["EDI"] = 5; flat_reg_index["DI"] = 5;
    // RSP (6): ESP, SP
    flat_reg_index["ESP"] = 6; flat_reg_index["SP"] = 6;
    // RBP (7): EBP, BP
    flat_reg_index["EBP"] = 7; flat_reg_index["BP"] = 7;
    // R8-R15 (8-15): RnD, RnW, RnB
    for (int i = 8; i <= 15; ++i) {
      flat_reg_index["R" + std::to_string(i) + "D"] = i - 8;
      flat_reg_index["R" + std::to_string(i) + "W"] = i - 8;
      flat_reg_index["R" + std::to_string(i) + "B"] = i - 8;
    }
    // RIP (16): EIP
    flat_reg_index["EIP"] = 16;

    // 4 seg bases (names match Arch register names: no underscore).
    const char *seg_names[] = {"SSBASE","GSBASE","CSBASE","FSBASE"};
    for (size_t i = 0; i < 4; ++i) flat_reg_index[seg_names[i]] = 17 + i;
    // 7 flags.
    const char *flag_names[] = {"CF","PF","AF","ZF","SF","DF","OF"};
    for (size_t i = 0; i < 7; ++i) flat_reg_index[flag_names[i]] = 21 + i;
    // 8 MMX.
    for (int i = 0; i < 8; ++i) {
      std::string name = "MM" + std::to_string(i);
      flat_reg_index[name] = 28 + i;
    }
    // 16 XMM.
    for (int i = 0; i < 16; ++i) {
      std::string name = "XMM" + std::to_string(i);
      flat_reg_index[name] = 36 + i;
    }
  }
};

}  // namespace remill
