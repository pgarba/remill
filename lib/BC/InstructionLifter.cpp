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

#include "InstructionLifter.h"

namespace remill {
namespace {

// Try to find the function that implements this semantics.
llvm::Function *GetInstructionFunction(llvm::Module *module,
                                       std::string_view function) {
  std::stringstream ss;
  ss << "ISEL_" << function;
  auto isel_name = ss.str();

  auto isel = FindGlobaVariable(module, isel_name);
  if (!isel) {
    return nullptr;  // Falls back on `UNIMPLEMENTED_INSTRUCTION`.
  }

  if (!isel->isConstant() || !isel->hasInitializer()) {
    assert(false);
  }

  auto sem = isel->getInitializer()->stripPointerCasts();
  return llvm::dyn_cast_or_null<llvm::Function>(sem);
}

// Try to find the flat wrapper for an ISEL function. The flat wrapper is
// named `<original_mangled_name>_flat` (created by flat-gen). We look up the
// original ISEL function first, then append "_flat" to its name.
llvm::Function *GetFlatInstructionFunction(llvm::Module *module,
                                           std::string_view function,
                                           llvm::Function *original_isel) {
  if (!original_isel) {
    return nullptr;
  }
  auto flat_name = original_isel->getName().str() + "_flat";
  if (auto *f = module->getFunction(flat_name)) {
    return f;
  }
  return nullptr;
}

}  // namespace

InstructionLifter::Impl::Impl(const Arch *arch_,
                              const IntrinsicTable *intrinsics_)
    : arch(arch_),
      intrinsics(intrinsics_),
      word_type(
          remill::NthArgument(intrinsics->async_hyper_call, remill::kPCArgNum)
              ->getType()),
      memory_ptr_type(remill::NthArgument(intrinsics->async_hyper_call,
                                          remill::kMemoryPointerArgNum)
                          ->getType()),
      module(intrinsics->async_hyper_call->getParent()),
      invalid_instruction(
          GetInstructionFunction(module, kInvalidInstructionISelName)),
      unsupported_instruction(
          GetInstructionFunction(module, kUnsupportedInstructionISelName)) {

  assert(invalid_instruction != nullptr);

  assert(unsupported_instruction != nullptr);
}

InstructionLifter::~InstructionLifter(void) {}

void InstructionLifter::SetFlatMode(bool flat) {
  impl->flat = flat;
  if (flat) {
    impl->InitFlatRegMap();
  }
}

InstructionLifter::InstructionLifter(const Arch *arch_,
                                     const IntrinsicTable *intrinsics_)
    : impl(new Impl(arch_, intrinsics_)) {}

// Lift a single instruction into a basic block. `is_delayed` signifies that
// this instruction will execute within the delay slot of another instruction.
LiftStatus InstructionLifter::LiftIntoBlock(Instruction &inst,
                                            llvm::BasicBlock *block,
                                            bool is_delayed,
                                            llvm::CallInst **CIInstruction) {
  return LiftIntoBlock(inst, block, GetStatePointer(block->getParent()),
                       is_delayed, CIInstruction);
}

// Return the state pointer for `func`.
//
// In flat mode the ISEL code operates on a local `State` alloca (named
// `STATE_LOCAL`) that the entry block materializes from the caller's flat
// state; passing that alloca directly lets `LoadRegAddress` place the register
// GEPs before it (so they dominate every use). In the classic ABI there is no
// such alloca, so we fall back to the `state` argument, which is available in
// every block.
llvm::Value *InstructionLifter::GetStatePointer(llvm::Function *func) const {
  if (func != impl->last_func) {
    impl->reg_ptr_cache.clear();
    impl->state_ptr = nullptr;
    impl->last_func = func;
  }

  if (!impl->state_ptr) {
    // Look for the flat-mode local `State` alloca in the entry block.
    for (auto &inst : func->getEntryBlock()) {
      if (auto alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
        if (alloca->getName() == "STATE_LOCAL") {
          impl->state_ptr = alloca;
          break;
        }
      }
    }
    if (!impl->state_ptr) {
      impl->state_ptr = NthArgument(func, kStatePointerArgNum);
    }
  }

  return impl->state_ptr;
}

// Lift a single instruction into a basic block.
LiftStatus InstructionLifter::LiftIntoBlock(Instruction &arch_inst,
                                            llvm::BasicBlock *block,
                                            llvm::Value *state_ptr,
                                            bool is_delayed,
                                            llvm::CallInst **CIInstruction) {

  llvm::Function *const func = block->getParent();
  llvm::Module *const module = func->getParent();
  llvm::Function *isel_func = nullptr;
  auto status = kLiftedInstruction;

  // Cache invalidation.
  if (func != impl->last_func) {
    impl->reg_ptr_cache.clear();
    impl->last_func = func;

    CHECK_EQ(impl->module, module)
        << "InstructionLifter isn't using the correct module!";
  }

  if (arch_inst.IsValid()) {
    isel_func = GetInstructionFunction(module, arch_inst.function);
  } else {
    isel_func = impl->invalid_instruction;
    arch_inst.operands.clear();
    status = kLiftedInvalidInstruction;
  }

  if (!isel_func) {
    LOG(ERROR) << "Missing semantics for instruction " << arch_inst.Serialize();
    isel_func = impl->unsupported_instruction;
    arch_inst.operands.clear();
    status = kLiftedUnsupportedInstruction;
  }

  llvm::IRBuilder<> ir(block);
  const auto [mem_ptr_ref, mem_ptr_ref_type] =
      LoadRegAddress(block, state_ptr, kMemoryVariableName);
  const auto [pc_ref, pc_ref_type] =
      LoadRegAddress(block, state_ptr, kPCVariableName);
  const auto [next_pc_ref, next_pc_ref_type] =
      LoadRegAddress(block, state_ptr, kNextPCVariableName);
  const auto next_pc = ir.CreateLoad(impl->word_type, next_pc_ref);

  // If this instruction appears within a delay slot, then we're going to assume
  // that the prior instruction updated `PC` to the target of the CTI, and that
  // the value in `NEXT_PC` on entry to this instruction represents the actual
  // address of this instruction, so we'll swap `PC` and `NEXT_PC`.
  //
  // TODO(pag): An alternate approach may be to call some kind of `DELAY_SLOT`
  //            semantics function.
  if (is_delayed) {
    llvm::Value *temp_args[] = {
        ir.CreateLoad(impl->memory_ptr_type, mem_ptr_ref)};
    ir.CreateStore(ir.CreateCall(impl->intrinsics->delay_slot_begin, temp_args),
                   mem_ptr_ref);

    // Leave `PC` and `NEXT_PC` alone; we assume that the semantics have done
    // the right thing initializing `PC` and `NEXT_PC` for the delay slots.

  } else {

    // Update the current program counter. Control-flow instructions may update
    // the program counter in the semantics code.
    ir.CreateStore(next_pc, pc_ref);
    ir.CreateStore(
        ir.CreateAdd(next_pc, llvm::ConstantInt::get(impl->word_type,
                                                     arch_inst.bytes.size())),
        next_pc_ref);
  }

  // Begin an atomic block.
  if (arch_inst.is_atomic_read_modify_write) {
    llvm::Value *temp_args[] = {
        ir.CreateLoad(impl->memory_ptr_type, mem_ptr_ref)};
    ir.CreateStore(ir.CreateCall(impl->intrinsics->atomic_begin, temp_args),
                   mem_ptr_ref);
  }

  std::vector<llvm::Value *> args;

  if (impl->flat) {
    // Pure-SSA flat mode: call the _flat ISEL wrapper.
    // Signature: F_flat(Memory *mem, ptr reg_0..reg_50, operands...)
    auto flat_isel = GetFlatInstructionFunction(module, arch_inst.function,
                                                 isel_func);
    if (!flat_isel) {
      // Fall back to the original ISEL if the flat wrapper doesn't exist.
      flat_isel = isel_func;
    }

    args.reserve(arch_inst.operands.size() + 2 + kFlatNumRegs);

    // First arg: memory pointer (by value).
    args.push_back(ir.CreateLoad(impl->memory_ptr_type, mem_ptr_ref));

    // Next 52 args: the REG_* allocas (pointers to register values).
    // The _flat ISEL wrapper expects pointer args; we pass the allocas.
    static const char *kRegNames52[kFlatNumRegs] = {
        "RAX","RBX","RCX","RDX","RSI","RDI","RSP","RBP",
        "R8","R9","R10","R11","R12","R13","R14","R15","RIP",
        "SS_BASE","GS_BASE","CS_BASE","FS_BASE",
        "CF","PF","AF","ZF","SF","DF","OF",
        "MM0","MM1","MM2","MM3","MM4","MM5","MM6","MM7",
        "XMM0","XMM1","XMM2","XMM3","XMM4","XMM5","XMM6","XMM7",
        "XMM8","XMM9","XMM10","XMM11","XMM12","XMM13","XMM14","XMM15"
    };
    // The _flat ISEL wrapper supports 60 registers (including X87 ST).
    static const char *kRegNames60[] = {
      "RAX","RBX","RCX","RDX","RSI","RDI","RSP","RBP",
      "R8","R9","R10","R11","R12","R13","R14","R15","RIP",
      "SS_BASE","GS_BASE","CS_BASE","FS_BASE",
      "CF","PF","AF","ZF","SF","DF","OF",
      "MM0","MM1","MM2","MM3","MM4","MM5","MM6","MM7",
      "XMM0","XMM1","XMM2","XMM3","XMM4","XMM5","XMM6","XMM7",
      "XMM8","XMM9","XMM10","XMM11","XMM12","XMM13","XMM14","XMM15",
      "ST0","ST1","ST2","ST3","ST4","ST5","ST6","ST7"};
    for (size_t i = 0; i < kFlatNumRegs; ++i) {
      auto reg_name_str = (std::string("REG_") + kRegNames60[i]).c_str();
      auto *val = FindVarInFunction(func, reg_name_str, true).first;
      auto *reg_alloca = llvm::dyn_cast_or_null<llvm::AllocaInst>(val);
      if (reg_alloca) {
        args.push_back(reg_alloca);
      } else {
        args.push_back(NthArgument(func, kFlatFirstRegArgNum + i));
      }
    }

    // Then the operands (same as original ISEL operands, starting at arg 2).
    auto isel_func_type = isel_func->getFunctionType();
    for (unsigned i = 2; i < isel_func_type->getNumParams(); ++i) {
      auto arg = NthArgument(isel_func, i);
      auto arg_type = arg->getType();
      auto operand = LiftOperand(arch_inst, block, state_ptr, arg, arch_inst.operands[i - 2]);
      auto op_type = operand->getType();
      assert(op_type == arg_type);
      args.push_back(operand);
    }

    auto CI = ir.CreateCall(flat_isel, args);
    if (CIInstruction) {
      *CIInstruction = CI;
    }
    ir.CreateStore(CI, mem_ptr_ref);
  } else {
    // Classic mode: first two args are memory and state pointer.
    args.reserve(arch_inst.operands.size() + 2);
    args.push_back(nullptr);
    args.push_back(state_ptr);

    auto isel_func_type = isel_func->getFunctionType();
    auto arg_num = 2U;

    for (auto &op : arch_inst.operands) {
      if (!(arg_num < isel_func_type->getNumParams())) {
        return kLiftedMismatchedISEL;
      }

      auto arg = NthArgument(isel_func, arg_num);
      auto arg_type = arg->getType();
      auto operand = LiftOperand(arch_inst, block, state_ptr, arg, op);
      arg_num += 1;
      auto op_type = operand->getType();
      assert(op_type == arg_type);

      args.push_back(operand);
    }

    // Pass in current value of the memory pointer.
    args[0] = ir.CreateLoad(impl->memory_ptr_type, mem_ptr_ref);

    // Call the function that implements the instruction semantics.
    auto CI = ir.CreateCall(isel_func, args);
    if (CIInstruction) {
      *CIInstruction = CI;
    }
    ir.CreateStore(CI, mem_ptr_ref);
  }

  // End an atomic block.
  if (arch_inst.is_atomic_read_modify_write) {
    llvm::Value *temp_args[] = {
        ir.CreateLoad(impl->memory_ptr_type, mem_ptr_ref)};
    ir.CreateStore(ir.CreateCall(impl->intrinsics->atomic_end, temp_args),
                   mem_ptr_ref);
  }

  // Restore the true target of the delayed branch.
  if (is_delayed) {

    // This is the delayed update of the program counter.
    ir.CreateStore(next_pc, pc_ref);

    // We don't know what the `NEXT_PC` is going to be because of the next
    // instruction size is unknown (really, it's likely to be
    // `arch->MaxInstructionSize()`), and for normal instructions, before they
    // are lifted, we do the `PC = NEXT_PC + size`, so this is fine.
    ir.CreateStore(next_pc, next_pc_ref);

    llvm::Value *temp_args[] = {
        ir.CreateLoad(impl->memory_ptr_type, mem_ptr_ref)};
    ir.CreateStore(ir.CreateCall(impl->intrinsics->delay_slot_end, temp_args),
                   mem_ptr_ref);
  }

  return status;
}

// Load the address of a register.
std::pair<llvm::Value *, llvm::Type *>
InstructionLifter::LoadRegAddress(llvm::BasicBlock *block,
                                  llvm::Value *state_ptr,
                                  std::string_view reg_name_) const {
  const auto func = block->getParent();
  const auto module = func->getParent();

  // Invalidate the cache.
  if (func != impl->last_func) {
    impl->reg_ptr_cache.clear();
    impl->state_ptr = nullptr;
    impl->last_func = func;

    CHECK_EQ(func->getParent(), impl->module);
  }

  std::string reg_name(reg_name_.data(), reg_name_.size());
  auto [reg_ptr_it, added] = impl->reg_ptr_cache.emplace(
      std::move(reg_name),
      std::pair<llvm::Value *, llvm::Type *>{nullptr, nullptr});

  if (reg_ptr_it->second.first) {
    (void) added;
    return reg_ptr_it->second;
  }

  // Pure-SSA flat mode: the pointer args ARE the register addresses.
  // Map the register name directly to its function argument.
  if (impl->flat) {
    // PC → arg 0 (addr_t *)
    if (reg_name_ == kPCVariableName) {
      auto *pc_arg = NthArgument(func, kFlatPCArgNum);
      reg_ptr_it->second = {pc_arg, pc_arg->getType()};
      return reg_ptr_it->second;
    }
    // NEXT_PC → arg 2 (addr_t *)
    if (reg_name_ == kNextPCVariableName) {
      auto *npc_arg = NthArgument(func, kFlatNextPCArgNum);
      reg_ptr_it->second = {npc_arg, npc_arg->getType()};
      return reg_ptr_it->second;
    }
    // MEMORY: find the "MEMORY" alloca in the entry block (created by
    // InitializeFlatLiftedFunction).
    if (reg_name_ == kMemoryVariableName) {
      for (auto &instr : func->getEntryBlock()) {
        if (instr.getName().str() == std::string(kMemoryVariableName)) {
          if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instr)) {
            reg_ptr_it->second = {alloca, alloca->getAllocatedType()};
            return reg_ptr_it->second;
          }
        }
      }
      // Fallback: create a local alloca for the memory pointer.
      // This can happen if the entry block doesn't have the MEMORY alloca
      // (e.g., the block was created before InitializeFlatLiftedFunction ran).
      llvm::IRBuilder<> ir(block);
      auto *mem_alloca = ir.CreateAlloca(impl->memory_ptr_type, nullptr, "MEMORY");
      auto *mem_arg = NthArgument(func, kFlatMemoryPointerArgNum);
      ir.CreateStore(mem_arg, mem_alloca);
      reg_ptr_it->second = {mem_alloca, impl->memory_ptr_type};
      return reg_ptr_it->second;
    }
    // Register: look up in the flat register map. The register "address" is
    // the REG_* alloca (created by InitializeFlatLiftedFunction). The by-value
    // function args are stored into these allocas at entry.
    {
      std::string lookup_name(reg_name_.data(), reg_name_.size());
      auto it = impl->flat_reg_index.find(lookup_name);
      if (it != impl->flat_reg_index.end()) {
        size_t idx = it->second;
        // Find the REG_* alloca for this register.
        static const char *kRegNames[kFlatNumRegs] = {
            "RAX","RBX","RCX","RDX","RSI","RDI","RSP","RBP",
            "R8","R9","R10","R11","R12","R13","R14","R15","RIP",
            "SS_BASE","GS_BASE","CS_BASE","FS_BASE",
            "CF","PF","AF","ZF","SF","DF","OF",
            "MM0","MM1","MM2","MM3","MM4","MM5","MM6","MM7",
            "XMM0","XMM1","XMM2","XMM3","XMM4","XMM5","XMM6","XMM7",
            "XMM8","XMM9","XMM10","XMM11","XMM12","XMM13","XMM14","XMM15"
        };
        auto reg_name_str = (std::string("REG_") + kRegNames[idx]).c_str();
        auto *val = FindVarInFunction(func, reg_name_str, true).first;
        auto *reg_alloca = llvm::dyn_cast_or_null<llvm::AllocaInst>(val);
        if (reg_alloca) {
          reg_ptr_it->second = {reg_alloca, reg_alloca->getAllocatedType()};
          return reg_ptr_it->second;
        }
        // Fallback: if the alloca isn't found (shouldn't happen), use the
        // by-value arg directly (it's not a pointer, so this will fail later).
        auto *reg_arg = NthArgument(func, kFlatFirstRegArgNum + idx);
        reg_ptr_it->second = {reg_arg, reg_arg->getType()};
        return reg_ptr_it->second;
      }
    }
    // In flat mode, registers not in the flat map are unsupported.
    // Fall back to FindVarInFunction (for MEMORY, segment bases, etc.),
    // then return nullptr if not found.
    const auto [var_ptr, var_ptr_type] = FindVarInFunction(func, reg_name_, true);
    if (var_ptr) {
      reg_ptr_it->second = {var_ptr, var_ptr_type};
      return reg_ptr_it->second;
    }
    LOG(FATAL) << "[flat] Register '" << reg_name_ << "' is not in the flat "
                   "ABI (51-register limit). This instruction uses a register "
                   "that cannot be represented in flat-lifted code. Consider "
                   "extending the flat ABI or using the original (non-flat) "
                   "mode for workloads that use X87/AVX512 registers.";
  }

  // It's already a variable in the function.
  const auto [var_ptr, var_ptr_type] = FindVarInFunction(func, reg_name_, true);
  if (var_ptr) {
    reg_ptr_it->second = {var_ptr, var_ptr_type};
    return reg_ptr_it->second;
  }

  // It's a register known to this architecture, so go and build a GEP to it
  // right now. We'll try to be careful about the placement of the actual
  // indexing instructions so that they always follow the definition of the
  // state pointer, and thus are most likely to dominate all future uses.
  if (auto reg = impl->arch->RegisterByName(reg_name_)) {
    llvm::Value *reg_ptr = nullptr;

    // The state pointer is an argument.
    if (auto state_arg = llvm::dyn_cast<llvm::Argument>(state_ptr); state_arg) {
      DCHECK_EQ(state_arg->getParent(), block->getParent());
      auto &target_block = block->getParent()->getEntryBlock();
      llvm::IRBuilder<> ir(&target_block, target_block.getFirstInsertionPt());
      reg_ptr = reg->AddressOf(state_ptr, ir);

      // The state pointer is an instruction, likely an `AllocaInst` (the
      // flat-mode local `State`) or a load. The GEPs must come *after* it so
      // that it dominates them.
    } else if (auto state_inst = llvm::dyn_cast<llvm::Instruction>(state_ptr);
               state_inst) {
      auto *insert_before = state_inst->getNextNode();
      if (!insert_before) {
        insert_before = state_inst->getParent()->getTerminator();
      }
      llvm::IRBuilder<> ir(insert_before);
      reg_ptr = reg->AddressOf(state_ptr, ir);

      // The state pointer is a constant, likely an `llvm::GlobalVariable`.
    } else if (auto state_const = llvm::dyn_cast<llvm::Constant>(state_ptr);
               state_const) {
      auto &target_block = block->getParent()->getEntryBlock();
      llvm::IRBuilder<> ir(&target_block, target_block.getFirstInsertionPt());
      reg_ptr = reg->AddressOf(state_ptr, ir);

      // Not sure.
    } else {
      assert(false);
    }

    reg_ptr_it->second = {reg_ptr, reg->type};
    return reg_ptr_it->second;
  }

  // Try to find it as a global variable.
  if (auto gvar = module->getGlobalVariable(reg_name)) {
    return {gvar, gvar->getValueType()};
  }

  // Invent a fake one and keep going.
  std::stringstream unk_var;
  unk_var << "__remill_unknown_register_" << reg_name;
  auto unk_var_name = unk_var.str();
  if (auto var = module->getGlobalVariable(unk_var_name)) {
    return {var, var->getValueType()};
  }

  // TODO(pag): Eventually refactor into a higher-level issue, perhaps a
  //            a hyper call to read an unknown register, or a lifting failure,
  //            with a more elaborate status value returned.
  LOG(ERROR) << "Could not locate variable or register " << reg_name_;

  return {new llvm::GlobalVariable(*module, impl->word_type, false,
                                   llvm::GlobalValue::ExternalLinkage,
                                   llvm::UndefValue::get(impl->word_type),
                                   unk_var_name),
          impl->word_type};
}

