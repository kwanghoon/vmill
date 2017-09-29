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

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ADT/Triple.h>

#include <llvm/IR/Function.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "remill/Arch/Arch.h"
#include "remill/Arch/Name.h"
#include "remill/BC/Compat/TargetLibraryInfo.h"
#include "remill/BC/IntrinsicTable.h"
#include "remill/BC/Lifter.h"
#include "remill/BC/Util.h"
#include "remill/OS/FileSystem.h"
#include "remill/OS/OS.h"

#include "vmill/Arch/Decoder.h"
#include "vmill/BC/Lifter.h"

namespace vmill {
namespace {

class LifterImpl : public Lifter {
 public:
  virtual ~LifterImpl(void);

  explicit LifterImpl(const std::shared_ptr<llvm::LLVMContext> &);

  LiftedFunction LiftIntoModule(
      uint64_t pc, const ByteReaderCallback &cb,
      const std::unique_ptr<llvm::Module> &module) override;

  // Host and target architectures.
  const remill::Arch * const host_arch;
  const remill::Arch * const target_arch;

  // LLVM context that manages all modules.
  const std::shared_ptr<llvm::LLVMContext> context;

  // Bitcode semantics for the target architecture.
  const std::unique_ptr<llvm::Module> semantics;

  // Tracks the Remill intrinsics present in `semantics`.
  remill::IntrinsicTable intrinsics;

  // Lifts instructions from the target architecture to bitcode that can run
  // on the host architecture.
  remill::InstructionLifter lifter;

