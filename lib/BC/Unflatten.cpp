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

// The unflatten pass: transform the merged+linked flat block module (one
// function per machine basic block, 63-arg by-value register file, control
// flow externalized through NEXT_PC + exit tail calls) into a single
// function with a real CFG. Design: flat/plan_unflatten.md.
//
// Per machine block the old block function has the shape
//
//   entry:  <straight-line body>; <pc bookkeeping>; br (or br i1) stub
//   stubN:  <state loads>; <exit tail call>; br common.ret
//   common.ret: ret <exit tail call result>
//
// The new function has, per machine block:
//
//   bb_P:    <60-wide phi farm>; <body, pc bookkeeping deleted, PC values
//            folded to constants>; br (or br i1) bb_P.stubN
//   bb_P.stubN: <state loads>; [call @__remill_dynamic_dispatch for
//            dynamic exits]; <br to the successor bb / ret / unreachable>
//
// The edge table (edges.json, from tools/flat_edges.py) is the source of
// truth for the successors; the old module's tail calls are ignored as
// control flow (they are wrong wherever mid-block NEXT_PC dataflow was
// corrupted by a dynamic call).

#include "remill/BC/Unflatten.h"

#include "remill/BC/ABI.h"
#include "remill/BC/Util.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/DCE.h"

#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

