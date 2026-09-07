/*
 * Copyright (c) 2020 Trail of Bits, Inc.
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

#include <llvm/Transforms/Utils/Cloning.h>

#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef _WIN32
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

#include "remill/Arch/Arch.h"
#include "remill/Arch/Name.h"
#include "remill/BC/ABI.h"
#include "remill/BC/Annotate.h"
#include "remill/BC/Compat/BitcodeReaderWriter.h"
#include "remill/BC/Compat/CallSite.h"
#include "remill/BC/Compat/DebugInfo.h"
#include "remill/BC/Compat/GlobalValue.h"
#include "remill/BC/Compat/IRReader.h"
#include "remill/BC/Compat/ToolOutputFile.h"
#include "remill/BC/Compat/VectorType.h"
#include "remill/BC/Compat/Verifier.h"
#include "remill/BC/IntrinsicTable.h"
#include "remill/BC/Util.h"
#include "remill/BC/Version.h"
#include "remill/OS/FileSystem.h"

namespace {
#ifdef _WIN32
extern "C" std::uint32_t GetProcessId(std::uint32_t handle);
#endif

// We are avoiding the `getpid` name here to make sure we don't
// conflict with the (deprecated) getpid function from the Windows
// ucrt headers
std::uint32_t nativeGetProcessID(void) {
#ifdef _WIN32
  return GetProcessId(0);
#else
  return getpid();
#endif
}
}  // namespace

namespace remill {

extern const char *kArchDefault;

namespace detail {

// This is an implementation of the non-UB technique to access private
// object/class members which takes advantage of the fact that explicit
// instantiations ignore access control checks of the provided template
// arguments.
//
// See:
// https://bloglitb.blogspot.com/2010/07/access-to-private-members-thats-easy.html

template <typename T, auto tag>
inline T member_pointer_stash = nullptr;

template <auto ptr, typename T, auto tag>
inline const int steal_member_pointer = [] {
  static_assert(std::is_same_v<decltype(ptr), T>);
  member_pointer_stash<decltype(ptr), tag> = ptr;
  return 0;
}();

#define REMILL_BYPASS_MEMBER_OBJECT_ACCESS(ns, cls, member, type) \
  constexpr int __temp_tag_##ns##_##cls##_##member = 0; \
  using __temp_type_##ns##_##cls##_##member = type ns::cls::*; \
  template const int steal_member_pointer<&ns::cls::member, \
                                          __temp_type_##ns##_##cls##_##member, \
                                          &__temp_tag_##ns##_##cls##_##member>

#define REMILL_BYPASS_MEMBER_FUNCTION_ACCESS(ns, cls, member, ret_type, ...) \
  constexpr int __temp_tag_##ns##_##cls##_##member = 0; \
  using __temp_type_##ns##_##cls##_##member = \
      ret_type (ns::cls::*)(__VA_ARGS__); \
  template const int steal_member_pointer<&ns::cls::member, \
                                          __temp_type_##ns##_##cls##_##member, \
                                          &__temp_tag_##ns##_##cls##_##member>

#define REMILL_BYPASS_CONST_MEMBER_FUNCTION_ACCESS(ns, cls, member, ret_type, \
                                                   ...) \
  constexpr int __temp_tag_##ns##_##cls##_##member = 0; \
  using __temp_type_##ns##_##cls##_##member = \
      ret_type (ns::cls::*)(__VA_ARGS__) const; \
  template const int steal_member_pointer<&ns::cls::member, \
                                          __temp_type_##ns##_##cls##_##member, \
                                          &__temp_tag_##ns##_##cls##_##member>

#define REMILL_ACCESS_MEMBER(ns, cls, member) \
  (::remill::detail::member_pointer_stash< \
      ::remill::detail::__temp_type_##ns##_##cls##_##member, \
      &::remill::detail::__temp_tag_##ns##_##cls##_##member>)

REMILL_BYPASS_MEMBER_OBJECT_ACCESS(llvm, Value, VTy, llvm::Type *);

}  // namespace detail

// Initialize the attributes for a lifted function.
void InitFunctionAttributes(llvm::Function *function) {

  // Make sure functions are treated as if they return. LLVM doesn't like
  // mixing must-tail-calls with no-return.
  function->removeFnAttr(llvm::Attribute::NoReturn);

  // Don't use any exception stuff.
  function->removeFnAttr(llvm::Attribute::UWTable);
  function->removeFnAttr(llvm::Attribute::NoInline);
  function->addFnAttr(llvm::Attribute::NoUnwind);
  function->addFnAttr(llvm::Attribute::InlineHint);
}

// Create a call from one lifted function to another.
llvm::CallInst *AddCall(llvm::BasicBlock *source_block, llvm::Value *dest_func,
                        const IntrinsicTable &intrinsics) {
  llvm::IRBuilder<> ir(source_block);
  auto args = LiftedFunctionArgs(source_block, intrinsics);
#if LLVM_VERSION_NUMBER < LLVM_VERSION(11, 0)
  return ir.CreateCall(dest_func, args);
#else
  if (auto func = llvm::dyn_cast<llvm::Function>(dest_func); func) {
    return ir.CreateCall(func, args);

  } else {
    llvm::Type *arg_types[kNumBlockArgs];
    arg_types[kStatePointerArgNum] = args[kStatePointerArgNum]->getType();
    arg_types[kMemoryPointerArgNum] = args[kMemoryPointerArgNum]->getType();
    arg_types[kPCArgNum] = args[kPCArgNum]->getType();
    auto func_type = llvm::FunctionType::get(arg_types[kMemoryPointerArgNum],
                                             arg_types, false);
    llvm::FunctionCallee callee(func_type, dest_func);
    return ir.CreateCall(callee, args);
  }
#endif
}

// Create a tail-call from one lifted function to another.
llvm::CallInst *AddTerminatingTailCall(llvm::Function *source_func,
                                       llvm::Value *dest_func,
                                       const IntrinsicTable &intrinsics) {
  if (source_func->isDeclaration()) {
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(source_func->getContext(), "", source_func));
  }
  return AddTerminatingTailCall(&(source_func->back()), dest_func, intrinsics);
}

llvm::CallInst *AddTerminatingTailCall(llvm::BasicBlock *source_block,
                                       llvm::Value *dest_func,
                                       const IntrinsicTable &intrinsics) {
  assert(nullptr != dest_func);

  (void) source_block;
  (void) dest_func;

  llvm::IRBuilder<> ir(source_block);

  // get the `NEXT_PC` and set it to `PC`
  auto next_pc = LoadNextProgramCounter(source_block, intrinsics);
  auto pc_ref = LoadProgramCounterRef(source_block);
  (void) new llvm::StoreInst(next_pc, pc_ref, source_block);

  auto call_target_instr = AddCall(source_block, dest_func, intrinsics);
  call_target_instr->setTailCall(true);

  ir.CreateRet(call_target_instr);
  return call_target_instr;
}

// Find a local variable defined in the entry block of the function. We use
// this to find register variables.
std::pair<llvm::Value *, llvm::Type *>
FindVarInFunction(llvm::BasicBlock *block, std::string_view name,
                  bool allow_failure) {
  return FindVarInFunction(block->getParent(), name, allow_failure);
}

// Find a local variable defined in the entry block of the function. We use
// this to find register variables.
std::pair<llvm::Value *, llvm::Type *>
FindVarInFunction(llvm::Function *function, std::string_view name_,
                  bool allow_failure) {
  llvm::StringRef name(name_.data(), name_.size());

  // Check function arguments by name (needed for flat-mode pointer args
  // like PC, NEXT_PC that are named function parameters).
  for (auto &arg : function->args()) {
    if (arg.getName() == name) {
      return {&arg, arg.getType()};
    }
  }

  if (!function->empty()) {
    for (auto &instr : function->getEntryBlock()) {
      if (instr.getName() == name) {
        if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instr)) {
          return {alloca, alloca->getAllocatedType()};
        }
        if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instr)) {
          return {gep, gep->getResultElementType()};
        }
        // AddressOf() returns a BitCast of the GEP; accept it as a pointer.
        if (auto *bc = llvm::dyn_cast<llvm::BitCastInst>(&instr)) {
          return {bc, bc->getType()};
        }
      }
    }
  }

  auto module = function->getParent();
  if (auto var = module->getGlobalVariable(name)) {
    return {var, var->getValueType()};
  }

  assert(allow_failure);
  return {nullptr, nullptr};
}

// Find the machine state pointer.
llvm::Value *LoadStatePointer(llvm::Function *function) {
  if (kNumFlatBlockArgs == function->arg_size()) {
    // Flat mode: the state is the local `State` alloca in the entry block.
    auto &entry = function->getEntryBlock();
    for (auto &inst : entry) {
      if (auto alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
        if (alloca->getName() == "STATE_LOCAL") {
          return alloca;
        }
      }
    }
    // Pure-SSA flat mode: no state alloca. Return the PC arg as a
    // placeholder; the tail call will be retargeted by
    // FinishFlatLiftedFunction.
    return NthArgument(function, kFlatPCArgNum);
  }

  CHECK(kNumBlockArgs == function->arg_size())
      << "Invalid block-like function. Expected three arguments: state "
      << "pointer, program counter, and memory pointer in function "
      << function->getName().str();

  static_assert(0 == kStatePointerArgNum,
                "Expected state pointer to be the first operand.");

  return NthArgument(function, kStatePointerArgNum);
}

// Return the memory pointer argument.
llvm::Value *LoadMemoryPointerArg(llvm::Function *function) {
  if (kNumFlatBlockArgs == function->arg_size()) {
    return NthArgument(function, kFlatMemoryPointerArgNum);
  }

  CHECK(kNumBlockArgs == function->arg_size())
      << "Invalid block-like function. Expected three arguments: state "
      << "pointer, program counter, and memory pointer in function "
      << function->getName().str();

  static_assert(2 == kMemoryPointerArgNum,
                "Expected state pointer to be the first operand.");

  return NthArgument(function, kMemoryPointerArgNum);
}

// Return the program counter argument.
llvm::Value *LoadProgramCounterArg(llvm::Function *function) {
  if (kNumFlatBlockArgs == function->arg_size()) {
    return NthArgument(function, kFlatPCArgNum);
  }

  CHECK(kNumBlockArgs == function->arg_size())
      << "Invalid block-like function. Expected three arguments: state "
      << "pointer, program counter, and memory pointer in function "
      << function->getName().str();

  static_assert(1 == kPCArgNum,
                "Expected state pointer to be the first operand.");

  return NthArgument(function, kPCArgNum);
}

llvm::Value *LoadStatePointer(llvm::BasicBlock *block) {
  return LoadStatePointer(block->getParent());
}

// Return the current program counter.
llvm::Value *LoadProgramCounter(llvm::BasicBlock *block,
                                const IntrinsicTable &intrinsics) {
  llvm::IRBuilder<> ir(block);
  return ir.CreateLoad(intrinsics.pc_type, LoadProgramCounterRef(block));
}

// Return a reference to the current program counter.
llvm::Value *LoadProgramCounterRef(llvm::BasicBlock *block) {
  return FindVarInFunction(block->getParent(), kPCVariableName).first;
}

// Return a reference to the next program counter.
llvm::Value *LoadNextProgramCounterRef(llvm::BasicBlock *block) {
  return FindVarInFunction(block->getParent(), kNextPCVariableName).first;
}

// Return the next program counter.
llvm::Value *LoadNextProgramCounter(llvm::BasicBlock *block,
                                    const IntrinsicTable &intrinsics) {
  llvm::IRBuilder<> ir(block);
  return ir.CreateLoad(intrinsics.pc_type, LoadNextProgramCounterRef(block));
}

// Return a reference to the return program counter.
llvm::Value *LoadReturnProgramCounterRef(llvm::BasicBlock *block) {
  return FindVarInFunction(block->getParent(), kReturnPCVariableName).first;
}

// Update the program counter in the state struct with a new value.
void StoreProgramCounter(llvm::BasicBlock *block, llvm::Value *pc) {
  (void) new llvm::StoreInst(pc, LoadProgramCounterRef(block), block);
}

// Update the next program counter in the state struct with a new value.
void StoreNextProgramCounter(llvm::BasicBlock *block, llvm::Value *pc) {
  (void) new llvm::StoreInst(pc, LoadNextProgramCounterRef(block), block);
}

// Update the program counter in the state struct with a hard-coded value.
void StoreProgramCounter(llvm::BasicBlock *block, uint64_t pc,
                         const IntrinsicTable &intrinsics) {
  auto pc_ptr = LoadProgramCounterRef(block);
  (void) new llvm::StoreInst(llvm::ConstantInt::get(intrinsics.pc_type, pc),
                             pc_ptr, block);
}

// Return the current memory pointer.
llvm::Value *LoadMemoryPointer(llvm::BasicBlock *block,
                               const IntrinsicTable &intrinsics) {
  llvm::IRBuilder<> ir(block);
  return ir.CreateLoad(intrinsics.mem_ptr_type, LoadMemoryPointerRef(block));
}

// Return an `llvm::Value *` that is an `i1` (bool type) representing whether
// or not a conditional branch is taken.
llvm::Value *LoadBranchTaken(llvm::BasicBlock *block) {
  llvm::IRBuilder<> ir(block);
  auto i8_type = llvm::Type::getInt8Ty(block->getContext());
  auto bt_ref = FindVarInFunction(block->getParent(), kBranchTakenVariableName).first;
  if (!bt_ref) {
    // Fallback: create a local alloca so we don't crash.
    // Branches won't work correctly, but this avoids the null-operand crash.
    auto *bt_alloca = ir.CreateAlloca(i8_type, nullptr, "BRANCH_TAKEN");
    ir.CreateStore(llvm::ConstantInt::get(i8_type, 0), bt_alloca);
    bt_ref = bt_alloca;
  }
  auto cond = ir.CreateLoad(i8_type, bt_ref);
  auto true_val = llvm::ConstantInt::get(cond->getType(), 1);
  return ir.CreateICmpEQ(cond, true_val);
}

// Return a reference to the branch taken
llvm::Value *LoadBranchTakenRef(llvm::BasicBlock *block) {
  return FindVarInFunction(block->getParent(), kBranchTakenVariableName).first;
}

// Return a reference to the memory pointer.
llvm::Value *LoadMemoryPointerRef(llvm::BasicBlock *block) {
  return FindVarInFunction(block->getParent(), kMemoryVariableName).first;
}

// Find a function with name `name` in the module `M`.
llvm::Function *FindFunction(llvm::Module *module, std::string_view name_) {
  llvm::StringRef name(name_.data(), name_.size());
  return module->getFunction(name);
}

// Find a global variable with name `name` in the module `M`.
llvm::GlobalVariable *FindGlobaVariable(llvm::Module *module,
                                        std::string_view name_) {
  llvm::StringRef name(name_.data(), name_.size());
  return module->getGlobalVariable(name, true);
}

// Loads the semantics for the `arch`-specific machine, i.e. the machine of the
// code that we want to lift.
std::unique_ptr<llvm::Module> LoadArchSemantics(const Arch *arch) {
  auto arch_name = GetArchName(arch->arch_name);
  std::string path = FindSemanticsBitcodeFile(arch_name);
  llvm::outs() << "Loading " << arch_name << " semantics from file " << path
               << "\n";
  auto module = LoadModuleFromFile(arch->context, path);
  arch->PrepareModule(module);
  arch->InitFromSemanticsModule(module.get());
  for (auto &func : *module) {
    Annotate<remill::Semantics>(&func);
  }
  return module;
}

// Try to verify a module.
bool VerifyModule(llvm::Module *module) {
  std::string error;
  llvm::raw_string_ostream error_stream(error);
  if (llvm::verifyModule(*module, &error_stream)) {
    error_stream.flush();
    LOG(ERROR) << "Error verifying module read from file: " << error;
    return false;
  } else {
    return true;
  }
}

// Reads an LLVM module from a file.
std::unique_ptr<llvm::Module>
LoadModuleFromFile(llvm::LLVMContext *context,
                   std::filesystem::path file_name) {
  llvm::SMDiagnostic err;
  auto module = llvm::parseIRFile(file_name.string(), err, *context);

  if (!module) {
    llvm::outs() << "Unable to parse module file " << file_name.string() << ": "
                 << err.getMessage().str();
    return {};
  }

  auto ec = module->materializeAll();  // Just in case.
  if (ec) {
    LOG(ERROR) << "Unable to materialize everything from " << file_name;
    return {};
  }

  if (!VerifyModule(module.get())) {
    LOG(ERROR) << "Error verifying module read from file " << file_name;
    return {};
  }

  return module;
}

// Store an LLVM module into a file.
bool StoreModuleToFile(llvm::Module *module, std::string_view file_name,
                       bool allow_failure) {
  DLOG(INFO) << "Saving bitcode to file " << file_name;

  std::stringstream ss;
  ss << file_name << ".tmp." << nativeGetProcessID();
  auto tmp_name = ss.str();

  std::string error;
  llvm::raw_string_ostream error_stream(error);

  if (llvm::verifyModule(*module, &error_stream)) {
    error_stream.flush();
    assert(allow_failure);
    (void) file_name;
    (void) error;
    return false;
  }

#if LLVM_VERSION_NUMBER > LLVM_VERSION(3, 5)
  std::error_code ec;
#  if LLVM_VERSION_NUMBER < LLVM_VERSION(7, 0)
  llvm::ToolOutputFile bc(tmp_name.c_str(), ec, llvm::sys::fs::F_RW);
#  else
  llvm::ToolOutputFile bc(tmp_name.c_str(), ec, llvm::sys::fs::OF_None);
#  endif
  assert(!ec);
#else
  llvm::tool_output_file bc(tmp_name.c_str(), error, llvm::sys::fs::F_RW);
  CHECK(error.empty() && !bc.os().has_error())
      << "Unable to open output bitcode file for writing: " << tmp_name << ": "
      << error;
#endif

#if LLVM_VERSION_NUMBER < LLVM_VERSION(7, 0)
  llvm::WriteBitcodeToFile(module, bc.os());
#else
  llvm::WriteBitcodeToFile(*module, bc.os());
#endif
  bc.keep();
  if (!bc.os().has_error()) {
    std::string file_name_(file_name.data(), file_name.size());
    MoveFile(tmp_name, file_name_);
    return true;

  } else {
    RemoveFile(tmp_name);
    assert(allow_failure);
    (void) file_name;
    return false;
  }
}

// Store a module, serialized to LLVM IR, into a file.
bool StoreModuleIRToFile(llvm::Module *module, std::string_view file_name_,
                         bool allow_failure) {
  std::string file_name(file_name_.data(), file_name_.size());
#if LLVM_VERSION_NUMBER > LLVM_VERSION(3, 5)
  std::error_code ec;
  llvm::raw_fd_ostream dest(file_name.c_str(), ec, llvm::sys::fs::OF_Text);
  auto good = !ec;
  auto error = ec.message();
#else
  std::string error;
  llvm::raw_fd_ostream dest(file_name.c_str(), error, llvm::sys::fs::OF_Text);
  auto good = error.empty();
#endif
  if (!good) {
    assert(!allow_failure);
    (void) file_name;
    (void) error;
    return false;
  }
  module->print(dest, nullptr);
  return true;
}

namespace {

#ifndef REMILL_BUILD_SEMANTICS_DIR_X86
#  error \
      "Macro `REMILL_BUILD_SEMANTICS_DIR_X86` must be defined to support X86 and AMD64 architectures."
#  define REMILL_BUILD_SEMANTICS_DIR_X86
#endif  // REMILL_BUILD_SEMANTICS_DIR_X86

#ifndef REMILL_INSTALL_SEMANTICS_DIR
#  error "Macro `REMILL_INSTALL_SEMANTICS_DIR` must be defined."
#  define REMILL_INSTALL_SEMANTICS_DIR
#endif  // REMILL_INSTALL_SEMANTICS_DIR

#define _S(x) #x
#define S(x) _S(x)
#define MAJOR_MINOR S(LLVM_VERSION_MAJOR) "." S(LLVM_VERSION_MINOR)

static const char *gSemanticsSearchPaths[] = {

    // Derived from the build.
    REMILL_BUILD_SEMANTICS_DIR_X86 "\0",
    REMILL_INSTALL_SEMANTICS_DIR "\0",
    "/usr/local/share/remill/" MAJOR_MINOR "/semantics",
    "/usr/share/remill/" MAJOR_MINOR "/semantics",
    "/share/remill/" MAJOR_MINOR "/semantics",
    "bc"};

}  // namespace

// Find the path to the semantics bitcode file associated with `kArchDefault`.
std::string FindTargetSemanticsBitcodeFile(void) {
  return FindSemanticsBitcodeFile(kArchDefault);
}

// Find the path to the semantics bitcode file associated with `REMILL_ARCH`,
// the architecture on which remill is compiled.
std::string FindHostSemanticsBitcodeFile(void) {
  return FindSemanticsBitcodeFile(REMILL_ARCH);
}

// Find the path to the semantics bitcode file.
std::string FindSemanticsBitcodeFile(std::string_view arch) {
  for (auto sem_dir : gSemanticsSearchPaths) {
    std::stringstream ss;
    ss << sem_dir << "/" << arch << ".bc";
    if (auto sem_path = ss.str(); FileExists(sem_path)) {
      return sem_path;
    }
  }

  assert(false);
  return "";
}

// Find the flat-mode semantics bitcode file (`<arch>_flat.bc`).
// Falls back to the regular bitcode if the flat version doesn't exist.
std::string FindFlatSemanticsBitcodeFile(std::string_view arch) {
  for (auto sem_dir : gSemanticsSearchPaths) {
    std::stringstream ss;
    ss << sem_dir << "/" << arch << "_flat.bc";
    if (auto sem_path = ss.str(); FileExists(sem_path)) {
      return sem_path;
    }
  }
  // Fall back to the regular bitcode (no _flat wrappers available).
  return FindSemanticsBitcodeFile(arch);
}

namespace {

// Convert an LLVM thing (e.g. `llvm::Value` or `llvm::Type`) into
// a `std::string`.
template <typename T>
inline static std::string DoLLVMThingToString(T *thing) {
  if (thing) {
    std::string str;
    llvm::raw_string_ostream str_stream(str);
    thing->print(str_stream);
    return str;
  } else {
    return "(null)";
  }
}

}  // namespace

std::string LLVMThingToString(llvm::Value *thing) {
  return DoLLVMThingToString(thing);
}

std::string LLVMThingToString(llvm::Type *thing) {
  return DoLLVMThingToString(thing);
}

llvm::Argument *NthArgument(llvm::Function *func, size_t index) {
  auto it = func->arg_begin();
  if (index >= static_cast<size_t>(std::distance(it, func->arg_end()))) {
    return nullptr;
  }
  std::advance(it, index);
  return &*it;
}

// Return a vector of arguments to pass to a lifted function, where the
// arguments are derived from `block`.
std::array<llvm::Value *, kNumBlockArgs>
LiftedFunctionArgs(llvm::BasicBlock *block, const IntrinsicTable &intrinsics) {
  auto func = block->getParent();

  // Set up arguments according to our ABI.
  std::array<llvm::Value *, kNumBlockArgs> args;

  if (FindVarInFunction(func, kPCVariableName, true).first) {
    args[kMemoryPointerArgNum] = LoadMemoryPointer(block, intrinsics);
    args[kStatePointerArgNum] = LoadStatePointer(block);
    args[kPCArgNum] = LoadProgramCounter(block, intrinsics);
  } else {
    args[kMemoryPointerArgNum] = NthArgument(func, kMemoryPointerArgNum);
    args[kStatePointerArgNum] = NthArgument(func, kStatePointerArgNum);
    args[kPCArgNum] = NthArgument(func, kPCArgNum);
  }

  return args;
}

// Apply a callback function to every semantics bitcode function.
void ForEachISel(llvm::Module *module, ISelCallback callback) {
  for (auto &global : module->globals()) {
    const auto &name = global.getName();
    if (name.starts_with("ISEL_") || name.starts_with("COND_")) {
      llvm::Function *sem = nullptr;
      if (global.hasInitializer()) {
        sem = llvm::dyn_cast<llvm::Function>(
            global.getInitializer()->stripPointerCasts());
      }
      callback(&global, sem);
    }
  }
}

// Clone function `source_func` into `dest_func`. This will strip out debug
// info during the clone.
void CloneFunctionInto(llvm::Function *source_func, llvm::Function *dest_func) {
  auto new_args = dest_func->arg_begin();
  ValueMap value_map;
  TypeMap type_map;
  MDMap md_map;
  for (llvm::Argument &old_arg : source_func->args()) {
    new_args->setName(old_arg.getName());
    value_map[&old_arg] = &*new_args;
    ++new_args;
  }

  CHECK_EQ(RecontextualizeType(source_func->getFunctionType(),
                               dest_func->getContext()),
           dest_func->getFunctionType());

  CloneFunctionInto(source_func, dest_func, value_map, type_map, md_map);
}

// Returns a list of callers of a specific function.
std::vector<llvm::CallInst *> CallersOf(llvm::Function *func) {
  if (!func) {
    return {};
  }

  std::vector<llvm::CallInst *> callers;
  for (auto user : func->users()) {
    if (auto cs = compat::llvm::CallSite(user); cs.isCall()) {
      if (cs.getCalledFunction() == func) {
        callers.push_back(llvm::cast<llvm::CallInst>(user));
      }
    }
  }
  return callers;
}

// Returns the name of a module.
std::string ModuleName(llvm::Module *module) {
#if LLVM_VERSION_NUMBER < LLVM_VERSION(3, 6)
  return module->getModuleIdentifier();
#else
  return module->getName().str();
#endif
}

std::string ModuleName(const std::unique_ptr<llvm::Module> &module) {
  return ModuleName(module.get());
}

namespace {

#if 0
static llvm::Constant *CloneConstant(llvm::Constant *val);

static std::vector<llvm::Constant *> CloneContents(
    llvm::ConstantAggregate *agg) {
  auto num_elems = agg->getNumOperands();
  std::vector<llvm::Constant *> clones(num_elems);
  for (auto i = 0U; i < num_elems; ++i) {
    clones[i] = CloneConstant(agg->getAggregateElement(i));
  }
  return clones;
}

static llvm::Constant *CloneConstant(llvm::Constant *val) {
  if (llvm::isa<llvm::ConstantData>(val) ||
      llvm::isa<llvm::ConstantAggregateZero>(val)) {
    return val;
  }

  std::vector<llvm::Constant *> elements;
  if (auto agg = llvm::dyn_cast<llvm::ConstantAggregate>(val)) {
    CloneContents(agg);
  }

  if (auto arr = llvm::dyn_cast<llvm::ConstantArray>(val)) {
    return llvm::ConstantArray::get(arr->getType(), elements);

  } else if (auto vec = llvm::dyn_cast<llvm::ConstantVector>(val)) {
    return llvm::ConstantVector::get(elements);

  } else if (auto obj = llvm::dyn_cast<llvm::ConstantStruct>(val)) {
    return llvm::ConstantStruct::get(obj->getType(), elements);

  } else {
    assert(false);
    return val;
  }
}

#endif

static llvm::Function *DeclareFunctionInModule(llvm::Function *func,
                                               llvm::Module *dest_module,
                                               ValueMap &value_map) {

  auto &moved_func = value_map[func];
  if (moved_func) {
    return llvm::dyn_cast<llvm::Function>(moved_func);
  }

  auto dest_func = dest_module->getFunction(func->getName());
  if (dest_func) {
    CHECK_EQ(
        RecontextualizeType(func->getFunctionType(), dest_module->getContext()),
        dest_func->getFunctionType());

    moved_func = dest_func;
    return dest_func;
  }

  assert(!func->hasLocalLinkage());

  const auto func_type = llvm::dyn_cast<llvm::FunctionType>(
      RecontextualizeType(func->getFunctionType(), dest_module->getContext()));

  dest_func = llvm::Function::Create(func_type, func->getLinkage(),
                                     func->getName(), dest_module);

  dest_func->copyAttributesFrom(func);
  dest_func->setVisibility(func->getVisibility());
  dest_func->setCallingConv(func->getCallingConv());
  if (func->hasSection()) {
    dest_func->setSection(func->getSection());
  }

  moved_func = dest_func;
  return dest_func;
}

static llvm::GlobalVariable *
DeclareVarInModule(llvm::GlobalVariable *var, llvm::Module *dest_module,
                   ValueMap &value_map, TypeMap &type_map);

static llvm::GlobalAlias *
DeclareAliasInModule(llvm::GlobalAlias *var, llvm::Module *dest_module,
                     ValueMap &value_map, TypeMap &type_map);

template <typename T>
static void ClearMetaData(T *value) {
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> mds;
  value->getAllMetadata(mds);
  for (auto md_info : mds) {
    value->setMetadata(md_info.first, nullptr);
  }
}

static llvm::Type *RecontextualizeType(llvm::Type *type,
                                       llvm::LLVMContext &context,
                                       TypeMap &cache) {
  if (&(type->getContext()) == &context) {
    return type;
  }

  auto &cached = cache[type];
  if (cached) {
    return cached;
  }

  switch (type->getTypeID()) {
    case llvm::Type::VoidTyID: return llvm::Type::getVoidTy(context);
    case llvm::Type::HalfTyID: return llvm::Type::getHalfTy(context);
    case llvm::Type::FloatTyID: return llvm::Type::getFloatTy(context);
    case llvm::Type::DoubleTyID: return llvm::Type::getDoubleTy(context);
    case llvm::Type::X86_FP80TyID: return llvm::Type::getX86_FP80Ty(context);
    case llvm::Type::FP128TyID: return llvm::Type::getFP128Ty(context);
    case llvm::Type::PPC_FP128TyID: return llvm::Type::getPPC_FP128Ty(context);
    case llvm::Type::LabelTyID: return llvm::Type::getLabelTy(context);
    case llvm::Type::MetadataTyID: return llvm::Type::getMetadataTy(context);
    //case llvm::Type::X86_MMXTyID: return llvm::Type::getX86_MMXTy(context);
    case llvm::Type::TokenTyID: return llvm::Type::getTokenTy(context);
    case llvm::Type::IntegerTyID: {
      auto int_type = llvm::dyn_cast<llvm::IntegerType>(type);
      cached =
          llvm::IntegerType::get(context, int_type->getPrimitiveSizeInBits());
      break;
    }
    case llvm::Type::FunctionTyID: {
      auto func_type = llvm::dyn_cast<llvm::FunctionType>(type);
      auto ret_type =
          RecontextualizeType(func_type->getReturnType(), context, cache);
      llvm::SmallVector<llvm::Type *, 4> param_types;
      for (auto param_type : func_type->params()) {
        param_types.push_back(RecontextualizeType(param_type, context, cache));
      }
      cached =
          llvm::FunctionType::get(ret_type, param_types, func_type->isVarArg());
      break;
    }

    case llvm::Type::StructTyID: {
      auto struct_type = llvm::dyn_cast<llvm::StructType>(type);
      llvm::StructType *new_struct_type = nullptr;
      if (struct_type->isLiteral()) {
        new_struct_type = llvm::StructType::create(context);
      } else {
        new_struct_type =
            llvm::StructType::create(context, struct_type->getName());
      }
      cached = new_struct_type;

      llvm::SmallVector<llvm::Type *, 4> elem_types;
      for (auto elem_type : struct_type->elements()) {
        elem_types.push_back(RecontextualizeType(elem_type, context, cache));
      }

      if (elem_types.size()) {
        new_struct_type->setBody(elem_types, struct_type->isPacked());
      }

      return new_struct_type;
    }

    case llvm::Type::ArrayTyID: {
      auto arr_type = llvm::dyn_cast<llvm::ArrayType>(type);
      auto elem_type = arr_type->getElementType();
      cached =
          llvm::ArrayType::get(RecontextualizeType(elem_type, context, cache),
                               arr_type->getNumElements());
      break;
    }

    case llvm::Type::PointerTyID: {
      auto ptr_type = llvm::dyn_cast<llvm::PointerType>(type);
      cached = llvm::PointerType::get(context, ptr_type->getAddressSpace());
      break;
    }

    case llvm::GetFixedVectorTypeId(): {
      auto arr_type = llvm::dyn_cast<llvm::FixedVectorType>(type);
      auto elem_type = arr_type->getElementType();
      cached = llvm::FixedVectorType::get(
          RecontextualizeType(elem_type, context, cache),
          arr_type->getNumElements());
      break;
    }

    default:
      assert(false);
      return nullptr;
  }

  return cached;
}

static llvm::Constant *
MoveConstantIntoModule(llvm::Constant *c, llvm::Module *dest_module,
                       ValueMap &value_map, TypeMap &type_map) {

  auto &moved_c = value_map[c];
  if (moved_c) {
    return llvm::dyn_cast<llvm::Constant>(moved_c);
  }

  auto &dest_context = dest_module->getContext();
  auto type = c->getType();
  const auto in_same_context = &(c->getContext()) == &dest_context;
  if (!in_same_context) {
    type = RecontextualizeType(type, dest_context, type_map);
  }

  if (auto gv = llvm::dyn_cast<llvm::GlobalVariable>(c); gv) {
    return DeclareVarInModule(gv, dest_module, value_map, type_map);

  } else if (auto ga = llvm::dyn_cast<llvm::GlobalAlias>(c); ga) {
    return DeclareAliasInModule(ga, dest_module, value_map, type_map);

  } else if (auto func = llvm::dyn_cast<llvm::Function>(c); func) {
    return DeclareFunctionInModule(func, dest_module, value_map);

  } else if (auto d = llvm::dyn_cast<llvm::ConstantData>(c)) {
    if (auto ci = llvm::dyn_cast<llvm::ConstantInt>(d); ci) {
      if (in_same_context) {
        moved_c = ci;
        return ci;
      } else {
        auto ret = llvm::ConstantInt::get(type, ci->getValue());
        moved_c = ret;
        return ret;
      }
    } else if (auto cf = llvm::dyn_cast<llvm::ConstantFP>(d); cf) {
      if (in_same_context) {
        moved_c = cf;
        return cf;
      } else {
        auto ret = llvm::ConstantFP::get(type, cf->getValueAPF());
        moved_c = ret;
        return ret;
      }
    } else if (auto u = llvm::dyn_cast<llvm::UndefValue>(d); u) {
      if (in_same_context) {
        moved_c = u;
        return u;
      } else {
        auto ret = llvm::UndefValue::get(type);
        moved_c = ret;
        return ret;
      }
    } else if (auto p = llvm::dyn_cast<llvm::ConstantPointerNull>(d); p) {
      if (in_same_context) {
        moved_c = p;
        return p;
      } else {
        auto ret =
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(type));
        moved_c = ret;
        return ret;
      }
    } else if (auto z = llvm::dyn_cast<llvm::ConstantAggregateZero>(d); z) {
      if (in_same_context) {
        moved_c = z;
        return z;
      } else {
        auto ret = llvm::ConstantAggregateZero::get(type);
        moved_c = ret;
        return ret;
      }
    } else if (auto a = llvm::dyn_cast<llvm::ConstantDataArray>(d); a) {
      if (in_same_context) {
        moved_c = a;
        return a;

      } else {
        const auto raw_data = a->getRawDataValues();
        const auto el_type = a->getElementType();
        if (el_type->isIntegerTy()) {
          switch (a->getElementByteSize()) {
            case 1: {
              auto ret = llvm::ConstantDataArray::get(
                  dest_context, llvm::arrayRefFromStringRef(raw_data));
              moved_c = ret;
              return ret;
            }
            case 2: {
              llvm::ArrayRef<uint16_t> ref(
                  reinterpret_cast<const uint16_t *>(raw_data.bytes_begin()),
                  reinterpret_cast<const uint16_t *>(raw_data.bytes_end()));
              auto ret = llvm::ConstantDataArray::get(dest_context, ref);
              moved_c = ret;
              return ret;
            }
            case 4: {
              llvm::ArrayRef<uint32_t> ref(
                  reinterpret_cast<const uint32_t *>(raw_data.bytes_begin()),
                  reinterpret_cast<const uint32_t *>(raw_data.bytes_end()));
              auto ret = llvm::ConstantDataArray::get(dest_context, ref);
              moved_c = ret;
              return ret;
            }
            case 8: {
              llvm::ArrayRef<uint64_t> ref(
                  reinterpret_cast<const uint64_t *>(raw_data.bytes_begin()),
                  reinterpret_cast<const uint64_t *>(raw_data.bytes_end()));
              auto ret = llvm::ConstantDataArray::get(dest_context, ref);
              moved_c = ret;
              return ret;
            }
          }
        } else if (el_type->isFloatTy()) {
          llvm::ArrayRef<float> ref(
              reinterpret_cast<const float *>(raw_data.bytes_begin()),
              reinterpret_cast<const float *>(raw_data.bytes_end()));
          auto ret = llvm::ConstantDataArray::get(dest_context, ref);
          moved_c = ret;
          return ret;

        } else if (el_type->isDoubleTy()) {
          llvm::ArrayRef<double> ref(
              reinterpret_cast<const double *>(raw_data.bytes_begin()),
              reinterpret_cast<const double *>(raw_data.bytes_end()));
          auto ret = llvm::ConstantDataArray::get(dest_context, ref);
          moved_c = ret;
          return ret;
        }

        assert(false);
        return nullptr;
      }
    } else if (auto v = llvm::dyn_cast<llvm::ConstantDataVector>(d); v) {
      if (in_same_context) {
        moved_c = v;
        return v;
      } else {
        assert(false);
        return nullptr;
      }

    } else if (in_same_context) {
      LOG(ERROR) << "Not adapting constant when moving to destination module: "
                 << LLVMThingToString(c);
      moved_c = c;
      return c;

    } else {
      assert(false);
      return nullptr;
    }
  } else if (auto ce = llvm::dyn_cast<llvm::ConstantExpr>(c)) {
    switch (ce->getOpcode()) {
      case llvm::Instruction::Add: {
        const auto b = llvm::dyn_cast<llvm::AddOperator>(ce);
        auto ret = llvm::ConstantExpr::getAdd(
            MoveConstantIntoModule(ce->getOperand(0), dest_module, value_map,
                                   type_map),
            MoveConstantIntoModule(ce->getOperand(1), dest_module, value_map,
                                   type_map),
            b->hasNoUnsignedWrap(), b->hasNoSignedWrap());
        moved_c = ret;
        return ret;
      }
      case llvm::Instruction::Sub: {
        const auto b = llvm::dyn_cast<llvm::SubOperator>(ce);
        auto ret = llvm::ConstantExpr::getSub(
            MoveConstantIntoModule(ce->getOperand(0), dest_module, value_map,
                                   type_map),
            MoveConstantIntoModule(ce->getOperand(1), dest_module, value_map,
                                   type_map),
            b->hasNoUnsignedWrap(), b->hasNoSignedWrap());
        moved_c = ret;
        return ret;
      }
      case llvm::Instruction::Xor: {
        auto ret = llvm::ConstantExpr::getXor(
            MoveConstantIntoModule(ce->getOperand(0), dest_module, value_map,
                                   type_map),
            MoveConstantIntoModule(ce->getOperand(1), dest_module, value_map,
                                   type_map));
        moved_c = ret;
        return ret;
      }
      case llvm::Instruction::Trunc: {
        auto ret = llvm::ConstantExpr::getTrunc(
            MoveConstantIntoModule(ce->getOperand(0), dest_module, value_map,
                                   type_map),
            type);
        moved_c = ret;
        return ret;
      }
      case llvm::Instruction::IntToPtr: {
        auto ret = llvm::ConstantExpr::getIntToPtr(
            MoveConstantIntoModule(ce->getOperand(0), dest_module, value_map,
                                   type_map),
            type);
        moved_c = ret;
        return ret;
      }
      case llvm::Instruction::PtrToInt: {
        auto ret = llvm::ConstantExpr::getPtrToInt(
            MoveConstantIntoModule(ce->getOperand(0), dest_module, value_map,
                                   type_map),
            type);
        moved_c = ret;
        return ret;
      }
      case llvm::Instruction::BitCast: {
        auto ret = llvm::ConstantExpr::getBitCast(
            MoveConstantIntoModule(ce->getOperand(0), dest_module, value_map,
                                   type_map),
            type);
        moved_c = ret;
        return ret;
      }
      case llvm::Instruction::AddrSpaceCast: {
        auto ret = llvm::ConstantExpr::getAddrSpaceCast(
            MoveConstantIntoModule(ce->getOperand(0), dest_module, value_map,
                                   type_map),
            type);
        moved_c = ret;
        return ret;
      }
      case llvm::Instruction::GetElementPtr: {
        const auto g = llvm::dyn_cast<llvm::GEPOperator>(ce);
        const auto ni = g->getNumIndices();
        const auto source_type = ::remill::RecontextualizeType(
            g->getSourceElementType(), dest_context);
        std::vector<llvm::Constant *> indices(ni);
        for (auto i = 0u; i < ni; ++i) {
          indices[i] = MoveConstantIntoModule(ce->getOperand(i + 1u),
                                              dest_module, value_map, type_map);
        }
        auto ret = llvm::ConstantExpr::getGetElementPtr(
            source_type,
            MoveConstantIntoModule(ce->getOperand(0), dest_module, value_map,
                                   type_map),
            indices, g->isInBounds(), g->getInRange());
        moved_c = ret;
        return ret;
      }
      default:
        if (auto bop = llvm::dyn_cast<llvm::BinaryOperator>(ce)) {
          auto ret = llvm::ConstantExpr::get(
              ce->getOpcode(),
              MoveConstantIntoModule(ce->getOperand(0), dest_module, value_map,
                                     type_map),
              MoveConstantIntoModule(ce->getOperand(1), dest_module, value_map,
                                     type_map));
          moved_c = ret;
          return ret;

        } else if (auto uop = llvm::dyn_cast<llvm::UnaryOperator>(ce)) {
#if LLVM_VERSION_NUMBER < LLVM_VERSION(16, 0)
          // In LLVM 16, cast is the only unary constexpr.
          if (!uop->isCast()) {
            auto ret = llvm::ConstantExpr::get(
                ce->getOpcode(),
                MoveConstantIntoModule(ce->getOperand(0), dest_module,
                                       value_map, type_map));
            moved_c = ret;
            return ret;
          }
#endif
          CHECK(uop->isCast());
          auto ret = llvm::ConstantExpr::getCast(
              ce->getOpcode(),
              MoveConstantIntoModule(ce->getOperand(0), dest_module, value_map,
                                     type_map),
              RecontextualizeType(ce->getType(), dest_context, type_map));
          moved_c = ret;
          return ret;

        } else if (in_same_context) {
          LOG(ERROR) << "Unsupported CE when moving across module boundaries: "
                     << LLVMThingToString(ce);
          moved_c = ce;
          return ce;

        } else {
          assert(false);
          return nullptr;
        }
    }
  } else if (auto ca = llvm::dyn_cast<llvm::ConstantAggregate>(c); ca) {
    if (auto a = llvm::dyn_cast<llvm::ConstantArray>(ca); a) {
      std::vector<llvm::Constant *> new_elems;
      new_elems.reserve(a->getNumOperands());
      for (auto it = a->op_begin(), end = a->op_end(); it != end; ++it) {
        new_elems.push_back(
            MoveConstantIntoModule(llvm::cast<llvm::Constant>(it->get()),
                                   dest_module, value_map, type_map));
      }

      auto ret = llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(type),
                                          new_elems);
      moved_c = ret;
      return ret;

    } else if (auto s = llvm::dyn_cast<llvm::ConstantStruct>(ca); s) {
      std::vector<llvm::Constant *> new_elems;
      new_elems.reserve(s->getNumOperands());
      for (auto it = s->op_begin(), end = s->op_end(); it != end; ++it) {
        new_elems.push_back(
            MoveConstantIntoModule(llvm::cast<llvm::Constant>(it->get()),
                                   dest_module, value_map, type_map));
      }

      auto ret = llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(type),
                                           new_elems);
      moved_c = ret;
      return ret;

    } else if (auto v = llvm::dyn_cast<llvm::ConstantVector>(ca); v) {
      std::vector<llvm::Constant *> new_elems;
      new_elems.reserve(v->getNumOperands());
      for (auto it = v->op_begin(), end = v->op_end(); it != end; ++it) {
        new_elems.push_back(
            MoveConstantIntoModule(llvm::cast<llvm::Constant>(it->get()),
                                   dest_module, value_map, type_map));
      }

      auto ret = llvm::ConstantVector::get(new_elems);
      moved_c = ret;
      return ret;

    } else if (in_same_context) {
      LOG(ERROR) << "Unsupported CA when moving across module boundaries: "
                 << LLVMThingToString(c);
      moved_c = c;
      return c;

    } else {
      assert(false);
      return nullptr;
    }

  } else if (in_same_context) {
    LOG(ERROR) << "Unsupported constant when moving across module boundaries: "
               << LLVMThingToString(c);
    moved_c = c;
    return c;

  } else {
    assert(false);
    return nullptr;
  }
}

llvm::GlobalVariable *
DeclareVarInModule(llvm::GlobalVariable *var, llvm::Module *dest_module,
                   ValueMap &value_map, TypeMap &type_map) {
  auto &moved_var = value_map[var];
  if (moved_var) {
    return llvm::dyn_cast<llvm::GlobalVariable>(moved_var);
  }

  auto &dest_context = dest_module->getContext();
  const auto type =
      ::remill::RecontextualizeType(var->getValueType(), dest_context);

  auto dest_var = dest_module->getGlobalVariable(var->getName());
  if (dest_var) {
    CHECK_EQ(type, dest_var->getValueType());
    moved_var = dest_var;
    return dest_var;
  }

  dest_var = new llvm::GlobalVariable(
      *dest_module, type, var->isConstant(), var->getLinkage(), nullptr,
      var->getName(), nullptr, var->getThreadLocalMode(),
      var->getType()->getAddressSpace());

  dest_var->copyAttributesFrom(var);
  if (var->hasSection()) {
    dest_var->setSection(var->getSection());
  }

  moved_var = dest_var;

  if (var->hasInitializer() && var->hasLocalLinkage()) {
    const auto initializer = var->getInitializer();
    dest_var->setInitializer(
        MoveConstantIntoModule(initializer, dest_module, value_map, type_map));
  } else {
    assert(!var->hasLocalLinkage());
  }

  return dest_var;
}


llvm::GlobalAlias *
DeclareAliasInModule(llvm::GlobalAlias *var, llvm::Module *dest_module,
                     ValueMap &value_map, TypeMap &type_map) {
  auto &moved_var = value_map[var];
  if (moved_var) {
    return llvm::dyn_cast<llvm::GlobalAlias>(moved_var);
  }

  const auto dest_type = llvm::dyn_cast<llvm::PointerType>(
      RecontextualizeType(var->getType(), dest_module->getContext(), type_map));
  for (auto &alias : dest_module->aliases()) {
    if (alias.getName() == var->getName()) {
      CHECK_EQ(dest_type, alias.getType());
      moved_var = &alias;
      return &alias;
    }
  }

  const auto elem_type = var->getValueType();
  const auto dest_var = llvm::GlobalAlias::create(
      elem_type, var->getType()->getAddressSpace(), var->getLinkage(),
      var->getName(), nullptr, dest_module);

  moved_var = dest_var;
  dest_var->setAliasee(MoveConstantIntoModule(var->getAliasee(), dest_module,
                                              value_map, type_map));

  return dest_var;
}


static void MoveInstructionIntoModule(llvm::Instruction *inst,
                                      llvm::Module *dest_module,
                                      ValueMap &value_map, TypeMap &type_map) {

  // Substitute the operands.
  for (auto &op : inst->operands()) {
    auto new_val_it = value_map.find(op.get());
    if (new_val_it != value_map.end() && new_val_it->second) {
      op.set(new_val_it->second);
      continue;
    }

    if (auto c = llvm::dyn_cast<llvm::Constant>(op.get()); c) {
      op.set(MoveConstantIntoModule(c, dest_module, value_map, type_map));
    }
  }

  // Substitute the source blocks for PHI nodes.
  if (auto phi = llvm::dyn_cast<llvm::PHINode>(inst)) {
    for (auto i = 0UL; i < phi->getNumIncomingValues(); ++i) {
      const auto incoming_block_ = value_map[phi->getIncomingBlock(i)];
      CHECK_NOTNULL(incoming_block_);
      const auto incoming_block =
          llvm::dyn_cast<llvm::BasicBlock>(incoming_block_);
      CHECK_NOTNULL(incoming_block);
      phi->setIncomingBlock(i, incoming_block);
    }

    // Substitute the called function.
  } else if (auto call = llvm::dyn_cast<llvm::CallInst>(inst)) {
    if (auto callee_func = call->getCalledFunction()) {
      if (callee_func->getParent() != dest_module) {
        call->setCalledFunction(
            DeclareFunctionInModule(callee_func, dest_module, value_map));
      }

    } else if (auto callee_val = call->getCalledOperand()) {
      auto &new_callee_val = value_map[callee_val];
      if (!new_callee_val) {
        if (auto callee_const = llvm::dyn_cast<llvm::Constant>(callee_val)) {
          new_callee_val = MoveConstantIntoModule(callee_const, dest_module,
                                                  value_map, type_map);

        } else {
          new_callee_val = callee_val;
        }
      }

      auto dest_func_type =
          llvm::dyn_cast<llvm::FunctionType>(RecontextualizeType(
              call->getFunctionType(), dest_module->getContext(), type_map));
      llvm::FunctionCallee callee(dest_func_type, new_callee_val);
      call->setCalledFunction(callee);
    }
  }
}

llvm::Metadata *CloneMetadataInto(llvm::Module *source_mod,
                                  llvm::Module *dest_mod, llvm::Metadata *md,
                                  ValueMap &value_map, TypeMap &type_map,
                                  MDMap &md_map) {

  llvm::Metadata *mapped_md = nullptr;
  auto [it, added] = md_map.emplace(md, mapped_md);
  if (!added) {
    return it->second;
  }

  llvm::LLVMContext &source_context = source_mod->getContext();
  llvm::LLVMContext &dest_context = dest_mod->getContext();

  if (llvm::ValueAsMetadata *val_md =
          llvm::dyn_cast<llvm::ValueAsMetadata>(md)) {
    llvm::Value *val = val_md->getValue();
    if (auto it = value_map.find(val); it != value_map.end()) {
      llvm::Value *mapped_val = it->second;
      mapped_md = llvm::ValueAsMetadata::get(mapped_val);

    } else if (auto cv = llvm::dyn_cast<llvm::Constant>(val)) {
      llvm::Value *mapped_cv =
          MoveConstantIntoModule(cv, dest_mod, value_map, type_map);
      if (!mapped_cv) {
        return nullptr;  // Couldn't move it.
      }
      mapped_md = llvm::ValueAsMetadata::get(mapped_cv);

    } else {
      return nullptr;
    }

  } else if (llvm::MDString *str = llvm::dyn_cast<llvm::MDString>(md)) {
    if (&source_context == &dest_context) {
      mapped_md = str;
    } else {
      mapped_md = llvm::MDString::get(dest_context, str->getString());
    }

  } else if (llvm::MDTuple *tuple = llvm::dyn_cast<llvm::MDTuple>(md)) {
    std::vector<llvm::Metadata *> mapped_ops;
    for (llvm::Metadata *op : tuple->operands()) {
      auto mapped_op = CloneMetadataInto(source_mod, dest_mod, op, value_map,
                                         type_map, md_map);
      if (!mapped_op) {
        return nullptr;  // Possibly cyclic or just not clonable.
      } else {
        mapped_ops.push_back(mapped_op);
      }
    }
    mapped_md = llvm::MDTuple::get(dest_context, mapped_ops);

    // Not supported.
  } else {
    return nullptr;
  }

  it->second = mapped_md;
  return mapped_md;
}

}  // namespace

// Clone function `source_func` into `dest_func`, using `value_map` to map over
// values. This will strip out debug info during the clone. This will strip out
// debug info during the clone.
//
// Note: this will try to clone globals referenced from the module of
//       `source_func` into the module of `dest_func`.
void CloneFunctionInto(llvm::Function *source_func, llvm::Function *dest_func,
                       ValueMap &value_map, TypeMap &type_map, MDMap &md_map) {

  auto func_name = source_func->getName().str();
  auto source_mod = source_func->getParent();
  auto dest_mod = dest_func->getParent();
  auto &source_context = source_mod->getContext();
  auto &dest_context = dest_func->getContext();

  // Make sure that when we're cloning functions that we don't
  // throw away register names and such.
  dest_func->getContext().setDiscardValueNames(false);

  dest_func->setAttributes(source_func->getAttributes());
  dest_func->setLinkage(source_func->getLinkage());
  dest_func->setVisibility(source_func->getVisibility());
  dest_func->setCallingConv(source_func->getCallingConv());

  dest_func->setIsMaterializable(source_func->isMaterializable());

  // Clone the basic blocks and their instructions.
  std::unordered_map<llvm::BasicBlock *, llvm::BasicBlock *> block_map;
  std::unordered_map<llvm::Instruction *,
                     llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4>>
      inst_mds;
  for (auto &old_block : *source_func) {
    auto new_block = llvm::BasicBlock::Create(dest_func->getContext(),
                                              old_block.getName(), dest_func);
    value_map[&old_block] = new_block;
    block_map[&old_block] = new_block;

    llvm::IRBuilder new_block_builder(new_block);

    for (auto &old_inst : old_block) {
      if (llvm::isa<llvm::DbgInfoIntrinsic>(old_inst)) {
        continue;
      }

      // Keep track of what the metadata should be.
      auto &mds = inst_mds[&old_inst];
      old_inst.getAllMetadata(mds);

      // Clear out the metadata before cloning.
      for (auto [md_id, val] : mds) {
        old_inst.setMetadata(md_id, nullptr);
      }

      auto new_inst = old_inst.clone();

      // Reset the metadata after cloning.
      for (auto [md_id, val] : mds) {
        old_inst.setMetadata(md_id, val);
      }

      new_block_builder.Insert(new_inst);

      value_map[&old_inst] = new_inst;
    }
  }

  // Fixup the references in the cloned instructions so that they point into
  // the cloned function, or point to declared globals in the module containing
  // `dest_func`.
  for (llvm::BasicBlock &old_block : *source_func) {
    for (llvm::Instruction &old_inst : old_block) {
      if (llvm::isa<llvm::DbgInfoIntrinsic>(old_inst)) {
        continue;
      }

      auto new_inst = llvm::dyn_cast<llvm::Instruction>(value_map[&old_inst]);
      new_inst->setDebugLoc(llvm::DebugLoc());
      new_inst->setName(old_inst.getName());

      MoveInstructionIntoModule(new_inst, dest_mod, value_map, type_map);
    }
  }

  // NOTE(pag): All fixed MD kinds are part of the custom map, and are
  //            initialized into the context upon construction. There are about
  //            40 of them.
  llvm::SmallVector<llvm::StringRef, 64> source_md_names;

  source_context.getMDKindNames(source_md_names);

  std::unordered_map<unsigned, unsigned> md_id_map;
  md_id_map.reserve(source_md_names.size());
  for (auto i = 0u; i < source_md_names.size(); ++i) {
    if (&source_context != &dest_context) {
      md_id_map[i] = dest_context.getMDKindID(source_md_names[i]);
    } else {
      md_id_map[i] = i;
    }
  }

  // Now port the metadata.
  for (llvm::BasicBlock &old_block : *source_func) {
    for (llvm::Instruction &old_inst : old_block) {
      if (llvm::isa<llvm::DbgInfoIntrinsic>(old_inst)) {
        continue;
      }

      llvm::Instruction *new_inst =
          llvm::dyn_cast<llvm::Instruction>(value_map[&old_inst]);
      if (!new_inst) {
        continue;
      }

      // Try to convert the metadata over, mapping metadata IDs along
      // the way.
      auto &mds = inst_mds[&old_inst];
      for (auto md_info : mds) {
        llvm::MDNode *new_md = llvm::dyn_cast_or_null<llvm::MDNode>(
            CloneMetadataInto(source_mod, dest_mod, md_info.second, value_map,
                              type_map, md_map));
        if (new_md) {
          new_inst->setMetadata(md_id_map[md_info.first], new_md);
        }
      }
    }
  }
}

// Replace all uses of a constant `old_c` with `new_c` inside of `module`.
//
// Returns the number of constant uses of `old_c`.
unsigned ReplaceAllUsesOfConstant(llvm::Constant *old_c, llvm::Constant *new_c,
                                  llvm::Module *module) {
  std::vector<llvm::Use *> repls;
  for (auto &use : old_c->uses()) {
    repls.emplace_back(&use);
  }

  ValueMap value_map;
  value_map.emplace(old_c, new_c);
  value_map.emplace(new_c, new_c);

  TypeMap type_map;

  auto num_const_uses = 0u;

  while (!repls.empty()) {
    const auto use = repls.back();
    llvm::User *const user = use->getUser();
    repls.pop_back();

    const auto used_c = llvm::dyn_cast<llvm::Constant>(use);
    CHECK_NOTNULL(used_c);

    // Ascend.
    if (auto user_c = llvm::dyn_cast<llvm::Constant>(user)) {
      ++num_const_uses;
      for (auto &user_c_use : user_c->uses()) {
        repls.emplace_back(&user_c_use);
      }

    } else if (auto user_inst = llvm::dyn_cast<llvm::Instruction>(user)) {
      use->set(MoveConstantIntoModule(used_c, module, value_map, type_map));

    } else {
      LOG(ERROR) << "Unrecognized user type";
    }
  }

  return num_const_uses;
}

// Move a function from one module into another module.
//
// TODO(pag): Make this work across distinct `llvm::LLVMContext`s.
void MoveFunctionIntoModule(llvm::Function *func, llvm::Module *dest_module) {
  const auto source_context = &(func->getContext());
  const auto dest_context = &(dest_module->getContext());
  CHECK_EQ(source_context, dest_context)
      << "Cannot move function across two independent LLVM contexts.";

  auto source_module = func->getParent();
  CHECK_NE(source_module, dest_module)
      << "Cannot move function to the same module.";

  const auto func_name = func->getName().str();
  auto existing_decl_in_dest_module = dest_module->getFunction(func_name);
  if (existing_decl_in_dest_module) {
    CHECK_NE(existing_decl_in_dest_module, func);
    CHECK_EQ(existing_decl_in_dest_module->getFunctionType(),
             func->getFunctionType());

    existing_decl_in_dest_module->setName(llvm::Twine::createNull());
    existing_decl_in_dest_module->setLinkage(llvm::GlobalValue::PrivateLinkage);
    existing_decl_in_dest_module->setVisibility(
        llvm::GlobalValue::DefaultVisibility);
  }

  const auto in_same_context = source_context == dest_context;

  // We need to possibly preserve `func` as a declaration in its source module.
  func->setName(llvm::Twine::createNull());
  auto replacement_decl_in_source_module = llvm::Function::Create(
      func->getFunctionType(), func->getLinkage(), func_name, source_module);

  replacement_decl_in_source_module->copyAttributesFrom(func);
  replacement_decl_in_source_module->setVisibility(func->getVisibility());
  replacement_decl_in_source_module->setCallingConv(func->getCallingConv());
  if (func->hasSection()) {
    replacement_decl_in_source_module->setSection(func->getSection());
  }

  ValueMap value_map;
  TypeMap type_map;

  // When mapping in the destination module, we'll reference `func` any time
  // we see the `replacement_decl_in_source_module` or `func`.
  (void) ReplaceAllUsesOfConstant(func, replacement_decl_in_source_module,
                                  source_module);
  value_map.emplace(replacement_decl_in_source_module, func);
  value_map.emplace(func, func);

  // Move `func` into the destination module.
  if (in_same_context) {
    func->removeFromParent();
    func->setName(func_name);
    dest_module->getFunctionList().push_back(func);

    // TODO(pag): Probably clone it into the destination module.
  } else {
    assert(false);
  }

  // There was a prior existing_decl_in_dest_module declaration in out target
  // module, so go and swap all uses of it with `func`. When doing this, we try
  // to rewrite all constants that might use `existing_decl_in_dest_module` into
  // constants that instead use `func`.
  if (existing_decl_in_dest_module) {
    value_map.emplace(existing_decl_in_dest_module, func);
    if (!ReplaceAllUsesOfConstant(existing_decl_in_dest_module, func,
                                  dest_module)) {
      existing_decl_in_dest_module->eraseFromParent();
    }
    existing_decl_in_dest_module = nullptr;
  }

  IF_LLVM_GTE_370(ClearMetaData(func);)

  // Fill up the locals so that they map to themselves.
  for (auto &arg : func->args()) {
    value_map.emplace(&arg, &arg);
  }
  for (auto &block : *func) {
    value_map.emplace(&block, &block);
    for (auto &inst : block) {
      ClearMetaData(&inst);
      value_map.emplace(&inst, &inst);
    }
  }

  // Now move all non-locals.
  for (auto &block : *func) {
    for (auto &inst : block) {
      MoveInstructionIntoModule(&inst, dest_module, value_map, type_map);
    }
  }
}

// Get an instance of `type` that belongs to `context`.
llvm::Type *RecontextualizeType(llvm::Type *type, llvm::LLVMContext &context) {
  if (&(type->getContext()) == &context) {
    return type;
  }

  TypeMap cache;
  return RecontextualizeType(type, context, cache);
}

// Produce a sequence of instructions that will load values from
// memory, building up the correct type. This will invoke the various
// memory read intrinsics in order to match the right type, or
// recursively build up the right type.
llvm::Value *LoadFromMemory(const IntrinsicTable &intrinsics,
                            llvm::BasicBlock *block, llvm::Type *type,
                            llvm::Value *mem_ptr, llvm::Value *addr) {

  const auto initial_addr = addr;
  auto module = intrinsics.error->getParent();
  auto &context = module->getContext();
  llvm::DataLayout dl(module->getDataLayout());
  llvm::Value *args_2[2] = {mem_ptr, addr};
  auto index_type = llvm::Type::getIntNTy(context, dl.getPointerSizeInBits(0));

  llvm::IRBuilder<> ir(block);

  switch (type->getTypeID()) {
    case llvm::Type::HalfTyID: {
      llvm::Type *types[] = {llvm::Type::getFloatTy(context)};
      auto converter = llvm::Intrinsic::getDeclaration(
          module, llvm::Intrinsic::convert_from_fp16, types);
      llvm::Value *conv_args[] = {
          ir.CreateCall(intrinsics.read_memory_16, args_2)};
      return ir.CreateFPTrunc(ir.CreateCall(converter, conv_args), type);
    }

    case llvm::Type::FloatTyID:
      return ir.CreateCall(intrinsics.read_memory_f32, args_2);

    case llvm::Type::DoubleTyID:
      return ir.CreateCall(intrinsics.read_memory_f64, args_2);

    case llvm::Type::X86_FP80TyID: {
      auto res = ir.CreateAlloca(type);
      llvm::Value *args_3[3] = {args_2[0], args_2[1], res};
      ir.CreateCall(intrinsics.read_memory_f80, args_3);
      return ir.CreateLoad(type, res);
    }

      //case llvm::Type::X86_MMXTyID:
      //  return ir.CreateBitCast(ir.CreateCall(intrinsics.read_memory_64, args_2),
      //                          type);

    case llvm::Type::IntegerTyID:
      switch (dl.getTypeAllocSize(type)) {
        case 1: return ir.CreateCall(intrinsics.read_memory_8, args_2);
        case 2: return ir.CreateCall(intrinsics.read_memory_16, args_2);
        case 4: return ir.CreateCall(intrinsics.read_memory_32, args_2);
        case 8: return ir.CreateCall(intrinsics.read_memory_64, args_2);
        default: break;
      }
      [[clang::fallthrough]];

    case llvm::Type::FP128TyID:
    case llvm::Type::PPC_FP128TyID: {
      const auto size = dl.getTypeAllocSize(type);
      auto res = ir.CreateAlloca(type);

      auto i8_array =
          llvm::ArrayType::get(llvm::Type::getInt8Ty(context), size);
      auto byte_array =
          ir.CreateBitCast(res, llvm::PointerType::get(i8_array, 0));

      auto gep_zero = llvm::ConstantInt::get(index_type, 0, false);
      // Load one byte at a time from memory, and store it into
      // `res`.
      for (auto i = 0U; i < size; ++i) {
        llvm::Value *gep_indices[2] = {
            gep_zero, llvm::ConstantInt::get(index_type, i, false)};
        auto call_arg_addr = ir.CreateAdd(
            addr, llvm::ConstantInt::get(addr->getType(), i, false));
        llvm::Value *call_args[2] = {mem_ptr, call_arg_addr};
        auto byte =
            ir.CreateCall(intrinsics.read_memory_8, llvm::ArrayRef(call_args));
        auto byte_ptr = ir.CreateInBoundsGEP(i8_array, byte_array,
                                             llvm::ArrayRef(gep_indices));
        ir.CreateStore(byte, byte_ptr);
      }

      return ir.CreateLoad(type, res);
    }

    // Building up a structure requires us to start with an undef value,
    // then inject each element value one at a time.
    case llvm::Type::StructTyID: {
      const auto struct_type = llvm::dyn_cast<llvm::StructType>(type);
      const auto layout = dl.getStructLayout(struct_type);
      llvm::Value *val = llvm::UndefValue::get(type);
      const auto num_elems = struct_type->getNumElements();
      for (auto i = 0u; i < num_elems; ++i) {
        const auto elem_type = struct_type->getStructElementType(i);
        const auto offset = layout->getElementOffset(i);
        addr = ir.CreateAdd(initial_addr, llvm::ConstantInt::get(
                                              addr->getType(), offset, false));
        auto elem_val =
            LoadFromMemory(intrinsics, block, elem_type, mem_ptr, addr);
        ir.SetInsertPoint(block);
        unsigned indexes[] = {i};
        val = ir.CreateInsertValue(val, elem_val, indexes);
      }
      return val;
    }

    // Build up the array in the same was as we do with structures.
    case llvm::Type::ArrayTyID: {
      auto arr_type = llvm::dyn_cast<llvm::ArrayType>(type);
      const auto num_elems = arr_type->getNumElements();
      const auto elem_type = arr_type->getElementType();
      const auto elem_size = dl.getTypeAllocSize(elem_type);
      llvm::Value *val = llvm::UndefValue::get(type);

      for (uint64_t index = 0, offset = 0; index < num_elems;
           ++index, offset += elem_size) {
        addr = ir.CreateAdd(initial_addr, llvm::ConstantInt::get(
                                              addr->getType(), offset, false));
        unsigned indexes[] = {static_cast<unsigned>(index)};
        auto elem_val =
            LoadFromMemory(intrinsics, block, elem_type, mem_ptr, addr);
        ir.SetInsertPoint(block);
        val = ir.CreateInsertValue(val, elem_val, indexes);
      }
      return val;
    }

    // Read pointers from memory by loading the correct sized integer,
    // then casting it to a pointer.
    case llvm::Type::PointerTyID: {
      auto ptr_type = llvm::dyn_cast<llvm::PointerType>(type);
      auto size_bits = dl.getTypeAllocSizeInBits(ptr_type);
      auto intptr_type =
          llvm::IntegerType::get(context, static_cast<unsigned>(size_bits));
      auto addr_val =
          LoadFromMemory(intrinsics, block, intptr_type, mem_ptr, addr);
      ir.SetInsertPoint(block);
      return ir.CreateIntToPtr(addr_val, ptr_type);
    }

    // Build up the vector in the nearly the same was as we do with arrays.
    case llvm::GetFixedVectorTypeId(): {
      auto vec_type = llvm::dyn_cast<llvm::FixedVectorType>(type);
      const auto num_elems = vec_type->getNumElements();
      const auto elem_type = vec_type->getElementType();
      const auto elem_size = dl.getTypeAllocSize(elem_type);
      llvm::Value *val = llvm::UndefValue::get(type);

      for (uint64_t index = 0, offset = 0; index < num_elems;
           ++index, offset += elem_size) {
        addr = ir.CreateAdd(initial_addr, llvm::ConstantInt::get(
                                              addr->getType(), offset, false));
        auto elem_val =
            LoadFromMemory(intrinsics, block, elem_type, mem_ptr, addr);
        ir.SetInsertPoint(block);
        val =
            ir.CreateInsertElement(val, elem_val, static_cast<unsigned>(index));
      }
      return val;
    }

    case llvm::Type::VoidTyID:
    case llvm::Type::LabelTyID:
    case llvm::Type::MetadataTyID:
    case llvm::Type::TokenTyID:
    case llvm::Type::FunctionTyID:
    default:
      assert(false);
      return nullptr;
  }
}

// Produce a sequence of instructions that will store a value to
// memory. This will invoke the various memory write intrinsics
// in order to match the right type, or recursively destructure
// the type into components which can be written to memory.
//
// Returns the new value of the memory pointer.
llvm::Value *StoreToMemory(const IntrinsicTable &intrinsics,
                           llvm::BasicBlock *block, llvm::Value *val_to_store,
                           llvm::Value *mem_ptr, llvm::Value *addr) {

  const auto initial_addr = addr;
  auto module = intrinsics.error->getParent();
  auto &context = module->getContext();
  llvm::DataLayout dl(module->getDataLayout());
  llvm::Value *args_3[3] = {mem_ptr, addr, val_to_store};
  auto index_type = llvm::Type::getInt32Ty(context);

  llvm::IRBuilder<> ir(block);

  auto type = val_to_store->getType();
  switch (type->getTypeID()) {
    case llvm::Type::HalfTyID: {
      llvm::Type *types[] = {llvm::Type::getFloatTy(context)};
      auto converter = llvm::Intrinsic::getDeclaration(
          module, llvm::Intrinsic::convert_to_fp16, types);
      llvm::Value *conv_args[] = {
          ir.CreateFPExt(val_to_store, llvm::Type::getFloatTy(context))};
      args_3[2] = ir.CreateCall(converter, conv_args);

      return ir.CreateCall(intrinsics.write_memory_16, args_3);
    }

    case llvm::Type::FloatTyID:
      return ir.CreateCall(intrinsics.write_memory_f32, args_3);

    case llvm::Type::DoubleTyID:
      return ir.CreateCall(intrinsics.write_memory_f64, args_3);

    case llvm::Type::X86_FP80TyID: {
      auto res = ir.CreateAlloca(type);
      auto fp80_type = llvm::Type::getX86_FP80Ty(context);
      auto fp80_value = ir.CreateFPTrunc(val_to_store, fp80_type);
      (void) ir.CreateStore(fp80_value, res);
      args_3[2] = res;
      return ir.CreateCall(intrinsics.write_memory_f80, args_3);
    }

      //case llvm::Type::X86_MMXTyID: {
      //  auto i64_type = llvm::Type::getInt64Ty(context);
      //  args_3[2] = ir.CreateBitCast(val_to_store, i64_type);
      //  return ir.CreateCall(intrinsics.write_memory_64, args_3);
      //}

    case llvm::Type::IntegerTyID:
      switch (dl.getTypeAllocSize(type)) {
        case 1: return ir.CreateCall(intrinsics.write_memory_8, args_3);
        case 2: return ir.CreateCall(intrinsics.write_memory_16, args_3);
        case 4: return ir.CreateCall(intrinsics.write_memory_32, args_3);
        case 8: return ir.CreateCall(intrinsics.write_memory_64, args_3);
        default: break;
      }
      [[clang::fallthrough]];

    case llvm::Type::FP128TyID:
    case llvm::Type::PPC_FP128TyID: {
      const auto size = dl.getTypeAllocSize(type);

      // Stack-allocate the value, so we can pull out one byte
      // at a time from it, and then write it into the target
      // address space.
      auto res = ir.CreateAlloca(type);
      ir.CreateStore(val_to_store, res);

      auto i8 = llvm::Type::getInt8Ty(context);
      auto i8_array =
          llvm::ArrayType::get(llvm::Type::getInt8Ty(context), size);
      auto byte_array =
          ir.CreateBitCast(res, llvm::PointerType::get(i8_array, 0));
      llvm::Value *gep_indices[2] = {
          llvm::ConstantInt::get(index_type, 0, false), nullptr};

      // Store one byte at a time to memory.
      for (auto i = 0U; i < size; ++i) {
        args_3[1] = ir.CreateAdd(
            addr, llvm::ConstantInt::get(addr->getType(), i, false));
        gep_indices[1] = llvm::ConstantInt::get(index_type, i, false);
        auto byte_ptr = ir.CreateInBoundsGEP(i8_array, byte_array, gep_indices);
        args_3[2] = ir.CreateLoad(i8, byte_ptr);
        args_3[0] = ir.CreateCall(intrinsics.write_memory_8, args_3);
      }

      return args_3[0];
    }

    // Store a structure by storing the individual elements of the structure.
    case llvm::Type::StructTyID: {
      auto struct_type = llvm::dyn_cast<llvm::StructType>(type);
      const auto layout = dl.getStructLayout(struct_type);
      const auto num_elems = struct_type->getNumElements();
      for (auto i = 0u; i < num_elems; ++i) {
        const auto offset = layout->getElementOffset(i);
        const auto elem_addr = ir.CreateAdd(
            initial_addr,
            llvm::ConstantInt::get(addr->getType(), offset, false));
        unsigned indexes[] = {i};
        const auto elem_val = ir.CreateExtractValue(val_to_store, indexes);
        mem_ptr =
            StoreToMemory(intrinsics, block, elem_val, mem_ptr, elem_addr);
        ir.SetInsertPoint(block);
      }
      return mem_ptr;
    }

    // Build up the array store in the same was as we do with structures.
    case llvm::Type::ArrayTyID: {
      auto arr_type = llvm::dyn_cast<llvm::ArrayType>(type);
      const auto num_elems = arr_type->getNumElements();
      const auto elem_type = arr_type->getElementType();
      const auto elem_size = dl.getTypeAllocSize(elem_type);

      for (uint64_t index = 0, offset = 0; index < num_elems;
           ++index, offset += elem_size) {

        auto elem_addr = ir.CreateAdd(
            initial_addr,
            llvm::ConstantInt::get(addr->getType(), offset, false));
        unsigned indexes[] = {static_cast<unsigned>(index)};
        auto elem_val = ir.CreateExtractValue(val_to_store, indexes);
        mem_ptr =
            StoreToMemory(intrinsics, block, elem_val, mem_ptr, elem_addr);
        ir.SetInsertPoint(block);
        offset += elem_size;
        index += 1;
      }
      return mem_ptr;
    }

    // Write pointers to memory by converting to the correct sized integer,
    // then storing that
    case llvm::Type::PointerTyID: {
      auto ptr_type = llvm::dyn_cast<llvm::PointerType>(type);
      auto size_bits = dl.getTypeAllocSizeInBits(ptr_type);
      auto intptr_type =
          llvm::IntegerType::get(context, static_cast<unsigned>(size_bits));
      return StoreToMemory(intrinsics, block,
                           ir.CreatePtrToInt(val_to_store, intptr_type),
                           mem_ptr, addr);
    }

    // Build up the vector store in the nearly the same was as we do with arrays.
    case llvm::GetFixedVectorTypeId(): {
      auto vec_type = llvm::dyn_cast<llvm::FixedVectorType>(type);
      const auto num_elems = vec_type->getNumElements();
      const auto elem_type = vec_type->getElementType();
      const auto elem_size = dl.getTypeAllocSize(elem_type);

      for (uint64_t index = 0, offset = 0; index < num_elems;
           ++index, offset += elem_size) {

        auto elem_addr = ir.CreateAdd(
            initial_addr,
            llvm::ConstantInt::get(addr->getType(), offset, false));
        auto elem_val =
            ir.CreateExtractElement(val_to_store, static_cast<unsigned>(index));
        mem_ptr =
            StoreToMemory(intrinsics, block, elem_val, mem_ptr, elem_addr);
        ir.SetInsertPoint(block);
        offset += elem_size;
        index += 1;
      }

      return mem_ptr;
    }

    case llvm::Type::VoidTyID:
    case llvm::Type::LabelTyID:
    case llvm::Type::MetadataTyID:
    case llvm::Type::TokenTyID:
    case llvm::Type::FunctionTyID:
    default:
      assert(false);
      return nullptr;
  }
}

// Create an array of index values to pass to a GetElementPtr instruction
// that will let us locate a particular register. Returns the final offset
// into `type` which was reached as the first value in the pair, and the type
// of what is at that offset in the second value of the pair.
std::pair<size_t, llvm::Type *>
BuildIndexes(const llvm::DataLayout &dl, llvm::Type *type, size_t offset,
             const size_t goal_offset,
             llvm::SmallVectorImpl<llvm::Value *> &indexes_out) {

  CHECK_LE(offset, goal_offset);
  CHECK_LE(goal_offset, (offset + dl.getTypeAllocSize(type)));

  size_t index = 0;
  const auto index_type = indexes_out[0]->getType();

  if (const auto struct_type = llvm::dyn_cast<llvm::StructType>(type);
      struct_type) {

    auto layout = dl.getStructLayout(struct_type);
    auto prev_elem_offset = 0;
    llvm::Type *prev_elem_type = nullptr;

    for (auto i = 0u, max_i = struct_type->getNumElements(); i < max_i; ++i) {
      auto elem_offset = layout->getElementOffset(i);
      auto elem_type = struct_type->getStructElementType(i);
      auto elem_size = dl.getTypeStoreSize(elem_type);

      // The goal offset comes after this element.
      if ((offset + elem_offset + elem_size) <= goal_offset) {
        prev_elem_offset = elem_offset;
        prev_elem_type = elem_type;
        continue;

        // Indexing into the `i`th element.
      } else if ((offset + elem_offset) <= goal_offset) {
        indexes_out.push_back(llvm::ConstantInt::get(index_type, i, false));
        return BuildIndexes(dl, elem_type, offset + elem_offset, goal_offset,
                            indexes_out);

        // We're indexing into some padding before the current element.
      } else if (i) {
        indexes_out.push_back(llvm::ConstantInt::get(index_type, i - 1, false));
        return {offset + prev_elem_offset, prev_elem_type};

        // We're indexing into some padding at the beginning of this structure.
      } else {
        return {offset, type};
      }
    }

  } else if (auto seq_type = llvm::dyn_cast<llvm::ArrayType>(type); seq_type) {
    const auto elem_type = seq_type->getElementType();
    const auto elem_size = dl.getTypeAllocSize(elem_type);
    const auto num_elems = seq_type->getNumElements();

    while ((offset + elem_size) <= goal_offset && index < num_elems) {
      offset += elem_size;
      index += 1;
    }

    CHECK_LE(offset, goal_offset);
    CHECK_LE(goal_offset, (offset + elem_size));

    indexes_out.push_back(llvm::ConstantInt::get(index_type, index, false));
    return BuildIndexes(dl, elem_type, offset, goal_offset, indexes_out);
  } else if (auto fvt_type = llvm::dyn_cast<llvm::FixedVectorType>(type);
             fvt_type) {

    // It is possible that this gets called on an unexpected type
    // such as FixedVectorType; if so, report the issue and fix if/when it
    // happens
    assert(false);
#if LLVM_VERSION_NUMBER >= LLVM_VERSION(11, 0)
  } else if (auto svt_type = llvm::dyn_cast<llvm::ScalableVectorType>(type);
             svt_type) {

    // same as above, but for scalable vectors
    assert(false);
#endif
  }

  return {offset, type};
}

// Given a pointer, `ptr`, and a goal byte offset to which we'd like to index,
// build either a constant expression or sequence of instructions that can
// index to that offset. `ir` is provided to support the instruction case
// and to give access to a module for data layouts.
llvm::Value *BuildPointerToOffset(llvm::IRBuilder<> &ir, llvm::Value *ptr,
                                  size_t dest_elem_offset,
                                  llvm::Type *dest_ptr_type) {

  // TODO(pag): Improve the API to take a `DataLayout`, perhaps.
  auto &context = ptr->getContext();
  const auto i32_type = llvm::Type::getInt32Ty(context);

  llvm::SmallVector<llvm::Value *, 16> indexes;

  auto ptr_type = llvm::dyn_cast<llvm::PointerType>(ptr->getType());
  CHECK_NOTNULL(ptr_type);
  const auto dest_elem_ptr_type =
      llvm::dyn_cast<llvm::PointerType>(dest_ptr_type);
  CHECK_NOTNULL(dest_elem_ptr_type);
  auto ptr_addr_space = ptr_type->getAddressSpace();
  const auto dest_ptr_addr_space = dest_elem_ptr_type->getAddressSpace();
  auto constant_ptr = llvm::dyn_cast<llvm::Constant>(ptr);

  // Change address spaces if necessary before indexing.
  if (dest_ptr_addr_space != ptr_addr_space) {
    ptr_type = llvm::PointerType::get(context, dest_ptr_addr_space);
    ptr_addr_space = dest_ptr_addr_space;

    if (constant_ptr) {
      constant_ptr =
          llvm::ConstantExpr::getAddrSpaceCast(constant_ptr, ptr_type);
      ptr = constant_ptr;
    } else {
      ptr = ir.CreateAddrSpaceCast(ptr, ptr_type);
    }
  }

  const auto i8_type = llvm::Type::getInt8Ty(context);

  if (dest_elem_offset) {
    indexes.push_back(
        llvm::ConstantInt::get(i32_type, dest_elem_offset, false));
    if (constant_ptr) {
      constant_ptr =
          llvm::ConstantExpr::getGetElementPtr(i8_type, constant_ptr, indexes);
      ptr = constant_ptr;
    } else {
      ptr = ir.CreateGEP(i8_type, ptr, indexes);
    }
  }

  if (constant_ptr) {
    return llvm::ConstantExpr::getBitCast(constant_ptr, dest_elem_ptr_type);
  } else {
    return ir.CreateBitCast(ptr, dest_elem_ptr_type);
  }
}


// Compute the total offset of a GEP chain.
std::pair<llvm::Value *, int64_t>
StripAndAccumulateConstantOffsets(const llvm::DataLayout &dl,
                                  llvm::Value *base) {
  const auto ptr_size = dl.getPointerSizeInBits(0);
  int64_t total_offset = 0;
  while (base) {
    if (auto gep = llvm::dyn_cast<llvm::GEPOperator>(base); gep) {
      llvm::APInt accumulated_offset(ptr_size, 0, false);
      if (!gep->accumulateConstantOffset(dl, accumulated_offset)) {
        break;
      }

      const auto curr_offset = accumulated_offset.getSExtValue();
      total_offset += curr_offset;

      base = gep->getPointerOperand();

    } else if (auto bc = llvm::dyn_cast<llvm::BitCastOperator>(base); bc) {
      base = bc->getOperand(0);

    } else if (auto itp = llvm::dyn_cast<llvm::IntToPtrInst>(base); itp) {
      base = itp->getOperand(0);

    } else if (auto pti = llvm::dyn_cast<llvm::PtrToIntOperator>(base); pti) {
      base = pti->getOperand(0);

    } else if (auto alias = llvm::dyn_cast<llvm::GlobalAlias>(base); alias) {
      base = alias->getAliasee();

    } else {
      break;
    }
  }
  return {base, total_offset};
}

// Scalarize a flat-lifted function in-place: run the inliner, mem2reg, and
// SROA (via the new pass manager, in-process) to eliminate the STATE_LOCAL
// alloca (the local `State` struct) that flat lifting materializes.
//
// We deliberately do NOT use the full O2 pipeline: it requires a
// TargetMachine and, with some bundled libLLVM builds, the PassBuilder ABI
// Fix invalid `zext ptr to i64` → `ptrtoint ptr to i64`.
// With opaque pointers, some lifted code emits zext on pointer values
// (valid with typed pointers, invalid with opaque). Replace with ptrtoint.
void remill::FixZextPtrToPtrToInt(llvm::Function *func) {
  std::vector<llvm::Instruction *> to_fix;
  for (auto &bb : *func) {
    for (auto &inst : bb) {
      if (auto *zext = llvm::dyn_cast<llvm::ZExtInst>(&inst)) {
        if (zext->getOperand(0)->getType()->isPointerTy()) {
          to_fix.push_back(zext);
        }
      }
    }
  }
  for (auto *zext : to_fix) {
    auto *ptrtoint = new llvm::PtrToIntInst(
        zext->getOperand(0), zext->getType(), "", zext);
    zext->replaceAllUsesWith(ptrtoint);
    zext->eraseFromParent();
  }

  // Inline trivial __remill_compare_* calls (1-arg i1→i1 identity/NOT).
  for (auto &bb : *func) {
    std::vector<llvm::CallInst *> to_inline;
    for (auto &inst : bb) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (!call || !call->getCalledFunction()) continue;
      auto name = call->getCalledFunction()->getName();
      if (name.starts_with("__remill_compare_")) {
        to_inline.push_back(call);
      }
    }
    for (auto *call : to_inline) {
      auto name = call->getCalledFunction()->getName();
      if (name.ends_with("neq")) {
        // __remill_compare_neq(x) = !x
        auto *xor_inst = llvm::BinaryOperator::CreateXor(
            call->getArgOperand(0),
            llvm::ConstantInt::getTrue(call->getArgOperand(0)->getType()),
            "", call);
        call->replaceAllUsesWith(xor_inst);
      } else {
        // identity: __remill_compare_eq/ult/ule/ugt/uge/sgt(x) = x
        call->replaceAllUsesWith(call->getArgOperand(0));
      }
      call->eraseFromParent();
    }
  }
}

// mismatch causes a segfault. A minimal pipeline (inliner + mem2reg + SROA)
// with a null TargetMachine is sufficient to scalarize the local `State` and
// is safe to run in-process.
llvm::Function *ScalarizeFlatFunction(llvm::Module *module,
                                      llvm::Function *func) {
  if (!module || !func) {
    return nullptr;
  }
  FixZextPtrToPtrToInt(func);

  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb(/*TM=*/nullptr, llvm::PipelineTuningOptions(),
                       std::nullopt, /*PIC=*/nullptr);
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  // Targeted inlining: inline ALL calls to defined functions in the target
  // function so no pointers escape. We do NOT run the O2 inliner pipeline
  // on the whole module because LLVM 21 has an assertion bug
  // (CmpInst::getFlippedStrictnessPredicate) triggered by the inliner when
  // the module contains conditional branches.
  {
    llvm::SmallVector<llvm::CallBase *, 32> to_inline;
    for (auto &bb : *func) {
      for (auto &inst : bb) {
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
          if (auto *callee = call->getCalledFunction()) {
            // Inline any call to a defined (non-declaration) function that
            // isn't the flat jump (which must remain as a tail call).
            if (!callee->isDeclaration() &&
                callee != func &&
                callee->getName() != "__remill_flat_jump" &&
                !call->isTailCall()) {
              to_inline.push_back(call);
            }
          }
        }
      }
    }
    // Inline in reverse order (inlining invalidates later iterators).
    // Check parent() to detect already-erased calls (use_empty() doesn't
    // work for void calls which never have uses).
    for (auto it = to_inline.rbegin(); it != to_inline.rend(); ++it) {
      auto *call = *it;
      if (call->getParent() == nullptr) continue;  // already erased
      llvm::InlineFunctionInfo ifi;
      (void)llvm::InlineFunction(*call, ifi);
    }
  }

  // Run mem2reg + SROA on the target function. Skip for functions with
  // conditional branches (LLVM assertion bug).
  bool has_cond_branch = false;
  for (auto &bb : *func) {
    if (auto *br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator())) {
      if (br->isConditional()) {
        has_cond_branch = true;
        break;
      }
    }
  }

  if (!has_cond_branch) {
    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::PromotePass());  // mem2reg
    fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    fpm.run(*func, fam);
  }

  // The function may have been renamed or (rarely) eliminated; re-look it up
  // by its original name.
  return module->getFunction(func->getName());
}

