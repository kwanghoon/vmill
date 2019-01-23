/*
 * Copyright (c) 2018 Trail of Bits, Inc.
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

#include "vmill/Executor/Executor.h"
#include "vmill/Executor/Interpreter.h"

#include <glog/logging.h>
#include <gflags/gflags.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Type.h>
#include "llvm/Support/Debug.h"

#include "remill/BC/Util.h"
#include "remill/OS/FileSystem.h"
#include "remill/Arch/Arch.h"
#include "remill/Arch/Instruction.h"
#include "remill/Arch/Name.h"
#include "remill/BC/Util.h"
#include "remill/OS/OS.h"

#include "vmill/BC/TraceLifter.h"
#include "vmill/Program/AddressSpace.h"
#include "vmill/Arch/Arch.h"
#include "vmill/Workspace/Workspace.h"


#include "klee/ExecutionState.h"
#include "klee/klee.h"
#include "klee/Interpreter.h"
 
#include <iostream>

namespace vmill {

thread_local Executor *gExecutor;

namespace {

static llvm::Module *LoadRuntimeBitcode(llvm::LLVMContext *context) {
  auto &runtime_bitcode_path = Workspace::RuntimeBitcodePath();
  LOG(INFO)
      << "Loading runtime bitcode file from " << runtime_bitcode_path;
  return remill::LoadModuleFromFile(context, runtime_bitcode_path,
                                    false  /* allow_failure */);
}

}  // namespace

Executor::Executor(void)
    : vmill_executor(),
      context(new llvm::LLVMContext),
      lifted_code(LoadRuntimeBitcode(context.get())),
      trace_manager(*lifted_code),
      lifter(*lifted_code, trace_manager),
      interpreter(nullptr) {}
 
void Executor::SetUp(void) {
  gExecutor = this;
}

void Executor::TearDown(void) {
  gExecutor = nullptr;
}

Executor::~Executor(void) {

  // Reset all task vars to have null initializers.
  for (unsigned i = 0; ; i++) {
    const std::string task_var_name = "__vmill_task_" + std::to_string(i);
    const auto task_var = lifted_code -> getGlobalVariable(task_var_name);
    if (!task_var) {
      break;
    }
    task_var->setInitializer(llvm::Constant::getNullValue(
        task_var->getInitializer()->getType()));
  }

  // Save the runtime, including lifted bitcode, into the workspace. Next
  // execution will load up this file.
  remill::StoreModuleToFile(
      lifted_code,
      Workspace::LocalRuntimeBitcodePath(),
      false);
}

void Executor::Run(void) {
  SetUp();
  LOG(INFO) << "Setting Up The Klee Interpreter";
  while (auto task = NextTask()) {
    LOG(INFO) << "Interpreting Tasks!";
    interpreter->Interpret(task);
  }
  //lifted_code -> dump();
  remill::StoreModuleIRToFile(lifted_code, "IR", false);

  LOG(INFO) << "Tearing Down the Executor";
  TearDown();
}

void Executor::AddInitialTask(const std::string &state, const uint64_t pc,
                              std::shared_ptr<AddressSpace> memory) {

  CHECK(memories.empty());

  const auto task_num = memories.size();
  memories.push_back(memory);

  const std::string task_var_name = "__vmill_task_" + std::to_string(task_num);
  auto task_var = lifted_code->getGlobalVariable(task_var_name);

  // Lazily create the task variable if it's missing.
  if (!task_var) {
    CHECK(task_num)
        << "Missing task variable " << task_var_name << " in runtime";

    // Make sure that task variables are no gaps in the ordering of task
    // variables.
    const std::string prev_task_var_name =
        "__vmill_task_" + std::to_string(task_num - 1);
    const auto prev_task_var = lifted_code->getGlobalVariable(
        prev_task_var_name);
    CHECK(prev_task_var != nullptr)
        << "Missing task variable " << prev_task_var_name << " in runtime";

    task_var = new llvm::GlobalVariable(
        *lifted_code, prev_task_var->getValueType(), false /* isConstant */,
        llvm::GlobalValue::ExternalLinkage,
        llvm::Constant::getNullValue(prev_task_var->getValueType()),
        task_var_name);
  }

  TaskContinuation cont;
  cont.continuation = lifter.GetLiftedFunction(memory.get(), pc);

  cont.state = state;
  cont.pc = pc;

  LOG(INFO) << "BEFORE INTERPRETER CONVERSION";
  LOG(INFO) << pc;
  llvm::dbgs() << cont.continuation -> getName();
  llvm::dbgs() << '\n';
  llvm::dbgs() << lifted_code->getFunction(cont.continuation -> getName());
  llvm::dbgs() << '\n';

  LOG(INFO) << "State size is " << cont.state.size();
  
  interpreter = std::unique_ptr<Interpreter>(Interpreter::CreateConcrete(lifted_code, this));
  tasks.push_back(interpreter->ConvertContinuationToTask(cont));
  
  LOG(INFO) << "GOT THROUGH INITIAL TASK";

}

AddressSpace *Executor::Memory(uintptr_t index) {
  auto mem = memories[index].get();
  LOG(INFO)
    << "Got AddressSpace at index " << index << '\n';
  return mem;
}

llvm::Function *Executor::GetLiftedFunction(
    AddressSpace *memory, uint64_t addr) {
  return lifter.GetLiftedFunction(memory, addr);
}

void *Executor::NextTask(void) {
  if (tasks.empty()) {
    return nullptr;
  } else {
    auto task = tasks.front();
    tasks.pop_front();
    return task;
  }
}

void Executor::AddTask(void *task) {
  tasks.push_back(task);
}

llvm::Function *Executor::RequestFunc(uint64_t pc, uint64_t idx) {
   CHECK(idx < memories.size());
   auto mem = Memory(idx);
   return GetLiftedFunction(mem, pc);
}

bool Executor::DoRead(uint64_t size, uint64_t address, uint64_t pc, void *val) {
    CHECK(address < memories.size());
    auto mem = Memory(address);
    switch(size) {
      case 8: {
        LOG(INFO) << "HIT CASE 8";
        return mem->TryRead(pc, reinterpret_cast<uint64_t *>(val));
      } case 4:{
        LOG(INFO) << "HIT CASE 4";
        uint32_t val32 = 0;

        return mem->TryRead(pc, reinterpret_cast<uint32_t *>(val));

      } case 2: {
        LOG(INFO) << "HIT CASE 2";
        uint16_t val16 = 0;

        return mem->TryRead(pc, reinterpret_cast<uint16_t *>(val));
      } case 1: {
        LOG(INFO) << "HIT CASE 1";
        uint8_t val8 = 0;
        return mem->TryRead(pc, reinterpret_cast<uint8_t *>(val));
      } default: {
        LOG(INFO) << "HIT CASE DEFAULT";
        LOG(INFO) << "Invalid Size To Write: " << size;
        return false;
      }
    }
}


bool Executor::DoWrite(uint64_t size, uint64_t address, uint64_t pc, uint64_t value) {
    CHECK(address < memories.size());
    auto mem = Memory(address);
    switch(size) {
      case 8:
        return mem->TryWrite(pc, value);
      case 4:
        return mem->TryWrite(pc, static_cast<uint32_t>(value));
      case 2:
        return mem->TryWrite(pc, static_cast<uint16_t>(value));
      case 1:
        return mem->TryWrite(pc, static_cast<uint8_t>(value));
      default:
        LOG(INFO) << "Invalid Size To Write: " << size;
        return false;
    }
}

}  //namespace vmill