// Clear out the cache of the current register values/addresses loaded.
void InstructionLifter::ClearCache(void) const {
  impl->reg_ptr_cache.clear();
  impl->state_ptr = nullptr;
  impl->last_func = nullptr;
}

// Load the value of a register.
llvm::Value *InstructionLifter::LoadRegValue(llvm::BasicBlock *block,
                                             llvm::Value *state_ptr,
                                             std::string_view reg_name) const {
  auto [ptr, ptr_ty] = LoadRegAddress(block, state_ptr, reg_name);
  CHECK_NOTNULL(ptr);
  return new llvm::LoadInst(ptr_ty, ptr, llvm::Twine::createNull(), block);
}

// Return a register value, or zero.
llvm::Value *InstructionLifter::LoadWordRegValOrZero(llvm::BasicBlock *block,
                                                     llvm::Value *state_ptr,
                                                     std::string_view reg_name,
                                                     llvm::ConstantInt *zero) {

  if (reg_name.empty()) {
    return zero;
  }

  auto val = LoadRegValue(block, state_ptr, reg_name);
  llvm::IntegerType *val_type =
      llvm::dyn_cast_or_null<llvm::IntegerType>(val->getType());
  llvm::IntegerType *word_type =
      llvm::dyn_cast_or_null<llvm::IntegerType>(zero->getType());

  assert(val_type);

  auto val_size = val_type->getBitWidth();
  auto word_size = word_type->getBitWidth();
  assert(val_size <= word_size);

  if (val_size < word_size) {
    val = new llvm::ZExtInst(val, word_type, llvm::Twine::createNull(), block);
  }

  return val;
}