// Eliminate __remill_read/write_memory calls for RSP-derived addresses by
// replacing them with direct GEP + load/store. This works because RSP is now
// a ptr in the flat ABI: any memory access whose address is ptrtoint(RSP) +
// offset can be converted to a direct pointer dereference.
static void EliminateStackMemoryAccesses(llvm::Function *func) {
  auto &context = func->getContext();
  auto *i64_ty = llvm::Type::getInt64Ty(context);
  auto *i8_ty = llvm::Type::getInt8Ty(context);
  auto *ptr_ty = llvm::PointerType::get(context, 0);

  // Helper: check if an i64 value is ptrtoint(X) or ptrtoint(X) +/- offset.
  // Returns true and fills base_ptr and offset if so.
  auto decompose_addr = [&](llvm::Value *addr, llvm::Value *&base_ptr,
                            llvm::Value *&offset) -> bool {
    // Case 1: addr is directly ptrtoint(X)
    if (auto *ptoi = llvm::dyn_cast<llvm::PtrToIntInst>(addr)) {
      base_ptr = ptoi->getOperand(0);
      offset = llvm::ConstantInt::get(i64_ty, 0);
      return true;
    }
    // Case 2: addr is add/sub(ptrtoint(X), offset)
    if (auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(addr)) {
      auto op = bin->getOpcode();
      if (op == llvm::Instruction::Add || op == llvm::Instruction::Sub) {
        auto *ptoi = llvm::dyn_cast<llvm::PtrToIntInst>(bin->getOperand(0));
        if (ptoi) {
          base_ptr = ptoi->getOperand(0);
          auto *off = bin->getOperand(1);
          if (op == llvm::Instruction::Sub) {
            if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(off)) {
              offset = llvm::ConstantInt::get(i64_ty, -ci->getSExtValue());
            } else {
              // sub with non-const: negate via add -x
              llvm::IRBuilder<> ir(bin);
              offset = ir.CreateNeg(off, "neg_off");
            }
          } else {
            offset = off;
          }
          return true;
        }
      }
    }
    return false;
  };

  // Pass 1: Replace __remill_read/write_memory calls with direct load/store.
  for (auto &block : *func) {
    llvm::SmallVector<llvm::Instruction *, 16> to_erase;
    for (auto &inst : block) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (!call) continue;
      auto *callee = call->getCalledFunction();
      if (!callee) continue;
      auto name = callee->getName();

      bool is_read = name.starts_with("__remill_read_memory_");
      bool is_write = name.starts_with("__remill_write_memory_");
      if (!is_read && !is_write) continue;

      // Address is arg 1 (arg 0 is the memory pointer).
      llvm::Value *addr = call->getArgOperand(1);
      if (!addr || !addr->getType()->isIntegerTy(64)) continue;

      llvm::Value *base_ptr = nullptr;
      llvm::Value *offset = nullptr;
      if (!decompose_addr(addr, base_ptr, offset)) continue;

      llvm::IRBuilder<> ir(call);
      auto *gep = ir.CreateInBoundsGEP(i8_ty, base_ptr, offset, "stack_ptr");

      if (is_read) {
        auto *load = ir.CreateLoad(call->getType(), gep, "stack_load");
        call->replaceAllUsesWith(load);
      } else {
        // Write: store the value, thread the memory pointer through unchanged.
        llvm::Value *val = call->getArgOperand(2);
        ir.CreateStore(val, gep);
        // The write returns a new memory pointer; replace uses with the input
        // memory pointer (stack writes don't affect the memory model).
        call->replaceAllUsesWith(call->getArgOperand(0));
      }
      to_erase.push_back(call);
    }
    for (auto *i : to_erase) i->eraseFromParent();
  }

  // Pass 2: Replace inttoptr(add/sub(ptrtoint(X), off)) with GEP(X, off).
  // This handles RSP updates: instead of inttoptr(rsp_i64 + 8), use GEP(rsp, 8).
  for (auto &block : *func) {
    llvm::SmallVector<llvm::Instruction *, 16> to_erase;
    for (auto &inst : block) {
      auto *itoptr = llvm::dyn_cast<llvm::IntToPtrInst>(&inst);
      if (!itoptr) continue;

      llvm::Value *addr = itoptr->getOperand(0);
      llvm::Value *base_ptr = nullptr;
      llvm::Value *offset = nullptr;
      if (!decompose_addr(addr, base_ptr, offset)) continue;

      llvm::IRBuilder<> ir(itoptr);
      auto *gep = ir.CreateInBoundsGEP(i8_ty, base_ptr, offset, "rsp_next");
      itoptr->replaceAllUsesWith(gep);
      to_erase.push_back(itoptr);
    }
    for (auto *i : to_erase) i->eraseFromParent();
  }

  // Pass 3: DCE any now-unused ptrtoint instructions.
  for (auto &block : *func) {
    llvm::SmallVector<llvm::Instruction *, 16> to_erase;
    for (auto &inst : block) {
      if (auto *ptoi = llvm::dyn_cast<llvm::PtrToIntInst>(&inst)) {
        if (ptoi->use_empty()) to_erase.push_back(ptoi);
      }
    }
    for (auto *i : to_erase) i->eraseFromParent();
  }
}

