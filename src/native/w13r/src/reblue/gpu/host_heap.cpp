/**
 * @file    gpu/host_heap.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/host_heap.h"

#include <mutex>
#include <new>

#include <o1heap.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "core/logging.h"
#include "core/profiling.h"

// Tracy mem/plot macros. These compile to nothing without
// REXGLUE_ENABLE_PROFILING.
#if defined(REXGLUE_ENABLE_PROFILING)
#define BD_MEM_ALLOC(ptr, size)                                                \
  do {                                                                         \
    if (TracyIsStarted)                                                        \
      TracyAllocN((ptr), (size), "HostResourceHeap");                          \
  } while (0)
#define BD_MEM_FREE(ptr)                                                       \
  do {                                                                         \
    if (TracyIsStarted)                                                        \
      TracyFreeN((ptr), "HostResourceHeap");                                   \
  } while (0)
#define BD_MEM_PLOT(name, val)                                                 \
  do {                                                                         \
    if (TracyIsStarted)                                                        \
      TracyPlot((name), static_cast<double>(val));                             \
  } while (0)
#else
#define BD_MEM_ALLOC(ptr, size) ((void)0)
#define BD_MEM_FREE(ptr) ((void)0)
#define BD_MEM_PLOT(name, val) ((void)0)
#endif

namespace bd::gpu {

namespace {

// 64 MiB in the 512 MiB vA0000000 window. Host shadow structs are small, so
// this is generous. The "HostHeap Peak Bytes" plot says when to raise it.
constexpr u32 kHeapSize = 64u * 1024u * 1024u;

struct HeapState {
  std::mutex mutex; // o1heap is not internally synchronized.
  O1HeapInstance *handle = nullptr;
  u32 guest_va = 0;
  void *host_base = nullptr;
  u64 live_count = 0;
  bool ready = false;
  bool oom_logged = false;
};

HeapState &state() {
  static HeapState s;
  return s;
}

#if defined(REXGLUE_ENABLE_PROFILING)
// Caller holds state().mutex.
void EmitPlotsLocked(const HeapState &s) {
  const O1HeapDiagnostics d = o1heapGetDiagnostics(s.handle);
  BD_MEM_PLOT("HostHeap Live Bytes", d.allocated);
  BD_MEM_PLOT("HostHeap Peak Bytes", d.peak_allocated);
  BD_MEM_PLOT("HostHeap Live Allocs", s.live_count);
  BD_MEM_PLOT("HostHeap OOM Count", d.oom_count);
}
#else
void EmitPlotsLocked(const HeapState &) {}
#endif

} // namespace

HostHeap &HostHeap::Get() {
  static HostHeap instance;
  return instance;
}

#if !defined(SVR_NATIVE_RENDERER)
bool HostHeap::Init() {
  HeapState &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.ready)
    return true;

  auto *memory = REX_KERNEL_MEMORY();
  if (!memory) {
    BD_ERROR("HostHeap::Init: no Memory instance");
    return false;
  }

  using namespace rex::memory;
  // (physical, 64 KB page) -> vA0000000, host_address_offset 0.
  BaseHeap *heap =
      memory->LookupHeapByType(/*physical=*/true, /*page_size=*/64 * 1024);
  if (!heap) {
    BD_ERROR(
        "HostHeap::Init: LookupHeapByType(physical, 64K) returned null");
    return false;
  }

  u32 guest_va = 0;
  const bool ok =
      heap->Alloc(kHeapSize, 64u * 1024u,
                  kMemoryAllocationReserve | kMemoryAllocationCommit,
                  kMemoryProtectRead | kMemoryProtectWrite,
                  /*top_down=*/true, &guest_va);
  if (!ok || !guest_va) {
    BD_ERROR("HostHeap::Init: failed to reserve {} bytes in vA0000000",
             kHeapSize);
    return false;
  }

  void *host_base = memory->TranslateVirtual<void *>(guest_va);
  O1HeapInstance *handle = o1heapInit(host_base, kHeapSize);
  if (!handle) {
    BD_ERROR("HostHeap::Init: o1heapInit failed (size {})", kHeapSize);
    return false;
  }

  s.handle = handle;
  s.guest_va = guest_va;
  s.host_base = host_base;
  s.ready = true;

