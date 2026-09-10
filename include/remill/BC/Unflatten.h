// Copyright (c) 2024 Saturn
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under an implied warranty of fitness for a particular
// purpose. See the License for the specific language governing
// permissions and limitations under the License.

#ifndef REMILL_BC_UNFLATTEN_H_
#define REMILL_BC_UNFLATTEN_H_

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

namespace remill {

// One machine basic block's terminal edge, as decoded from the block's raw
// bytes (tools/flat_edges.py). The edge table is the source of truth for
// control flow; the linked module's tail calls are only a hint (they are
// wrong whenever NEXT_PC dataflow was corrupted mid-block).
struct FlatEdge {
  enum class Kind { jmp, jcc, call, ret, indjmp, other };

  uint64_t start_pc = 0;
  uint64_t end_pc = 0;
  Kind kind = Kind::other;
  // Machine successors:
  //   jmp:    target[0]
  //   jcc:    target[0] = taken, target[1] = not-taken
  //   call:   target[0] = return address (the continuation block)
  //   ret:    (dynamic; target[0] = 0)
  //   indjmp: target[0] = statically known target, or 0 if unknown
  std::array<uint64_t, 2> target = {0, 0};
  // Per arm: is the target a lifted block in the module?
  std::array<bool, 2> lifted = {false, false};
  // kind == call: the called function's PC (statically known).
  std::optional<uint64_t> call_target;

  size_t num_arms() const {
    switch (kind) {
      case Kind::jcc:
        return 2;
      case Kind::jmp:
      case Kind::call:
      case Kind::indjmp:
        return 1;
      default:
        return 0;
    }
  }
};

// Machine edge table: start pc -> edge, for every block of the flattened
// function.
struct FlatEdgeTable {
  std::map<uint64_t, FlatEdge> edges;

  // Parse the JSON sidecar emitted by tools/flat_edges.py. Returns true on
  // success; on failure returns false and sets *err.
  static bool Load(const std::string &path, FlatEdgeTable *out, std::string *err);
};

// Unflatten a merged+linked flat block module into a single function with a
// real control-flow graph.
//
// The module must contain one block function per machine block, named
// `sub_<hex>` with the flat block ABI (63 by-value arguments: PC, memory,
// NEXT_PC, then the 60 architectural state values). Each block's exit is a
// tail call (to the successor block function or to @__remill_flat_jump).
//
// The result is a single function `@new_name` with the unflattened ABI:
// (ptr %memory, <60 state values, block-ABI order minus PC/NEXT_PC>). Every
// machine block becomes a basic block; static exits become real br edges;
// 60-wide phi farms merge state at join points; dynamic exits (indirect
// calls, indirect branches, ret, unlifted targets) become mid-function
// calls to the external @__remill_dynamic_dispatch followed by a br to
// the statically known continuation (or an unreachable terminator when the
// continuation is not statically known -- v1 limitation for the cold paths
// of quotearg, which the static test prefix never reaches).
//
// The original block functions are renamed `@sub_<hex>_dead` and their
// now-dead private globals are DCE'd; @__remill_flat_jump is left in place
// (unused) so the module stays loadable.
//
// When \p optimize is true, the new function is also run through a
// conservative instcombine + DCE pass (NO simplifycfg -- the fork's
// simplifycfg is known to mis-delete around calls/unreachables). This folds
// the dead per-instruction PC GEPs and constant PCs the flat envelope
// carried. The unflattened control-flow graph is preserved.
//
// Returns the new function, or null (with *err set) on failure.
llvm::Function *UnflattenModule(llvm::Module &module, const FlatEdgeTable &table,
                                const std::string &new_name, std::string *err,
                                bool optimize = false);

}  // namespace remill

#endif  // REMILL_BC_UNFLATTEN_H_