// Stack frame reconstruction: normalize all RSP/RBP-derived pointers to
// GEP(initial_RSP, total_offset), then forward store→load pairs and
// eliminate dead stores. This makes the IR look like compiler output.
static void ForwardStackSlotAccesses(llvm::Function *func) {
  auto &ctx = func->getContext();
  auto *i64 = llvm::Type::getInt64Ty(ctx);
  auto *i8 = llvm::Type::getInt8Ty(ctx);

  // Find the RSP and RBP function arguments (ptr type, indices 6 and 7).
  llvm::Argument *rsp_arg = nullptr;
  llvm::Argument *rbp_arg = nullptr;
  auto args = func->arg_begin();
  for (size_t i = 0; i < func->arg_size(); ++i, ++args) {
    if (i == 3 + 6) rsp_arg = &*args;   // kFlatFirstRegArgNum + 6
    if (i == 3 + 7) rbp_arg = &*args;   // kFlatFirstRegArgNum + 7
    if (i >= 3 + 7) break;
  }
  if (!rsp_arg) return;

  // Phase 1: Compute the stack offset for every RSP/RBP-derived value.
  // Map: Value* → cumulative byte offset from initial RSP.
  llvm::DenseMap<llvm::Value *, int64_t> offsets;
  offsets[rsp_arg] = 0;
  if (rbp_arg) offsets[rbp_arg] = 0;

  llvm::SmallVector<llvm::Value *> worklist;
  worklist.push_back(rsp_arg);
  if (rbp_arg) worklist.push_back(rbp_arg);

  size_t wl_idx = 0;
  while (wl_idx < worklist.size()) {
    auto *v = worklist[wl_idx++];
    auto base_off = offsets[v];

    for (auto *user : v->users()) {
      // GEP(i8, X, offset) → offset + GEP_offset
      if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
        if (gep->getSourceElementType() == i8 &&
            gep->getPointerOperand() == v) {
          auto *idx = gep->getOperand(1);
          if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
            int64_t new_off = base_off +
                static_cast<int64_t>(ci->getSExtValue());
            if (!offsets.count(user)) {
              offsets[user] = new_off;
              worklist.push_back(user);
            }
          }
        }
      }
      // ptrtoint(X) → track for inttoptr pattern
      if (auto *pti = llvm::dyn_cast<llvm::PtrToIntInst>(user)) {
        if (pti->getOperand(0) == v) {
          if (!offsets.count(pti)) {
            offsets[pti] = base_off;
            worklist.push_back(pti);
          }
        }
      }
      // add(ptrtoint(X), C) → offset + C
      if (auto *bo = llvm::dyn_cast<llvm::BinaryOperator>(user)) {
        if (bo->getOpcode() == llvm::Instruction::Add ||
            bo->getOpcode() == llvm::Instruction::Sub) {
          auto *lhs = llvm::dyn_cast<llvm::PtrToIntInst>(bo->getOperand(0));
          auto *rhs = llvm::dyn_cast<llvm::ConstantInt>(bo->getOperand(1));
          if (lhs && rhs && offsets.count(lhs)) {
            int64_t new_off = (bo->getOpcode() == llvm::Instruction::Add)
                                  ? offsets[lhs] + rhs->getSExtValue()
                                  : offsets[lhs] - rhs->getSExtValue();
            if (!offsets.count(user)) {
              offsets[user] = new_off;
              worklist.push_back(user);
            }
          }
        }
      }
      // inttoptr(add/sub result) → offset from the add/sub
      if (auto *itp = llvm::dyn_cast<llvm::IntToPtrInst>(user)) {
        auto *src = itp->getOperand(0);
        if (offsets.count(src)) {
          if (!offsets.count(user)) {
            offsets[user] = offsets[src];
            worklist.push_back(user);
          }
        }
      }
    }
  }

  // Phase 2: Normalize all RSP-derived pointers to GEP(rsp_arg, offset).
  // Replace each tracked pointer value with a canonical GEP.
  llvm::IRBuilder<> builder(ctx);
  llvm::SmallVector<llvm::Value *> old_vals;
  llvm::SmallVector<llvm::GetElementPtrInst *> new_geps;

  for (auto &[val, off] : offsets) {
    if (val == rsp_arg || val == rbp_arg) continue;
    if (!llvm::isa<llvm::Instruction>(val)) continue;
    auto *inst = llvm::cast<llvm::Instruction>(val);
    // Only normalize ptr-typed values.
    if (!inst->getType()->isPointerTy()) continue;
    // Create canonical GEP after the instruction.
    builder.SetInsertPoint(inst);
    auto *gep = llvm::cast<llvm::GetElementPtrInst>(
        builder.CreateGEP(i8, rsp_arg, builder.getInt64(off), "rsp_slot"));
    old_vals.push_back(val);
    new_geps.push_back(gep);
  }

  // Apply replacements (replaceAllUsesWith).
  for (size_t i = 0; i < old_vals.size(); ++i) {
    old_vals[i]->replaceAllUsesWith(new_geps[i]);
  }

  // Phase 3: Store-load forwarding.
  // After normalization, all accesses to the same slot use the same GEP.
  // For each load, find the most recent store to the same GEP and forward.
  llvm::SmallVector<std::pair<llvm::LoadInst *, llvm::Value *>, 64> forwards;

  for (auto &bb : *func) {
    llvm::SmallVector<llvm::Instruction *> insts;
    for (auto &inst : bb) insts.push_back(&inst);

    for (size_t i = 0; i < insts.size(); ++i) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(insts[i]);
      if (!load) continue;
      auto *load_ptr = load->getPointerOperand();
      if (!llvm::isa<llvm::GetElementPtrInst>(load_ptr)) continue;
      if (llvm::cast<llvm::GetElementPtrInst>(load_ptr)
              ->getSourceElementType() != i8)
        continue;

      // Search backwards for a store to the same pointer.
      for (size_t j = i; j-- > 0;) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(insts[j]);
        if (!store) continue;
        if (store->getPointerOperand() == load_ptr) {
          forwards.push_back({load, store->getValueOperand()});
          break;
        }
      }
    }
  }

  // Apply forwards.
  for (auto &[load, val] : forwards) {
    builder.SetInsertPoint(load);
    load->replaceAllUsesWith(val);
    load->eraseFromParent();
  }

  // Phase 4: Remove dead stores (stores with no uses of the pointer).
  llvm::SmallVector<llvm::StoreInst *, 64> dead_stores;
  for (auto &bb : *func) {
    for (auto &inst : bb) {
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        auto *ptr = store->getPointerOperand();
        if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
          if (gep->getSourceElementType() == i8 && gep->use_empty()) {
            dead_stores.push_back(store);
          }
        }
      }
    }
  }
  for (auto *store : dead_stores) {
    store->eraseFromParent();
  }
}