#if defined(REXGLUE_ENABLE_PROFILING)
  if (TracyIsStarted) {
    TracyPlotConfig("HostHeap Live Bytes", tracy::PlotFormatType::Memory, true,
                    true, 0);
    TracyPlotConfig("HostHeap Peak Bytes", tracy::PlotFormatType::Memory, true,
                    true, 0);
    TracyPlotConfig("HostHeap Live Allocs", tracy::PlotFormatType::Number, true,
                    true, 0);
    TracyPlotConfig("HostHeap OOM Count", tracy::PlotFormatType::Number, true,
                    true, 0);
  }
#endif
  return true;
}

void *HostHeap::Alloc(std::size_t size, std::size_t alignment) {
  BD_CPU_ZONE("HostHeap::Alloc");
  HeapState &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.ready) {
    BD_ERROR("HostHeap::Alloc before Init");
    return nullptr;
  }
  if (size == 0)
    size = 1;
  // o1heap only guarantees O1HEAP_ALIGNMENT (16), so reject larger to avoid
  // silent under-alignment.
  if (alignment > O1HEAP_ALIGNMENT) {
    BD_ERROR("HostHeap::Alloc alignment {} > {} unsupported", alignment,
             static_cast<std::size_t>(O1HEAP_ALIGNMENT));
    return nullptr;
  }

  void *p = o1heapAllocate(s.handle, size);
  if (!p) {
    if (!s.oom_logged) {
      const O1HeapDiagnostics d = o1heapGetDiagnostics(s.handle);
      BD_ERROR("HostHeap OOM: req {} allocated {}/{} peak {} (raise "
               "kHeapSize)",
               size, d.allocated, d.capacity, d.peak_allocated);
      s.oom_logged = true;
    }
    EmitPlotsLocked(s);
    return nullptr;
  }

  ++s.live_count;
  BD_MEM_ALLOC(p, size);
  EmitPlotsLocked(s);
  return p;
}

void HostHeap::Free(void *host_ptr) {
  if (!host_ptr)
    return;
  BD_CPU_ZONE("HostHeap::Free");
  HeapState &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.ready)
    return;
  BD_MEM_FREE(host_ptr);
  o1heapFree(s.handle, host_ptr);
  if (s.live_count)
    --s.live_count;
  EmitPlotsLocked(s);
}

u32 HostHeap::AllocGuest(std::size_t size, std::size_t alignment) {
  void *host = Alloc(size, alignment);
  if (!host)
    return 0;
  return REX_KERNEL_MEMORY()->HostToGuestVirtual(host);
}

void HostHeap::FreeGuest(u32 guest_va) {
  if (!guest_va)
    return;
  void *host = REX_KERNEL_MEMORY()->TranslateVirtual<void *>(guest_va);
  Free(host);
}

HostHeap::Snapshot HostHeap::GetSnapshot() {
  HeapState &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  Snapshot out{};
  out.ready = s.ready;
  out.live_count = s.live_count;
  if (s.ready) {
    const O1HeapDiagnostics d = o1heapGetDiagnostics(s.handle);
    out.capacity = d.capacity;
    out.allocated = d.allocated;
    out.peak_allocated = d.peak_allocated;
    out.oom_count = d.oom_count;
  }
  return out;
}

#else  // SVR_NATIVE_RENDERER

// SvR 2008: the renderer's objects live in host memory. The hooks observe SvR's own D3D objects, so
// the game never holds a pointer to one of these and none of its 512 MB is reserved for them (a
// 64 MB reservation starves SvR's own physical allocations: Bink's frame textures fail to allocate
// and the movie decoder then writes through an uninitialised pitch).
bool HostHeap::Init() {
  HeapState &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.ready = true;
  return true;
}

void *HostHeap::Alloc(std::size_t size, std::size_t alignment) {
  if (alignment > 16) {
    BD_ERROR("HostHeap::Alloc alignment {} > 16 unsupported", alignment);
    return nullptr;
  }
  void *p = ::operator new(size ? size : 1, std::align_val_t{16}, std::nothrow);
  if (p) {
    HeapState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.live_count;
  }
  return p;
}

void HostHeap::Free(void *host_ptr) {
  if (!host_ptr)
    return;
  ::operator delete(host_ptr, std::align_val_t{16});
  HeapState &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.live_count)
    --s.live_count;
}

u32 HostHeap::AllocGuest(std::size_t, std::size_t) {
  BD_ERROR("HostHeap::AllocGuest: no guest-visible host objects in SvR");
  return 0;
}

void HostHeap::FreeGuest(u32) {}

HostHeap::Snapshot HostHeap::GetSnapshot() {
  HeapState &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  Snapshot out{};
  out.ready = s.ready;
  out.live_count = s.live_count;
  return out;
}

#endif  // SVR_NATIVE_RENDERER

} // namespace bd::gpu
