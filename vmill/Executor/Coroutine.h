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

#include <cstdint>

#ifndef VMILL_EXECUTOR_COROUTINE_H_
#define VMILL_EXECUTOR_COROUTINE_H_

#include "vmill/Util/ZoneAllocator.h"

struct ArchState;
struct Memory;

namespace vmill {

struct Task;

// Executes some code (lifted code, runtime code) on another stack, in such a
// way that the runtime can "pause" its execute (while waiting on a
// `std::future`) and then the executor can resume back into the paused
// execution.
class alignas(16) Coroutine {
 public:
  Coroutine(void);

  void Pause(Task *task);
  void Resume(Task *task);

  inline bool ExecutingNow(void) const {
    return 0 < on_stack;
  }

 private:
  Coroutine(const Coroutine &) = delete;
  Coroutine(const Coroutine &&) = delete;
  void operator=(const Coroutine &) = delete;
  void operator=(const Coroutine &&) = delete;

  // Convenient pointer into `stack`.
  uint8_t *stack_end;

  // Rounding mode at the time of a yield/resume.
  int32_t fpu_rounding_mode;

  // Are we current executing on the coroutine stack?
  uint32_t on_stack;

  ZoneAllocation stack;

  static ZoneAllocator gAllocator;
};

}  // namespace vmill

#endif  // VMILL_EXECUTOR_COROUTINE_H_