namespace {

// Parse a flat block function name `sub_<hex-pc>` (see TraceLifter).
bool FlatBlockFunctionPc(llvm::Function *func, uint64_t *pc) {
  auto name = func->getName();
  if (!name.starts_with("sub_")) {
    return false;
  }
  auto digits = name.drop_front(4);
  if (digits.empty()) {
    return false;
  }
  uint64_t value = 0;
  for (char c : digits) {
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value += uint64_t(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value += uint64_t(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      value += uint64_t(c - 'A' + 10);
    } else {
      return false;
    }
  }
  *pc = value;
  return true;
}

// Resolve the value of a load from the PC or NEXT_PC pointer: walk back to
// the last store of that pointer in the load's own block; if there is none,
// the dispatcher seeded the cell with the entry pc at block start.
static bool ResolvePcLoad(llvm::Function *func, llvm::LoadInst *ld,
                          uint64_t entry, unsigned depth, uint64_t *pc) {
  if (depth > 256) {
    return false;
  }
  auto *ptr = ld->getPointerOperand();
  if (ptr != func->getArg(kFlatPCArgNum) &&
      ptr != func->getArg(kFlatNextPCArgNum)) {
    return false;
  }
  auto *block = ld->getParent();
  llvm::BasicBlock::iterator it = block->begin();
  for (; &*it != ld; ++it) {
    if (it == block->end()) {
      return false;
    }
  }
  for (; it != block->begin();) {
    --it;
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&*it);
    if (store && store->getPointerOperand() == ptr) {
      auto *v = store->getValueOperand();
      if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        *pc = ci->getZExtValue();
        return true;
      }
      if (auto *inner = llvm::dyn_cast<llvm::LoadInst>(v)) {
        return ResolvePcLoad(func, inner, entry, depth + 1, pc);
      }
      // The store is typically `add (entry pc load), C`; resolve the add by
      // finding its constant and pc-load operands.
      if (auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(v)) {
        if (bin->getOpcode() != llvm::Instruction::Add &&
            bin->getOpcode() != llvm::Instruction::Or) {
          return false;
        }
        for (int i = 0; i < 2; ++i) {
          auto *ci = llvm::dyn_cast<llvm::ConstantInt>(bin->getOperand(i));
          auto *inner = llvm::dyn_cast<llvm::LoadInst>(bin->getOperand(1 - i));
          if (ci && inner) {
            uint64_t base = 0;
            if (ResolvePcLoad(func, inner, entry, depth + 1, &base)) {
              *pc = base + ci->getZExtValue();
              return true;
            }
          }
        }
      }
      return false;
    }
  }
  // No store in the block: the dispatcher seeded the cell at block entry.
  if (block == &func->getEntryBlock()) {
    *pc = entry;
    return true;
  }
  return false;
}