llvm::Value *InstructionLifter::LiftShiftRegisterOperand(
    Instruction &inst, llvm::BasicBlock *block, llvm::Value *state_ptr,
    llvm::Argument *arg, Operand &op) {

  llvm::Function *func = block->getParent();
  llvm::Module *module = func->getParent();
  auto &context = module->getContext();
  auto &arch_reg = op.shift_reg.reg;

  auto arg_type = arg->getType();
  CHECK(arg_type->isIntegerTy())
      << "Expected " << arch_reg.name << " to be an integral type "
      << "for instruction at " << std::hex << inst.pc;

  const llvm::DataLayout data_layout(module->getDataLayout());
  auto reg = LoadRegValue(block, state_ptr, arch_reg.name);
  auto reg_type = reg->getType();
  auto reg_size = SizeOfTypeInBits(data_layout, reg_type);
  auto word_size = impl->arch->address_size;
  auto op_type = llvm::Type::getIntNTy(context, op.size);

  const uint64_t zero = 0;
  const uint64_t one = 1;
  const uint64_t shift_size = op.shift_reg.shift_size;

  const auto shift_val = llvm::ConstantInt::get(op_type, shift_size);

  llvm::IRBuilder<> ir(block);

  auto curr_size = reg_size;
  if (Operand::ShiftRegister::kExtendInvalid != op.shift_reg.extend_op) {

    auto extract_type =
        llvm::Type::getIntNTy(context, op.shift_reg.extract_size);

    if (reg_size > op.shift_reg.extract_size) {
      curr_size = op.shift_reg.extract_size;
      reg = ir.CreateTrunc(reg, extract_type);

    } else {
      assert(reg_size == op.shift_reg.extract_size);
    }

    if (op.size > op.shift_reg.extract_size) {
      switch (op.shift_reg.extend_op) {
        case Operand::ShiftRegister::kExtendSigned:
          reg = ir.CreateSExt(reg, op_type);
          curr_size = op.size;
          break;
        case Operand::ShiftRegister::kExtendUnsigned:
          reg = ir.CreateZExt(reg, op_type);
          curr_size = op.size;
          break;
        default:
          assert(false);
          break;
      }
    }
  }

  assert(curr_size <= op.size);

  if (curr_size < op.size) {
    reg = ir.CreateZExt(reg, op_type);
    curr_size = op.size;
  }

  if (Operand::ShiftRegister::kShiftInvalid != op.shift_reg.shift_op) {

    assert(shift_size < op.size);

    switch (op.shift_reg.shift_op) {

      // Left shift.
      case Operand::ShiftRegister::kShiftLeftWithZeroes:
        reg = ir.CreateShl(reg, shift_val);
        break;

      // Masking shift left.
      case Operand::ShiftRegister::kShiftLeftWithOnes: {
        const auto mask_val =
            llvm::ConstantInt::get(reg_type, ~((~zero) << shift_size));
        reg = ir.CreateOr(ir.CreateShl(reg, shift_val), mask_val);
        break;
      }

      // Logical right shift.
      case Operand::ShiftRegister::kShiftUnsignedRight:
        reg = ir.CreateLShr(reg, shift_val);
        break;

      // Arithmetic right shift.
      case Operand::ShiftRegister::kShiftSignedRight:
        reg = ir.CreateAShr(reg, shift_val);
        break;

      // Rotate left.
      case Operand::ShiftRegister::kShiftLeftAround: {
        const uint64_t shr_amount = (~shift_size + one) & (op.size - one);
        const auto shr_val = llvm::ConstantInt::get(op_type, shr_amount);
        const auto val1 = ir.CreateLShr(reg, shr_val);
        const auto val2 = ir.CreateShl(reg, shift_val);
        reg = ir.CreateOr(val1, val2);
        break;
      }

      // Rotate right.
      case Operand::ShiftRegister::kShiftRightAround: {
        const uint64_t shl_amount = (~shift_size + one) & (op.size - one);
        const auto shl_val = llvm::ConstantInt::get(op_type, shl_amount);
        const auto val1 = ir.CreateLShr(reg, shift_val);
        const auto val2 = ir.CreateShl(reg, shl_val);
        reg = ir.CreateOr(val1, val2);
        break;
      }

      case Operand::ShiftRegister::kShiftInvalid: break;
    }
  }

  if (word_size > op.size) {
    reg = ir.CreateZExt(reg, impl->word_type);
  } else {
    assert(word_size == op.size);
  }

  return reg;
}

