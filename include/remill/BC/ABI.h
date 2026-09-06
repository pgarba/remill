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

#pragma once

#include <string_view>

namespace llvm {
class Function;
}  // namespace llvm
namespace remill {

// Describes the arguments to a basic block function.
enum : size_t {
  kStatePointerArgNum = 0,
  kPCArgNum = 1,
  kMemoryPointerArgNum = 2,
  kNumBlockArgs = 3
};

// Describes the arguments to a flat-mode lifted function. `pc` and `next_pc`
// are passed as in-out pointers; `memory` is passed by value; every
// architectural register is passed as an in-out pointer (a reference). The
// register arguments start at `kFlatFirstRegArgNum` and follow the canonical
// order defined by `kFlatRegNames` (see `lib/Arch/X86/Arch.cpp`).
//
// Pure-SSA mode: the pointer args ARE the register addresses. No state struct,
// no GEPs, no SROA. The ISEL `_flat` wrappers (produced by flat-gen) take the
// same register pointers and operate on them directly.
enum : size_t {
  kFlatPCArgNum = 0,             // addr_t *pc
  kFlatMemoryPointerArgNum = 1,  // Memory *memory (by value)
  kFlatNextPCArgNum = 2,         // addr_t *next_pc
  kFlatFirstRegArgNum = 3,       // first register pointer
  kFlatNumRegs = 17 + 4 + 7 + 8 + 16,  // GPRs + seg bases + flags + MMX + XMM
  kNumFlatBlockArgs = 3 + (17 + 4 + 7 + 8 + 16)  // pc + mem + next_pc + regs
};

extern const std::string_view kMemoryVariableName;
extern const std::string_view kStateVariableName;
extern const std::string_view kPCVariableName;
extern const std::string_view kNextPCVariableName;
extern const std::string_view kReturnPCVariableName;
extern const std::string_view kBranchTakenVariableName;

extern const std::string_view kInvalidInstructionISelName;
extern const std::string_view kUnsupportedInstructionISelName;
extern const std::string_view kIgnoreNextPCVariableName;

}  // namespace remill