// Resolve a static successor pc expression: a constant, or
// `<pc of some instruction> + C` where the base pc is a load from the PC or
// NEXT_PC pointer (threaded through the block by the ISEL). Returns false
// for dynamic exits (ret, indirect branch/call).
bool FlatExitTargetPc(llvm::Function *func, llvm::CallInst *call,
                      uint64_t *target) {
  auto *next_pc_arg = func->getArg(kFlatNextPCArgNum);

  // The successor is the value last stored to the NEXT_PC pointer before the
  // `__remill_flat_jump` tail call (the dispatcher reads *next_pc).
  llvm::Value *next_pc = nullptr;
  auto *block = call->getParent();
  for (auto it = std::prev(block->end()); it != block->begin(); --it) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&*it);
    if (store && store->getPointerOperand() == next_pc_arg) {
      next_pc = store->getValueOperand();
      break;
    }
  }
  // Conditional branch exits: the ISEL stores `select cond, taken_pc,
  // not_taken_pc` to NEXT_PC in the branch block, then splits with
  // `br i1 cond`. The exit block itself has no NEXT_PC store; recover the
  // arm of the select that belongs to this exit block.
  if (!next_pc) {
    llvm::BranchInst *branch = nullptr;
    for (auto *pred : llvm::predecessors(block)) {
      auto *br = llvm::dyn_cast_or_null<llvm::BranchInst>(pred->getTerminator());
      if (br && br->isConditional() &&
          (br->getSuccessor(0) == block || br->getSuccessor(1) == block)) {
        branch = br;
        break;
      }
    }
    if (branch) {
      llvm::Value *stored = nullptr;
      // Last NEXT_PC store in the branch block.
      auto *bblock = branch->getParent();
      for (auto it = std::prev(bblock->end()); it != bblock->begin(); --it) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&*it);
        if (store && store->getPointerOperand() == next_pc_arg) {
          stored = store->getValueOperand();
          break;
        }
      }
      if (auto *sel = llvm::dyn_cast<llvm::SelectInst>(stored)) {
        if (sel->getCondition() == branch->getCondition()) {
          next_pc = (branch->getSuccessor(0) == block) ? sel->getTrueValue()
                                                       : sel->getFalseValue();
        }
      }
    }
    if (!next_pc) {
      return false;
    }
  }

  uint64_t entry = 0;
  if (!FlatBlockFunctionPc(func, &entry)) {
    return false;
  }

  if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(next_pc)) {
    *target = ci->getZExtValue();
    return true;
  }
  if (auto *ld = llvm::dyn_cast<llvm::LoadInst>(next_pc)) {
    return ResolvePcLoad(func, ld, entry, 0, target);
  }
  if (auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(next_pc)) {
    if (bin->getOpcode() != llvm::Instruction::Add &&
        bin->getOpcode() != llvm::Instruction::Or) {
      return false;
    }
    for (int i = 0; i < 2; ++i) {
      auto *ci = llvm::dyn_cast<llvm::ConstantInt>(bin->getOperand(i));
      auto *inner = llvm::dyn_cast<llvm::LoadInst>(bin->getOperand(1 - i));
      if (ci && inner) {
        uint64_t base = 0;
        if (ResolvePcLoad(func, inner, entry, 0, &base)) {
          *target = base + ci->getZExtValue();
          return true;
        }
      }
    }
  }
  return false;
}

}  // namespace

