/**
 * @file    gpu/host_resource_heap.cpp
 * @brief   Guest VA -> ResourceType registry backing FromGuest resolution.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/host_resource_heap.h"

#include "core/logging.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace bd::gpu {

#if defined(SVR_NATIVE_RENDERER)

namespace {

// Handles [kHandleBase, kHandleEnd) in 16-byte steps: 0x7F000000-0x7FC7FFFF sits between the top
// guest heap (which ends below 0x7F000000) and the GPU register window, so no guest pointer is
// ever in it. Freed handles are reused.
constexpr u32 kHandleBase = 0x7F000010u;
constexpr u32 kHandleEnd = 0x7FC80000u;

struct Entry {
  void *host;
  ResourceType type;
};

std::shared_mutex g_registry_mutex;
std::unordered_map<u32, Entry> g_registry;
std::vector<u32> g_free_handles;
u32 g_next_handle = kHandleBase;

} // namespace

u32 HostResourceHeap::RegisterHost(void *host, ResourceType type) {
  std::unique_lock lock(g_registry_mutex);
  u32 handle = 0;
  if (!g_free_handles.empty()) {
    handle = g_free_handles.back();
    g_free_handles.pop_back();
  } else if (g_next_handle < kHandleEnd) {
    handle = g_next_handle;
    g_next_handle += 16;
  } else {
    BD_ERROR("HostResourceHeap: out of handles ({} live)", g_registry.size());
    return 0;
  }
  g_registry[handle] = {host, type};
  return handle;
}

void HostResourceHeap::Unregister(u32 handle) {
  if (!handle)
    return;
  std::unique_lock lock(g_registry_mutex);
  if (g_registry.erase(handle))
    g_free_handles.push_back(handle);
}

void *HostResourceHeap::LookupHost(u32 handle) {
  if (handle < kHandleBase || handle >= kHandleEnd)
    return nullptr;
  std::shared_lock lock(g_registry_mutex);
  auto it = g_registry.find(handle);
  return it != g_registry.end() ? it->second.host : nullptr;
}

bool HostResourceHeap::GetType(u32 guest_va, ResourceType *out_type) {
  std::shared_lock lock(g_registry_mutex);
  auto it = g_registry.find(guest_va);
  if (it == g_registry.end())
    return false;
  *out_type = it->second.type;
  return true;
}

#else

namespace {

// FromGuest rejects unregistered (sentinel) VAs, and the stored type lets
// Release/DestroyByType dispatch without a uniform offset runtime type field
// (the X360 prefix size differs per resource kind).
std::shared_mutex g_registry_mutex;
std::unordered_map<u32, ResourceType> g_registry;

} // namespace

void HostResourceHeap::Register(u32 guest_va, ResourceType type) {
  std::unique_lock lock(g_registry_mutex);
  g_registry[guest_va] = type;
}

void HostResourceHeap::Unregister(u32 guest_va) {
  std::unique_lock lock(g_registry_mutex);
  g_registry.erase(guest_va);
}

bool HostResourceHeap::IsRegistered(u32 guest_va) {
  std::shared_lock lock(g_registry_mutex);
  return g_registry.count(guest_va) != 0;
}

bool HostResourceHeap::GetType(u32 guest_va, ResourceType *out_type) {
  std::shared_lock lock(g_registry_mutex);
  auto it = g_registry.find(guest_va);
  if (it == g_registry.end())
    return false;
  *out_type = it->second;
  return true;
}

#endif  // SVR_NATIVE_RENDERER

} // namespace bd::gpu