namespace {

static llvm::Type *IntendedArgumentType(llvm::Argument *arg) {
  for (auto user : arg->users()) {
    if (auto cast_inst = llvm::dyn_cast<llvm::IntToPtrInst>(user)) {
      return cast_inst->getType();
    }
  }
  return arg->getType();
}

static llvm::Value *
ConvertToIntendedType(Instruction &inst, Operand &op, llvm::BasicBlock *block,
                      llvm::Value *val, llvm::Type *intended_type) {
  auto val_type = val->getType();
  if (val->getType() == intended_type) {
    return val;
  } else if (auto val_ptr_type = llvm::dyn_cast<llvm::PointerType>(val_type)) {
    if (intended_type->isPointerTy()) {
      return new llvm::BitCastInst(val, intended_type, val->getName(), block);
    } else if (intended_type->isIntegerTy()) {
      return new llvm::PtrToIntInst(val, intended_type, val->getName(), block);
    }
  } else if (val_type->isFloatingPointTy()) {
    if (intended_type->isIntegerTy()) {
      return new llvm::BitCastInst(val, intended_type, val->getName(), block);
    }
  }

  assert(false);
  return nullptr;
}

}  // namespace

// Load a register operand. This deals uniformly with write- and read-operands
// for registers. In the case of write operands, the argument type is always
// a pointer. In the case of read operands, the argument type is sometimes
// a pointer (e.g. when passing a vector to an instruction semantics function).
llvm::Value *InstructionLifter::LiftRegisterOperand(Instruction &inst,
                                                    llvm::BasicBlock *block,
                                                    llvm::Value *state_ptr,
                                                    llvm::Argument *arg,
                                                    Operand &op) {

  llvm::Function *func = block->getParent();
  llvm::Module *module = func->getParent();
  auto &arch_reg = op.reg;

  const auto real_arg_type = arg->getType();

  // LLVM on AArch64 and on amd64 Windows converts things like `RnW<uint64_t>`,
  // which is a struct containing a `uint64_t *`, into a `uintptr_t` when they
  // are being passed as arguments.
  auto arg_type = IntendedArgumentType(arg);

  if (llvm::isa<llvm::PointerType>(arg_type)) {
    auto [val, val_type] = LoadRegAddress(block, state_ptr, arch_reg.name);
    return ConvertToIntendedType(inst, op, block, val, real_arg_type);

  } else {
    CHECK(arg_type->isIntegerTy() || arg_type->isFloatingPointTy())
        << "Expected " << arch_reg.name << " to be an integral or float type "
        << "for instruction at " << std::hex << inst.pc;

    auto val = LoadRegValue(block, state_ptr, arch_reg.name);

    const llvm::DataLayout data_layout(module->getDataLayout());
    auto val_type = val->getType();
    auto val_size = data_layout.getTypeAllocSizeInBits(val_type);
    auto arg_size = data_layout.getTypeAllocSizeInBits(arg_type);
    const auto word_size = impl->arch->address_size;

    if (val_size < arg_size) {
      if (arg_type->isIntegerTy()) {
        CHECK(val_type->isIntegerTy())
            << "Expected " << arch_reg.name << " to be an integral type "
            << "for instruction at " << std::hex << inst.pc;

        assert(word_size == arg_size);

        val = new llvm::ZExtInst(val, impl->word_type,
                                 llvm::Twine::createNull(), block);

      } else if (arg_type->isFloatingPointTy()) {
        CHECK(val_type->isFloatingPointTy())
            << "Expected " << arch_reg.name << " to be a floating point type "
            << "for instruction at " << std::hex << inst.pc;

        val = new llvm::FPExtInst(val, arg_type, llvm::Twine::createNull(),
                                  block);
      }

    } else if (val_size > arg_size) {
      if (arg_type->isIntegerTy()) {
        CHECK(val_type->isIntegerTy())
            << "Expected " << arch_reg.name << " to be an integral type "
            << "for instruction at " << std::hex << inst.pc;

        assert(word_size == arg_size);

        val = new llvm::TruncInst(val, arg_type, llvm::Twine::createNull(),
                                  block);

      } else if (arg_type->isFloatingPointTy()) {
        CHECK(val_type->isFloatingPointTy())
            << "Expected " << arch_reg.name << " to be a floating point type "
            << "for instruction at " << std::hex << inst.pc;

        val = new llvm::FPTruncInst(val, arg_type, llvm::Twine::createNull(),
                                    block);
      }
    }

    return ConvertToIntendedType(inst, op, block, val, real_arg_type);
  }
}