void LinkFlatBlockExits(llvm::Module *module) {
  if (!module) {
    return;
  }
  auto *flat_jump = module->getFunction("__remill_flat_jump");
  if (!flat_jump) {
    return;
  }

  auto &context = module->getContext();
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::IRBuilder<> builder(context);

  // Per-target private cell holding the target's entry pc. Successor blocks
  // read (and write back) their entry pc from their PC pointer argument,
  // which the runtime dispatcher provides per block; a mutable private
  // global plays the same role for direct links.
  llvm::DenseMap<llvm::Function *, llvm::GlobalVariable *> pc_cells;

  // Collect all dispatcher tail calls first: the exits may be merged into a
  // common block (SimplifyCFG), so the calls are not necessarily the return
  // value of the terminator. Note: this LLVM's CallBase::getCalledFunction()
  // returns null for every direct call (it compares the callee's *return*
  // type against the call's function type), so match via getCalledOperand().
  llvm::SmallVector<llvm::CallInst *, 8> calls;
  for (auto &func : *module) {
    if (func.isDeclaration()) {
      continue;
    }
    for (auto &block : func) {
      for (auto &inst : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
        if (call && call->isTailCall() &&
            call->getCalledOperand() == flat_jump) {
          calls.push_back(call);
        }
      }
    }
  }


  unsigned relinked = 0;
  unsigned kept = 0;
  for (auto *call : calls) {
    auto *func = call->getFunction();
    uint64_t target_pc = 0;
    if (!FlatExitTargetPc(func, call, &target_pc)) {
      ++kept;  // Dynamic exit (ret / indirect): keep the dispatcher.
      continue;
    }

    char name[24];
    snprintf(name, sizeof(name), "sub_%llx",
             static_cast<unsigned long long>(target_pc));
    auto *target = module->getFunction(name);
    if (!target || target->isDeclaration() || target == func ||
        // Compare against the caller's (block function) type, not the
        // `__remill_flat_jump` stub's: the stub declares XMM params as
        // `ptr byval(...)` while block functions take `<2 x i64>` by value.
        target->getFunctionType() != func->getFunctionType()) {
      ++kept;  // Target not lifted (yet) or signature mismatch.
      continue;
    }

    auto it = pc_cells.find(target);
    llvm::GlobalVariable *cell;
    if (it != pc_cells.end()) {
      cell = it->second;
    } else {
      cell = new llvm::GlobalVariable(
          *module, i64, /*isConstant=*/false,
          llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantInt::get(i64, target_pc),
          "flat_pc_" + std::to_string(target_pc));
      pc_cells[target] = cell;
    }

    // GEP the target's entry-pc cell and hand it to the target as its PC
    // pointer argument; the remaining 62 operands are positionally
    // identical to the target's register arguments.
    builder.SetInsertPoint(call);
    auto *pc_ptr = builder.CreateInBoundsGEP(
        i64, cell, llvm::ConstantInt::get(i32, 0), "target_pc");
    call->setOperand(kFlatPCArgNum, pc_ptr);
    call->setCalledFunction(target);
    ++relinked;
  }

  llvm::errs() << "LinkFlatBlockExits: " << relinked
               << " exit(s) linked directly to block functions, " << kept
               << " kept __remill_flat_jump dispatch\n";
}

