// Full automated flat-opcode wrapper generator.
//
// For each ISEL opcode `F(memory, State*, operands...)` in the bitcode,
// synthesize a flat wrapper `F_flat(memory, reg_0..reg_50, operands...)` that:
//   1. Allocates a local `State`.
//   2. Calls `__remill_flat_state_load` to materialize it from the 52 reg
//      pointers.
//   3. Inlines the original opcode body (passing the local State + operands),
//      capturing the returned memory pointer.
//   4. Calls `__remill_flat_state_store` to write the State back out.
//   5. Returns the memory pointer.
//
// Then run `ScalarizeFlatFunction` (inliner + mem2reg + SROA) to scalarize
// the local State, eliminating the 51 loads/stores. After SROA, the flat
// wrapper operates directly on the reg_i pointers.
//
// The operands are passed through unchanged (they're already pointers to the
// actual registers in the caller's state, or scalar values).
//
// The ISEL body is inlined into the wrapper at build time so the wrapper is
// self-contained (the `internal` ISEL won't survive linking into a separate
// lifted module).

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "remill/BC/Util.h"

using namespace llvm;

// The canonical flat register order (52 registers).
// 17 GPRs, 3 seg bases, 7 flags, 8 MMX, 16 XMM.
static const char *kFlatRegNames[52] = {
    // 17 GPRs.
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rsp", "rbp",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip",
    // 4 seg bases.
    "ss_base", "gs_base", "cs_base", "fs_base",
    // 7 flags.
    "cf", "pf", "af", "zf", "sf", "df", "of",
    // 8 MMX.
    "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7",
    // 16 XMM.
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
};

// The names of the flat-state load/store helpers (compiled into the bitcode).
static const char *kFlatStateLoadName = "__remill_flat_state_load";
static const char *kFlatStateStoreName = "__remill_flat_state_store";

