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

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ManagedStatic.h>

#include "grr/Snapshot.h"

#include "remill/Arch/Arch.h"
#include "remill/Arch/Name.h"
#include "remill/OS/FileSystem.h"
#include "remill/OS/OS.h"

#include "vmill/BC/Executor.h"
#include "vmill/BC/Lifter.h"
#include "vmill/BC/Runtime.h"
#include "vmill/Context/AddressSpace.h"
#include "vmill/Context/Context.h"

DEFINE_string(workspace, ".", "Path to workspace in which the snapshot file is"
                              " stored, and in which files will be placed.");

DEFINE_string(executor, "native", "Type of the executor to run.");

DECLARE_string(arch);
DECLARE_string(os);

namespace grr {
namespace {

using AddressSpaceIdToMemoryMap = std::unordered_map<int64_t, void *>;

// Load a snapshot from a file.
ProgramSnapshotPtr LoadSnapshotFromFile(void) {
  const std::string snapshot_path = FLAGS_workspace + "/snapshot";
  CHECK(remill::FileExists(snapshot_path))
      << "Snapshot file " << snapshot_path << " does not exist. Make sure "
      << "to create it with grr-snapshot.";

  std::ifstream fs(snapshot_path, std::ios::binary);
  CHECK(fs)
      << "Snapshot file " << snapshot_path
      << " could not be opened for reading";

  ProgramSnapshotPtr snap(new grr::snapshot::Program);
  CHECK(snap->ParseFromIstream(&fs))
      << "Unable parse snapshot file " << snapshot_path;

  LOG(INFO)
      << "Parsed snapshot file " << snapshot_path;

  return snap;
}

// Load in the data from the snapshotted page range into the address space.
static void LoadPageRangeFromFile(vmill::AddressSpace *addr_space,
                                  const grr::snapshot::PageRange &range) {
  std::stringstream ss;
  ss << FLAGS_workspace << "/memory/" << range.name();
  auto path = ss.str();
  CHECK(remill::FileExists(path))
      << "File " << path << " with the data of the page range [" << std::hex
      << range.base() << ", " << std::hex << range.limit()
      << ") does not exist.";

  auto range_size = static_cast<uint64_t>(range.limit() - range.base());
  CHECK(range_size == remill::FileSize(path))
      << "File " << path << " with the data of the page range [" << std::hex
      << range.base() << ", " << std::hex << range.limit()
      << ") is not the right size.";

  LOG(INFO)
      << "Loading file " << path << " into range [" << std::hex << range.base()
      << ", " << range.limit() << ")" << std::dec;

  auto fd = open(path.c_str(), O_RDONLY);

  uint8_t buff[4096];
  uint64_t base_addr = static_cast<uint64_t>(range.base());
  while (range_size) {
    auto amount_read_ = read(fd, buff, 4096);
    if (-1 == amount_read_) {
      CHECK(!range_size)
          << "Failed to read all page range data from " << path;
      break;
    }

    auto amount_read = static_cast<uint64_t>(amount_read_);
    for (uint64_t i = 0; i < amount_read; ++i) {
      CHECK(addr_space->TryWrite(base_addr + i, buff[i]))
          << "Unable to copy byte from " << path << " into address space "
          << " at address " << std::hex << (base_addr + i);
    }

    base_addr += amount_read;
    range_size -= amount_read;
  }

  close(fd);
}


// Go through the snapshotted pages and copy them into the address space.
static void LoadAddressSpaceFromSnapshot(
    const vmill::ContextPtr &context,
    AddressSpaceIdToMemoryMap &addr_space_ids,
    const grr::snapshot::AddressSpace &orig_addr_space) {

  LOG(INFO)
      << "Initializing address space " << orig_addr_space.id();

  auto id = orig_addr_space.id();
  CHECK(!addr_space_ids.count(id))
      << "Address space " << std::dec << orig_addr_space.id()
      << " has already been deserialized.";

  void *memory = nullptr;

  // Create the address space, either as a clone of a parent, or as a new one.
  if (orig_addr_space.has_parent_id()) {
    int64_t parent_id = orig_addr_space.parent_id();
    CHECK(addr_space_ids.count(parent_id))
        << "Cannot find parent address space " << std::dec << parent_id
        << " for address space " << std::dec << orig_addr_space.id();

    const auto &parent_mem = addr_space_ids[parent_id];
    memory = context->CloneAddressSpace(parent_mem);
  } else {
    memory = context->CreateAddressSpace();
  }

  auto emu_addr_space = context->AddressSpaceOf(memory);

  // Bring in the ranges.
  for (const auto &page : orig_addr_space.page_ranges()) {
    CHECK(page.limit() > page.base())
        << "Invalid page map information with base " << std::hex << page.base()
        << " being greater than or equal to the page limit " << std::hex
        << page.limit() << " in address space " << std::dec
        << orig_addr_space.id();

    auto base = static_cast<uint64_t>(page.base());
    auto limit = static_cast<uint64_t>(page.limit());
    auto size = limit - base;
    emu_addr_space->AddMap(base, size);
    LoadPageRangeFromFile(emu_addr_space, page);
    emu_addr_space->SetPermissions(base, size, page.can_read(),
                                   page.can_write(), page.can_exec());
  }
}

}  // namespace

static void Run(const ProgramSnapshotPtr &snapshot) {

  LOG(INFO) << "Creating execution context.";
  auto context = vmill::Context::Create();

  LOG(INFO) << "Loading address space information from snapshot";
  AddressSpaceIdToMemoryMap address_space_ids;
  for (const auto &address_space : snapshot->address_spaces()) {
    LoadAddressSpaceFromSnapshot(
        context, address_space_ids, address_space);
  }

  LOG(INFO) << "Loading task information.";
  for (const auto &thread : snapshot->threads()) {
    int64_t addr_space_id = thread.address_space_id();
    CHECK(address_space_ids.count(addr_space_id))
        << "Invalid address space id " << std::dec << addr_space_id
        << " for task";

    auto memory = address_space_ids[addr_space_id];
    auto state = context->AllocateStateInRuntime(thread.state());
    auto pc = static_cast<uint64_t>(thread.pc());

    LOG(INFO)
        << "Adding task starting execution at " << std::hex << pc
        << " in address space " << std::dec << addr_space_id;

    context->ScheduleTask({state, pc, memory});
  }

  vmill::Task task;
  while (context->TryDequeueTask(&task)) {
    context->ResumeTask(task);
  }

//  context->Execute();
}

}  // namespace grr