// Merge the globals and functions of `extra` into `base`. Entries with the
// same name are assumed identical (same lift run/version) and dropped from
// `extra`. Used to assemble the full-function module from per-block lift
// outputs.
void MergeFlatBlockModules(llvm::Module *base, llvm::Module *extra) {
  if (!base || !extra || base == extra) {
    return;
  }
  // Globals: per-block lift outputs normally have none. Copy declarations;
  // defined globals are rare enough to reject rather than risk a wrong
  // initializer remap.
  llvm::SmallVector<llvm::GlobalVariable *, 8> gvs;
  for (auto &gv : extra->globals()) {
    gvs.push_back(&gv);
  }
  for (auto *gv : gvs) {
    if (base->getGlobalVariable(gv->getName())) {
      gv->eraseFromParent();
      continue;
    }
    if (!gv->isDeclaration()) {
      llvm::errs() << "  [link_blocks] warning: dropping defined global "
                   << gv->getName() << " (unsupported initializer)\n";
      gv->eraseFromParent();
      continue;
    }
    auto *copy = new llvm::GlobalVariable(
        *base, gv->getValueType(), gv->isConstant(), gv->getLinkage(),
        nullptr, gv->getName());
    copy->setAlignment(llvm::Align(gv->getAlignment()));
    gv->eraseFromParent();
  }
  llvm::SmallVector<llvm::Function *, 8> funcs;
  for (auto &fn : *extra) {
    funcs.push_back(&fn);
  }
  for (auto *fn : funcs) {
    if (base->getFunction(fn->getName())) {
      fn->eraseFromParent();
      continue;
    }
    MoveFunctionIntoModule(fn, base);
  }
}

