// SvR 2008: vertex and index buffers for the native renderer's draws (see svr_geometry.h).
//
// Geometry is read at draw time from the device state the game's draw is about to consume, which
// is what the GPU fetches:
//   stream N: vertex fetch constant 95 - N (device + 0x778 - 8 * N: GPU address of the stream's
//     vertex 0, SetStreamSource offset included, | 3; size | endian), the D3DVertexBuffer at
//     device + 0x30AC + 4 * N and the stride / 4 at device + 0x30F0 + N;
//   indices: the D3DIndexBuffer at device + 0x3094 (SetIndices only stores the pointer).
// Each guest range gets a host copy holding the bytes the GPU sees. The CPU rewrites buffers
// whenever it likes (Lock/Unlock between draws, new data loaded into reused memory), so a copy is
// compared with guest memory (content hash) the first time it's used in a frame and after every
// Unlock over it. New content gets a new host buffer (draws already recorded keep the old one);
// content that keeps changing streams through the per-frame upload ring instead.

#include "svr_geometry.h"
#include "gpu/cpu_zones.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <xxhash.h>

#include <plume_render_interface.h>

#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/physical_buffers.h"
#include "gpu/resources.h"
#include "svr_hook.h"
#include "svr_resources.h"

namespace bd::gpu {
namespace {

constexpr u32 kPhysicalMemoryBytes = 0x20000000u;
// No SvR buffer comes near this; a bigger size is a garbage header.
constexpr u32 kMaxBufferBytes = 64u << 20;
// Streams bound from the device. Slot 15 stays empty: re:Blue's stand-in inputs for semantics a
// declaration lacks read it (gpu/vertex_declaration.cpp). SvR uses streams 0-4.
constexpr u32 kStreamCount = 15;
// New content on this many checks in a row: the buffer is dynamic and goes through the upload ring.
constexpr u32 kDynamicAfterChanges = 2;
// Upload ring chunks are 16 MB; bigger dynamic buffers keep getting host buffers.
constexpr u32 kMaxRingBytes = 4u << 20;
// Copies no draw has used for this long are released (their guest memory is likely freed).
constexpr u64 kEvictAfterFrames = 600;

struct Mirror {
  u32 endian = 0;  // xenos::Endian the GPU applies reading this range
  bool index = false;
  bool has_content = false;
  u32 changes = 0;        // consecutive checks that found new content
  u64 hash = 0;
  u64 sample = 0;         // SampleHash of the content when `hash` was taken (large buffers)
  u64 full_frame = 0;     // frame `hash` was last taken in
  u64 same_since = 0;     // frame the content was last found new
  u64 checked_frame = 0;  // frame the copy was last compared in; 0 = compare at the next use
  u64 used_frame = 0;
  std::unique_ptr<plume::RenderBuffer> buffer;  // static content
  plume::RenderBufferReference ring{};          // dynamic content, bindable while ring_epoch is current
  u64 ring_epoch = 0;
};

std::mutex g_mutex;
std::map<u64, Mirror> g_mirrors;  // (GPU address << 32) | dword-rounded size
u32 g_largest = 0;                // largest size in g_mirrors: bounds SvrInvalidateGeometry's search
u64 g_last_sweep = 0;

u64 Key(u32 address, u32 size) { return (u64(address) << 32) | size; }

// SVR_GEOMETRY_PARANOID=1: compare at every draw, to find buffers the CPU rewrites between draws
// without an Unlock.
bool CompareEveryDraw() {
  static const bool on = [] {
    const char *env = std::getenv("SVR_GEOMETRY_PARANOID");
    return env && *env && *env != '0';
  }();
  return on;
}

const u8 *GpuMemory(u32 address, u32 size) {
  if (!address || !size || size > kMaxBufferBytes || u64(address) + size > kPhysicalMemoryBytes)
    return nullptr;
  auto *memory = REX_KERNEL_MEMORY();
  return memory ? memory->TranslatePhysical<const u8 *>(address) : nullptr;
}

// Xenos byte-swaps every 32-bit word it fetches (vertex fetch) or DMAs (indices) by the endian mode
// before splitting it into components, so the host copy holds the swapped bytes. XDK vertex
// buffers and BeginVertices data are 8in32 (re:Blue's bswap32), INDEX16 8in16, INDEX32 8in32.
// 16-bit pairs then come out in the order the shaders' .yxwz swap (swapFloats) expects, whatever
// the mode, because Xenos takes component X from the high half of the swapped word.
void CopyAsGpuReads(void *dst, const u8 *src, u32 size, u32 endian) {
  switch (endian & 3u) {
  case 1:  // k8in16
    rex::memory::copy_and_swap_16_unaligned(dst, src, size / 2);
    break;
  case 2:  // k8in32
    rex::memory::copy_and_swap_32_unaligned(dst, src, size / 4);
    break;
  case 3:  // k16in32
    rex::memory::copy_and_swap_16_in_32_unaligned(dst, src, size / 4);
    break;
  default:  // kNone
    std::memcpy(dst, src, size);
    break;
  }
}

// Draws already recorded keep a retired copy until its frame slot's fence.
void Retire(Mirror &m) {
  if (m.buffer)
    RetirePhysicalBuffer(std::move(m.buffer));
  m.ring = {};
}

std::unique_ptr<plume::RenderBuffer> CreateCopy(const u8 *src, u32 size, u32 endian, bool index) {
  auto *device = Video::HostDevice();
  if (!device)
    return nullptr;
  const plume::RenderHeapType heap = GeometryHeapType(device, GeometryClass::Static);
  auto buffer = CreateHostBuffer(device,
                                 index ? plume::RenderBufferDesc::IndexBuffer(size, heap)
                                       : plume::RenderBufferDesc::VertexBuffer(size, heap),
                                 index ? "svr-ib" : "svr-vb");
  if (!buffer)
    return nullptr;
  void *mapped = buffer->map();
  if (!mapped)
    return nullptr;
  CopyAsGpuReads(mapped, src, size, endian);
  buffer->unmap();
  return buffer;
}

// WWE '12 has no Unlock hooks, so every buffer drawn is hashed in full once a frame: the static
// meshes (wrestlers, arena, crowd), megabytes each frame, were ~4 % of the render thread
// (2026-10-20 profile). A large buffer whose content has not changed for kSettledFrames gets a
// sampled hash (32 slices of 64 bytes) each frame and its full hash every kFullHashFrames frames.
// Dynamic buffers change every few frames and never settle; a settled buffer rewritten in place
// shows at once if a slice changes, or within a few frames at worst.
constexpr u32 kSampledHashMin = 32 * 1024;
constexpr u64 kSettledFrames = 300;
constexpr u64 kFullHashFrames = 32;

}  // namespace
// Bytes hashed in full and buffers checked by sample, for the [perf] line (present.cpp).
std::atomic<u64> g_geometry_hashed_bytes{0}, g_geometry_sampled{0};
namespace {

u64 SampleHash(const u8 *data, u32 size) {
  constexpr u32 kSlices = 32;
  u64 hash = size;
  for (u32 i = 0; i < kSlices; ++i)
    hash = XXH3_64bits_withSeed(data + u64(size - 64) * i / (kSlices - 1), 64, hash);
  return hash;
}

// The host copy of [address, address + size) (GPU address) as the GPU reads it.
bool ResolveLocked(u32 address, u32 size, u32 endian, bool index, u64 frame,
                   plume::RenderBufferReference &out) {
  size = (size + 3u) & ~3u;  // the GPU reads whole words (an odd INDEX16 count ends mid-word)
  const u8 *src = GpuMemory(address, size);
  if (!src)
    return false;
  Mirror &m = g_mirrors[Key(address, size)];
  g_largest = std::max(g_largest, size);
  m.used_frame = frame;
  const bool same_kind = m.has_content && m.endian == endian && m.index == index;
  const bool ring_current = m.ring.ref && m.ring_epoch == UploadEpoch();
  if (same_kind && m.checked_frame == frame && !CompareEveryDraw()) {
    if (m.buffer) {
      out = m.buffer.get();
      return true;
    }
    if (ring_current) {
      out = m.ring;
      return true;
    }
  }
  const bool sampled = size >= kSampledHashMin && !CompareEveryDraw();
  const u64 sample = sampled ? SampleHash(src, size) : 0;
  if (sampled && same_kind && m.buffer && m.checked_frame != 0 && sample == m.sample &&
      frame - m.same_since >= kSettledFrames && frame - m.full_frame < kFullHashFrames) {
    m.checked_frame = frame;
    g_geometry_sampled.fetch_add(1, std::memory_order_relaxed);
    out = m.buffer.get();
    return true;
  }
  g_geometry_hashed_bytes.fetch_add(size, std::memory_order_relaxed);
  const u64 hash = XXH3_64bits(src, size);
  if (same_kind && hash == m.hash) {
    m.checked_frame = frame;
    m.sample = sample;
    m.full_frame = frame;
    if (m.buffer) {
      out = m.buffer.get();
      return true;
    }
    if (ring_current) {
      out = m.ring;
      return true;
    }
    // Dynamic content that stopped changing (its ring copy is from an older frame): it settles
    // into a host buffer below.
  }
  const u32 changes = (same_kind && hash != m.hash) ? m.changes + 1 : 0;
  plume::RenderBufferReference fresh_ring{};
  std::unique_ptr<plume::RenderBuffer> fresh;
  if (changes >= kDynamicAfterChanges && size <= kMaxRingBytes) {
    const ConstantAllocation alloc = AllocateUploadBytes(size, 256);
    if (alloc.memory) {
      CopyAsGpuReads(alloc.memory, src, size, endian);
      fresh_ring = alloc.ref;
    }
  }
  if (!fresh_ring.ref) {
    fresh = CreateCopy(src, size, endian, index);
    if (!fresh) {
      // No device or out of memory: keep serving the old copy and look again at the next use.
      m.checked_frame = 0;
      if (!m.buffer)
        return false;
      out = m.buffer.get();
      return true;
    }
  }
  Retire(m);
  m.endian = endian;
  m.index = index;
  m.hash = hash;
  m.sample = sample;
  m.full_frame = frame;
  m.same_since = frame;
  m.changes = changes;
  m.checked_frame = frame;
  m.has_content = true;
  if (fresh_ring.ref) {
    m.ring = fresh_ring;
    m.ring_epoch = UploadEpoch();
    out = m.ring;
  } else {
    m.buffer = std::move(fresh);
    out = m.buffer.get();
  }
  return true;
}

void SweepLocked(u64 frame) {
  if (frame < g_last_sweep + 60)
    return;
  g_last_sweep = frame;
  for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
    if (frame > it->second.used_frame + kEvictAfterFrames) {
      Retire(it->second);
      it = g_mirrors.erase(it);
    } else {
      ++it;
    }
  }
}

struct StreamBinding {
  plume::RenderBufferReference ref{};
  u32 size = 0;
  u32 stride = 0;
};

// SetStreamSource (sub_8225A648) keeps stream N in vertex fetch constant 95 - N: the last two
// dwords of fetch slot 31 for stream 0, i.e. device + 0x778 - 8 * N.
void StreamFetch(const D3DDevice &device, u32 stream, u32 &dword0, u32 &dword1) {
  const u32 constant = 95u - stream;
  const DeviceFetchConstant &slot = device.fetchConstants[constant / 3u];
  dword0 = u32(slot.dword[(constant % 3u) * 2u]);
  dword1 = u32(slot.dword[(constant % 3u) * 2u + 1u]);
}

// WWE '12 bring-up: says why a stream did not resolve (first failures only).
void WWE12LogStreamFailure(u32 stream, const char *why, u32 vb_va, u32 dword0, u32 dword1) {
  static std::atomic<u32> s_logged{0};
  if (s_logged.fetch_add(1, std::memory_order_relaxed) < 16)
    BD_WARN("WWE12 stream {} unresolved: {} (vb 0x{:08X}, fetch 0x{:08X} 0x{:08X})", stream, why,
            vb_va, dword0, dword1);
}

bool ResolveStreamLocked(const D3DDevice &device, u32 stream, u64 frame, StreamBinding &out) {
  const u32 vb_va = u32(device.streamSource[stream]);
  u32 dword0, dword1;
  StreamFetch(device, stream, dword0, dword1);
  if (!vb_va) {
    WWE12LogStreamFailure(stream, "no vertex buffer", vb_va, dword0, dword1);
    // Which of the 96 vertex fetch constants hold a vertex fetch right now (type 3)? WWE '12
    // may write fetch constants directly instead of going through SetStreamSource.
    static std::atomic<u32> s_dumped{0};
    if (s_dumped.fetch_add(1, std::memory_order_relaxed) < 4) {
      std::string valid;
      for (u32 c = 0; c < 96; ++c) {
        const DeviceFetchConstant &slot = device.fetchConstants[c / 3u];
        const u32 d0 = u32(slot.dword[(c % 3u) * 2u]);
        const u32 d1 = u32(slot.dword[(c % 3u) * 2u + 1u]);
        if ((d0 & kFetchTypeMask) == kFetchTypeVertex)
          valid += fmt::format(" c{}={:08X}/{:08X}", c, d0, d1);
      }
      BD_WARN("WWE12 vertex fetch constants set:{}", valid.empty() ? " none" : valid.c_str());
    }
    return false;
  }
  if ((dword0 & kFetchTypeMask) != kFetchTypeVertex) {
    WWE12LogStreamFailure(stream, "fetch constant not a vertex one", vb_va, dword0, dword1);
    return false;
  }
  const u32 address = dword0 & 0x1FFFFFFCu;  // GPU address of the stream's vertex 0
  const u32 size = dword1 & 0x03FFFFFCu;     // bytes from there to the end of the buffer
  const u32 endian = dword1 & 3u;
  // One copy of the whole D3DVertexBuffer, shared by streams set at different offsets into it.
  u32 base = address;
  u32 base_size = size;
  if (const auto *vb = bd::mem::at<const D3DVertexBuffer>(vb_va)) {
    const u32 vb_base = SvrGpuAddress(u32(vb->FetchLo) & ~3u);
    const u32 vb_size = u32(vb->FetchHi) & 0x03FFFFFCu;
    if (address >= vb_base && u64(address) + size <= u64(vb_base) + vb_size) {
      base = vb_base;
      base_size = vb_size;
    }
  }
  plume::RenderBufferReference ref;
  if (!ResolveLocked(base, base_size, endian, /*index=*/false, frame, ref)) {
    WWE12LogStreamFailure(stream, "guest range not mirrored", vb_va, dword0, dword1);
    return false;
  }
  out.ref = plume::RenderBufferReference(ref.ref, ref.offset + (address - base));
  out.size = size;
  out.stride = u32(device.streamStrideDiv4[stream]) * 4u;
  return true;
}

struct IndexBinding {
  plume::RenderBufferReference ref{};
  u32 size = 0;
  plume::RenderFormat format = plume::RenderFormat::R16_UINT;
};

// SetIndices (sub_8225A768) only stores the D3DIndexBuffer at device + 0x3094; DrawIndexedVertices
// (sub_825A4340) reads Common (bit 31 = INDEX32, bits 30-29 = endian), Address (+0x18, through
// GPU_CONVERT_CPU_TO_GPU_ADDRESS) and Size (+0x1C, exact bytes) at draw time.
bool ResolveIndicesLocked(const D3DDevice &device, u64 frame, IndexBinding &out) {
  const auto *ib = bd::mem::at<const D3DIndexBuffer>(u32(device.indexBuffer));
  if (!ib)
    return false;
  const u32 common = ib->resource.Common;
  const u32 size = ib->FetchHi;  // Size
  if (!ResolveLocked(SvrGpuAddress(ib->FetchLo), size, (common >> 29) & 3u, /*index=*/true, frame,
                     out.ref))
    return false;
  out.size = size;
  out.format = (common & 0x80000000u) ? plume::RenderFormat::R32_UINT
                                      : plume::RenderFormat::R16_UINT;
  return true;
}

// D3DVertexBuffer_Unlock (sub_82252818) / D3DIndexBuffer_Unlock (sub_82253178), after the game's
// own: the CPU has finished writing the buffer. SvR re-fills dynamic buffers this way between draws
// (sub_825F9A28: a 120-byte quad; sub_8257E530: positions every frame).
void VertexBufferUnlocked(u32 vb_va) {
  if (const auto *vb = bd::mem::at<const D3DVertexBuffer>(vb_va))
    SvrInvalidateGeometry(u32(vb->FetchLo) & ~3u, u32(vb->FetchHi) & 0x03FFFFFCu);
}

void IndexBufferUnlocked(u32 ib_va) {
  if (const auto *ib = bd::mem::at<const D3DIndexBuffer>(ib_va))
    SvrInvalidateGeometry(ib->FetchLo, ib->FetchHi);
}

}  // namespace