// Lift an immediate operand.
llvm::Value *
InstructionLifter::LiftImmediateOperand(Instruction &inst, llvm::BasicBlock *,
                                        llvm::Argument *arg, Operand &arch_op) {
  auto arg_type = arg->getType();
  if (arch_op.size > impl->arch->address_size) {
    CHECK(arg_type->isIntegerTy(static_cast<uint32_t>(arch_op.size)))
        << "Argument to semantics function for instruction at " << std::hex
        << inst.pc << " is not an integer. This may not be surprising because "
        << "the immediate operand is " << arch_op.size << " bits, but the "
        << "machine word size is " << impl->arch->address_size << " bits.";

    assert(arch_op.size <= 64);

    return llvm::ConstantInt::get(arg_type, arch_op.imm.val,
                                  arch_op.imm.is_signed);

  } else {
    CHECK(arg_type->isIntegerTy(impl->arch->address_size))
        << "Bad semantics function implementation for instruction at "
        << std::hex << inst.pc << ". Integer constants that are "
        << "smaller than the machine word size should be represented as "
        << "machine word sized arguments to semantics functions.";

    return llvm::ConstantInt::get(impl->word_type, arch_op.imm.val,
                                  arch_op.imm.is_signed);
  }
}

// Lift an expression operand.
llvm::Value *InstructionLifter::LiftExpressionOperand(Instruction &inst,
                                                      llvm::BasicBlock *block,
                                                      llvm::Value *state_ptr,
                                                      llvm::Argument *arg,
                                                      Operand &op) {
  auto val = LiftExpressionOperandRec(inst, block, state_ptr, arg, op.expr);
  llvm::Function *func = block->getParent();
  llvm::Module *module = func->getParent();
  const auto real_arg_type = arg->getType();

  // LLVM on AArch64 and on amd64 Windows converts things like `RnW<uint64_t>`,
  // which is a struct containing a `uint64_t *`, into a `uintptr_t` when they
  // are being passed as arguments.
  auto arg_type = IntendedArgumentType(arg);

  if (llvm::isa<llvm::PointerType>(arg_type)) {
    return ConvertToIntendedType(inst, op, block, val, real_arg_type);

  } else {
    CHECK(arg_type->isIntegerTy() || arg_type->isFloatingPointTy())
        << "Expected " << op.Serialize() << " to be an integral or float type "
        << "for instruction at " << std::hex << inst.pc;

    const llvm::DataLayout data_layout(module->getDataLayout());
    auto val_type = val->getType();
    auto val_size = data_layout.getTypeAllocSizeInBits(val_type);
    auto arg_size = data_layout.getTypeAllocSizeInBits(arg_type);
    const auto word_size = impl->arch->address_size;

    if (val_size < arg_size) {
      if (arg_type->isIntegerTy()) {
        CHECK(val_type->isIntegerTy())
            << "Expected " << op.Serialize() << " to be an integral type "
            << "for instruction at " << std::hex << inst.pc;

        assert(word_size == arg_size);

        val = new llvm::ZExtInst(val, impl->word_type, "", block);

      } else if (arg_type->isFloatingPointTy()) {
        CHECK(val_type->isFloatingPointTy())
            << "Expected " << op.Serialize() << " to be a floating point type "
            << "for instruction at " << std::hex << inst.pc;

        val = new llvm::FPExtInst(val, arg_type, "", block);
      }

    } else if (val_size > arg_size) {
      if (arg_type->isIntegerTy()) {
        CHECK(val_type->isIntegerTy())
            << "Expected " << op.Serialize() << " to be an integral type "
            << "for instruction at " << std::hex << inst.pc;

        assert(word_size == arg_size);

        val = new llvm::TruncInst(val, arg_type, "", block);

      } else if (arg_type->isFloatingPointTy()) {
        CHECK(val_type->isFloatingPointTy())
            << "Expected " << op.Serialize() << " to be a floating point type "
            << "for instruction at " << std::hex << inst.pc;

        val = new llvm::FPTruncInst(val, arg_type, "", block);
      }
    }

    return ConvertToIntendedType(inst, op, block, val, real_arg_type);
  }
}

