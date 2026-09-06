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

// Describes the arguments to a flat-mode lifted function (ABI v2: by-value).
// `pc` and `next_pc` are in-out pointers (threaded between blocks); `memory`
// is ptr; every architectural register is passed **by value** (i64 for GPRs/
// seg/MMX, i8 for flags, <2 x i64> for XMM). Internally, the lifter stores
// the by-value args into local allocas which are passed as pointers to the
// `_flat` ISEL wrappers. SROA promotes the allocas to SSA values.
enum : size_t {
  kFlatPCArgNum = 0,             // addr_t *pc (ptr, threaded)
  kFlatMemoryPointerArgNum = 1,  // Memory *memory (ptr)
  kFlatNextPCArgNum = 2,         // addr_t *next_pc (ptr, threaded)
  kFlatFirstRegArgNum = 3,       // first register value (by-value)
  kFlatNumRegs = 17 + 4 + 7 + 8 + 16,  // GPRs + seg bases + flags + MMX + XMM
  kNumFlatBlockArgs = 3 + (17 + 4 + 7 + 8 + 16)  // pc + mem + next_pc + regs
};

// Register index → LLVM type helper.
// 0-19: GPRs + seg bases → i64
// 20-26: flags → i8
// 27-34: MMX → i64
// 35-51: XMM → <2 x i64>
inline bool FlatRegIsFlag(size_t idx) { return idx >= 20 && idx < 27; }
inline bool FlatRegIsXMM(size_t idx) { return idx >= 35 && idx < 52; }

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
