/**
 * @file    core/memory_helpers.cpp
 * @brief   The validating half of the Xbox 360 address space accessor.
 * @license BSD 3-Clause, see LICENSE
 */
#include "core/memory_helpers.h"

#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <atomic>

namespace bd::mem {

namespace {
std::atomic<bool> g_replay_physical_views{false};
}

void set_replay_physical_views(bool on) {
  g_replay_physical_views.store(on, std::memory_order_relaxed);
}

bool ready() {
  auto *rt = rex::Runtime::instance();
  return rt && rt->memory();
}

void *try_translate(u32 va, u32 align) {
  // The guarded Runtime map, not REX_KERNEL_MEMORY: the checked accessors run
  // off the guest thread, where the kernel state pointer that macro chases is
  // not guaranteed to be there.
  auto *rt = rex::Runtime::instance();
  auto *memory = rt ? rt->memory() : nullptr;
  if (!memory || !va || (va & (align - 1)))
    return nullptr;

  if (va >= 0xA0000000u && g_replay_physical_views.load(std::memory_order_relaxed))
    return memory->TranslateVirtual<void *>(va);
  auto *heap = memory->LookupHeap(va);
  u32 protect = 0;
  if (!heap || !heap->QueryProtect(va, &protect) ||
      !(protect & rex::memory::kMemoryProtectRead)) {
    return nullptr;
  }
  return memory->TranslateVirtual<void *>(va);
}

} // namespace bd::mem