int main(int argc, char **argv) {

  std::stringstream ss;
  ss << std::endl << std::endl
     << "  " << argv[0] << " \\" << std::endl
     << "    [--executor EXEC_KIND] \\" << std::endl
     << "    [--workspace WORKSPACE_DIR]" << std::endl
     << "    [--runtime RUNTIME_PATH]" << std::endl;

  google::InitGoogleLogging(argv[0]);
  google::SetUsageMessage(ss.str());
  google::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_workspace.empty()) {
    FLAGS_workspace = remill::CurrentWorkingDirectory();
  }

  CHECK(!FLAGS_workspace.empty())
      << "Must specify a valid path to --workspace.";

  CHECK(FLAGS_arch.empty() && FLAGS_os.empty())
      << "The architecture and OS names must NOT be manually specified.";

  auto snapshot = grr::LoadSnapshotFromFile();

  // Take the target architecture from the snapshot file.
  FLAGS_arch = snapshot->arch();
  const auto arch_name = remill::GetArchName(FLAGS_arch);
  CHECK(remill::kArchInvalid != arch_name)
      << "Invalid architecture " << FLAGS_arch;

  // Take the target OS from the snapshot file.
  FLAGS_os = snapshot->os();
  const auto os_name = remill::GetOSName(FLAGS_os);
  CHECK(remill::kOSInvalid != os_name)
      << "Invalid OS " << FLAGS_os;

  grr::Run(snapshot);

  llvm::llvm_shutdown();
  google::ShutDownCommandLineFlags();
  google::ShutdownGoogleLogging();
  return EXIT_SUCCESS;
}