// Lift an expression operand.
llvm::Value *InstructionLifter::LiftExpressionOperandRec(
    Instruction &inst, llvm::BasicBlock *block, llvm::Value *state_ptr,
    llvm::Argument *arg, const OperandExpression *op) {
  if (auto llvm_op = std::get_if<LLVMOpExpr>(op)) {
    auto lhs =
        LiftExpressionOperandRec(inst, block, state_ptr, nullptr, llvm_op->op1);
    llvm::Value *rhs = nullptr;
    if (llvm_op->op2) {
      rhs = LiftExpressionOperandRec(inst, block, state_ptr, nullptr,
                                     llvm_op->op2);
    }
    llvm::IRBuilder<> ir(block);
    switch (llvm_op->llvm_opcode) {
      case llvm::Instruction::Add: return ir.CreateAdd(lhs, rhs);
      case llvm::Instruction::Sub: return ir.CreateSub(lhs, rhs);
      case llvm::Instruction::Mul: return ir.CreateMul(lhs, rhs);
      case llvm::Instruction::Shl: return ir.CreateShl(lhs, rhs);
      case llvm::Instruction::LShr: return ir.CreateLShr(lhs, rhs);
      case llvm::Instruction::AShr: return ir.CreateAShr(lhs, rhs);
      case llvm::Instruction::ZExt: return ir.CreateZExt(lhs, op->type);
      case llvm::Instruction::SExt: return ir.CreateSExt(lhs, op->type);
      case llvm::Instruction::Trunc: return ir.CreateTrunc(lhs, op->type);
      case llvm::Instruction::And: return ir.CreateAnd(lhs, rhs);
      case llvm::Instruction::Or: return ir.CreateOr(lhs, rhs);
      case llvm::Instruction::URem: return ir.CreateURem(lhs, rhs);
      case llvm::Instruction::Xor: return ir.CreateXor(lhs, rhs);
      default:
        assert(false);
        return nullptr;
    }
  } else if (auto reg_op = std::get_if<const Register *>(op)) {
    if (!arg || !llvm::isa<llvm::PointerType>(arg->getType())) {
      return LoadRegValue(block, state_ptr, (*reg_op)->name);
    } else {
      auto *ptr = LoadRegAddress(block, state_ptr, (*reg_op)->name).first;
      CHECK_NOTNULL(ptr) << "Register '" << (*reg_op)->name
                         << "' is not in the flat ABI (51-register limit). "
                            "This instruction uses a register that cannot be "
                            "represented in flat-lifted code.";
      return ptr;
    }

  } else if (auto ci_op = std::get_if<llvm::Constant *>(op)) {
    return *ci_op;

  } else if (auto str_op = std::get_if<std::string>(op)) {
    if (!arg || !llvm::isa<llvm::PointerType>(arg->getType())) {
      return LoadRegValue(block, state_ptr, *str_op);
    } else {
      auto *ptr = LoadRegAddress(block, state_ptr, *str_op).first;
      CHECK_NOTNULL(ptr) << "Register '" << *str_op
                         << "' is not in the flat ABI (51-register limit). "
                            "This instruction uses a register that cannot be "
                            "represented in flat-lifted code.";
      return ptr;
    }
  } else {
    assert(false);
    return nullptr;
  }
}