 private:
  LifterImpl(void) = delete;
};

LifterImpl::LifterImpl(const std::shared_ptr<llvm::LLVMContext> &context_)
    : Lifter(),
      host_arch(remill::GetHostArch()),
      target_arch(remill::GetTargetArch()),
      context(context_),
      semantics(remill::LoadTargetSemantics(context.get())),
      intrinsics(semantics.get()),
      lifter(remill::AddressType(semantics.get()), &intrinsics) {
  host_arch->PrepareModule(semantics.get());
}

LifterImpl::~LifterImpl(void) {}

// The 'version' of this trace is a hash of the instruction bytes.
static uint64_t TraceHash(const InstructionMap &insts) {
  std::stringstream is;
  for (const auto &entry : insts) {
    is << entry.second.bytes;
  }
  std::hash<std::string> hash;
  return hash(is.str());
}

// The function's lifted name contains both its position in memory (`pc`) and
// the contents of memory (instruction bytes). This makes it sensitive to self-
// modifying code.
static std::string LiftedFunctionName(uint64_t pc, uint64_t hash) {
  std::stringstream ns;
  ns << "$" << std::hex << pc << "_" << std::hex << hash;
  return ns.str();
}

// Optimize the lifted function. This ends up being pretty slow because it
// goes and optimizes everything else in the module (a.k.a. semantics module).
static void RunO3(llvm::Function *func) {
  auto module = func->getParent();

  llvm::legacy::FunctionPassManager func_manager(module);
  llvm::legacy::PassManager module_manager;

  auto TLI = new llvm::TargetLibraryInfoImpl(
      llvm::Triple(module->getTargetTriple()));

  TLI->disableAllFunctions();  // `-fno-builtin`.

  llvm::PassManagerBuilder builder;
  builder.OptLevel = 3;
  builder.SizeLevel = 2;
  builder.Inliner = llvm::createFunctionInliningPass(
      std::numeric_limits<int>::max());
  builder.LibraryInfo = TLI;  // Deleted by `llvm::~PassManagerBuilder`.
  builder.DisableUnrollLoops = false;  // Unroll loops!
  builder.DisableUnitAtATime = false;
  builder.SLPVectorize = false;
  builder.LoopVectorize = false;
  builder.VerifyInput = true;
  builder.VerifyOutput = true;

  builder.populateFunctionPassManager(func_manager);
  builder.populateModulePassManager(module_manager);
  func_manager.doInitialization();
  func_manager.run(*func);
  func_manager.doFinalization();
  module_manager.run(*module);
}

LiftedFunction LifterImpl::LiftIntoModule(
    uint64_t pc, const ByteReaderCallback &cb,
    const std::unique_ptr<llvm::Module> &module) {

  auto context_ptr = context.get();
  CHECK(&(module.get()->getContext()) == context_ptr);

  const auto insts = Decode(target_arch, pc, cb);
  const auto hash = TraceHash(insts);
  const auto func_name = LiftedFunctionName(pc, hash);

  // Already lifted; don't re-do things.
  auto dest_func = module->getFunction(func_name);
  if (dest_func) {
    return {pc, hash, dest_func};
  }

  auto func = remill::DeclareLiftedFunction(semantics.get(), func_name);
  remill::CloneBlockFunctionInto(func);

  // Function that will create basic blocks as needed.
  std::unordered_map<uint64_t, llvm::BasicBlock *> blocks;
  auto GetOrCreateBlock = [func, context_ptr, &blocks] (uint64_t block_pc) {
    auto &block = blocks[block_pc];
    if (!block) {
      block = llvm::BasicBlock::Create(*context_ptr, "", func);
    }
    return block;
  };

  // Create a branch from the entrypoint of the lifted function to the basic
  // block representing the first decoded instruction.
  auto entry_block = GetOrCreateBlock(pc);
  llvm::BranchInst::Create(entry_block, &(func->front()));

  // Guarantee that a basic block exists, even if the first instruction
  // failed to decode.
  if (!insts.count(pc)) {
    remill::AddTerminatingTailCall(entry_block, intrinsics.error);
  }

  // Lift each instruction into its own basic block.
  for (const auto &entry : insts) {
    auto block = GetOrCreateBlock(entry.first);
    auto &inst = const_cast<remill::Instruction &>(entry.second);
    LOG(ERROR) << inst.Serialize();
    if (!lifter.LiftIntoBlock(inst, block)) {
      remill::AddTerminatingTailCall(block, intrinsics.error);
      continue;
    }

    // Connect together the basic blocks.
    switch (inst.category) {
      case remill::Instruction::kCategoryInvalid:
      case remill::Instruction::kCategoryError:
        remill::AddTerminatingTailCall(block, intrinsics.error);
        break;

      case remill::Instruction::kCategoryNormal:
      case remill::Instruction::kCategoryNoOp:
        llvm::BranchInst::Create(GetOrCreateBlock(inst.next_pc), block);
        break;

      case remill::Instruction::kCategoryDirectJump:
      case remill::Instruction::kCategoryDirectFunctionCall:
        llvm::BranchInst::Create(GetOrCreateBlock(inst.branch_taken_pc),
                                 block);
        break;

      case remill::Instruction::kCategoryIndirectJump:
        remill::AddTerminatingTailCall(block, intrinsics.jump);
        break;

      case remill::Instruction::kCategoryIndirectFunctionCall:
        remill::AddTerminatingTailCall(block, intrinsics.function_call);
        break;

      case remill::Instruction::kCategoryFunctionReturn:
        remill::AddTerminatingTailCall(block, intrinsics.function_return);
        break;

      case remill::Instruction::kCategoryConditionalBranch:
      case remill::Instruction::kCategoryConditionalAsyncHyperCall:
        llvm::BranchInst::Create(GetOrCreateBlock(inst.branch_taken_pc),
                                 GetOrCreateBlock(inst.branch_not_taken_pc),
                                 remill::LoadBranchTaken(block), block);
        break;

      case remill::Instruction::kCategoryAsyncHyperCall:
        remill::AddTerminatingTailCall(block, intrinsics.async_hyper_call);
        break;
    }
  }

  // Terminate any stragglers.
  for (auto pc_to_block : blocks) {
    auto block = pc_to_block.second;
    if (!block->getTerminator()) {
      remill::AddTerminatingTailCall(block, intrinsics.missing_block);
    }
  }

  // Optimize the lifted function.
  RunO3(func);

  dest_func = llvm::Function::Create(
      func->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
      func_name, module.get());

  remill::CloneFunctionInto(func, dest_func);

  func->eraseFromParent();

  return {pc, hash, dest_func};
}

}  // namespace

Lifter *Lifter::Create(
    const std::shared_ptr<llvm::LLVMContext> &context) {
  return new LifterImpl(context);
}

Lifter::Lifter(void) {}
Lifter::~Lifter(void) {}

}  // namespace vmill
