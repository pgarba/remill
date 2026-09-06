/*
 * Copyright (c) 2019 Trail of Bits, Inc.
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
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <remill/Arch/Arch.h>
#include <remill/Arch/Instruction.h>
#include <remill/Arch/Name.h>
#include <remill/BC/ABI.h>
#include <remill/BC/Annotate.h>
#include <remill/BC/IntrinsicTable.h>
#include <remill/BC/Lifter.h>
#include <remill/BC/Optimizer.h>
#include <remill/BC/Util.h>
#include <remill/OS/OS.h>
#include <remill/Version/Version.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

// Simple command-line argument parser to replace gflags.
static std::string GetArgValue(int argc, char *argv[], const char *flag) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], flag) == 0 && i + 1 < argc) {
      return argv[i + 1];
    }
  }
  return "";
}

static uint64_t GetUInt64Arg(int argc, char *argv[], const char *flag, uint64_t default_val) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], flag) == 0 && i + 1 < argc) {
      char *end = nullptr;
      uint64_t val = strtoull(argv[i + 1], &end, 10);
      if (end != argv[i + 1]) {
        return val;
      }
    }
  }
  return default_val;
}

// Global variables to replace gflags
static std::string g_arch;
static std::string g_os;
static uint64_t g_address = 0;
static uint64_t g_entry_address = 0;
static std::string g_bytes;
static std::string g_ir_out;
static std::string g_bc_out;
static std::string g_slice_inputs;
static std::string g_slice_outputs;
static bool g_flat = false;      // Old flat mode: STATE_LOCAL + SROA
static bool g_flat_ssa = false;  // New pure-SSA flat mode: no struct, inline _flat ISEL

using Memory = std::map<uint64_t, uint8_t>;

// Unhexlify the data passed to `--bytes`, and fill in `memory` with each
// such byte.
static Memory UnhexlifyInputBytes(uint64_t addr_mask) {
  Memory memory;

  for (size_t i = 0; i < g_bytes.size(); i += 2) {
    char nibbles[] = {g_bytes[i], g_bytes[i + 1], '\0'};
    char *parsed_to = nullptr;
    auto byte_val = strtol(nibbles, &parsed_to, 16);

    if (parsed_to != &(nibbles[2])) {
      std::cerr << "Invalid hex byte value '" << nibbles
                << "' specified in --bytes." << std::endl;
      exit(EXIT_FAILURE);
    }

    auto byte_addr = g_address + (i / 2);
    auto masked_addr = byte_addr & addr_mask;

    // Make sure that if a really big number is specified for `--address`,
    // that we don't accidentally wrap around and start filling out low
    // byte addresses.
    if (masked_addr < byte_addr) {
      std::cerr << "Too many bytes specified to --bytes, would result "
                << "in a 32-bit overflow.";
      exit(EXIT_FAILURE);

    } else if (masked_addr < g_address) {
      std::cerr << "Too many bytes specified to --bytes, would result "
                << "in a 64-bit overflow.";
      exit(EXIT_FAILURE);
    }

    memory[byte_addr] = static_cast<uint8_t>(byte_val);
  }

  return memory;
}

class SimpleTraceManager : public remill::TraceManager {
 public:
  virtual ~SimpleTraceManager(void) = default;

  explicit SimpleTraceManager(Memory &memory_) : memory(memory_) {}

 protected:
  // Called when we have lifted, i.e. defined the contents, of a new trace.
  // The derived class is expected to do something useful with this.
  void SetLiftedTraceDefinition(uint64_t addr,
                                llvm::Function *lifted_func) override {
    traces[addr] = lifted_func;
  }

  // Get a declaration for a lifted trace. The idea here is that a derived
  // class might have additional global info available to them that lets
  // them declare traces ahead of time. In order to distinguish between
  // stuff we've lifted, and stuff we haven't lifted, we allow the lifter
  // to access "defined" vs. "declared" traces.
  //
  // NOTE: This is permitted to return a function from an arbitrary module.
  llvm::Function *GetLiftedTraceDeclaration(uint64_t addr) override {
    auto trace_it = traces.find(addr);
    if (trace_it != traces.end()) {
      return trace_it->second;
    } else {
      return nullptr;
    }
  }

  // Get a definition for a lifted trace.
  //
  // NOTE: This is permitted to return a function from an arbitrary module.
  llvm::Function *GetLiftedTraceDefinition(uint64_t addr) override {
    return GetLiftedTraceDeclaration(addr);
  }

  // Try to read an executable byte of memory. Returns `true` of the byte
  // at address `addr` is executable and readable, and updates the byte
  // pointed to by `byte` with the read value.
  bool TryReadExecutableByte(uint64_t addr, uint8_t *byte) override {
    auto byte_it = memory.find(addr);
    if (byte_it != memory.end()) {
      *byte = byte_it->second;
      return true;
    } else {
      return false;
    }
  }

 public:
  Memory &memory;
  std::unordered_map<uint64_t, llvm::Function *> traces;
};

// Looks for calls to a function like `__remill_function_return`, and
// replace its state pointer with a null pointer so that the state
// pointer never escapes.
static void MuteStateEscape(llvm::Module *module, const char *func_name) {
  auto func = module->getFunction(func_name);
  if (!func) {
    return;
  }

  for (auto user : func->users()) {
    if (auto call_inst = llvm::dyn_cast<llvm::CallInst>(user)) {
      auto arg_op = call_inst->getArgOperand(remill::kStatePointerArgNum);
      call_inst->setArgOperand(remill::kStatePointerArgNum,
                               llvm::UndefValue::get(arg_op->getType()));
    }
  }
}

static void SetVersion(void) {
  std::stringstream ss;
  auto vs = remill::version::GetVersionString();
  if (0 == vs.size()) {
    vs = "unknown";
  }
  ss << vs << "\n";
  if (!remill::version::HasVersionData()) {
    ss << "No extended version information found!\n";
  } else {
    ss << "Commit Hash: " << remill::version::GetCommitHash() << "\n";
    ss << "Commit Date: " << remill::version::GetCommitDate() << "\n";
    ss << "Last commit by: " << remill::version::GetAuthorName() << " ["
       << remill::version::GetAuthorEmail() << "]\n";
    ss << "Commit Subject: [" << remill::version::GetCommitSubject() << "]\n";
    ss << "\n";
    if (remill::version::HasUncommittedChanges()) {
      ss << "Uncommitted changes were present during build.\n";
    } else {
      ss << "All changes were committed prior to building.\n";
    }
  }
  google::SetVersionString(ss.str());
}

int main(int argc, char *argv[]) {
  SetVersion();
  google::InitGoogleLogging(argv[0]);

  // Parse command-line flags (gflags replacement)
  g_arch = GetArgValue(argc, argv, "--arch");
  g_os = GetArgValue(argc, argv, "--os");
  g_address = GetUInt64Arg(argc, argv, "--address", 0);
  g_entry_address = GetUInt64Arg(argc, argv, "--entry_address", 0);
  g_bytes = GetArgValue(argc, argv, "--bytes");
  g_ir_out = GetArgValue(argc, argv, "--ir_out");
  g_bc_out = GetArgValue(argc, argv, "--bc_out");
  g_slice_inputs = GetArgValue(argc, argv, "--slice_inputs");
  g_slice_outputs = GetArgValue(argc, argv, "--slice_outputs");
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--flat") == 0) {
      g_flat = true;
    } else if (strcmp(argv[i], "--flat-ssa") == 0) {
      g_flat_ssa = true;
      g_flat = true;  // flat-ssa implies flat ABI
    }
  }

  if (g_bytes.empty()) {
    std::cerr << "Please specify a sequence of hex bytes to --bytes."
              << std::endl;
    return EXIT_FAILURE;
  }

  if (g_bytes.size() % 2) {
    std::cerr << "Please specify an even number of nibbles to --bytes."
              << std::endl;
    return EXIT_FAILURE;
  }

  if (!g_entry_address) {
    g_entry_address = g_address;
  }

  // Make sure `--address` and `--entry_address` are in-bounds for the target
  // architecture's address size.
  llvm::LLVMContext context;
  auto arch = remill::Arch::Get(context, g_os, g_arch);
  const uint64_t addr_mask = ~0ULL >> (64UL - arch->address_size);
  if (g_address != (g_address & addr_mask)) {
    std::cerr << "Value " << std::hex << g_address
              << " passed to --address does not fit into 32-bits. Did mean"
              << " to specify a 64-bit architecture to --arch?" << std::endl;
    return EXIT_FAILURE;
  }

  if (g_entry_address != (g_entry_address & addr_mask)) {
    std::cerr
        << "Value " << std::hex << g_entry_address
        << " passed to --entry_address does not fit into 32-bits. Did mean"
        << " to specify a 64-bit architecture to --arch?" << std::endl;
    return EXIT_FAILURE;
  }

  // Load the appropriate semantics bitcode.
  std::unique_ptr<llvm::Module> module;
  if (g_flat_ssa) {
    // Pure-SSA mode: load the flat bitcode with _flat ISEL wrappers.
    auto arch_name = remill::GetArchName(arch->arch_name);
    std::string path = remill::FindFlatSemanticsBitcodeFile(arch_name);
    llvm::outs() << "Loading " << arch_name << " flat semantics from " << path
                 << "\n";
    module = remill::LoadModuleFromFile(arch->context,
                                         std::filesystem::path(path));
    arch->PrepareModule(module);
    arch->InitFromSemanticsModule(module.get());
    for (auto &func : *module) {
      remill::Annotate<remill::Semantics>(&func);
    }
  } else if (g_flat) {
    // Old flat mode: load original semantics (uses STATE_LOCAL + SROA).
    module = remill::LoadArchSemantics(arch);
  } else {
    module = remill::LoadArchSemantics(arch);
  }

  const auto state_ptr_type = arch->StatePointerType();
  const auto mem_ptr_type = arch->MemoryPointerType();

  Memory memory = UnhexlifyInputBytes(addr_mask);
  SimpleTraceManager manager(memory);
  remill::IntrinsicTable intrinsics(module);
  remill::InstructionLifter inst_lifter(arch, intrinsics);

  // Pure-SSA flat mode: pointer args ARE register addresses (no struct).
  // Old flat mode (--flat): uses STATE_LOCAL + SROA.
  inst_lifter.SetFlatMode(g_flat_ssa);

  remill::TraceLifter trace_lifter(inst_lifter, manager, g_flat, g_flat_ssa);

  // Lift all discoverable traces starting from `--entry_address` into
  // `module`.
  trace_lifter.Lift(g_entry_address);

  // Optimize the module, but with a particular focus on only the functions
  // that we actually lifted.
  remill::OptimizationGuide guide = {};
  remill::OptimizeModule(arch, module, manager.traces, guide);

  // Old flat mode: scalarize the STATE_LOCAL alloca with SROA.
  // Pure-SSA mode: no SROA needed (no state struct).
  if (g_flat && !g_flat_ssa) {
    for (auto &lifted_entry : manager.traces) {
      remill::ScalarizeFlatFunction(module.get(), lifted_entry.second);
    }
  }


  // Create a new module in which we will move all the lifted functions. Prepare
  // the module for code of this architecture, i.e. set the data layout, triple,
  // etc.
  llvm::Module dest_module("lifted_code", context);
  arch->PrepareModuleDataLayout(&dest_module);

  llvm::Function *entry_trace = nullptr;
  const auto make_slice =
      !g_slice_inputs.empty() || !g_slice_outputs.empty();

  // Move the lifted code into a new module. This module will be much smaller
  // because it won't be bogged down with all of the semantics definitions.
  // This is a good JITing strategy: optimize the lifted code in the semantics
  // module, move it to a new module, instrument it there, then JIT compile it.
  // Verification is skipped in flat mode for now (pre-existing readnone
  // attributes in the bitcode cause noise). The MoveFunctionIntoModule will
  // catch real structural errors.

  for (auto &lifted_entry : manager.traces) {
    if (lifted_entry.first == g_entry_address) {
      entry_trace = lifted_entry.second;
    }

    // In pure-SSA flat mode, inline the _flat ISEL calls into the lifted
    // function so the output is self-contained (no external ISEL references).
    if (g_flat_ssa) {
      auto *func = lifted_entry.second;
      // Collect all non-tail calls to defined functions (the _flat wrappers).
      llvm::SmallVector<llvm::CallBase *, 16> calls;
      for (auto &block : *func) {
        for (auto &inst : block) {
          if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
            auto *callee = call->getCalledFunction();
            if (callee && callee != func && !callee->isDeclaration() &&
                !call->isTailCall()) {
              calls.push_back(call);
            }
          }
        }
      }
      // Inline each call (iterate in reverse since inlining invalidates
      // subsequent iterators).
      for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
        auto *call = *it;
        if (call->use_empty()) continue;  // already inlined/erased
        llvm::InlineFunctionInfo ifi;
        auto result = llvm::InlineFunction(*call, ifi);
        (void)result;
      }
    }

    remill::MoveFunctionIntoModule(lifted_entry.second, &dest_module);

    // Optimize the moved function in the destination module (smaller scope,
    // won't touch the 1589 _flat wrappers in the semantics module).
    if (g_flat_ssa) {
      auto *moved_func = dest_module.getFunction(
          lifted_entry.second->getName());
      if (moved_func) {
        remill::OptimizeFlatSSAFunction(&dest_module, moved_func);
      }
    }

    // If we are providing a prototype, then we'll be re-optimizing the new
    // module, and we want everything to get inlined.
    if (make_slice) {
      lifted_entry.second->setLinkage(llvm::GlobalValue::InternalLinkage);
      lifted_entry.second->removeFnAttr(llvm::Attribute::NoInline);
      lifted_entry.second->addFnAttr(llvm::Attribute::InlineHint);
      lifted_entry.second->addFnAttr(llvm::Attribute::AlwaysInline);
    }
  }

  // We have a prototype, so go create a function that will call our entrypoint.
  if (make_slice) {
    CHECK_NOTNULL(entry_trace);

    llvm::SmallVector<llvm::StringRef, 4> input_reg_names;
    llvm::SmallVector<llvm::StringRef, 4> output_reg_names;
    llvm::StringRef(g_slice_inputs)
        .split(input_reg_names, ',', -1, false /* KeepEmpty */);
    llvm::StringRef(g_slice_outputs)
        .split(output_reg_names, ',', -1, false /* KeepEmpty */);

    CHECK(!(input_reg_names.empty() && output_reg_names.empty()))
        << "Empty lists passed to both --slice_inputs and --slice_outputs";

    // Use the registers to build a function prototype.
    llvm::SmallVector<llvm::Type *, 8> arg_types;
    arg_types.push_back(mem_ptr_type);

    for (auto &reg_name : input_reg_names) {
      const auto reg = arch->RegisterByName(reg_name.str());
      CHECK(reg != nullptr)
          << "Invalid register name '" << reg_name.str()
          << "' used in input slice list '" << g_slice_inputs << "'";

      arg_types.push_back(reg->type);
    }

    const auto first_output_reg_index = arg_types.size();

    // Outputs are "returned" by pointer through arguments.
    for (auto &reg_name : output_reg_names) {
      const auto reg = arch->RegisterByName(reg_name.str());
      CHECK(reg != nullptr)
          << "Invalid register name '" << reg_name.str()
          << "' used in output slice list '" << g_slice_outputs << "'";

      arg_types.push_back(llvm::PointerType::get(reg->type, 0));
    }

    const auto state_type = llvm::PointerType::get(context, 0);
    const auto func_type =
        llvm::FunctionType::get(mem_ptr_type, arg_types, false);
    const auto func = llvm::Function::Create(
        func_type, llvm::GlobalValue::ExternalLinkage, "slice", &dest_module);

    // Store all of the function arguments (corresponding with specific registers)
    // into the stack-allocated `State` structure.
    auto entry = llvm::BasicBlock::Create(context, "", func);
    llvm::IRBuilder<> ir(entry);

    const auto state_ptr = ir.CreateAlloca(state_type);

    const remill::Register *pc_reg =
        arch->RegisterByName(arch->ProgramCounterRegisterName());

    CHECK(pc_reg != nullptr)
        << "Could not find the register in the state structure "
        << "associated with the program counter.";

    // Store the program counter into the state.
    const auto pc_reg_ptr = pc_reg->AddressOf(state_ptr, entry);
    const auto trace_pc =
        llvm::ConstantInt::get(pc_reg->type, g_entry_address, false);
    ir.SetInsertPoint(entry);
    ir.CreateStore(trace_pc, pc_reg_ptr);

    auto args_it = func->arg_begin();
    for (auto &reg_name : input_reg_names) {
      const auto reg = arch->RegisterByName(reg_name.str());
      auto &arg = *++args_it;  // Pre-increment, as first arg is memory pointer.
      arg.setName(reg_name);
      CHECK_EQ(arg.getType(), reg->type);
      auto reg_ptr = reg->AddressOf(state_ptr, entry);
      ir.SetInsertPoint(entry);
      ir.CreateStore(&arg, reg_ptr);
    }

    llvm::Value *mem_ptr = &*func->arg_begin();

    llvm::Value *trace_args[remill::kNumBlockArgs] = {};
    trace_args[remill::kStatePointerArgNum] = state_ptr;
    trace_args[remill::kMemoryPointerArgNum] = mem_ptr;
    trace_args[remill::kPCArgNum] = trace_pc;

    mem_ptr = ir.CreateCall(entry_trace, trace_args);

    // Go read all output registers out of the state and store them
    // into the output parameters.
    args_it = func->arg_begin();
    for (size_t i = 0, j = 0; i < func->arg_size(); ++i, ++args_it) {
      if (i < first_output_reg_index) {
        continue;
      }

      const auto &reg_name = output_reg_names[j++];
      const auto reg = arch->RegisterByName(reg_name.str());
      auto &arg = *args_it;
      arg.setName(reg_name + "_output");

      auto reg_ptr = reg->AddressOf(state_ptr, entry);
      ir.SetInsertPoint(entry);
      ir.CreateStore(ir.CreateLoad(reg->type, reg_ptr), &arg);
    }

    // Return the memory pointer, so that all memory accesses are
    // preserved.
    ir.CreateRet(mem_ptr);

    // We want the stack-allocated `State` to be subject to scalarization
    // and mem2reg, but to "encourage" that, we need to prevent the
    // `alloca`d `State` from escaping.
    MuteStateEscape(&dest_module, "__remill_error");
    MuteStateEscape(&dest_module, "__remill_function_call");
    MuteStateEscape(&dest_module, "__remill_function_return");
    MuteStateEscape(&dest_module, "__remill_jump");
    MuteStateEscape(&dest_module, "__remill_missing_block");

    guide.slp_vectorize = true;
    guide.loop_vectorize = true;
    remill::OptimizeBareModule(&dest_module, guide);
  }

  int ret = EXIT_SUCCESS;

  if (!g_ir_out.empty()) {
    if (!remill::StoreModuleIRToFile(&dest_module, g_ir_out, true)) {
      LOG(ERROR) << "Could not save LLVM IR to " << g_ir_out;
      ret = EXIT_FAILURE;
    }
  }
  if (!g_bc_out.empty()) {
    if (!remill::StoreModuleToFile(&dest_module, g_bc_out, true)) {
      LOG(ERROR) << "Could not save LLVM bitcode to " << g_bc_out;
      ret = EXIT_FAILURE;
    }
  }

  return ret;
}
