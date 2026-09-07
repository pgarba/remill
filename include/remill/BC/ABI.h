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
  kFlatNumRegs = 17 + 4 + 7 + 8 + 16 + 8,  // GPRs+seg+flags+MMX+XMM+X87
  kNumFlatBlockArgs = 3 + (17 + 4 + 7 + 8 + 16 + 8)  // pc + mem + next_pc + regs
};

// Register index → LLVM type helper.
// 0-16: GPRs (17) → i64
// 17-20: seg bases (4) → i64
// 21-27: flags (7) → i8
// 28-35: MMX (8) → i64
// 36-51: XMM (16) → <2 x i64>
// 52-59: X87 ST(0-7) (8) → i128
inline bool FlatRegIsFlag(size_t idx) { return idx >= 21 && idx < 28; }
inline bool FlatRegIsXMM(size_t idx) { return idx >= 36 && idx < 52; }
inline bool FlatRegIsX87(size_t idx) { return idx >= 52 && idx < 60; }
// RSP is at index 6 (0=RAX,1=RBX,2=RCX,3=RDX,4=RSI,5=RDI,6=RSP).
// RBP is at index 7. Both are passed as ptr (pointers into the stack buffer).
inline constexpr size_t kFlatRSPIndex = 6;
inline constexpr size_t kFlatRBPIndex = 7;
inline constexpr size_t kFlatRIPIndex = 16;  // 0=RAX,...,15=R15,16=RIP
inline bool FlatRegIsRSP(size_t idx) { return idx == kFlatRSPIndex; }
inline bool FlatRegIsRBP(size_t idx) { return idx == kFlatRBPIndex; }
inline bool FlatRegIsStackPtr(size_t idx) { return FlatRegIsRSP(idx) || FlatRegIsRBP(idx); }

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