// Zero-extend a value to be the machine word size.
llvm::Value *InstructionLifter::LiftAddressOperand(Instruction &inst,
                                                   llvm::BasicBlock *block,
                                                   llvm::Value *state_ptr,
                                                   llvm::Argument *,
                                                   Operand &op) {
  auto &arch_addr = op.addr;
  const auto word_type = llvm::dyn_cast<llvm::IntegerType>(impl->word_type);
  const auto zero = llvm::ConstantInt::get(word_type, 0, false);
  const auto word_size = impl->arch->address_size;

  assert(word_size >= arch_addr.base_reg.size);

  assert(word_size >= arch_addr.index_reg.size);

  auto addr =
      LoadWordRegValOrZero(block, state_ptr, arch_addr.base_reg.name, zero);
  auto index =
      LoadWordRegValOrZero(block, state_ptr, arch_addr.index_reg.name, zero);
  auto scale = llvm::ConstantInt::get(
      word_type, static_cast<uint64_t>(arch_addr.scale), true);
  auto segment = LoadWordRegValOrZero(block, state_ptr,
                                      arch_addr.segment_base_reg.name, zero);

  llvm::IRBuilder<> ir(block);

  if (zero != index) {
    addr = ir.CreateAdd(addr, ir.CreateMul(index, scale));
  }

  if (arch_addr.displacement) {
    if (0 < arch_addr.displacement) {
      addr = ir.CreateAdd(
          addr, llvm::ConstantInt::get(
                    word_type, static_cast<uint64_t>(arch_addr.displacement)));
    } else {
      addr = ir.CreateSub(
          addr, llvm::ConstantInt::get(
                    word_type, static_cast<uint64_t>(-arch_addr.displacement)));
    }
  }

  // Compute the segmented address.
  if (zero != segment) {
    addr = ir.CreateAdd(addr, segment);
  }

  // Memory address is smaller than the machine word size (e.g. 32-bit address
  // used in 64-bit).
  if (arch_addr.address_size < word_size) {
    auto addr_type = llvm::Type::getIntNTy(
        block->getContext(), static_cast<unsigned>(arch_addr.address_size));

    addr = ir.CreateZExt(ir.CreateTrunc(addr, addr_type), word_type);
  }

  return addr;
}