// Inline all non-tail calls to defined internal functions (the _flat ISEL
// wrappers) so the merged module is self-contained. Tail calls to the block
// functions and to `__remill_flat_jump` are left alone.
void InlineFlatISelCalls(llvm::Module *module) {
  if (!module) {
    return;
  }
  for (unsigned pass = 0; pass < 32; ++pass) {
    llvm::SmallVector<llvm::CallBase *, 64> calls;
    for (auto &func : *module) {
      if (func.isDeclaration()) {
        continue;
      }
      for (auto &block : func) {
        for (auto &inst : block) {
          auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
          if (!call || call->isTailCall()) {
            continue;
          }
          auto *callee =
              llvm::dyn_cast_or_null<llvm::Function>(call->getCalledOperand());
          if (!callee || callee->isDeclaration() || callee == &func) {
            continue;
          }
          if (!callee->hasInternalLinkage() &&
              !callee->hasPrivateLinkage()) {
            continue;
          }
          calls.push_back(call);
        }
      }
    }
    if (calls.empty()) {
      break;
    }
    bool inlined_any = false;
    for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
      auto *call = *it;
      if (call->use_empty()) {
        continue;
      }
      llvm::InlineFunctionInfo ifi;
      if (llvm::InlineFunction(*call, ifi).isSuccess()) {
        inlined_any = true;
      }
    }
    if (!inlined_any) {
      break;  // Avoid an infinite loop on a stubborn call.
    }
  }
}

// Optimize a pure-SSA flat function after inlining the _flat ISEL wrappers.
// Runs: mem2reg + SROA + instcombine. This scalarizes the State allocas that
// were inlined from the _flat wrappers and simplifies the flag computation
// patterns.
llvm::Function *OptimizeFlatSSAFunction(llvm::Module *module,
                                        llvm::Function *func) {
  if (!module || !func) {
    return nullptr;
  }
  FixZextPtrToPtrToInt(func);

  // Eliminate stack memory accesses (RSP ptr → direct load/store).
  EliminateStackMemoryAccesses(func);

  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb(/*TM=*/nullptr, llvm::PipelineTuningOptions(),
                       std::nullopt, /*PIC=*/nullptr);
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  // Run mem2reg + SROA + instcombine + simplifycfg + DCE on the target
  // function only (not the whole module, to avoid crashes on other functions).
  // SROA with ModifyCFG crashes on functions with conditional branches
  // (LLVM assertion in CmpInst::getFlippedStrictnessPredicate), so we
  // skip SROA for those.
  bool has_cond_branch = false;
  for (auto &bb : *func) {
    if (auto *br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator())) {
      if (br->isConditional()) {
        has_cond_branch = true;
        break;
      }
    }
  }

  // Run optimization. For branch functions, skip SROA (LLVM 21 assertion
  // bug) but still run simplifycfg/DCE to reduce size.
  // IMPORTANT: No InstCombine in the loop — it simplifies GEP chains
  // (GEP(GEP(RSP,-8),8) → RSP), breaking RSP tracking needed by
  // ForwardStackSlotAccesses. InstCombine runs once at the end, AFTER
  // store-load forwarding is complete.
  for (int iter = 0; iter < 3; ++iter) {
    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::PromotePass());  // mem2reg
    if (!has_cond_branch) {
      fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    }
    fpm.addPass(llvm::SimplifyCFGPass());
    fpm.addPass(llvm::DCEPass());
    fpm.run(*func, fam);
  }

  // Run stack elimination again after optimization (new patterns may emerge
  // from instcombine simplifying the address arithmetic).
  func = module->getFunction(func->getName());
  if (func) {
    EliminateStackMemoryAccesses(func);
    // Normalize RSP-derived pointers and forward store→load pairs.
    ForwardStackSlotAccesses(func);
    // Final cleanup: DCE dead stores and instructions.
    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::InstCombinePass());
    fpm.addPass(llvm::DCEPass());
    fpm.addPass(llvm::SimplifyCFGPass());
    fpm.run(*func, fam);
  }

  return module->getFunction(func->getName());
}

}  // namespace remill