namespace remill {

namespace {

constexpr size_t kNumState = kFlatNumRegs;  // 60 architectural state values

bool ParseHexU64(const std::string &s, uint64_t *out) {
  if (s.empty()) {
    return false;
  }
  char *end = nullptr;
  const auto v = std::strtoull(s.c_str(), &end, 16);
  if (end == s.c_str() || *end != '\0') {
    return false;
  }
  *out = v;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Edge table JSON (tools/flat_edges.py).
// ---------------------------------------------------------------------------

namespace {

std::string HexU64(uint64_t v) {
  char buf[20];
  snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
  return buf;
}

std::optional<uint64_t> JsonHex(const json::Object &o, StringRef key) {
  if (auto s = o.getString(key)) {
    uint64_t u64 = 0;
    if (ParseHexU64(s->str(), &u64)) {
      return u64;
    }
    return std::nullopt;
  }
  if (auto num = o.getNumber(key)) {
    return static_cast<uint64_t>(*num);
  }
  return std::nullopt;
}

}  // namespace

bool FlatEdgeTable::Load(const std::string &path, FlatEdgeTable *out,
                         std::string *err) {
  auto buffer_or = MemoryBuffer::getFile(path);
  if (!buffer_or) {
    *err = "cannot read edge table " + path + ": could not open file";
    return false;
  }
  auto buffer = std::move(buffer_or.get());
  std::string text(buffer->getBuffer());
  auto value = json::parse(text);
  if (!value) {
    *err = "cannot parse edge table " + path + ": " +
           llvm::toString(value.takeError());
    return false;
  }
  const auto *root = value->getAsObject();
  if (!root) {
    *err = "edge table " + path + ": top level is not an object";
    return false;
  }
  for (const auto &kv : *root) {
    const auto key = StringRef(kv.first);
    uint64_t start = 0;
    if (!ParseHexU64(key.str(), &start)) {
      *err = "edge table: bad block key '" + key.str() + "'";
      return false;
    }
    const auto *entry = kv.second.getAsObject();
    if (!entry) {
      *err = "edge table: entry for block 0x" + HexU64(start) +
             " is not an object";
      return false;
    }
    FlatEdge e;
    e.start_pc = start;
    if (auto end = JsonHex(*entry, "end")) {
      e.end_pc = *end;
    }
    const auto kind_str = entry->getString("kind");
    const auto kind = kind_str ? kind_str->str() : std::string();
    if (kind == "jmp") {
      e.kind = FlatEdge::Kind::jmp;
      if (auto t = JsonHex(*entry, "target")) {
        e.target[0] = *t;
      }
    } else if (kind == "jcc") {
      e.kind = FlatEdge::Kind::jcc;
      if (auto t = JsonHex(*entry, "taken")) {
        e.target[0] = *t;
      }
      if (auto t = JsonHex(*entry, "notaken")) {
        e.target[1] = *t;
      }
    } else if (kind == "call") {
      e.kind = FlatEdge::Kind::call;
      if (auto r = JsonHex(*entry, "ret")) {
        e.target[0] = *r;
      }
      if (auto t = JsonHex(*entry, "target")) {
        e.call_target = *t;
      }
    } else if (kind == "ret") {
      e.kind = FlatEdge::Kind::ret;
    } else if (kind == "indjmp") {
      e.kind = FlatEdge::Kind::indjmp;
    } else {
      e.kind = FlatEdge::Kind::other;
    }
    if (const auto *lifted = entry->getArray("lifted")) {
      for (size_t i = 0; i < lifted->size() && i < 2; ++i) {
        if (auto b = (*lifted)[i].getAsBoolean()) {
          e.lifted[i] = *b;
        }
      }
    }
    out->edges[start] = std::move(e);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Per-block anatomy and emission state.
// ---------------------------------------------------------------------------

namespace {

struct BlockInfo {
  uint64_t pc = 0;
  Function *fn = nullptr;
  const FlatEdge *edge = nullptr;

  // New-function targets.
  BasicBlock *new_bb = nullptr;
  std::vector<PHINode *> phis;  // kNumState phis at the top of new_bb

  // Old-function anatomy.
  BasicBlock *body_bb = nullptr;  // the single straight-line body BB
  BranchInst *split_br = nullptr;  // the jcc `br i1` (null for 1 arm)

  struct Stub {
    BasicBlock *bb = nullptr;  // old BB containing the exit tail call
    CallInst *call = nullptr;  // the exit tail call (63 operands)
    // Normalized state vector (kNumState) from the exit call: RSP/RBP
    // unwrapped to ptr. The RIP slot is a PC expression and is never
    // cloned; it is replaced by constants from the edge table.
    std::vector<Value *> state;
    // New stub BB, the state values feeding the successor phis, and the
    // lifted successor (null when dynamic / unlifted).
    BasicBlock *new_bb = nullptr;
    std::vector<Value *> succ_state;
    BasicBlock *succ_bb = nullptr;
    // True when this stub is emitted inline at the end of the body BB
    // (merged layout); then new_bb is null.
    bool in_body = false;
  };
  std::vector<Stub> stubs;

  // call-kind blocks: the mid-body dynamic function call being rewritten.
  CallInst *dyn_call = nullptr;

  // True when the exit tail call lives in the body BB itself (the "merged"
  // layout: body, stub and exit are one straight-line BB).
  bool merged = false;
};

struct UnflattenState {
  Module *module = nullptr;
  Function *new_fn = nullptr;
  BasicBlock *fn_entry = nullptr;
  // Shared runtime-join block (plan D5): every dynamic / off-manifest exit
  // whose continuation is not statically known dispatches here. It threads
  // the dispatch call's updated state (60 phis) and returns the memory
  // pointer, so the function is a real, returning function rather than a
  // trap. The real Saturn runtime branches to the resolved continuation
  // instead of returning.
  BasicBlock *dyn_join = nullptr;
  // Per dead-end dispatch exit: the stub BB that branches to dyn_join, and
  // where its post-dispatch state lives (blocks[src_pc].stubs[arm]).
  struct DynJoinPred {
    BasicBlock *inc_bb = nullptr;
    uint64_t src_pc = 0;
    size_t arm = 0;
    Value *target = nullptr;  // dynamic target PC (drives the join's switch)
  };
  std::vector<DynJoinPred> dyn_join_preds;
  StructType *state_tuple = nullptr;
  FunctionCallee dispatch;  // @__remill_dynamic_dispatch
  std::map<uint64_t, BlockInfo> blocks;
  std::map<BasicBlock *, uint64_t> pc_of;  // new body bb -> block pc
  // Continuation of unlifted targets (optional cold sidecar).
  std::map<uint64_t, uint64_t> cold_succ;
  std::string err;
};

bool Fail(UnflattenState &st, Twine msg) {
  st.err = "Unflatten: " + msg.str();
  llvm::errs() << st.err << "\n";
  return false;
}

bool BlockPc(const Function &fn, uint64_t *pc) {
  const auto name = fn.getName();
  if (!name.starts_with("sub_") || name.contains('.')) {
    return false;  // skips trampoline leftovers like sub_8a80.1
  }
  return ParseHexU64(name.substr(4).str(), pc);
}

// Resolve a PC expression to a constant: an i64 constant, a load of the
// PC or NEXT_PC cell (resolved through the nearest preceding store in the
// block), or arithmetic / selects / truncations over such values. Port of
// ResolvePcLoad (Util.cpp) with per-block entry pcs.
Value *PcCellOf(const Function &fn, Value *ptr) {
  // Canonical cell: strip zero-offset GEPs.
  while (auto *gep = dyn_cast<GetElementPtrInst>(ptr)) {
    bool all_zero = true;
    for (unsigned i = 1; i < gep->getNumOperands(); ++i) {
      auto *off = dyn_cast<ConstantInt>(gep->getOperand(i));
      if (!off || !off->isZero()) {
        all_zero = false;
        break;
      }
    }
    if (!all_zero) {
      return nullptr;
    }
    ptr = gep->getOperand(0);
  }
  if (ptr == fn.getArg(kFlatPCArgNum)) {
    return fn.getArg(kFlatPCArgNum);
  }
  if (ptr == fn.getArg(kFlatNextPCArgNum)) {
    return fn.getArg(kFlatNextPCArgNum);
  }
  return nullptr;
}

bool ResolvePcConst(const Function &fn, Value *v, uint64_t entry,
                    unsigned depth, uint64_t *out) {
  if (depth > 24) {
    return false;
  }
  if (auto *ci = dyn_cast<ConstantInt>(v)) {
    *out = ci->getZExtValue();
    return true;
  }
  if (auto *ld = dyn_cast<LoadInst>(v)) {
    auto *cell = PcCellOf(fn, ld->getPointerOperand());
    if (!cell) {
      return false;
    }
    auto *bb = ld->getParent();
    // Nearest preceding store to the same cell in this block.
    for (auto it = std::prev(bb->getFirstInsertionPt()); &*it != ld; --it) {
      auto *i = cast<Instruction>(&*it);
      if (auto *store = dyn_cast<StoreInst>(i)) {
        if (PcCellOf(fn, store->getPointerOperand()) == cell) {
          return ResolvePcConst(fn, store->getValueOperand(), entry,
                                depth + 1, out);
        }
      }
      if (i->isTerminator()) {
        break;
      }
    }
    // No store: the dispatcher seeded both cells with the entry pc.
    if (bb == &fn.getEntryBlock()) {
      *out = entry;
      return true;
    }
    return false;
  }
  if (auto *bin = dyn_cast<BinaryOperator>(v)) {
    uint64_t a = 0, b = 0;
    if (ResolvePcConst(fn, bin->getOperand(0), entry, depth + 1, &a) &&
        ResolvePcConst(fn, bin->getOperand(1), entry, depth + 1, &b)) {
      switch (bin->getOpcode()) {
        case Instruction::Add: *out = a + b; return true;
        case Instruction::Sub: *out = a - b; return true;
        case Instruction::Or:  *out = a | b; return true;
        case Instruction::And: *out = a & b; return true;
        case Instruction::Xor: *out = a ^ b; return true;
        default: return false;
      }
    }
    return false;
  }
  if (auto *sel = dyn_cast<SelectInst>(v)) {
    uint64_t t = 0, f = 0;
    if (ResolvePcConst(fn, sel->getTrueValue(), entry, depth + 1, &t) &&
        ResolvePcConst(fn, sel->getFalseValue(), entry, depth + 1, &f)) {
      if (t == f) {
        *out = t;
        return true;
      }
      if (auto *c = dyn_cast<ConstantInt>(sel->getCondition())) {
        *out = c->isOne() ? t : f;
        return true;
      }
    }
    return false;
  }
  if (auto *cast = dyn_cast<CastInst>(v)) {
    switch (cast->getOpcode()) {
      case Instruction::ZExt:
      case Instruction::SExt:
      case Instruction::Trunc:
        return ResolvePcConst(fn, cast->getOperand(0), entry, depth + 1,
                              out);
      case Instruction::PtrToInt: {
        if (auto *itp = dyn_cast<IntToPtrInst>(cast->getOperand(0))) {
          return ResolvePcConst(fn, itp->getOperand(0), entry, depth + 1,
                                out);
        }
        return false;
      }
      default:
        return false;
    }
  }
  return false;
}

bool IsPcBookkeeping(const Function &fn, Value *v) {
  auto *inst = dyn_cast<Instruction>(v);
  if (!inst) {
    return false;
  }
  auto *pc_arg = fn.getArg(kFlatPCArgNum);
  auto *next_pc_arg = fn.getArg(kFlatNextPCArgNum);
  if (auto *load = dyn_cast<LoadInst>(inst)) {
    return load->getPointerOperand() == pc_arg ||
           load->getPointerOperand() == next_pc_arg;
  }
  if (auto *store = dyn_cast<StoreInst>(inst)) {
    return store->getPointerOperand() == pc_arg ||
           store->getPointerOperand() == next_pc_arg;
  }
  return false;
}

// Classify one old block function.
bool AnalyzeBlock(UnflattenState &st, BlockInfo &b) {
  auto &fn = *b.fn;
  const std::string where = "sub_" + HexU64(b.pc);

  // 1. Find the exit tail calls.
  std::vector<CallInst *> exit_calls;
  std::set<BasicBlock *> stub_bbs;
  for (auto &bb : fn) {
    for (auto &inst : bb) {
      auto *call = dyn_cast<CallInst>(&inst);
      if (!call || !call->isTailCall()) {
        continue;
      }
      auto *cf = dyn_cast<Function>(call->getCalledOperand());
      if (!cf) {
        continue;
      }
      uint64_t tpc = 0;
      if (cf->getName() != "__remill_flat_jump" && !BlockPc(*cf, &tpc)) {
        continue;
      }
      if (call->arg_size() != kNumFlatBlockArgs) {
        return Fail(st, Twine(where) + " exit call has " +
                            Twine(call->arg_size()) + " operands");
      }
      exit_calls.push_back(call);
      stub_bbs.insert(call->getParent());
    }
  }
  if (exit_calls.empty()) {
    return Fail(st, Twine(where) + ": no exit tail call found");
  }

  // 2. Body BB = the first BB. Two layouts:
  //    - merged: the exit tail call lives in the body BB itself (body,
  //      stub and exit are one straight-line BB ending in `ret`);
  //    - split:  the body ends in a br (or br i1) to arm BB(s), each
  //      holding one exit tail call followed by a br to the common.ret
  //      BB.
  b.body_bb = &fn.getEntryBlock();
  b.merged = stub_bbs.count(b.body_bb) != 0;
  if (b.merged) {
    if (exit_calls.size() != 1) {
      return Fail(st, Twine(where) + ": merged layout must have exactly "
                            "one exit call, found " +
                            Twine(exit_calls.size()));
    }
  } else {
    auto *br = dyn_cast<BranchInst>(b.body_bb->getTerminator());
    if (!br) {
      return Fail(st, Twine(where) +
                        ": body BB must end in a br to stub arm(s) or "
                        "contain the exit call");
    }
    for (unsigned i = 0; i < br->getNumSuccessors(); ++i) {
      if (!stub_bbs.count(br->getSuccessor(i))) {
        return Fail(st, Twine(where) + ": br successor " +
                            br->getSuccessor(i)->getName().str() +
                            " is not an exit stub");
      }
    }
    if (exit_calls.size() != br->getNumSuccessors()) {
      return Fail(st, Twine(where) + ": " +
                        Twine(br->getNumSuccessors()) +
                        " stub arm(s) but " + Twine(exit_calls.size()) +
                        " exit tail call(s)");
    }
    if (br->isConditional()) {
      b.split_br = br;
    }
  }

  // 3. jcc (split layout): verify the arm mapping.
  //    Convention (FlatExitTargetPc): the split block stores
  //    `select cond, taken, nottaken` to NEXT_PC, then `br i1 cond`; the
  //    successor bound to the select's true value is the taken arm.
  if (b.edge->kind == FlatEdge::Kind::jcc && b.split_br) {
    auto *bb = b.body_bb;
    auto *br = b.split_br;
    Value *stored = nullptr;
    for (auto it = std::prev(bb->getFirstInsertionPt()); &*it != br; --it) {
      if (auto *store = dyn_cast<StoreInst>(&*it)) {
        if (store->getPointerOperand() == fn.getArg(kFlatNextPCArgNum)) {
          stored = store->getValueOperand();
        }
      }
    }
    if (auto *sel = stored ? dyn_cast<SelectInst>(stored) : nullptr) {
      if (sel->getCondition() == br->getCondition()) {
        uint64_t tv = 0, fv = 0;
        if (ResolvePcConst(fn, sel->getTrueValue(), b.pc, 0, &tv) &&
            tv != b.edge->target[0]) {
          return Fail(st, Twine(where) + ": taken arm resolves to 0x" +
                              HexU64(tv) + ", edge table says 0x" +
                              HexU64(b.edge->target[0]));
        }
        if (ResolvePcConst(fn, sel->getFalseValue(), b.pc, 0, &fv) &&
            fv != b.edge->target[1]) {
          return Fail(st, Twine(where) + ": not-taken arm resolves to 0x" +
                              HexU64(fv) + ", edge table says 0x" +
                              HexU64(b.edge->target[1]));
        }
      }
    }
  }

  // 4. Build the stubs with their normalized state vectors.
  //    Exit calls are collected in BB order, which matches the split-br
  //    arm order (taken arm first).
  b.stubs.resize(exit_calls.size());
  for (size_t idx = 0; idx < exit_calls.size(); ++idx) {
    auto *call = exit_calls[idx];
    auto &s = b.stubs[idx];
    if (s.bb) {
      return Fail(st, Twine(where) + ": two exit calls in one stub BB");
    }
    s.bb = call->getParent();
    s.in_body = b.merged;
    s.call = call;
    s.state.resize(kNumState);
    for (size_t i = 0; i < kNumState; ++i) {
      Value *v = call->getOperand(kFlatFirstRegArgNum + i);
      if (FlatRegIsStackPtr(i)) {
        if (auto *p2i = dyn_cast<PtrToIntInst>(v)) {
          v = p2i->getOperand(0);
        }
        // RSP/RBP are ptr in the unflattened ABI. Most exits pass a ptr
        // directly; a few (X87 state blocks) pass the i64 form, which
        // EmitStub converts with inttoptr.
        if (!isa<PointerType>(v->getType()) && !isa<IntegerType>(v->getType())) {
          return Fail(st, Twine(where) + ": slot " + Twine(i) +
                              " of the exit vector is not a pointer or i64");
        }
      }
      s.state[i] = v;
    }
  }

  // 5. call-kind blocks: locate the mid-body dynamic function call
  //    (`@__remill_function_call` or a 3-arg devirt dispatch).
  if (b.edge->kind == FlatEdge::Kind::call) {
    for (auto &inst : *b.body_bb) {
      auto *call = dyn_cast<CallInst>(&inst);
      if (!call || call->isTailCall() || call->arg_size() != 3) {
        continue;
      }
      auto *cf = dyn_cast<Function>(call->getCalledOperand());
      if (!cf) {
        continue;
      }
      const auto name = cf->getName();
      if (name == "__remill_function_call" ||
          (name.starts_with("sub_") &&
           cf->getFunctionType()->getNumParams() == 3)) {
        b.dyn_call = call;
      }
    }
    if (!b.dyn_call) {
      return Fail(st, Twine(where) +
                          ": call-kind block without a 3-arg dynamic call");
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Cloning.
// ---------------------------------------------------------------------------

struct CloneCtx {
  using VMap = DenseMap<Value *, Value *>;
  UnflattenState &st;
  BlockInfo &b;
  VMap vm;
  // PC-related instructions whose value folds to a constant (ConstantInt
  // for i64 values, ConstantExpr for inttoptrs).
  std::map<Instruction *, Value *> fold;
  // Bookkeeping instructions deleted outright.
  std::set<Instruction *> dead;
  std::string err;
};

Instruction *CloneInto(CloneCtx &c, BasicBlock *dst, Instruction *src);

Value *CloneOperand(CloneCtx &c, BasicBlock *dst, Value *op) {
  if (auto *inst = dyn_cast<Instruction>(op)) {
    if (auto fit = c.fold.find(inst); fit != c.fold.end()) {
      c.vm[inst] = fit->second;
      return fit->second;
    }
    return CloneInto(c, dst, inst);
  }
  auto *mapped = c.vm.lookup(op);
  if (mapped) {
    return mapped;
  }
  if (isa<Argument>(op)) {
    c.err = "unmapped argument in clone";
    return nullptr;
  }
  return op;  // constants, globals, etc.
}

Instruction *CloneInto(CloneCtx &c, BasicBlock *dst, Instruction *src) {
  if (auto *mapped = c.vm.lookup(src)) {
    return cast<Instruction>(mapped);
  }
  if (c.dead.count(src)) {
    std::string refs;
    for (const auto &U : src->uses()) {
      auto *ui = dyn_cast<Instruction>(U.getUser());
      if (ui && c.dead.count(ui) == 0) {
        refs += " <- " + Twine(ui->getName()).str();
      }
    }
    c.err = ("sub_" + HexU64(c.b.pc) +
             ": dead bookkeeping still referenced: " +
             Twine(src->getName()) + " (" + Twine(src->getOpcodeName()) +
             ") parent=" + Twine(src->getParent()
                                 ? src->getParent()->getName() : "<none>") +
             refs)
                .str();
    return nullptr;
  }
  // An inttoptr of a tracked PC folds its operand to a constant.
  auto fit = c.fold.find(src);
  for (unsigned i = 0; i < src->getNumOperands(); ++i) {
    Value *nv;
    if (fit != c.fold.end() && i == 0) {
      nv = fit->second;
    } else {
      nv = CloneOperand(c, dst, src->getOperand(i));
    }
    if (c.err.empty()) {
      c.vm[src->getOperand(i)] = nv;
    } else {
      return nullptr;
    }
  }
  // This LLVM build's Cloning.h does not expose CloneInstruction; clone() is
  // available and preserves all instruction-level flags. Remap operands.
  Instruction *cl = src->clone();
  for (unsigned i = 0; i < src->getNumOperands(); ++i) {
    Value *op = src->getOperand(i);
    const auto it = c.vm.find(op);
    cl->setOperand(i, it == c.vm.end() ? op : it->second);
  }
  // Devirt dispatch: the lifter emits mid-block dynamic calls as 3-arg calls
  // to the *block* name (e.g. @sub_8a80), but in the merged module that name
  // carries the 63-arg block signature, making the 3-arg call type-invalid.
  // Retarget to the canonical 3-arg dynamic-call helper.
  if (auto *call = dyn_cast<CallInst>(cl)) {
    if (call->arg_size() == 3 && !call->isTailCall()) {
      auto *cf = dyn_cast<Function>(call->getCalledOperand());
      if (cf && cf->getName().starts_with("sub_") &&
          cf->getFunctionType()->getNumParams() != 3) {
        if (auto *fcall =
                c.st.module->getFunction("__remill_function_call")) {
          call->setCalledOperand(fcall);
        }
      }
    }
  }
  // Strip per-block metadata: alias scopes / noalias / initializes are
  // unsound once blocks share one function (plan D7).
  SmallVector<std::pair<unsigned, MDNode *>, 4> mds;
  src->getAllMetadata(mds);
  for (auto &md : mds) {
    cl->setMetadata(md.first, nullptr);
  }
  // The destination terminator is always created after all clones (by
  // CloneBody/EmitStub), so appending is safe.
  cl->insertInto(dst, InsertPosition(dst));
  c.vm[src] = cl;
  return cl;
}

// Compute the PC folds and the dead bookkeeping set for block B's body.
bool IsCommonRetBB(const BasicBlock &bb) {
  return bb.getTerminator() && isa<ReturnInst>(bb.getTerminator()) &&
         bb.getName() == "common.ret";
}

bool PrecomputePcFolds(CloneCtx &c, std::string *err) {
  auto &fn = *c.b.fn;
  auto &context = fn.getContext();
  const auto i64 = Type::getInt64Ty(context);

  // Resolve a value against the fold map built so far (program order is
  // forward-only, so one pass per block suffices).
  auto resolve = [&](Value *v, uint64_t *out) -> bool {
    if (auto *inst = dyn_cast<Instruction>(v)) {
      auto it = c.fold.find(inst);
      if (it != c.fold.end()) {
        if (auto *ci = dyn_cast<ConstantInt>(it->second)) {
          *out = ci->getZExtValue();
          return true;
        }
        // A folded constant pointer: fold it back through ptrtoint.
        if (auto *ce = dyn_cast<ConstantExpr>(it->second)) {
          if (ce->getOpcode() == Instruction::IntToPtr) {
            if (auto *src = dyn_cast<ConstantInt>(ce->getOperand(0))) {
              *out = src->getZExtValue();
              return true;
            }
          }
        }
      }
    }
    return ResolvePcConst(fn, v, c.b.pc, 0, out);
  };

  // 1. Fold every resolvable PC-related instruction: PC-cell loads,
  //    PC arithmetic / selects / casts, and the inttoptrs of PC values
  //    (stack bases). Unresolvable ones are left alone.
  for (auto &bb : fn) {
    if (IsCommonRetBB(bb)) {
      continue;
    }
    for (auto &inst : bb) {
      auto *i = cast<Instruction>(&inst);
      if (c.fold.count(i)) {
        continue;
      }
      if (auto *itp = dyn_cast<IntToPtrInst>(i)) {
        uint64_t pc = 0;
        if (resolve(itp->getOperand(0), &pc)) {
          c.fold[i] = ConstantExpr::getIntToPtr(ConstantInt::get(i64, pc),
                                                itp->getType());
        }
        continue;
      }
      if (auto *ld = dyn_cast<LoadInst>(i)) {
        if (PcCellOf(fn, ld->getPointerOperand())) {
          uint64_t pc = 0;
          if (ResolvePcConst(fn, ld, c.b.pc, 0, &pc)) {
            c.fold[i] = ConstantInt::get(i64, pc);
          }
        }
        continue;
      }
      // A select of two PC values: fold when the condition is constant or
      // the arms agree.
      if (auto *sel = dyn_cast<SelectInst>(i)) {
        if (!isa<IntegerType>(i->getType())) {
          continue;
        }
        uint64_t t = 0, f = 0;
        if (resolve(sel->getTrueValue(), &t) &&
            resolve(sel->getFalseValue(), &f)) {
          if (t == f) {
            c.fold[i] = ConstantInt::get(i64, t);
            continue;
          }
          if (auto *cc = dyn_cast<ConstantInt>(sel->getCondition())) {
            c.fold[i] =
                ConstantInt::get(i64, cc->isOne() ? t : f);
            continue;
          }
        }
        continue;
      }
      bool pcish = false;
      if (auto *bo = dyn_cast<BinaryOperator>(i)) {
        switch (bo->getOpcode()) {
          case Instruction::Add:
          case Instruction::Sub:
          case Instruction::Or:
          case Instruction::And:
          case Instruction::Xor:
            pcish = true;
            break;
          default:
            break;
        }
      } else if (auto *cst = dyn_cast<CastInst>(i)) {
        switch (cst->getOpcode()) {
          case Instruction::ZExt:
          case Instruction::SExt:
          case Instruction::Trunc:
          case Instruction::PtrToInt:
            pcish = true;
            break;
          default:
            break;
        }
      }
      if (!pcish || !isa<IntegerType>(i->getType())) {
        continue;
      }
      bool all = true;
      uint64_t val = 0;
      for (unsigned k = 0; k < i->getNumOperands(); ++k) {
        uint64_t o = 0;
        if (!resolve(i->getOperand(k), &o)) {
          all = false;
          break;
        }
        switch (i->getOpcode()) {
          case Instruction::Add: val += o; break;
          case Instruction::Sub: val -= o; break;
          case Instruction::Or:  val |= o; break;
          case Instruction::And: val &= o; break;
          case Instruction::Xor: val ^= o; break;
          case Instruction::ZExt:
          case Instruction::SExt:
          case Instruction::Trunc: val = o; break;
          case Instruction::PtrToInt: val = o; break;
          default: all = false; break;
        }
        if (!all) {
          break;
        }
      }
      if (!all) {
        continue;
      }
      c.fold[i] = ConstantInt::get(i64, val);
    }
  }

  // 2. Dead bookkeeping closure: PC/NEXT_PC loads/stores, the PC arithmetic
  //    and selects feeding them (or the folded inttoptrs), and nothing else
  //    live references.
  std::set<Instruction *> seed;
  for (auto &bb : fn) {
    if (IsCommonRetBB(bb)) {
      continue;
    }
    for (auto &inst : bb) {
      auto *i = cast<Instruction>(&inst);
      if (IsPcBookkeeping(fn, i) || c.fold.count(i)) {
        seed.insert(i);
      }
    }
  }
  auto ignorable_user = [&](Value *v) {
    if (v->use_empty()) {
      return true;
    }
    for (auto *u : v->users()) {
      auto *ui = dyn_cast<Instruction>(u);
      if (!ui || seed.count(ui) == 0) {
        return false;
      }
      if (IsPcBookkeeping(fn, ui) || c.fold.count(ui) != 0 ||
          (isa<CallInst>(ui) && cast<CallInst>(ui)->isTailCall()) ||
          ui == c.b.dyn_call) {
        continue;
      }
      return false;
    }
    return true;
  };
  bool changed = true;
  while (changed) {
    changed = false;
    std::vector<Instruction *> add;
    for (auto *i : seed) {
      for (unsigned k = 0; k < i->getNumOperands(); ++k) {
        auto *op = dyn_cast<Instruction>(i->getOperand(k));
        if (!op || seed.count(op)) {
          continue;
        }
        const bool pcish =
            IsPcBookkeeping(fn, op) || isa<SelectInst>(op) ||
            (isa<BinaryOperator>(op) &&
             (cast<BinaryOperator>(op)->getOpcode() == Instruction::Add ||
              cast<BinaryOperator>(op)->getOpcode() == Instruction::Or ||
              cast<BinaryOperator>(op)->getOpcode() == Instruction::And ||
              cast<BinaryOperator>(op)->getOpcode() == Instruction::Xor ||
              cast<BinaryOperator>(op)->getOpcode() == Instruction::Sub));
        if (pcish && ignorable_user(op)) {
          add.push_back(op);
          changed = true;
        }
      }
    }
    for (auto *op : add) {
      if (!seed.count(op)) {
        seed.insert(op);
      }
    }
  }
  for (auto *i : seed) {
    c.dead.insert(i);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Emission.
// ---------------------------------------------------------------------------

Value *EntrySlot(const UnflattenState &st, const BlockInfo &b, size_t slot) {
  if (!b.phis.empty()) {
    return b.phis[slot];
  }
  return st.new_fn->getArg(1 + slot);
}

Value *BuildStateTuple(IRBuilder<> &builder, StructType *tuple,
                       const std::vector<Value *> &vals) {
  Value *t = UndefValue::get(tuple);
  for (size_t i = 0; i < vals.size(); ++i) {
    t = builder.CreateInsertValue(t, vals[i], i);
  }
  return t;
}

bool IsExitCall(const BlockInfo &b, Instruction *i) {
  for (const auto &st : b.stubs) {
    if (st.call == i) {
      return true;
    }
  }
  return false;
}

// The terminal dynamic call (@__remill_function_call) is replaced by the
// dispatch-state call; the mid-block devirt dispatch (3-arg sub_8a80) is
// kept in place.
bool IsTerminalDynCall(const BlockInfo &b, Instruction *i) {
  auto *call = dyn_cast<CallInst>(i);
  if (!call || call != b.dyn_call) {
    return false;
  }
  auto *cf = dyn_cast<Function>(call->getCalledOperand());
  return cf && cf->getName() == "__remill_function_call";
}

bool CloneBody(CloneCtx &c, std::string *err) {
  auto &b = c.b;
  auto &fn = *b.fn;
  auto *dst = b.new_bb;
  auto &context = fn.getContext();

  // Remap the block arguments to the new function's values.
  c.vm[fn.getArg(kFlatMemoryPointerArgNum)] = c.st.new_fn->getArg(0);
  for (size_t i = 0; i < kNumState; ++i) {
    c.vm[fn.getArg(kFlatFirstRegArgNum + i)] = EntrySlot(c.st, b, i);
  }
  // The PC argument only appears in bookkeeping (dead) and, for devirt
  // blocks, as the identity operand of the kept 3-arg dispatch call.
  c.vm[fn.getArg(kFlatPCArgNum)] =
      ConstantExpr::getIntToPtr(
          ConstantInt::get(Type::getInt64Ty(context), b.pc),
          PointerType::getUnqual(context));

  // Clone the body.
  for (auto &inst : *b.body_bb) {
    auto *i = cast<Instruction>(&inst);
    if (i->isTerminator()) {
      continue;
    }
    if (c.dead.count(i) || IsExitCall(b, i) || IsTerminalDynCall(b, i)) {
      continue;
    }
    if (!CloneInto(c, dst, i)) {
      *err = c.err;
      return false;
    }
  }

  // Body terminator: split layout branches to the stub arm(s); the merged
  // layout falls through to EmitStub, which terminates the body in place.
  if (b.stubs.empty()) {
    *err = "sub_" + HexU64(b.pc) + ": no exit stubs to terminate with";
    return false;
  }
  if (!b.merged) {
    if (b.split_br) {
      auto *cond = c.vm.lookup(b.split_br->getCondition());
      if (!cond) {
        *err = "sub_" + HexU64(b.pc) + ": split br condition not cloned";
        return false;
      }
      BranchInst::Create(b.stubs[0].new_bb, b.stubs[1].new_bb, cond, dst);
    } else {
      BranchInst::Create(b.stubs[0].new_bb, dst);
    }
  }
  return true;
}

// The statically known successor pc of a stub (0 = none).
//
// Source of truth: the edge table. The IR's exit callee is used only for
// ret exits whose return address is statically known (the linker tail-called
// the return continuation).
uint64_t StubSuccPc(const BlockInfo &b, size_t arm) {
  const auto kind = b.edge->kind;
  const auto &stub = b.stubs[arm];
  auto *cf = dyn_cast<Function>(stub.call->getCalledOperand());
  const bool ir_dispatch = !cf || cf->getName() == "__remill_flat_jump";
  uint64_t ir_pc = 0;
  if (cf && !ir_dispatch) {
    BlockPc(*cf, &ir_pc);
  }
  switch (kind) {
    case FlatEdge::Kind::jmp:
    case FlatEdge::Kind::call:
    case FlatEdge::Kind::indjmp:
      return b.edge->target[0];
    case FlatEdge::Kind::jcc:
      if (b.merged) {
        // The IR dispatches on the condition at runtime; no static arm.
        return 0;
      }
      return b.edge->target[arm];
    case FlatEdge::Kind::ret:
      // A statically known return address, when the linker recorded one.
      return ir_dispatch ? 0 : ir_pc;
    default:
      return 0;
  }
}

// The new-function BB that carries a stub's outgoing edge (and thus feeds
// the successor's phi farm).
BasicBlock *StubOutBlock(const BlockInfo &b, size_t arm) {
  const auto &stub = b.stubs[arm];
  return stub.in_body ? b.new_bb : stub.new_bb;
}

bool EmitStub(CloneCtx &c, size_t arm, std::string *err) {
  auto &b = c.b;
  auto &s = b.stubs[arm];
  auto &fn = *b.fn;
  auto *dst = s.in_body ? b.new_bb : s.new_bb;
  auto &context = fn.getContext();
  const auto kind = b.edge->kind;
  const auto i64 = Type::getInt64Ty(context);

  auto *cf = dyn_cast<Function>(s.call->getCalledOperand());
  const bool ir_dispatch = !cf || cf->getName() == "__remill_flat_jump";

  // 1. Clone the state values the successor needs (all slots but RIP, which
  //    is a PC expression replaced by constants).
  s.succ_state.resize(kNumState);
  for (size_t i = 0; i < kNumState; ++i) {
    if (i == kFlatRIPIndex) {
      continue;
    }
    auto *v = CloneOperand(c, dst, s.state[i]);
    if (!v) {
      *err = "sub_" + HexU64(b.pc) + ": " + c.err;
      return false;
    }
    s.succ_state[i] = v;
  }

  // 1b. Normalize slot types to the unflattened ABI (RSP/RBP are ptr; a
  //     few X87 exits pass the i64 form).
  {
    IRBuilder<> tbuilder(dst);
    for (size_t i = 0; i < kNumState; ++i) {
      if (i == kFlatRIPIndex) {
        continue;  // filled after successor resolution
      }
      auto *expected =
          fn.getArg(kFlatFirstRegArgNum + i)->getType();
      auto *v = s.succ_state[i];
      if (v->getType() == expected) {
        continue;
      }
      if (isa<PointerType>(expected) &&
          isa<IntegerType>(v->getType())) {
        s.succ_state[i] = tbuilder.CreateIntToPtr(v, expected);
      } else if (isa<PointerType>(v->getType()) &&
                 isa<IntegerType>(expected)) {
        s.succ_state[i] = tbuilder.CreatePtrToInt(v, expected);
      } else {
        *err = ("sub_" + HexU64(b.pc) + ": slot " + Twine(i) +
                " has unexpected type")
                   .str();
        return false;
      }
    }
  }

  // 2. Successor resolution (edge table is the source of truth).
  const uint64_t succ_pc = StubSuccPc(b, arm);
  BasicBlock *succ = nullptr;
  if (succ_pc && c.st.blocks.count(succ_pc)) {
    succ = c.st.blocks[succ_pc].new_bb;
  }
  s.succ_bb = succ;

  IRBuilder<> builder(dst);

  // 3. Machine ret with a static return address: branch to it.
  if (kind == FlatEdge::Kind::ret && succ) {
    s.succ_state[kFlatRIPIndex] = ConstantInt::get(i64, succ_pc);
    BranchInst::Create(succ, dst);
    return true;
  }
  // Machine ret without a static return address: the flattened function
  // returns.
  if (kind == FlatEdge::Kind::ret) {
    builder.CreateRet(c.st.new_fn->getArg(0));
    return true;
  }

  // 4. Static lifted edge (non-dispatch exit): the successor's entry pc
  //    goes into the RIP slot and the CFG carries the edge.
  if (succ && !ir_dispatch) {
    s.succ_state[kFlatRIPIndex] = ConstantInt::get(i64, succ_pc);
    BranchInst::Create(succ, dst);
    return true;
  }

  // 5. Dynamic or unlifted exits: dispatch-state call (plan D5/D6).
  Value *target_val = nullptr;
  if (kind == FlatEdge::Kind::call && b.dyn_call) {
    uint64_t pc_const = 0;
    if (ResolvePcConst(fn, b.dyn_call->getOperand(1), b.pc, 0, &pc_const)) {
      target_val = ConstantInt::get(i64, pc_const);
    } else if (b.edge->call_target) {
      target_val = ConstantInt::get(i64, *b.edge->call_target);
    } else {
      target_val = c.vm.lookup(b.dyn_call->getOperand(1));
      if (!target_val) {
        *err = "sub_" + HexU64(b.pc) +
               ": dynamic call target not resolvable";
        return false;
      }
    }
  } else if (kind == FlatEdge::Kind::indjmp) {
    // The jump target (a register value); try to clone, else constant.
    target_val = CloneOperand(c, dst, s.state[kFlatRIPIndex]);
    if (!target_val) {
      uint64_t pc_const = 0;
      if (ResolvePcConst(fn, s.state[kFlatRIPIndex], b.pc, 0, &pc_const)) {
        target_val = ConstantInt::get(i64, pc_const);
      } else {
        target_val = PoisonValue::get(i64);
      }
    }
  } else if (succ_pc || b.edge->target[0]) {
    // Static but unlifted (or merged-jcc) target.
    target_val = ConstantInt::get(i64, b.edge->target[0] ? b.edge->target[0]
                                                         : succ_pc);
  } else {
    target_val = PoisonValue::get(i64);
  }

  // Pre-state: the stub vector, with the RIP slot resolved. For a call it
  // is the return address; otherwise a PC expression, or the dynamic
  // target register as the best available value.
  {
    uint64_t pc_const = 0;
    Value *pre_rip = nullptr;
    if (kind == FlatEdge::Kind::call && b.edge->target[0]) {
      pre_rip = ConstantInt::get(i64, b.edge->target[0]);
    } else if (ResolvePcConst(fn, s.state[kFlatRIPIndex], b.pc, 0,
                              &pc_const)) {
      pre_rip = ConstantInt::get(i64, pc_const);
    } else {
      pre_rip = target_val;
    }
    s.succ_state[kFlatRIPIndex] = pre_rip;
  }
  auto *result = builder.CreateCall(
      c.st.dispatch,
      {c.st.new_fn->getArg(0), target_val,
       BuildStateTuple(builder, c.st.state_tuple, s.succ_state)});

  // Successor state = the dispatch result (plan D6: full clobber).
  for (size_t i = 0; i < kNumState; ++i) {
    s.succ_state[i] = builder.CreateExtractValue(result, i);
  }
  if (succ) {
    // The CFG encodes the successor; pin the RIP slot to its entry pc.
    s.succ_state[kFlatRIPIndex] = ConstantInt::get(i64, succ_pc);
    BranchInst::Create(succ, dst);
    return true;
  }
  // Dynamic / off-manifest exit with no statically known continuation
  // (plan D5): the runtime resolves the target and returns the updated
  // state (the extractvalues above). Branch to the shared dyn_join, which
  // merges those states and returns. (v1: the runtime is a no-op stub, so
  // this returns instead of trapping; the real Saturn runtime would branch
  // to the resolved continuation.)
  BranchInst::Create(c.st.dyn_join, dst);
  c.st.dyn_join_preds.push_back({dst, b.pc, arm, target_val});
  return true;
}

}  // namespace

llvm::Function *UnflattenModule(Module &module, const FlatEdgeTable &table,
                                const std::string &new_name,
                                std::string *err, bool optimize) {
  UnflattenState st;
  st.module = &module;

  // 1. Collect and analyze the block functions.
  for (auto &fn : module) {
    if (fn.isDeclaration() || !fn.getName().starts_with("sub_")) {
      continue;
    }
    uint64_t pc = 0;
    if (!BlockPc(fn, &pc)) {
      continue;
    }
    if (fn.getFunctionType()->getNumParams() != kNumFlatBlockArgs) {
      if (!Fail(st, Twine(fn.getName()) +
                        " is not a 63-arg flat block function")) {
        return nullptr;
      }
    }
    auto e = table.edges.find(pc);
    if (e == table.edges.end()) {
      if (!Fail(st, Twine(fn.getName()) + " missing from the edge table")) {
        return nullptr;
      }
    }
    BlockInfo b;
    b.pc = pc;
    b.fn = &fn;
    b.edge = &e->second;
    if (!AnalyzeBlock(st, b)) {
      return nullptr;
    }
    st.blocks[pc] = std::move(b);
  }
  if (st.blocks.empty()) {
    return Fail(st, "no block functions found") ? nullptr : nullptr;
  }

  const uint64_t entry_pc = st.blocks.begin()->first;
  auto &context = module.getContext();
  auto &model = *st.blocks.begin()->second.fn;

  // 2. New function: (ptr %memory, <60 state values, block-ABI order>).
  std::vector<Type *> types;
  types.reserve(1 + kNumState);
  types.push_back(PointerType::getUnqual(context));
  for (size_t i = 0; i < kNumState; ++i) {
    types.push_back(model.getArg(kFlatFirstRegArgNum + i)->getType());
  }
  st.state_tuple =
      StructType::create(context, ArrayRef<Type *>(types.data() + 1, kNumState));
  auto *fn_type =
      FunctionType::get(PointerType::getUnqual(context), types, false);
  st.new_fn = Function::Create(fn_type, Function::ExternalLinkage, new_name,
                               &module);
  st.new_fn->getArg(0)->setName(
      model.getArg(kFlatMemoryPointerArgNum)->getName());
  for (size_t i = 0; i < kNumState; ++i) {
    st.new_fn->getArg(1 + i)->setName(
        model.getArg(kFlatFirstRegArgNum + i)->getName());
  }

  // @__remill_dynamic_dispatch: (ptr, i64, <60>) -> <60>.
  {
    auto *dispatch_type = FunctionType::get(
        st.state_tuple,
        {PointerType::getUnqual(context), Type::getInt64Ty(context),
         st.state_tuple},
        false);
    st.dispatch =
        module.getOrInsertFunction("__remill_dynamic_dispatch",
                                   dispatch_type);
  }

  // 3. Basic blocks: fn_entry, then per block a body BB and stub BBs.
  st.fn_entry = BasicBlock::Create(context, "fn_entry", st.new_fn);
  st.dyn_join = BasicBlock::Create(context, "dyn_join", st.new_fn);
  for (auto &kv : st.blocks) {
    char name[32];
    snprintf(name, sizeof(name), "bb_%llx",
             static_cast<unsigned long long>(kv.first));
    kv.second.new_bb = BasicBlock::Create(context, name, st.new_fn);
    st.pc_of[kv.second.new_bb] = kv.first;
  }
  for (auto &kv : st.blocks) {
    for (size_t i = 0; i < kv.second.stubs.size(); ++i) {
      auto &stub = kv.second.stubs[i];
      if (stub.in_body) {
        continue;  // merged layout: the stub is inlined in the body BB
      }
      char name[48];
      snprintf(name, sizeof(name), "bb_%llx.stub%u",
               static_cast<unsigned long long>(kv.first),
               static_cast<unsigned>(i));
      stub.new_bb = BasicBlock::Create(context, name, st.new_fn);
    }
  }
  {
    BasicBlock *entry_bb = nullptr;
    for (auto &kv : st.blocks) {
      if (kv.first == entry_pc) {
        entry_bb = kv.second.new_bb;
      }
    }
    BranchInst::Create(entry_bb, st.fn_entry);
  }

  // 4. Predecessors per block: static lifted edges + the function entry.
  //    (Per-stub, so the merged layout and the IR-vs-edge-table
  //    reconciliation both work.)
  struct Pred {
    bool from_entry;
    uint64_t src_pc;
    size_t arm;
  };
  std::map<uint64_t, std::vector<Pred>> preds_of;
  // Dynamic / off-manifest exits (non-ret, no static successor) branch to the
  // shared dyn_join. Count them so the phi farms can reserve a join incoming
  // (plan D5 re-entry).
  size_t num_dyn_join = 0;
  for (auto &kv : st.blocks) {
    const auto &b = kv.second;
    for (size_t arm = 0; arm < b.stubs.size(); ++arm) {
      const auto t = StubSuccPc(b, arm);
      if (!t || !st.blocks.count(t)) {
        if (b.edge->kind != FlatEdge::Kind::ret) {
          ++num_dyn_join;
        }
        continue;
      }
      preds_of[t].push_back(Pred{false, b.pc, arm});
    }
  }
  preds_of[entry_pc].push_back(Pred{true, 0, 0});
  const bool use_dyn_join = num_dyn_join > 0;

  // 5. Phi farms: one phi per state slot; incoming from the function entry
  //    (for the entry block) and one per static predecessor stub.
  for (auto &kv : st.blocks) {
    auto &b = kv.second;
    const auto &preds = preds_of[b.pc];
    for (size_t slot = 0; slot < kNumState; ++slot) {
      auto *phi = PHINode::Create(
          model.getArg(kFlatFirstRegArgNum + slot)->getType(),
          preds.size() + (use_dyn_join ? 1 : 0),
          model.getArg(kFlatFirstRegArgNum + slot)->getName(), b.new_bb);
      for (const auto &p : preds) {
        Value *incoming =
            p.from_entry ? st.new_fn->getArg(1 + slot)
                         : static_cast<Value *>(PoisonValue::get(phi->getType()));
        BasicBlock *inc_bb =
            p.from_entry ? st.fn_entry
                         : StubOutBlock(st.blocks[p.src_pc], p.arm);
        phi->addIncoming(incoming, inc_bb);
      }
      // dyn_join (via its re-entry switch) is a predecessor of every block;
      // its incoming is filled in step 6b with the join's state.
      if (use_dyn_join) {
        phi->addIncoming(PoisonValue::get(phi->getType()), st.dyn_join);
      }
      b.phis.push_back(phi);
    }
  }

  // 6. Clone the bodies and emit the stubs.
  for (auto &kv : st.blocks) {
    auto &b = kv.second;
    CloneCtx c{st, b, DenseMap<Value *, Value *>(), {}, {}, {}};
    std::string e;
    if (!PrecomputePcFolds(c, &e)) {
      *err = e;
      return nullptr;
    }
    if (!CloneBody(c, &e)) {
      *err = e;
      return nullptr;
    }
    for (size_t arm = 0; arm < b.stubs.size(); ++arm) {
      if (!EmitStub(c, arm, &e)) {
        *err = e;
        return nullptr;
      }
    }
    // Fill the successor phis' incoming values for this block's stubs.
    for (size_t arm = 0; arm < b.stubs.size(); ++arm) {
      auto &s = b.stubs[arm];
      if (!s.succ_bb) {
        continue;
      }
      const uint64_t succ_pc = st.pc_of[s.succ_bb];
      auto &succ = st.blocks[succ_pc];
      const auto *inc_bb =
          s.in_body ? b.new_bb : s.new_bb;
      for (size_t i = 0; i < kNumState; ++i) {
        auto *phi = succ.phis[i];
        for (unsigned inc = 0; inc < phi->getNumIncomingValues(); ++inc) {
          if (phi->getIncomingBlock(inc) == inc_bb) {
            phi->setIncomingValue(inc, s.succ_state[i]);
          }
        }
      }
    }
  }

  // 6b. Dynamic join block (plan D5 re-entry): a target-PC phi + 60 state
  //     phis (one incoming per dead-end dispatch exit) + a switch that
  //     re-enters at any lifted block. A call/indjmp could resume at any
  //     block, so this is a conservative (sound) model of dynamic re-entry;
  //     the no-op stub never actually re-enters at runtime, but making every
  //     block reachable from the join keeps the full CFG under an optimizer.
  //     The default case (target names no lifted block) returns the memory
  //     pointer, so the function is a real, returning function.
  if (use_dyn_join) {
    auto &jctx = st.new_fn->getContext();
    auto *j64 = Type::getInt64Ty(jctx);
    auto *target_phi = PHINode::Create(j64, st.dyn_join_preds.size(),
                                       "dyn_target", st.dyn_join);
    std::vector<PHINode *> join_state(kNumState);
    for (size_t slot = 0; slot < kNumState; ++slot) {
      join_state[slot] = PHINode::Create(
          model.getArg(kFlatFirstRegArgNum + slot)->getType(),
          st.dyn_join_preds.size(),
          model.getArg(kFlatFirstRegArgNum + slot)->getName(), st.dyn_join);
    }
    for (const auto &p : st.dyn_join_preds) {
      target_phi->addIncoming(p.target, p.inc_bb);
      for (size_t slot = 0; slot < kNumState; ++slot) {
        join_state[slot]->addIncoming(
            st.blocks[p.src_pc].stubs[p.arm].succ_state[slot], p.inc_bb);
      }
    }
    // Switch on the dynamic target PC to every lifted block's entry BB;
    // default (off-manifest / unknown target) returns the memory pointer.
    auto *dyn_default = BasicBlock::Create(jctx, "dyn_default", st.new_fn);
    auto *sw = SwitchInst::Create(target_phi, dyn_default,
                                  static_cast<unsigned>(st.blocks.size()),
                                  InsertPosition(st.dyn_join));
    for (auto &kv : st.blocks) {
      sw->addCase(ConstantInt::get(j64, kv.first), kv.second.new_bb);
    }
    IRBuilder<> jb(dyn_default);
    jb.CreateRet(st.new_fn->getArg(0));
    // Fill every block's dyn_join phi incoming with the join's state.
    for (auto &kv : st.blocks) {
      for (size_t slot = 0; slot < kNumState; ++slot) {
        auto *phi = kv.second.phis[slot];
        for (unsigned inc = 0; inc < phi->getNumIncomingValues(); ++inc) {
          if (phi->getIncomingBlock(inc) == st.dyn_join) {
            phi->setIncomingValue(inc, join_state[slot]);
          }
        }
      }
    }
  } else {
    // No dead-end exits: the join is unused; drop the empty BB.
    st.dyn_join->eraseFromParent();
    st.dyn_join = nullptr;
  }

  // 7. Retire the old block functions and DCE the leftovers (flat_pc_*
  //    cells, the dispatch stub, the 3-arg devirt declarations, ...).
  std::vector<Function *> old;
  for (auto &kv : st.blocks) {
    old.push_back(kv.second.fn);
  }
  for (auto *fn : old) {
    fn->deleteBody();
  }
  for (auto *fn : old) {
    fn->eraseFromParent();
  }
  // Trampoline leftovers (sub_<hex>.<n>): now dead.
  std::vector<Function *> trampolines;
  for (auto &fn : module) {
    if (fn.getName().starts_with("sub_") && !fn.isDeclaration() &&
        fn.getName().contains('.')) {
      trampolines.push_back(&fn);
    }
  }
  for (auto *fn : trampolines) {
    fn->deleteBody();
    fn->eraseFromParent();
  }
  // GlobalDCE to retire the now-dead per-block globals (flat_pc_*, the
  // dispatcher, unused 3-arg devirt declarations). Requires the full
  // analysis-manager stack registered through PassBuilder, mirroring
  // OptimizeModule (Util.cpp). A bare ModuleAnalysisManager segfaults.
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  {
    llvm::PassBuilder pb(nullptr, llvm::PipelineTuningOptions(),
                         std::nullopt, nullptr);
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::ModulePassManager mpm;
    mpm.addPass(llvm::GlobalDCEPass());
    mpm.run(module, mam);
  }

  // 7b. Optional conservative optimization (plan M5): instcombine + DCE on
  //     the new function. NO simplifycfg (fork bug: it mis-deletes around
  //     calls / trailing unreachables). This folds the dead per-instruction
  //     PC GEPs and constant PCs the flat envelope carried. The CFG is
  //     preserved; dead (off-manifest / cold) block bodies may empty out.
  if (optimize) {
    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::InstCombinePass());
    fpm.addPass(llvm::DCEPass());
    fpm.run(*st.new_fn, fam);
  }

  // 8. Verify.
  std::string verify_err;
  raw_string_ostream os(verify_err);
  if (verifyModule(module, &os)) {
    *err = "Unflatten: module failed verification:\n" + os.str();
    return nullptr;
  }
  return st.new_fn;
}

}  // namespace remill