// Lift an operand for use by the instruction.
llvm::Value *
InstructionLifter::LiftOperand(Instruction &inst, llvm::BasicBlock *block,
                               llvm::Value *state_ptr, llvm::Argument *arg,
                               Operand &arch_op) {
  auto arg_type = arg->getType();
  switch (arch_op.type) {
    case Operand::kTypeInvalid:
      assert(false);
      return nullptr;

    case Operand::kTypeShiftRegister:
      assert(Operand::kActionRead == arch_op.action);

      return LiftShiftRegisterOperand(inst, block, state_ptr, arg, arch_op);

    case Operand::kTypeRegister:
      if (arch_op.size != arch_op.reg.size) {
        assert(false);
      }
      return LiftRegisterOperand(inst, block, state_ptr, arg, arch_op);

    case Operand::kTypeImmediate:
      return LiftImmediateOperand(inst, block, arg, arch_op);

    case Operand::kTypeAddress:
      if (arg_type != impl->word_type) {
        assert(false);
      }

      return LiftAddressOperand(inst, block, state_ptr, arg, arch_op);

    case Operand::kTypeExpression:
    case Operand::kTypeRegisterExpression:
    case Operand::kTypeImmediateExpression:
    case Operand::kTypeAddressExpression:
      return LiftExpressionOperand(inst, block, state_ptr, arg, arch_op);
  }

  assert(false);

  return nullptr;
}

}  // namespace remill