// Check whether a function is an ISEL opcode. ISEL opcodes are functions that:
//   - Take a `Memory *` as the first argument.
//   - Take a `State *` as the second argument.
//   - Return a `Memory *`.
//   - Are defined (not just declared).
//   - Have a C++ mangled name starting with `_ZN`.
static bool IsISelOpcode(Function *f) {
  if (!f || f->isDeclaration()) {
    return false;
  }
  // Must have at least 2 arguments (memory, state).
  if (f->arg_size() < 2) {
    return false;
  }
  // The second argument must be a pointer (State *).
  auto *arg1 = f->getArg(1);
  if (!isa<PointerType>(arg1->getType())) {
    return false;
  }
  // The first argument must be a pointer (Memory *).
  auto *arg0 = f->getArg(0);
  if (!isa<PointerType>(arg0->getType())) {
    return false;
  }
  // The return type must be a pointer (Memory *).
  if (!isa<PointerType>(f->getReturnType())) {
    return false;
  }
  // The function name must start with `_ZN` (C++ mangled).
  auto name = f->getName();
  if (!name.starts_with("_ZN")) {
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <amd64.bc> <output.bc>\n", argv[0]);
    return 1;
  }

  LLVMContext context;
  auto buffer_or_err = MemoryBuffer::getFile(argv[1]);
  if (!buffer_or_err) {
    fprintf(stderr, "Failed to read file: %s\n", argv[1]);
    return 1;
  }
  auto module_result = parseBitcodeFile((*buffer_or_err)->getMemBufferRef(), context);
  if (!module_result) {
    fprintf(stderr, "Failed to parse bitcode: %s\n", argv[1]);
    return 1;
  }
  auto module_uptr = std::move(*module_result);
  auto *module = module_uptr.get();

  // Find the State type from the @__remill_state global variable.
  auto *state_global = module->getGlobalVariable("__remill_state");
  if (!state_global) {
    fprintf(stderr, "__remill_state global not found.\n");
    return 1;
  }
  auto *state_ty = state_global->getValueType();

  // Find the flat-state load/store helpers.
  auto *flat_load = module->getFunction(kFlatStateLoadName);
  auto *flat_store = module->getFunction(kFlatStateStoreName);
  if (!flat_load || !flat_store) {
    fprintf(stderr, "Flat-state helpers not found: %s / %s\n",
            kFlatStateLoadName, kFlatStateStoreName);
    return 1;
  }

  fprintf(stderr, "Found State type and flat-state helpers.\n");

  // Iterate over all functions and find ISEL opcodes.
  std::vector<Function *> isel_opcodes;
  for (auto &f : *module) {
    if (IsISelOpcode(&f)) {
      isel_opcodes.push_back(&f);
    }
  }

  fprintf(stderr, "Found %zu ISEL opcodes.\n", isel_opcodes.size());

  // For each ISEL opcode, create a flat wrapper.
  auto &ctx = module->getContext();
  auto ptr_ty = PointerType::get(ctx, 0);

  int num_transformed = 0;
  int num_failed = 0;
  for (auto *isel : isel_opcodes) {
    auto isel_name = isel->getName().str();
    auto flat_name = isel_name + "_flat";

    // Skip if the flat wrapper already exists.
    if (module->getFunction(flat_name)) {
      continue;
    }

    // Build the flat wrapper signature:
    //   ptr F_flat(ptr %memory, ptr reg_0..reg_50, operands...)
    auto isel_type = isel->getFunctionType();
    std::vector<Type *> param_types;
    param_types.push_back(ptr_ty);  // memory
    for (int i = 0; i < 52; ++i) {
      param_types.push_back(ptr_ty);  // reg_0..reg_50
    }
    // The original operands (skip the first 2 args: memory, state).
    for (unsigned i = 2; i < isel_type->getNumParams(); ++i) {
      param_types.push_back(isel_type->getParamType(i));
    }

    auto func_type = FunctionType::get(ptr_ty, param_types, false);
    auto *flat_func = Function::Create(func_type, Function::ExternalLinkage,
                                       flat_name, module);

    // Create the entry block.
    auto *entry = BasicBlock::Create(ctx, "entry", flat_func);
    IRBuilder<> ir(entry);

    // Get the arguments.
    auto arg_it = flat_func->arg_begin();
    auto *memory_arg = &*arg_it++;  // %memory
    std::vector<Argument *> reg_args(52);
    for (int i = 0; i < 52; ++i) {
      reg_args[i] = &*arg_it++;
    }
    std::vector<Argument *> operand_args;
    for (unsigned i = 2; i < isel_type->getNumParams(); ++i) {
      operand_args.push_back(&*arg_it++);
    }

    // Allocate a local State.
    auto *state_alloca = ir.CreateAlloca(state_ty, nullptr, "STATE_LOCAL");

    // Call __remill_flat_state_load to materialize the local State from the
    // register pointers.
    std::vector<Value *> load_args;
    load_args.push_back(state_alloca);  // state
    for (int i = 0; i < 52; ++i) {
      load_args.push_back(reg_args[i]);
    }
    ir.CreateCall(flat_load, load_args);

    // Create a call to the original opcode, passing the local State and the
    // operands, then inline the opcode body into the wrapper.
    std::vector<Value *> call_args;
    call_args.push_back(memory_arg);  // %memory
    call_args.push_back(state_alloca);  // %state (local State)
    for (auto *op : operand_args) {
      call_args.push_back(op);
    }
    // Create a call to the ISEL function, and immediately return its result.
    // This ensures the call's result (the memory value) is used, so that
    // InlineFunction splices the inlined body's return value into the `ret`
    // (preserving it) rather than dropping it.
    auto *call = ir.CreateCall(isel, call_args);
    auto *ret_inst = ir.CreateRet(call);

    // Inline the ISEL body into the wrapper. The call is replaced by the
    // inlined body, and the `ret` now returns the inlined body's return value
    // (the memory pointer).
    InlineFunctionInfo ifi;
    auto inline_result = InlineFunction(*call, ifi);
    if (!inline_result.isSuccess()) {
      ++num_failed;
      if (num_failed <= 3) {
        fprintf(stderr, "  Inline failed for %s: %s\n",
                isel_name.c_str(), inline_result.getFailureReason());
        fflush(stderr);
      }
      flat_func->deleteBody();
      flat_func->eraseFromParent();
      continue;
    }

    // After inlining, the entry block ends with a `ret` instruction that
    // returns the memory value. Insert the flat_store call before the `ret`.
    //
    // The `ret` is the last instruction in the entry block.
    ir.SetInsertPoint(ret_inst);
    std::vector<Value *> store_args;
    for (int i = 0; i < 52; ++i) {
      store_args.push_back(reg_args[i]);
    }
    store_args.push_back(state_alloca);  // state (last argument)
    ir.CreateCall(flat_store, store_args);

    // The `ret` is now at the end of the entry block, after the flat_store
    // call. It returns the memory value (the inlined body's return value).

    ++num_transformed;
  }

  fprintf(stderr, "Transformed %d ISEL opcodes (%d failed).\n",
          num_transformed, num_failed);

  // Verify the module.
  std::string verify_err;
  raw_string_ostream verify_stream(verify_err);
  if (verifyModule(*module, &verify_stream)) {
    fprintf(stderr, "Verification failed: %s\n", verify_err.c_str());
    return 1;
  }

  fprintf(stderr, "Module verified successfully.\n");

  // Write the pre-SROA bitcode first (so we can verify the transformation
  // worked even if SROA crashes).
  std::string pre_sroa_path = std::string(argv[2]) + ".pre_sroa.bc";
  {
    std::error_code ec;
    raw_fd_ostream out(pre_sroa_path, ec);
    WriteBitcodeToFile(*module, out);
    out.close();
    fprintf(stderr, "Wrote pre-SROA bitcode to %s\n", pre_sroa_path.c_str());
  }

  // Run SROA once over the whole module. This scalarizes the local State in
  // ALL flat wrappers at once (the inliner + mem2reg + SROA run over the whole
  // module, so all flat wrappers are scalarized in a single pass).
  fprintf(stderr, "Running SROA over the whole module (scalarizing all flat wrappers)...\n");
  fflush(stderr);
  auto *first_flat = module->getFunction(isel_opcodes[0]->getName().str() + "_flat");
  if (first_flat) {
    auto *scalarized = remill::ScalarizeFlatFunction(module, first_flat);
    fprintf(stderr, "SROA complete. First flat wrapper scalarized: %s\n",
            scalarized ? "yes" : "no");
  }
  fflush(stderr);

  // Verify the module again.
  verify_err.clear();
  raw_string_ostream verify_stream2(verify_err);
  if (verifyModule(*module, &verify_stream2)) {
    fprintf(stderr, "Verification failed after SROA: %s\n", verify_err.c_str());
    return 1;
  }

  fprintf(stderr, "Module verified after SROA.\n");

  // Write the transformed bitcode.
  std::error_code ec;
  raw_fd_ostream out(argv[2], ec);
  WriteBitcodeToFile(*module, out);
  out.close();
  fprintf(stderr, "Wrote %s\n", argv[2]);

  return 0;
}