void SvrBindDrawGeometry(u32 device_guest, bool indexed) {
  WWE12_CPU_ZONE(kGeometry);
  const auto *device = bd::mem::at<const D3DDevice>(device_guest);
  if (!device)
    return;
  const u64 frame = SvrFrameIndex();
  const GuestVertexDeclaration *decl = SvrLookupVertexDeclaration(u32(device->vertexDeclaration));
  StreamBinding streams[kStreamCount];
  bool used[kStreamCount] = {};
  IndexBinding indices;
  bool have_indices = false;
  u32 missing = 0;
  {
    std::lock_guard lock(g_mutex);
    SweepLocked(frame);
    for (u32 n = 0; n < kStreamCount; ++n) {
      used[n] = decl ? decl->vertexStreams[n] : u32(device->streamSource[n]) != 0u;
      if (used[n] && !ResolveStreamLocked(*device, n, frame, streams[n]))
        missing |= 1u << n;
    }
    if (indexed)
      have_indices = ResolveIndicesLocked(*device, frame, indices);
  }
  if (missing || (indexed && !have_indices)) {
    static std::atomic<u32> s_warned{0};
    const u32 k = s_warned.fetch_add(1, std::memory_order_relaxed);
    if (k < 16 || (k & (k - 1)) == 0)
      BD_WARN("SvR geometry #{}: unresolved streams 0x{:X}{} (index buffer 0x{:08X})", k, missing,
              indexed && !have_indices ? ", no index buffer" : "", u32(device->indexBuffer));
  }
  // Outside g_mutex: the Video setters take state().mutex.
  for (u32 n = 0; n < kStreamCount; ++n) {
    if (used[n])
      Video::SetVertexStream(n, streams[n].ref, streams[n].size, streams[n].stride);
  }
  if (indexed)
    Video::SetIndexBufferRef(indices.ref, have_indices ? indices.size : 0u, indices.format);
}

void SvrInvalidateGeometry(u32 address, u32 size) {
  if (!address || !size)
    return;
  const u32 start = SvrGpuAddress(address);
  const u64 end = u64(start) + size;
  std::lock_guard lock(g_mutex);
  const u32 from = start > g_largest ? start - g_largest : 0u;
  for (auto it = g_mirrors.lower_bound(Key(from, 0));
       it != g_mirrors.end() && (it->first >> 32) < end; ++it) {
    if ((it->first >> 32) + u32(it->first) > start)
      it->second.checked_frame = 0;
  }
}

}  // namespace bd::gpu

#if !defined(WWE12_NO_BUFFER_UNLOCK)  // WWE '12: no Unlock hooks; buffers are hashed per frame
SVR_HOOK_AFTER(D3DVertexBuffer_Unlock, bd::gpu::VertexBufferUnlocked);
SVR_HOOK_AFTER(D3DIndexBuffer_Unlock, bd::gpu::IndexBufferUnlocked);
#endif
