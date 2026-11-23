// SvR 2008 host resources for the game's own D3D textures and surfaces (see svr_resources.h).

#include "svr_resources.h"
#include "gpu/cpu_zones.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#if defined(_WIN32)
#include <windows.h>
#else
#include <sched.h>
#include <sys/resource.h>
#include <thread>
#endif

#include <fmt/format.h>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/host_resource_heap.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/output.h"
#include "gpu/resources.h"
#include "gpu/vertex_declaration.h"
#include "wwe12_display.h"  // FPS counter, frame limit (global function)

namespace svr {
void HookProfileTick(uint64_t frame);  // wwe13_native_stubs.cpp (live hook CPU profile)
void GuestProbeTick(uint64_t frame);   // wwe13_native_stubs.cpp (guest write probe)
}

// src/native/crash_trace.cpp: hang watchdog, fed once per presented frame.
void SvrWatchdogHeartbeat();
// src/native/display_size.cpp.
bool SvrDisplaySize(uint32_t &w, uint32_t &h);
bool SvrOnSteamDeck();

REXCVAR_DEFINE_BOOL(svr_release_stale_targets, true, "SvR",
                    "Free host render targets the game no longer uses (false: keep them, the "
                    "0.6.0 behaviour, which leaks video memory).");
REXCVAR_DEFINE_BOOL(svr_clear_new_surfaces, true, "SvR",
                    "Clear every new host render surface to transparent black. Host surfaces live in "
                    "recycled GPU memory: NVIDIA happens to hand them out black, older Intel/AMD "
                    "drivers with leftovers (suspected cause of the right-strip blocks, #7/#11/#13).");
REXCVAR_DEFINE_BOOL(svr_debug_magenta, false, "SvR",
                    "Dev: fill every new host render target with magenta, so whatever the game reads "
                    "before drawing it shows up (NVIDIA starts them black, older Intel/AMD with "
                    "leftover memory: the right-strip blocks of issues #7/#11/#13).");
REXCVAR_DEFINE_BOOL(svr_seed_gpu_textures, true, "SvR",
                    "Start GPU-written textures from what the CPU put in their memory (wrestler "
                    "skin composites: eyes and teeth).");
REXCVAR_DEFINE_BOOL(svr_resolve_readback, true, "SvR",
                    "Copy the first resolves of each work texture back to the game's memory, as "
                    "the Xbox does (custom attires are built from it).");
REXCVAR_DEFINE_BOOL(svr_dump_readbacks, false, "SvR",
                    "Debug: save each read-back of a work texture 1024 px wide or more to "
                    "logs\readback_*.rgba (16-byte header: width, height, row pitch, va)");
REXCVAR_DEFINE_BOOL(svr_log_readbacks, false, "SvR",
                    "Debug: log every read-back of a work texture (memory range written) and "
                    "every one-off resolve that is not read back, at info");
REXCVAR_DEFINE_INT32(svr_readback_streak, 32, "SvR",
                     "Resolves of one work texture copied back in a row before the copies pause "
                     "until it rests (then it is copied once more). Was 6: attires the game builds "
                     "over more resolves stayed white")
    .range(1, 1000);
// 11-21: at a scene start every per-frame target (shadow / lighting maps, 64x64 tiles...) is read back
// synchronously for its first svr_readback_streak frames, each resolve with its own GPU wait (bursts of
// 40-50 ms frames). On: those streak read-backs are collected and done once per target per frame at the
// frame's Swap, all with ONE GPU wait. Still every frame during the streak (attire builds, issue #27).
REXCVAR_DEFINE_BOOL(svr_readback_batch, false, "SvR",
                    "Streak read-backs batched at the frame's Swap (one GPU wait, once per target) "
                    "instead of one synchronous read-back per resolve");
REXCVAR_DEFINE_BOOL(svr_rebuild_target_mips, true, "SvR",
                    "Rebuild the smaller mip levels of GPU-painted textures (wrestler skins: eyes, "
                    "teeth, blood) from their first level after each update.");
REXCVAR_DEFINE_BOOL(svr_match_guest_mips, true, "SvR",
                    "Sample the texture mip levels the game uses at 720p when rendering above it "
                    "(its eye/teeth textures break at the base level).");
REXCVAR_DEFINE_BOOL(svr_scale_square_surfaces, false, "SvR",
                    "Render square work surfaces (shadow maps, reflections) at the internal "
                    "resolution too; off keeps them at the Xbox size, as the 16:9 screen ones are "
                    "the only ones that need it.");
REXCVAR_DEFINE_INT32(svr_render_scale, 0, "SvR",
                     "Internal resolution: 360, 480, 540 (below the Xbox, for weak GPUs), 720, 1080, 1440, 2160 (4K) or 2880 (also 1 = 720p, "
                     "2 = 1440p, 3 = 4K, 4 = 2880p). 0 = auto: from the display (1440p on "
                     "1080p/1440p screens, 4K on 4K screens), 720p on Steam Deck and on "
                     "integrated GPUs");

// Secondary shader keys, generated by tools/gen_shader_keys.py (global scope).
#if defined(WWE12_SHADER_KEYS)
struct WWE12ShaderKey {
  uint64_t full, virt, phys;
  uint32_t vertex;
};
extern const WWE12ShaderKey g_wwe12_shader_keys[];
extern const size_t g_wwe12_shader_key_count;
#endif

REXCVAR_DECLARE(bool, svr_readback_async);  // resolve.cpp
namespace bd::gpu {

// Host-side creation from re:Blue's CreateTexture/CreateSurface hooks (gpu/hooks/resource.cpp).
GuestTexture *SvrCreateHostTexture(u32 width, u32 height, u32 depth, u32 levels, u32 usage,
                                   u32 format, u32 d3d_type);
GuestTexture *SvrCreateHostSurface(u32 width, u32 height, u32 format);

namespace {

std::mutex g_mutex;

// A host resource made for a guest surface or GPU-written texture, with the guest description it
// was made from. Road to WrestleMania unloads characters between scenes and reuses those guest
// objects for other textures (another wrestler's composited skin at another size): a host
// resource kept by address alone then showed the previous character's skin mixed into the new one.
struct HostTarget {
  GuestTexture *host = nullptr;
  u32 desc[2] = {};  // surface: size bits, format; texture: fetch dwords 1 (format, base), 2 (size)
  u32 extent = 0;    // surface: the tiled-rendering extent it was sized to (see below), w << 16 | h
  u64 last_used = 0; // frame it was last drawn to, resolved into or sampled
  bool in_memory = false;  // texture: its last resolve was read back into the game's memory
  u64 created = 0;  // frame the host resource was made in
  u64 resolves = 0;  // texture: resolves into it so far (an async read-back must still be the last)
};
std::unordered_map<u32, HostTarget> g_surfaces;  // D3DSurface VA -> host render target
std::unordered_map<u32, HostTarget> g_targets;   // texture VA -> GPU-written host texture
// Bumped (under g_mutex) whenever g_targets gains or loses an entry: SvrResolveTexture's per-thread
// fast-path cache (wwe13_draw_fast_path) holds only textures that were not targets at that count.
std::atomic<u64> g_targets_gen{0};

// Predicated tiling: WWE '12 renders its 3D scenes (matches, entrances) into an EDRAM surface one
// tile wide (320x736 for 4 tiles of 1280x720), draws the scene once, and the Xbox GPU replays it
// per tile; then the game resolves each tile (rects 0-320, 320-640... of the full frame). The
// host has no EDRAM limit: the surface (and its depth buffer) grows to the full extent the
// resolves reach, the scene is drawn once at full size, and every tile resolve copies it whole.
std::unordered_map<u32, u32> g_tiled_extent;  // D3DSurface VA -> w << 16 | h (guest pixels)
// The surface description (size bits << 32 | format) each g_tiled_extent entry belongs to. The game
// reuses the memory of freed surface objects: an extent left by a scene surface was applied to a
// 1120x1120 shadow surface at the same address in the Road to WrestleMania intro, its colour grew
// to 1280 wide while its depth buffer did not, and the depth clear wrote past the depth image: GPU
// lost, picture frozen while the game ran on (0.6.0 and 0.7.0, RTX 4060, 2026-10-17).
std::unordered_map<u32, u64> g_tiled_desc;
std::unordered_map<u32, float> g_surface_scale;  // D3DSurface VA -> host pixels per guest pixel
// Full extent the tiled surfaces of a given description grew to, keyed by size bits << 32 | format.
// Each scene allocates new surface objects; without this they started one tile wide and grew during
// their first frame, whose tile resolves past the first tile copied an empty target: a flash of black
// over the right of the picture before the bell (NVIDIA), leftover garbage on other GPUs.
std::unordered_map<u64, u32> g_learned_extent;
std::atomic<u64> g_surface_frame{0};  // presented frames, for HostTarget::last_used
std::atomic<u32> g_surface_scale_gen{1};  // bumped on each g_surface_scale change

// The texture's GPUTEXTURE_FETCH_CONSTANT (D3DTexture+0x1C), as host-order dwords.
bool ReadFetch(u32 texture_va, rex::graphics::xenos::xe_gpu_texture_fetch_t &fetch) {
  const auto *texture = bd::mem::at<const D3DTexture>(texture_va);
  if (!texture)
    return false;
  for (int i = 0; i < 6; ++i)
    (&fetch.dword_0)[i] = u32(texture->Format.dword[i]);
  return fetch.type == rex::graphics::xenos::FetchConstantType::kTexture;
}

// Lets go of a host resource whose guest object now describes something else, as the game's own
// D3DResource_Release does at the last reference. A surface goes back to the surface registry,
// which hands it to the next surface of the same size and format and frees it after 900 idle
// frames; a texture is destroyed once the GPU is done with the frame that last used it
// (QueueResourceDestroy, then DestroyResourceNow unlinks it from the resolve bookkeeping). They
// used to be kept forever: every new scene's tiled surfaces regrow (320, 640, 960, 1280 wide) and
// render targets change shape, so a long session piled up gigabytes of video memory.
std::vector<GuestTexture *> g_retired;
u64 g_released = 0;
void RetireHost(GuestTexture *host) {
  if (!host)
    return;
  if (!REXCVAR_GET(svr_release_stale_targets)) {
    g_retired.push_back(host);
    BD_INFO("SvR: {} stale host target(s) set aside", g_retired.size());
    return;
  }
  const u32 va = HostResourceHeap::ToGuest(host);
  ResourceType type;
  if (!va || !HostResourceHeap::GetType(va, &type))
    return;
  host->x360.as_surface.resource.ReferenceCount = 0u;
  Video::QueueResourceDestroy(va, type);
  if ((++g_released & 63) == 1)
    BD_INFO("SvR: {} stale host target(s) released", g_released);
}

// Whether g_targets' entry for texture_va still matches the texture there; a stale one is retired
// and removed. Caller holds g_mutex.
bool TargetCurrent(u32 texture_va, std::unordered_map<u32, HostTarget>::iterator it) {
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  if (!ReadFetch(texture_va, fetch) ||
      (fetch.dword_1 == it->second.desc[0] && fetch.dword_2 == it->second.desc[1]))
    return true;
  BD_INFO("SvR target 0x{:08X}: now {}x{} fmt {} (was another texture) -> rebuilt", texture_va,
          fetch.size_2d.width + 1, fetch.size_2d.height + 1, u32(fetch.format));
  RetireHost(it->second.host);
  g_targets.erase(it);
  g_targets_gen.fetch_add(1, std::memory_order_relaxed);
  return false;
}

}  // namespace

// Scaled guest size in host pixels. The scales are 1, 1.5, 2, 3 or 4 and the game's surface sizes
// are even, so the product is exact; rounding only guards odd sizes.
static u32 Scaled(u32 v, float scale) { return u32(float(v) * scale + 0.5f); }

REXCVAR_DEFINE_BOOL(svr_auto_lowend, true, "SvR",
                    "Automatic resolution picks 540p (4 logical processors or fewer) or 480p (2 or fewer) on integrated "
                    "GPUs; false = keep 720p. Has no effect when svr_render_scale is set");

// Logical processors this process may run on (the smaller of the machine's count and the affinity mask).
REXCVAR_DEFINE_INT32(svr_lowend_cpu_count, 0, "SvR",
                     "Test: pretend the PC has this many logical processors for svr_auto_lowend (0 = detect)")
    .range(0, 256);

// Diagnostic dumps: a wide-char path on Windows, the UTF-8 path elsewhere.
static FILE *SvrOpenWrite(const std::filesystem::path &path) {
#if defined(_WIN32)
  return _wfopen(path.c_str(), L"wb");
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

static unsigned LogicalProcessorsAvailable() {
  if (const int forced = REXCVAR_GET(svr_lowend_cpu_count); forced > 0) return unsigned(forced);
#if defined(_WIN32)
  unsigned n = unsigned(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
  DWORD_PTR proc = 0, sys = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc) {
    unsigned a = 0;
    for (DWORD_PTR m = proc; m; m &= m - 1) ++a;
    n = std::min(n, a);
  }
#else
  unsigned n = std::thread::hardware_concurrency();
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_COUNT(&set) > 0)
    n = n ? std::min(n, unsigned(CPU_COUNT(&set))) : unsigned(CPU_COUNT(&set));
#endif
  return n ? n : 1;
}

float SvrRenderScale() {
  static std::atomic<float> s_scale{0.0f};
  if (const float scale = s_scale.load(std::memory_order_relaxed); scale > 0.0f)
    return scale;
  float scale = 1.0f;
  // Graphics built into the processor (Intel, AMD APUs) share the system memory: 720p keeps
  // them at 60 fps (tested on an Intel Raptor Lake iGPU), so automatic mode stays at 1x there.
  const auto *device = Video::HostDevice();
  const bool integrated =
      device && device->getDescription().type == plume::RenderDeviceType::INTEGRATED;
  if (const int configured = REXCVAR_GET(svr_render_scale); configured > 0) {
    // A multiple (1-4) or a height: 720, 1080 (1.5x), 1440, 2160, 2880.
    // 360 (0.5x), 480 (2/3x) and 540 (0.75x) draw below the Xbox's own 720p, for the weakest
    // integrated GPUs.
    scale = configured <= 4 ? float(configured)
            : configured == 360  ? 0.5f
            : configured == 480  ? 2.0f / 3.0f
            : configured == 540  ? 0.75f
            : configured == 1080 ? 1.5f
                                 : float(std::clamp((configured + 360) / 720, 1, 4));
  } else if (integrated && REXCVAR_GET(svr_auto_lowend) && LogicalProcessorsAvailable() <= 4) {
    // Integrated GPU on a 2-core / 4-thread (or smaller) CPU (Ryzen 3 2200U / Vega 3, Athlon, old Intel U parts):
    // both the GPU and the CPU are short, so automatic mode draws below the Xbox's 720p.
    const unsigned cpus = LogicalProcessorsAvailable();
    scale = cpus <= 2 ? 2.0f / 3.0f : 0.75f;
    BD_INFO("SvR native: low-end PC detected ({} logical processors, integrated GPU {}): {}p "
            "(svr_auto_lowend=false or svr_render_scale=720 turns this off)",
            cpus, device->getDescription().name, cpus <= 2 ? 480 : 540);
  } else if (integrated) {
    BD_INFO("SvR native: integrated GPU ({}, {} logical processors), automatic resolution stays at 720p",
            device->getDescription().name, LogicalProcessorsAvailable());
  } else if (const u64 vram = device ? device->getDescription().dedicatedVideoMemory : 0;
             vram > 0 && vram < 3584ull * 1024 * 1024) {
    // Small discrete GPUs (GeForce 940MX, GT 1030, RX 560 with 2 GB): 2x on a 1080p screen is
    // too much for them (a 940MX player had to pick 540p by hand), so automatic mode stays at 1x.
    BD_INFO("SvR native: GPU with {} MB of video memory ({}), automatic resolution stays at 720p",
            vram / (1024 * 1024), device->getDescription().name);
  } else if (!SvrOnSteamDeck()) {
    // The larger of the window and the display, so a window on a big screen still renders
    // sharp (downscaled at present); 720p multiples rounded to nearest: 1080/1440 -> 2, 2160 -> 3.
    // The Deck keeps 1x even when docked: its GPU holds 60 fps there.
    u32 w = 0, h = 0, dw = 0, dh = 0;
    if (!Output::RenderSize(w, h))
      h = 0;
    if (SvrDisplaySize(dw, dh))
      h = std::max(h, dh);
    if (h)
      scale = float(std::clamp(u32((h + 360u) / 720u), 1u, 4u));
  }
  float expected = 0.0f;
  if (s_scale.compare_exchange_strong(expected, scale)) {
    BD_INFO("SvR native: internal resolution {}x{} ({}x)", Scaled(1280, scale),
            Scaled(720, scale), scale);
    return scale;
  }
  return expected;
}

GuestTexture *SvrResolveSurface(u32 surface_va) {
  if (!surface_va)
    return nullptr;
  if (auto *host = HostResourceHeap::FromGuest<GuestTexture>(surface_va))
    return host;
  std::lock_guard lock(g_mutex);
  const auto *surface = bd::mem::at<const D3DSurface>(surface_va);
  if (!surface)
    return nullptr;
  // Size packing as decoded by D3DSurface_GetDesc (see gpu/d3d.h).
  const u32 bits = surface->SizeBits;
  const u32 format = surface->Format;
  const u64 desc = u64(bits) << 32 | format;
  if (auto t = g_tiled_desc.find(surface_va); t != g_tiled_desc.end() && t->second != desc) {
    g_tiled_extent.erase(surface_va);  // left by another surface at this address
    g_tiled_desc.erase(t);
  }
  if (auto it = g_surfaces.find(surface_va); it != g_surfaces.end()) {
    if (it->second.desc[0] == bits && it->second.desc[1] == format &&
        it->second.extent == (g_tiled_extent.count(surface_va) ? g_tiled_extent[surface_va] : 0u)) {
      it->second.last_used = g_surface_frame.load(std::memory_order_relaxed);
      return it->second.host;
    }
    RetireHost(it->second.host);
    g_surfaces.erase(it);
  }
  u32 width = (bits >> 18) + 1;
  u32 height = ((bits >> 3) & 0x7FFF) + 1;
  u32 extent = 0;
  if (!g_tiled_extent.count(surface_va)) {
    // A new surface object shaped like a known tiled one starts at the extent that one reached.
    // 320x736 and 640x736 are the one-tile EDRAM surfaces of the 1280x720 scene (4 or 2 tiles
    // wide; the 2-tile one made the right half of the picture flash black before the bell).
    const auto learned = g_learned_extent.find(desc);
    if (learned != g_learned_extent.end()) {
      g_tiled_extent[surface_va] = learned->second;
      g_tiled_desc[surface_va] = desc;
    } else if ((width == 320 || width == 640) && height == 736) {
      g_tiled_extent[surface_va] = (1280u << 16) | 736u;
      g_tiled_desc[surface_va] = desc;
    } else if (width == 768 && height == 320) {
      // The created superstar's portrait (736x1280, saved with the superstar and shown on the
      // selection screen): 4 tiles of 768x320 stacked, drawn once when the superstar is saved.
      // Learned during that frame, the extent came too late: only the top tile (the head) was
      // drawn, the rest of the saved portrait was noise (issue #34).
      g_tiled_extent[surface_va] = (768u << 16) | 1280u;
      g_tiled_desc[surface_va] = desc;
    }
  }
  if (auto it = g_tiled_extent.find(surface_va); it != g_tiled_extent.end()) {
    extent = it->second;
    width = std::max(width, extent >> 16);
    height = std::max(height, extent & 0xFFFFu);
  }
  // Square surfaces (shadow maps, reflections) follow svr_scale_square_surfaces.
  const bool screen_like = extent || width * 9u == height * 16u ||
                           (width == height && REXCVAR_GET(svr_scale_square_surfaces));
  const float scale = screen_like ? SvrRenderScale() : 1.0f;
  g_surface_scale[surface_va] = scale;
  g_surface_scale_gen.fetch_add(1, std::memory_order_release);
  GuestTexture *host = SvrCreateHostSurface(Scaled(width, scale), Scaled(height, scale), format);
  if (host && REXCVAR_GET(svr_debug_magenta))
    Video::ClearTexture(host, 0xFFFF00FFu);
  else if (host && REXCVAR_GET(svr_clear_new_surfaces))
    Video::ClearTexture(host, 0u);
  BD_INFO("SvR surface 0x{:08X}: {}x{} format 0x{:08X} -> host {}", surface_va, width, height,
          format, host ? "ok" : "FAILED");
  g_surfaces.emplace(surface_va, HostTarget{host, {bits, format}, extent,
                                            g_surface_frame.load(std::memory_order_relaxed)});
  return host;
}

// WWE '12 allocates new D3DSurface objects for each scene (match, entrance, menu) and frees the
// old ones itself, with no D3D call the renderer sees: their host targets stayed in g_surfaces
// forever, two per scene at the full tiled size (about 50 MB at 1440p). A surface the game hasn't
// asked for in 30 s is let go; its content doesn't need to survive (on the Xbox, EDRAM is
// overwritten by the next surface anyway), and a surface used again later is simply recreated.
//
// GPU-written textures (g_targets) pile up the same way, one set per match. Only those that can
// be rebuilt are let go, after 2 min without being resolved into or sampled: screen images (16:9,
// or resolved from a scaled surface), redrawn on every frame they are used, and textures whose
// last resolve was read back into the game's memory (wrestler skin composites), mirrored from
// there if used again. Anything else may hold content drawn once (0.7.0 test: the reversal
// button prompt disappeared when every idle texture was let go), so it stays.
static void ReleaseIdleSurfaces(u64 frame) {
  constexpr u64 kIdleFrames = 1800;
  constexpr u64 kTargetIdleFrames = 7200;
  if (frame % 60 != 0 || frame < kIdleFrames || !REXCVAR_GET(svr_release_stale_targets))
    return;
  std::lock_guard lock(g_mutex);
  for (auto it = g_surfaces.begin(); it != g_surfaces.end();) {
    if (it->second.host && frame - it->second.last_used > kIdleFrames) {
      RetireHost(it->second.host);
      it = g_surfaces.erase(it);
    } else {
      ++it;
    }
  }
  if (frame < kTargetIdleFrames)
    return;
  for (auto it = g_targets.begin(); it != g_targets.end();) {
    const GuestTexture *host = it->second.host;
    const bool rebuildable = !host || it->second.in_memory || it->second.extent > 4 ||
                             u64(host->width) * 9 == u64(host->height) * 16;
    if (rebuildable && frame - it->second.last_used > kTargetIdleFrames) {
      RetireHost(it->second.host);
      it = g_targets.erase(it);
      g_targets_gen.fetch_add(1, std::memory_order_relaxed);
    } else {
      ++it;
    }
  }
}

namespace {
struct PendingClear {
  u32 surface = 0, flags = 0, color = 0;
};
PendingClear g_pending_clear;
float g_last_clear_depth = 0.0f;
u32 g_last_clear_stencil = 0;
}  // namespace

void SvrNoteResolveClear(u32 surface_va, u32 resolve_flags, u32 color_argb) {
  u32 flags = 0;
  if (resolve_flags & 0x100u)
    flags |= kClearTarget;
  if (resolve_flags & 0x200u)
    flags |= kClearZBuffer | kClearStencil;
  if (!surface_va || !flags)
    return;
  std::lock_guard lock(g_mutex);
  g_pending_clear = {surface_va, flags, color_argb};
}

void SvrNoteGameClear(u32 surface_va, float depth, u32 stencil) {
  std::lock_guard lock(g_mutex);
  g_last_clear_depth = depth;
  g_last_clear_stencil = stencil;
  if (g_pending_clear.surface == surface_va)
    g_pending_clear = {};  // the game clears it itself
}

bool SvrTakePendingClear(u32 surface_va, u32 &clear_flags, u32 &color_argb, float &depth,
                         u32 &stencil) {
  std::lock_guard lock(g_mutex);
  if (!g_pending_clear.surface || g_pending_clear.surface != surface_va)
    return false;
  clear_flags = g_pending_clear.flags;
  color_argb = g_pending_clear.color;
  depth = g_last_clear_depth;
  stencil = g_last_clear_stencil;
  g_pending_clear = {};
  return true;
}

void SvrNoteTiledResolve(u32 color_va, u32 depth_va, u32 right, u32 bottom) {
  if (!right || !bottom || right > 8192 || bottom > 8192)
    return;
  std::lock_guard lock(g_mutex);
  for (const u32 va : {color_va, depth_va}) {
    const auto *surface = va ? bd::mem::at<const D3DSurface>(va) : nullptr;
    if (!surface)
      continue;
    const u32 bits = surface->SizeBits;
    const u32 width = (bits >> 18) + 1;
    const u32 height = ((bits >> 3) & 0x7FFF) + 1;
    if (right <= width && bottom <= height)
      continue;  // an ordinary sub-rect resolve
    const u64 desc = u64(bits) << 32 | surface->Format;
    if (auto t = g_tiled_desc.find(va); t == g_tiled_desc.end() || t->second != desc) {
      g_tiled_extent.erase(va);  // none yet, or left by another surface at this address
      g_tiled_desc[va] = desc;
    }
    u32 &extent = g_tiled_extent[va];
    const u32 grown = (std::max(extent >> 16, right) << 16) | std::max(extent & 0xFFFFu, bottom);
    if (grown != extent) {
      BD_INFO("SvR tiled surface 0x{:08X}: {}x{} -> drawn at {}x{}", va, width, height,
              std::max(width, grown >> 16), std::max(height, grown & 0xFFFFu));
      extent = grown;
      g_learned_extent[desc] = grown;
    }
  }
}

float SvrSurfaceScale(u32 surface_va) {
  // Asked several times per draw, nearly always for the bound target: the last answer is kept per
  // thread until g_surface_scale changes (the lock alone cost ~3% of the render thread).
  struct Last {
    u32 va = 0, gen = 0;
    float scale = 0.0f;
  };
  thread_local Last last;
  const u32 gen = g_surface_scale_gen.load(std::memory_order_acquire);
  if (last.gen == gen && last.va == surface_va)
    return last.scale;
  float scale = 0.0f;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_surface_scale.find(surface_va); it != g_surface_scale.end())
      scale = it->second;
  }
  if (scale <= 0.0f)
    scale = SvrRenderScale();
  last = {surface_va, gen, scale};
  return scale;
}

GuestTexture *SvrResolveTarget(u32 texture_va, float scale) {
  if (!texture_va)
    return nullptr;
  if (auto *host = HostResourceHeap::FromGuest<GuestTexture>(texture_va))
    return host;
  if (scale <= 0.0f)
    scale = SvrRenderScale();
  // A target's extent holds its scale in quarters (4 = 1x), 0 for one never made.
  const u32 quarters = u32(scale * 4.0f + 0.5f);
  std::lock_guard lock(g_mutex);
  if (auto it = g_targets.find(texture_va); it != g_targets.end() && TargetCurrent(texture_va, it)) {
    if (it->second.extent == 0 || it->second.extent == quarters) {
      it->second.last_used = g_surface_frame.load(std::memory_order_relaxed);
      return it->second.host;
    }
    RetireHost(it->second.host);  // resolved from a surface of another scale now
    g_targets.erase(it);
  }
  g_targets_gen.fetch_add(1, std::memory_order_relaxed);  // an entry is added on every path below
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  if (!ReadFetch(texture_va, fetch)) {
    g_targets.emplace(texture_va, HostTarget{});
    return nullptr;
  }
  const u32 width = fetch.size_2d.width + 1;
  const u32 height = fetch.size_2d.height + 1;
  const u32 levels = fetch.mip_max_level + 1;
  // ConvertGuestFormat maps a raw GPUTEXTUREFORMAT byte by its low 6 bits; the tag keeps a small
  // byte from matching one of re:Blue's D3DFormat values (k_8_8_8_8 = 6 = its kIndex32).
  // GPU-written formats with no host equivalent (WWE '12 composites wrestler skins into
  // k_4_4_4_4 textures): the host keeps them as 8:8:8:8, the format the game renders them in, so
  // resolves stay plain copies and sampling returns the same normalized values.
  u32 host_format = 0xC0000000u | u32(fetch.format);
  // That includes the 1x1 ones each wrestler gets at match load (one pixel drawn into a 1x1
  // surface, then resolved), sampled by every 3D draw: without them skin turns flat red.
  if (ConvertGuestFormat(host_format, false) == plume::RenderFormat::UNKNOWN)
    host_format = 0xC0000000u | u32(rex::graphics::xenos::TextureFormat::k_8_8_8_8);
  // Cube targets (the environment map WWE '12 renders each frame for wrestlers' reflections,
  // resolved face by face) need host cubes: as 2D textures the faces were lost and skin and eyes
  // reflected black.
  const bool cube = fetch.dimension == rex::graphics::xenos::DataDimension::kCube;
  GuestTexture *host = SvrCreateHostTexture(Scaled(width, scale), Scaled(height, scale),
                                            cube ? 6 : 1, levels,
                                            0, host_format, cube ? 18u : 0u);
  BD_INFO("SvR target 0x{:08X}: {}x{}{} fmt {} levels {} -> host {}", texture_va, width, height,
          cube ? " cube" : "", u32(fetch.format), levels, host ? "ok" : "FAILED");
  // On the Xbox a GPU-written texture keeps what the CPU put in its memory wherever resolves never
  // land: WWE '12 fills its wrestler skin composites (k_4_4_4_4) with the base skin, eyes and
  // teeth included, and resolves only patches into the base level. Start the host texture from
  // that memory, else the unpatched parts of its base level stay black.
  bool seeded = false;
  if (host && !cube && quarters == 4 && levels > 1 && REXCVAR_GET(svr_seed_gpu_textures)) {
    const auto *mem = bd::mem::try_at<const u8>(fetch.base_address << 12);
    size_t nonzero = 0;
    for (u32 i = 0; mem && i < 65536; i += 64)
      nonzero += mem[i] != 0;
    GuestTexture *seed = nonzero ? GetOrCreateNativeMirror(texture_va, 0) : nullptr;
    seeded = seed && Video::SeedTextureBaseLevel(host, seed);
    BD_INFO("SvR target 0x{:08X}: memory {} non-zero sample(s) of 1024, seeded {}", texture_va,
            nonzero, seeded);
  }
  // Host targets live in recycled (placed) GPU memory: whatever the resolves never cover would
  // show what was there before. Black on NVIDIA by luck, coloured noise on the Steam Deck (AMD)
  // in the crowd during entrances. Start them transparent black.
  if (host && !cube && !seeded)
    Video::ClearTexture(host, REXCVAR_GET(svr_debug_magenta) ? 0xFFFF00FFu : 0u);
  const u64 now = g_surface_frame.load(std::memory_order_relaxed);
  g_targets.emplace(texture_va, HostTarget{host, {fetch.dword_1, fetch.dword_2}, quarters, now,
                                           false, now});
  return host;
}

void SvrListTargets(std::vector<SvrTargetInfo> &out) {
  out.clear();
  std::lock_guard lock(g_mutex);
  for (const auto &[va, target] : g_targets)
    if (target.host)
      out.push_back({va, target.host, target.extent ? float(target.extent) / 4.0f : 1.0f});
}

// Presented frames, for SvrAfterResolve's per-frame target detection.
static std::atomic<u64> g_frame_index{0};

// Multi-level targets (wrestler skin composites) waiting for their read-back: the game paints
// them in bursts of patch resolves (skin at load, blood mid-match), each GPU wait a stutter, so one
// read-back per burst, a couple of frames after its last resolve (SvrProcessDeferredReadbacks).
namespace {
struct DeferredReadback {
  GuestTexture *dst = nullptr;
  u64 last_frame = 0;
  // Texture header (fetch dwords 1: format and memory, 2: size) at the last resolve. Issue #33:
  // the editors free and reuse their work textures; a read-back two or more frames later must not
  // write into memory that now holds something else.
  u32 desc[2] = {};
};
std::unordered_map<u32, DeferredReadback> g_deferred_readbacks;
void ReadBackTarget(u32 texture_va, GuestTexture *dst);
// svr_readback_batch: streak read-backs waiting for this frame's Swap (texture VA -> host target), under g_mutex.
std::unordered_map<u32, GuestTexture *> g_batch_readbacks;
std::atomic<u64> g_rb_immediate{0}, g_rb_batched_requests{0}, g_rb_batched_done{0}, g_rb_batches{0};
void FinishReadBack(u32 texture_va, GuestTexture *dst, std::vector<u8> &pixels, u32 row_pitch);
// svr_readback_async (resolve.cpp): rested targets whose GPU copy is in flight. Game thread only
// (SvrProcessDeferredReadbacks).
struct AsyncReadbackJob {
  u32 texture_va = 0;
  GuestTexture *dst = nullptr;
  u64 ticket = 0;
  u64 resolves = 0;  // HostTarget::resolves when the copy was recorded
  u64 created = 0;   // HostTarget::created (a new host texture at the same address is not it)
  u32 desc[2] = {};
};
std::vector<AsyncReadbackJob> g_async_jobs;
}  // namespace

void SvrAfterResolve(u32 texture_va, GuestTexture *dst, u32 level, u32 face) {
  if (!dst)
    return;
  {
    // New content on the GPU only, until a read back below copies it to the game's memory.
    std::lock_guard lock(g_mutex);
    if (const auto it = g_targets.find(texture_va); it != g_targets.end() && it->second.host == dst) {
      it->second.in_memory = false;
      ++it->second.resolves;
    }
  }
  if (REXCVAR_GET(svr_log_readbacks)) {
    // First resolve of each burst only (a target resting for a second before it), so the
    // per-frame ones do not drown the one-off pictures.
    static std::mutex s_log_mutex;
    static std::unordered_map<u32, u64> s_last_logged;
    const u64 frame = g_frame_index.load(std::memory_order_relaxed);
    bool log = false;
    {
      std::lock_guard log_lock(s_log_mutex);
      auto [lt, fresh] = s_last_logged.try_emplace(texture_va, frame);
      log = fresh || frame > lt->second + 60;
      lt->second = frame;
    }
    if (log)
      BD_INFO("SvR resolve 0x{:08X}: host {}x{} levels {} -> level {} face {} (frame {})",
              texture_va, dst->width, dst->height, dst->mipLevels, level, face, frame);
  }
  if (level != 0 || face != 0 || !REXCVAR_GET(svr_resolve_readback))
    return;
  // Header of the texture at this resolve, for the deferred read-backs' check.
  u32 desc[2] = {};
  {
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    if (ReadFetch(texture_va, fetch)) {
      desc[0] = fetch.dword_1, desc[1] = fetch.dword_2;
      // Cube targets (the wrestlers' reflection maps) are never read by the CPU, and
      // WriteGuestTextureBase cannot write them: no GPU wait for nothing.
      if (fetch.dimension == rex::graphics::xenos::DataDimension::kCube)
        return;
    }
  }
  // Screen images (the scene, its bloom and depth-of-field chain: 16:9) are resolved on every
  // frame and never read by the CPU. At 1x (Steam Deck, 720p) they used to be read back, and
  // reading them back in the middle of a frame broke the rest of it (a black or noisy block in
  // entrances and cutscenes). Targets resolved from a surface drawn above 1x (render scale) are
  // mostly scene images too. But a one-off resolve of either kind is a picture the game saves
  // (issue #34: a created superstar's 736x1280 portrait, drawn at the render scale, came out as
  // noise on the selection screen; #35: a created arena's thumbnail): read it back at the start
  // of a later frame, once it was not resolved again, scaled down to the texture's size.
  bool scene_like = u64(dst->width) * 9 == u64(dst->height) * 16;
  bool fresh_target = false;
  {
    std::lock_guard lock(g_mutex);
    const auto it = g_targets.find(texture_va);
    if (it == g_targets.end() || it->second.host != dst)
      return;
    scene_like = scene_like || it->second.extent > 4;
    // Made for this very resolve: a picture taken now (issue #35: a created arena's 1280x720
    // shot when it is saved), which the CPU may read before the next frame.
    fresh_target = it->second.created == g_surface_frame.load(std::memory_order_relaxed);
  }
  if (scene_like) {
    bool immediate = false;
    {
      std::lock_guard lock(g_mutex);
      const u64 frame = g_frame_index.load(std::memory_order_relaxed);
      struct OneOff {
        u64 last_frame = 0;
        bool repeated = false;
      };
      static std::unordered_map<u32, OneOff> s_screen;
      auto [ot, fresh] = s_screen.try_emplace(texture_va, OneOff{frame, false});
      OneOff &o = ot->second;
      if (!fresh) {
        if (frame > o.last_frame + 60)
          o.repeated = false;  // rested for a second: a new picture
        else if (frame != o.last_frame)
          o.repeated = true;   // resolved again on another frame: a per-frame screen image
        o.last_frame = frame;
      }
      if (o.repeated) {
        g_deferred_readbacks.erase(texture_va);
        return;
      }
      // A mipmapped one (the CAW portrait, its tiles all resolved in one frame) is compressed by
      // the CPU in that same frame: two frames later was too late, so read it back now.
      if (dst->mipLevels > 1 || fresh_target) {
        g_deferred_readbacks.erase(texture_va);
        immediate = true;
      } else {
        g_deferred_readbacks[texture_va] = DeferredReadback{dst, frame, {desc[0], desc[1]}};
      }
      if (REXCVAR_GET(svr_log_readbacks))
        BD_INFO("SvR target 0x{:08X}: {}x{} one-off resolve, read back {}", texture_va, dst->width,
                dst->height, immediate ? "now" : "if not resolved again");
    }
    if (immediate)
      ReadBackTarget(texture_va, dst);
    return;
  }
  // Per-frame targets (shadows, lighting, the scene) are resolved on every frame; the composites
  // the CPU reads are built at load, or now and then in a match. Only those are worth the GPU
  // wait: a target resolved on more than svr_readback_streak frames in a row is skipped until it
  // rests, then read back once (its final content). Issue #27: attires resolved part by part
  // (a 2048x1024 one every ~8 frames on a Radeon 610M) went past the old limit of 6, the later
  // parts never reached the game's memory and the CPU-compressed attire came out white. Same
  // bug and fix as Jarrod's WWE 2K14 port (v0.2.10: 4 -> 32 frames).
  const u64 max_streak = u64(std::max(1, REXCVAR_GET(svr_readback_streak)));
  constexpr u64 kRestFrames = 30;
  {
    std::lock_guard lock(g_mutex);
    const auto it = g_targets.find(texture_va);
    if (it == g_targets.end() || it->second.host != dst)
      return;
    const u64 frame = g_frame_index.load(std::memory_order_relaxed);
    if (dst->mipLevels > 1) {
      g_deferred_readbacks[texture_va] = DeferredReadback{dst, frame, {desc[0], desc[1]}};
      return;
    }
    struct Streak {
      u64 last_frame = 0, frames = 0;
    };
    static std::unordered_map<u32, Streak> s_streaks;
    Streak &st = s_streaks[texture_va];
    if (st.frames == 0 || frame > st.last_frame + kRestFrames)
      st.frames = 1;
    else if (frame != st.last_frame)
      ++st.frames;
    st.last_frame = frame;
    if (st.frames > max_streak) {
      // Read the final content back once the target rests (SvrProcessDeferredReadbacks).
      g_deferred_readbacks[texture_va] = DeferredReadback{dst, frame, {desc[0], desc[1]}};
      static u32 s_logged = 0;
      if (s_logged < 16 && st.frames == max_streak + 1 && ++s_logged)
        BD_INFO("SvR target 0x{:08X}: {}x{} resolved on {} frames in a row, read back when it "
                "rests", texture_va, dst->width, dst->height, st.frames);
      return;
    }
    if (REXCVAR_GET(svr_readback_batch)) {
      g_batch_readbacks[texture_va] = dst;
      g_rb_batched_requests.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  g_rb_immediate.fetch_add(1, std::memory_order_relaxed);
  ReadBackTarget(texture_va, dst);
}

// svr_readback_batch: this frame's streak read-backs, one GPU wait (Swap, before the deferred ones).
static void SvrFlushBatchedReadbacks() {
  std::vector<u32> vas;
  std::vector<GuestTexture *> dsts;
  {
    std::lock_guard lock(g_mutex);
    for (const auto &[va, dst] : g_batch_readbacks) {
      const auto it = g_targets.find(va);
      if (it != g_targets.end() && it->second.host == dst) {
        vas.push_back(va);
        dsts.push_back(dst);
      }
    }
    g_batch_readbacks.clear();
  }
  if (!dsts.empty()) {
    std::vector<std::vector<u8>> pixels;
    std::vector<u32> pitches;
    Video::ReadbackTextureLevel0Batch(dsts, pixels, pitches);
    for (size_t i = 0; i < dsts.size(); ++i)
      if (!pixels[i].empty())
        FinishReadBack(vas[i], dsts[i], pixels[i], pitches[i]);
    g_rb_batched_done.fetch_add(dsts.size(), std::memory_order_relaxed);
    g_rb_batches.fetch_add(1, std::memory_order_relaxed);
  }
  using Clock = std::chrono::steady_clock;
  static Clock::time_point s_last = Clock::now();
  if (Clock::now() - s_last >= std::chrono::seconds(10)) {
    s_last = Clock::now();
    BD_INFO("[readback] 10 s: immediate {} | batched requests {} done {} in {} waits (svr_readback_batch {})",
            g_rb_immediate.exchange(0), g_rb_batched_requests.exchange(0), g_rb_batched_done.exchange(0),
            g_rb_batches.exchange(0), REXCVAR_GET(svr_readback_batch));
  }
}

// Called once per frame (game thread, no lock held): read back the bursts that have settled.
static void SvrProcessDeferredReadbacks(u64 frame) {
  std::vector<std::pair<u32, GuestTexture *>> ready;
  {
    std::lock_guard lock(g_mutex);
    for (auto it = g_deferred_readbacks.begin(); it != g_deferred_readbacks.end();) {
      if (frame < it->second.last_frame + 2) {
        ++it;
        continue;
      }
      const auto t = g_targets.find(it->first);
      rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
      const bool same_texture = ReadFetch(it->first, fetch) &&
                                fetch.dword_1 == it->second.desc[0] &&
                                fetch.dword_2 == it->second.desc[1];
      if (t != g_targets.end() && t->second.host == it->second.dst && same_texture) {
        ready.emplace_back(it->first, it->second.dst);
      } else if (t != g_targets.end() && t->second.host == it->second.dst) {
        static u32 s_logged = 0;
        if (s_logged < 32 && ++s_logged)
          BD_INFO("SvR target 0x{:08X}: texture header changed since its resolve ({:08X} {:08X} "
                  "-> {:08X} {:08X}), read-back dropped", it->first, it->second.desc[0],
                  it->second.desc[1], fetch.dword_1, fetch.dword_2);
      }
      it = g_deferred_readbacks.erase(it);
    }
  }
  if (!REXCVAR_GET(svr_readback_async) && g_async_jobs.empty()) {
    for (const auto &[texture_va, dst] : ready)
      ReadBackTarget(texture_va, dst);
    return;
  }
  // Async: finish the copies the GPU has completed (still the target's last resolve, same host
  // texture and header), then start this frame's ones.
  for (size_t i = 0; i < g_async_jobs.size();) {
    AsyncReadbackJob &job = g_async_jobs[i];
    std::vector<u8> pixels;
    u32 row_pitch = 0;
    bool failed = false;
    if (!Video::ReadbackTextureLevelPoll(job.ticket, pixels, row_pitch, failed)) {
      ++i;
      continue;
    }
    bool current = false;
    {
      std::lock_guard lock(g_mutex);
      const auto t = g_targets.find(job.texture_va);
      rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
      current = !failed && t != g_targets.end() && t->second.host == job.dst &&
                t->second.resolves == job.resolves && t->second.created == job.created &&
                ReadFetch(job.texture_va, fetch) &&
                fetch.dword_1 == job.desc[0] && fetch.dword_2 == job.desc[1];
    }
    if (current)
      FinishReadBack(job.texture_va, job.dst, pixels, row_pitch);
    else if (REXCVAR_GET(svr_log_readbacks))
      BD_INFO("SvR target 0x{:08X}: async read-back dropped ({})", job.texture_va,
              failed ? "failed" : "resolved again or header changed");
    g_async_jobs[i] = g_async_jobs.back();
    g_async_jobs.pop_back();
  }
  for (const auto &[texture_va, dst] : ready) {
    if (!REXCVAR_GET(svr_readback_async)) {
      ReadBackTarget(texture_va, dst);
      continue;
    }
    // A job still in flight for this target is older (a rest needs a new resolve): it is dropped
    // when it lands (resolve count differs), this one carries the newer content.
    AsyncReadbackJob job{texture_va, dst, 0, 0, 0, {}};
    {
      std::lock_guard lock(g_mutex);
      const auto t = g_targets.find(texture_va);
      rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
      if (t == g_targets.end() || t->second.host != dst || !ReadFetch(texture_va, fetch))
        continue;
      job.resolves = t->second.resolves;
      job.created = t->second.created;
      job.desc[0] = fetch.dword_1, job.desc[1] = fetch.dword_2;
    }
    job.ticket = Video::ReadbackTextureLevelBegin(dst, 0, 0);
    if (job.ticket)
      g_async_jobs.push_back(job);
    else
      ReadBackTarget(texture_va, dst);
  }
}

namespace {
void ReadBackTarget(u32 texture_va, GuestTexture *dst) {
  std::vector<u8> pixels;
  u32 row_pitch = 0;
  if (!Video::ReadbackTextureLevel(dst, 0, 0, pixels, row_pitch))
    return;
  FinishReadBack(texture_va, dst, pixels, row_pitch);
}

// The read-back texels of dst's level 0 into the game's memory, then its smaller levels rebuilt.
void FinishReadBack(u32 texture_va, GuestTexture *dst, std::vector<u8> &pixels, u32 row_pitch) {
  if (REXCVAR_GET(svr_dump_readbacks) && (dst->width >= 1024 || dst->height >= 1024)) {
    static std::atomic<u32> s_dumps{0};
    static std::mutex s_dump_mutex;
    static std::unordered_map<u32, u32> s_dumps_per_target;
    bool dump = false;
    {
      std::lock_guard dump_lock(s_dump_mutex);
      dump = s_dumps_per_target[texture_va]++ < 4;  // a few per target, not 64 of the first
    }
    const u32 n = dump ? s_dumps.fetch_add(1) : 0;
    if (dump && n < 64) {
      const auto path = rex::filesystem::GetExecutableFolder() / "logs" /
                        fmt::format("readback_{:02}_{:08X}_{}x{}.rgba", n, texture_va, dst->width,
                                    dst->height);
      if (FILE *f = SvrOpenWrite(path)) {
        const u32 header[4] = {dst->width, dst->height, row_pitch, texture_va};
        std::fwrite(header, 4, 4, f);
        std::fwrite(pixels.data(), 1, pixels.size(), f);
        std::fclose(f);
      }
    }
  }
  // A target drawn above 1x is larger on the host than in the game's memory: average each
  // texture texel's footprint (area filter, any scale).
  u32 guest_w = dst->width, guest_h = dst->height;
  {
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    if (ReadFetch(texture_va, fetch))
      guest_w = fetch.size_2d.width + 1, guest_h = fetch.size_2d.height + 1;
  }
  std::vector<u8> guest_pixels;
  const u8 *write_pixels = pixels.data();
  u32 write_pitch = row_pitch;
  if ((guest_w != dst->width || guest_h != dst->height) && guest_w <= dst->width &&
      guest_h <= dst->height && guest_w && guest_h) {
    write_pitch = guest_w * 4u;
    guest_pixels.resize(size_t(write_pitch) * guest_h);
    for (u32 y = 0; y < guest_h; ++y) {
      const u32 y0 = u32(u64(y) * dst->height / guest_h);
      const u32 y1 = std::max(y0 + 1, u32(u64(y + 1) * dst->height / guest_h));
      for (u32 x = 0; x < guest_w; ++x) {
        const u32 x0 = u32(u64(x) * dst->width / guest_w);
        const u32 x1 = std::max(x0 + 1, u32(u64(x + 1) * dst->width / guest_w));
        u32 sum[4] = {};
        for (u32 sy = y0; sy < y1; ++sy) {
          const u8 *row = &pixels[size_t(sy) * row_pitch];
          for (u32 sx = x0; sx < x1; ++sx)
            for (int k = 0; k < 4; ++k)
              sum[k] += row[sx * 4u + k];
        }
        const u32 n = (y1 - y0) * (x1 - x0);
        u8 *o = &guest_pixels[size_t(y) * write_pitch + x * 4u];
        for (int k = 0; k < 4; ++k)
          o[k] = u8((sum[k] + n / 2) / n);
      }
    }
    write_pixels = guest_pixels.data();
  } else {
    guest_w = dst->width, guest_h = dst->height;
  }
  std::string range;
  const bool written = WriteGuestTextureBase(texture_va, write_pixels, write_pitch, guest_w,
                                             guest_h, &range);
  if (written && REXCVAR_GET(svr_dump_readbacks) && (dst->width >= 1024 || dst->height >= 1024)) {
    // What landed in memory, raw (tiled), next to the read-back dumps.
    static std::atomic<u32> s_raw{0};
    std::vector<u8> raw;
    u32 pitch_blocks = 0;
    const u32 n = s_raw.fetch_add(1);
    if (n < 16 && ReadGuestTextureBaseRaw(texture_va, raw, pitch_blocks)) {
      const auto path = rex::filesystem::GetExecutableFolder() / "logs" /
                        fmt::format("guestmem_{:02}_{:08X}_{}x{}.raw", n, texture_va, guest_w,
                                    guest_h);
      if (FILE *f = SvrOpenWrite(path)) {
        const u32 header[4] = {guest_w, guest_h, pitch_blocks, texture_va};
        std::fwrite(header, 4, 4, f);
        std::fwrite(raw.data(), 1, raw.size(), f);
        std::fclose(f);
      }
    }
  }
  if (REXCVAR_GET(svr_log_readbacks) || !written) {
    static std::atomic<u32> s_not_written{0};
    if (written || s_not_written.fetch_add(1, std::memory_order_relaxed) < 32)
      BD_INFO("SvR target 0x{:08X}: {}x{} read back, written: {} ({})", texture_va, dst->width,
              dst->height, written, range);
  }
  if (written) {
    std::lock_guard lock(g_mutex);
    if (const auto it = g_targets.find(texture_va); it != g_targets.end() && it->second.host == dst)
      it->second.in_memory = true;
  }
  // The game paints its skin composites (base skin, eyes, teeth, then blood mid-match) into
  // level 0 only; the smaller levels seen from afar were never filled on the host. Build them
  // from level 0 with a 2x2 box filter, as the console's CPU does.
  if (dst->mipLevels > 1 && REXCVAR_GET(svr_rebuild_target_mips)) {
    std::vector<u8> prev = std::move(pixels);
    u32 prev_pitch = row_pitch, pw = dst->width, ph = dst->height;
    for (u32 level = 1; level < dst->mipLevels; ++level) {
      const u32 w = std::max(1u, pw / 2), h = std::max(1u, ph / 2);
      const u32 pitch = (w * 4u + 255u) & ~255u;
      std::vector<u8> next(size_t(pitch) * h);
      for (u32 y = 0; y < h; ++y) {
        const u32 y0 = std::min(y * 2, ph - 1), y1 = std::min(y * 2 + 1, ph - 1);
        for (u32 x = 0; x < w; ++x) {
          const u32 x0 = std::min(x * 2, pw - 1), x1 = std::min(x * 2 + 1, pw - 1);
          const u8 *a = &prev[size_t(y0) * prev_pitch + x0 * 4u];
          const u8 *b = &prev[size_t(y0) * prev_pitch + x1 * 4u];
          const u8 *c = &prev[size_t(y1) * prev_pitch + x0 * 4u];
          const u8 *d = &prev[size_t(y1) * prev_pitch + x1 * 4u];
          u8 *o = &next[size_t(y) * pitch + x * 4u];
          for (int k = 0; k < 4; ++k)
            o[k] = u8((u32(a[k]) + b[k] + c[k] + d[k] + 2u) / 4u);
        }
      }
      if (!Video::UploadTextureLevel(dst, level, next.data(), pitch))
        break;
      prev = std::move(next);
      prev_pitch = pitch;
      pw = w;
      ph = h;
    }
  }
  // Large ones (wrestler composites) at info: shows when the game rebuilds them mid-match.
  if (dst->width >= 512 && dst->mipLevels == 1)
    BD_INFO("SvR target 0x{:08X}: {}x{} resolve written back to memory: {}", texture_va,
            dst->width, dst->height, written);
  else
    BD_DEBUG("SvR target 0x{:08X}: {}x{} resolve written back to memory: {}", texture_va,
             dst->width, dst->height, written);
}
}  // namespace

namespace {

std::shared_mutex g_object_mutex;
std::unordered_map<u32, GuestShader *> g_shaders;  // guest shader object -> host
std::unordered_map<u64, GuestShader *> g_shaders_by_hash;
std::unordered_map<u32, GuestVertexDeclaration *> g_decls;  // guest declaration -> host
std::unordered_map<u64, GuestVertexDeclaration *> g_decls_by_hash;

}  // namespace

void SvrOnShaderCreated(u32 shader_va, u32 function_va, bool pixel) {
  if (!shader_va || !function_va)
    return;
  SvrRegisterShaderContainer(shader_va, bd::mem::at<const ShaderContainer>(function_va), pixel);
}

void SvrRegisterShaderContainer(u32 shader_va, const void *container_bytes, bool pixel) {
  const auto *container = static_cast<const ShaderContainer *>(container_bytes);
  if (!shader_va || !container)
    return;
  // Keyed like CreateShader's cache lookup (the whole container), plus the stage.
  const u32 length = u32(container->virtualSize) + u32(container->physicalSize);
  const u64 hash = XXH3_64bits(container, length) ^ (pixel ? 0x9E3779B97F4A7C15ull : 0);
  std::unique_lock lock(g_object_mutex);
  GuestShader *&host = g_shaders_by_hash[hash];
  if (!host)
    host = CreateShader(reinterpret_cast<const be_u32 *>(container),
                        pixel ? ResourceType::PixelShader : ResourceType::VertexShader);
  g_shaders[shader_va] = host;
}

#if defined(WWE12_NO_CREATE_SHADERS)
// WWE '12 builds its D3D shader objects itself (no CreateVertexShader / CreatePixelShader
// calls), with the .xsc container stored right after the object (docs/d3d_functions.md):
// pixel shader at +0x28, vertex shader at +0x368. Find it from the object the first time the
// shader is bound; if the offset ever differs, scan for the container signature (0x102A11xx,
// bit 0 = vertex shader).
static u32 WWE12FindShaderContainer(u32 shader_va, bool pixel) {
  const u32 expected = shader_va + (pixel ? 0x28u : 0x368u);
  auto is_container = [pixel](u32 va) {
    const auto *word = bd::mem::at<const be_u32>(va);
    if (!word)
      return false;
    const u32 flags = u32(*word);
    return (flags & 0xFFFFFF00u) == 0x102A1100u && bool(flags & 1u) == !pixel;
  };
  if (is_container(expected))
    return expected;
  for (u32 off = 0x18; off < 0x1000; off += 4) {
    if (is_container(shader_va + off))
      return shader_va + off;
  }
  return 0;
}
#endif

#if defined(WWE12_NO_CREATE_SHADERS)

// Once loaded, WWE '12 alters a shader's .xsc container in memory (the microcode goes to
// physical memory, load-time fields get filled), so its XXH3 no longer matches the cache key
// computed from the extracted file. Try the whole container first, then the secondary keys
// from tools/gen_shader_keys.py: the virtual part with fields 0x1C/0x20 cleared, and the
// microcode alone (right after the virtual part, or at the physical address in the object).
static u64 WWE12MatchShader(u32 shader_va, u32 container_va, bool pixel, const char **how) {
  const auto *c = bd::mem::at<const ShaderContainer>(container_va);
  const u32 vsize = u32(c->virtualSize), psize = u32(c->physicalSize);
  *how = "none";
  if (!vsize || vsize > 0x100000 || psize > 0x100000)
    return 0;
  const u64 full = XXH3_64bits(c, vsize + psize);
  if (ShaderCacheHas(full)) {
    *how = "full";
    return full;
  }
#if defined(WWE12_SHADER_KEYS)
  static const auto maps = [] {
    std::pair<std::unordered_map<u64, u64>, std::unordered_map<u64, u64>> m;
    for (size_t i = 0; i < g_wwe12_shader_key_count; ++i) {
      const auto &k = g_wwe12_shader_keys[i];
      const u64 stage = k.vertex ? 0 : 0x9E3779B97F4A7C15ull;
      m.first.emplace(k.virt ^ stage, k.full);
      m.second.emplace(k.phys ^ stage, k.full);
    }
    return m;
  }();
  const u64 stage = pixel ? 0x9E3779B97F4A7C15ull : 0;
  std::vector<u8> virt(reinterpret_cast<const u8 *>(c), reinterpret_cast<const u8 *>(c) + vsize);
  if (vsize >= 0x24)
    std::fill(virt.begin() + 0x1C, virt.begin() + 0x24, u8(0));
  if (auto it = maps.first.find(XXH3_64bits(virt.data(), vsize) ^ stage); it != maps.first.end()) {
    *how = "virtual";
    return it->second;
  }
  if (auto it = maps.second.find(XXH3_64bits(reinterpret_cast<const u8 *>(c) + vsize, psize) ^ stage);
      it != maps.second.end()) {
    *how = "microcode-inline";
    return it->second;
  }
  // Physical microcode address kept in the object (+0x18 pixel, +0x20 vertex).
  u32 phys = bd::mem::load<u32>(shader_va + (pixel ? 0x18u : 0x20u));
  if (phys && phys < 0x20000000u)
    phys |= 0xA0000000u;
  if (const auto *code = phys ? bd::mem::at<const u8>(phys) : nullptr) {
    if (auto it = maps.second.find(XXH3_64bits(code, psize) ^ stage); it != maps.second.end()) {
      *how = "microcode-physical";
      return it->second;
    }
  }
#endif
  return 0;
}

static void WWE12OnShaderFound(u32 shader_va, u32 container_va, bool pixel) {
  static std::atomic<u32> s_logged{0};
  const char *how = "none";
  const u64 hash = WWE12MatchShader(shader_va, container_va, pixel, &how);
  GuestShader *host = nullptr;
  {
    std::unique_lock lock(g_object_mutex);
    if (hash) {
      GuestShader *&cached = g_shaders_by_hash[hash ^ (pixel ? 0x9E3779B97F4A7C15ull : 0)];
      if (!cached)
        cached = CreateShaderFromHash(hash, pixel ? ResourceType::PixelShader
                                                  : ResourceType::VertexShader);
      host = cached;
    }
    if (host)
      g_shaders[shader_va] = host;
  }
  if (s_logged.fetch_add(1) < 48) {
    const auto *w = bd::mem::at<const be_u32>(container_va);
    BD_INFO("WWE12 shader 0x{:08X} {} container +0x{:X}: {} (0x{:016X}) | hdr {:08X} {:08X} {:08X} "
            "1C={:08X} 20={:08X} | obj+18 {:08X} obj+20 {:08X}",
            shader_va, pixel ? "PS" : "VS", container_va - shader_va, how, hash, u32(w[0]),
            u32(w[1]), u32(w[2]), u32(w[7]), u32(w[8]), bd::mem::load<u32>(shader_va + 0x18),
            bd::mem::load<u32>(shader_va + 0x20));
  }
  if (!host)
    SvrOnShaderCreated(shader_va, container_va, pixel);  // logs the cache miss
}
#endif

GuestShader *SvrLookupShader(u32 shader_va) {
  if (!shader_va)
    return nullptr;
  {
    std::shared_lock lock(g_object_mutex);
    auto it = g_shaders.find(shader_va);
    if (it != g_shaders.end())
      return it->second;
  }
#if defined(WWE12_NO_CREATE_SHADERS)
  // First bind of a shader object: its D3DResource::Common type says which stage it is.
  const auto *common = bd::mem::at<const be_u32>(shader_va);
  if (!common)
    return nullptr;
  const u32 type = u32(*common) & 0xFu;
  if (type != 6u && type != 7u)  // D3DCOMMON_TYPE_VERTEXSHADER / PIXELSHADER
    return nullptr;
  const bool pixel = type == 7u;
  const u32 container_va = WWE12FindShaderContainer(shader_va, pixel);
  if (!container_va)
    return nullptr;
  WWE12OnShaderFound(shader_va, container_va, pixel);
  std::shared_lock lock(g_object_mutex);
  auto it = g_shaders.find(shader_va);
  return it != g_shaders.end() ? it->second : nullptr;
#else
  return nullptr;
#endif
}

void SvrOnVertexDeclarationCreated(u32 decl_va, u32 elements_va) {
  if (!decl_va || !elements_va)
    return;
  SvrRegisterVertexDeclaration(decl_va, bd::mem::at<GuestVertexElement>(elements_va));
}

void SvrRegisterVertexDeclaration(u32 decl_va, const GuestVertexElement *elements) {
  if (!decl_va || !elements)
    return;
  // Elements up to the D3DDECL_END terminator (stream 0xFF), padding zeroed for a stable key.
  std::vector<GuestVertexElement> normalized;
  for (const auto *e = elements; normalized.size() < 64; ++e) {
    normalized.push_back(*e);
    normalized.back().padding = 0;
    if (u16(e->stream) == 0xFF)
      break;
  }
  const u64 hash =
      XXH3_64bits(normalized.data(), normalized.size() * sizeof(GuestVertexElement));
  std::unique_lock lock(g_object_mutex);
  GuestVertexDeclaration *&host = g_decls_by_hash[hash];
  if (!host)
    host = CreateVertexDeclaration(normalized.data());
  g_decls[decl_va] = host;
}

#if defined(WWE12_NO_CREATE_SHADERS)
// WWE '12 builds its D3DVertexDeclaration objects itself (no CreateVertexDeclaration calls).
// The D3DVERTEXELEMENT9 array lives inside the object, after a few private fields (an element
// with type D3DDECLTYPE_FLOAT3 = 0x002A23B9 was seen at +0x34). Its exact offset is not pinned
// yet: find the first run of plausible elements ending with the D3DDECL_END terminator
// (stream 0xFF) and log the offset found, plus the object's first words, for the first objects.
static bool WWE12PlausibleElement(const GuestVertexElement &e) {
  const u32 type = u32(e.type);
  return u16(e.stream) < 16u && u16(e.offset) < 0x400u && type != 0u &&
         (type & 0xFF000000u) == 0u && e.method <= 6u && e.usage <= 13u && e.usageIndex < 16u;
}

// Layout seen in game (2026-10-10): the element count at +0x18 and the elements from +0x34,
// with no D3DDECL_END terminator. Returns the elements with a terminator appended, or an empty
// vector if the object does not look like that.
static constexpr u32 kWWE12DeclCountOffset = 0x18;
static constexpr u32 kWWE12DeclElementsOffset = 0x34;

static std::vector<GuestVertexElement> WWE12ReadDeclElements(u32 decl_va) {
  std::vector<GuestVertexElement> elements;
  const auto *count_word = bd::mem::at<const be_u32>(decl_va + kWWE12DeclCountOffset);
  if (!count_word)
    return elements;
  const u32 count = u32(*count_word);
  if (count == 0 || count > 32)
    return elements;
  for (u32 i = 0; i < count; i++) {
    const auto *e =
        bd::mem::at<const GuestVertexElement>(decl_va + kWWE12DeclElementsOffset + i * 12u);
    if (!e || !WWE12PlausibleElement(*e))
      return {};
    elements.push_back(*e);
  }
  GuestVertexElement end{};  // D3DDECL_END: stream 0xFF, type D3DDECLTYPE_UNUSED
  end.stream = 0xFFu;
  end.type = 0xFFFFFFFFu;
  elements.push_back(end);
  return elements;
}

static void WWE12LogDeclObject(u32 decl_va, size_t element_count) {
  static std::atomic<u32> logged{0};
  if (logged.fetch_add(1) >= 24)
    return;
  std::string words;
  for (u32 i = 0; i < 32; i++) {
    const auto *w = bd::mem::at<const be_u32>(decl_va + i * 4u);
    if (!w)
      break;
    words += fmt::format("{:08X}{}", u32(*w), (i % 8 == 7) ? " | " : " ");
  }
  BD_INFO("WWE12 vertex declaration 0x{:08X}: {} element(s) | {}", decl_va, element_count,
          words);
}
#endif

GuestVertexDeclaration *SvrLookupVertexDeclaration(u32 decl_va) {
  if (!decl_va)
    return nullptr;
  {
    std::shared_lock lock(g_object_mutex);
    auto it = g_decls.find(decl_va);
    if (it != g_decls.end())
      return it->second;
  }
#if defined(WWE12_NO_CREATE_SHADERS)
  const auto *common = bd::mem::at<const be_u32>(decl_va);
  if (!common || (u32(*common) & 0xFu) != 5u)  // D3DCOMMON_TYPE_VERTEXDECLARATION
    return nullptr;
  std::vector<GuestVertexElement> elements = WWE12ReadDeclElements(decl_va);
  WWE12LogDeclObject(decl_va, elements.empty() ? 0 : elements.size() - 1);
  if (elements.empty())
    return nullptr;
  // Same keying as SvrOnVertexDeclarationCreated: padding zeroed, terminator included.
  for (auto &e : elements)
    e.padding = 0;
  const u64 hash = XXH3_64bits(elements.data(), elements.size() * sizeof(GuestVertexElement));
  std::unique_lock lock(g_object_mutex);
  GuestVertexDeclaration *&host = g_decls_by_hash[hash];
  if (!host)
    host = CreateVertexDeclaration(elements.data());
  g_decls[decl_va] = host;
  return host;
#else
  return nullptr;
#endif
}

namespace {

// A mirrored texture as last checked: the fetch constant and content it was built from.
struct MirrorCheck {
  u32 fetch[6] = {};
  u64 hash = 0;
  u64 frame = 0;
  u64 sample = 0;      // SampleHash of the content the full hash was taken on
  u64 full_frame = 0;  // frame of the last full hash
  GuestTexture *host = nullptr;
};
std::unordered_map<u32, MirrorCheck> g_mirror_checks;  // D3D texture VA -> last check
std::atomic<u64> g_frame{1};
std::atomic<u64> g_mirror_builds{0}, g_mirror_build_ns{0};  // texture mirrors (re)built, CPU time

// Hashing every texture's whole content on every frame it is used cost ~5% of the render thread
// (2026-10-17 profile). Textures of 16 KB and more get a sampled hash each frame (48 slices of 64
// bytes spread over the base level and the mips, plus the fetch constant): enough to see a Bink
// frame or a menu reloaded into the same memory at once. Their full hash is still taken at least
// every kFullHashFrames frames, so a change between the samples shows after a second at worst.
constexpr size_t kSampledHashMin = 16 * 1024;
constexpr u64 kFullHashFrames = 60;
u64 SampleHash(const NativeMirrorSource &src) {
  u64 hash = XXH3_64bits(src.fetch, sizeof(src.fetch));
  const auto slices = [&hash](const u8 *data, size_t size, size_t count) {
    if (!data || size < 64)
      return;
    for (size_t i = 0; i < count; ++i)
      hash = XXH3_64bits_withSeed(data + (size - 64) * i / (count - 1), 64, hash);
  };
  slices(src.base, src.base_size, 32);
  slices(src.mips, src.mips_size, 16);
  return hash;
}

}  // namespace
std::atomic<u64> g_texture_hashed_bytes{0}, g_texture_sampled{0};  // for the [perf] line
namespace {

u64 HashSource(const NativeMirrorSource &src) {
  g_texture_hashed_bytes.fetch_add(src.base_size + src.mips_size, std::memory_order_relaxed);
  u64 hash = XXH3_64bits(src.fetch, sizeof(src.fetch));
  if (src.base_size)
    hash = XXH3_64bits_withSeed(src.base, src.base_size, hash);
  if (src.mips_size)
    hash = XXH3_64bits_withSeed(src.mips, src.mips_size, hash);
  return hash;
}

// Texture mirrors are keyed by the game's D3DTexture address, and each match loads its wrestlers,
// arena and crowd at new addresses: the mirrors of the previous matches stayed in video memory
// for the whole session. A texture not drawn with for 60 s is let go (its mirror is rebuilt from
// the game's memory if it is ever used again). Caller holds g_mutex.
void ReleaseIdleMirrors(u64 frame) {
  constexpr u64 kIdleFrames = 3600;
  if (frame % 60 != 30 || frame < kIdleFrames || !REXCVAR_GET(svr_release_stale_targets))
    return;
  for (auto it = g_mirror_checks.begin(); it != g_mirror_checks.end();) {
    if (frame - it->second.frame > kIdleFrames) {
      if (it->second.host)
        EvictNativeTexture(it->first);
      it = g_mirror_checks.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

void Wwe12QuitEntryTick(u64 frame);
extern std::atomic<u64> g_render_thread_pso_compiles, g_render_thread_pso_known, g_render_thread_pso_ns;

}  // namespace bd::gpu
namespace rex::kernel::xboxkrnl {
void GetFileIoStats(uint64_t *bytes, uint64_t *calls, uint64_t *nanoseconds);
}
namespace bd::gpu {

// Loading statistics: once a second, when anything loads (or frames drop), one [load] line with
// that second's file reads (MB, calls, time in them), texture mirrors built (count, CPU time),
// pipelines compiled on the render thread and frames presented. Tells disk, CPU and GPU bound
// loading screens apart.
static void LoadStatsTick() {
  using clock = std::chrono::steady_clock;
  static clock::time_point last = clock::now();
  static u64 last_bytes = 0, last_calls = 0, last_io_ns = 0, last_mirrors = 0, last_mirror_ns = 0,
             last_pso = 0, last_frame = 0;
  const auto now = clock::now();
  const double seconds = std::chrono::duration<double>(now - last).count();
  if (seconds < 1.0)
    return;
  last = now;
  u64 bytes, calls, io_ns;
  rex::kernel::xboxkrnl::GetFileIoStats(&bytes, &calls, &io_ns);
  const u64 mirrors = g_mirror_builds.load(), mirror_ns = g_mirror_build_ns.load();
  const u64 pso = g_render_thread_pso_compiles.load(), frame = g_frame.load();
  const u64 pso_known = g_render_thread_pso_known.load(), pso_ns = g_render_thread_pso_ns.load();
  static u64 last_pso_known = 0, last_pso_ns = 0;
  const double mb = double(bytes - last_bytes) / (1024.0 * 1024.0);
  const double fps = double(frame - last_frame) / seconds;
  // Whole-process CPU time (user + kernel) used during this second: with the file-read time above it
  // tells a disk-bound load (reads) from a CPU-bound one (cpu near 1000 ms per core) and from waiting.
  static double last_cpu_ms = 0;
  double cpu_now_ms = 0;
  {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
      const auto ticks = [](const FILETIME &f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime); };
      cpu_now_ms = (ticks(kernel) + ticks(user)) / 10000.0;
    }
#else
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
      const auto ms = [](const timeval &t) { return double(t.tv_sec) * 1000.0 + double(t.tv_usec) / 1000.0; };
      cpu_now_ms = ms(ru.ru_utime) + ms(ru.ru_stime);
    }
#endif
  }
  const double cpu_ms = cpu_now_ms - last_cpu_ms;
  last_cpu_ms = cpu_now_ms;
  if (bytes != last_bytes || mirrors != last_mirrors || pso != last_pso || fps < 55.0)
    BD_INFO("[load] {:.1f} s: read {:.1f} MB in {} calls ({} ms) | textures {} ({} ms) | "
            "pipelines {} ({} known, {} ms) | cpu {:.0f} ms | {:.0f} fps",
            seconds, mb, calls - last_calls, (io_ns - last_io_ns) / 1000000, mirrors - last_mirrors,
            (mirror_ns - last_mirror_ns) / 1000000, pso - last_pso, pso_known - last_pso_known,
            (pso_ns - last_pso_ns) / 1000000, cpu_ms, fps);
  last_pso_known = pso_known, last_pso_ns = pso_ns;
  last_bytes = bytes, last_calls = calls, last_io_ns = io_ns, last_mirrors = mirrors,
  last_mirror_ns = mirror_ns, last_pso = pso, last_frame = frame;
}

// ---- live in-run A/B (test tool) ------------------------------------------------------------
// Separate 4-minute runs on the UHD 630 differ by up to 2 fps with the same settings; switching the
// option inside one run and comparing neighbouring segments removes that. Off by default.
REXCVAR_DEFINE_STRING(wwe13_live_ab, "", "WWE13",
                      "Live in-run A/B: name of a bool option toggled every wwe13_live_ab_seconds "
                      "(off first); each segment logs its frame times as an [ab] line");
REXCVAR_DEFINE_INT32(wwe13_live_ab_seconds, 10, "WWE13", "Live in-run A/B segment length")
    .range(2, 600);

std::atomic<u64> g_live_ab_draws{0};  // draw hook calls (hooks/draw.cpp), for the [ab] scene column

namespace {
void LiveAbTick() {
  static const std::string spec = REXCVAR_GET(wwe13_live_ab);
  if (spec.empty())
    return;
  // Arms are separated by ';' (or '+'); each arm is a comma list of options switched together
  // (a bundle). Segments alternate off / arm 1 / off / arm 2 / ... so every on-segment has two
  // off neighbours from the same minute.
  static const std::vector<std::vector<std::string>> arms = [] {
    std::vector<std::vector<std::string>> out(1);
    std::string cur;
    for (size_t i = 0; i <= spec.size(); ++i) {
      const char c = i < spec.size() ? spec[i] : ';';
      if (c == ',' || c == ';' || c == '+') {
        if (!cur.empty())
          out.back().push_back(cur);
        cur.clear();
        if ((c == ';' || c == '+') && !out.back().empty() && i < spec.size())
          out.emplace_back();
      } else {
        cur += c;
      }
    }
    if (out.back().empty())
      out.pop_back();
    return out;
  }();
  if (arms.empty())
    return;
  static const std::vector<std::string> arm_names = [] {
    std::vector<std::string> out;
    for (const auto &arm : arms) {
      std::string n;
      for (const auto &f : arm)
        n += (n.empty() ? "" : ",") + f;
      out.push_back(n);
    }
    return out;
  }();
  // "name=value": on sets value, off sets 0 (numeric options); plain name: true / false.
  const auto set_arm = [](size_t arm, bool v) {
    for (const auto &f : arms[arm]) {
      const size_t eq = f.find('=');
      const std::string name = eq == std::string::npos ? f : f.substr(0, eq);
      const std::string value = eq == std::string::npos ? (v ? "true" : "false")
                                                        : (v ? f.substr(eq + 1) : "0");
      if (!rex::cvar::SetFlagByName(name, value))
        BD_WARN("[ab] cannot set {}={}", name, value);
    }
  };
  using clock = std::chrono::steady_clock;
  static bool started = false, on = false;
  static size_t arm = 0;
  static clock::time_point seg_start, last;
  static std::vector<float> ms;
  static u32 segment = 0;
  const auto now = clock::now();
  if (!started) {
    started = true;
    for (size_t a = 0; a < arms.size(); ++a)
      set_arm(a, false);
    seg_start = last = now;
    ms.reserve(4096);
    g_live_ab_draws.store(0, std::memory_order_relaxed);
    return;
  }
  ms.push_back(std::chrono::duration<float, std::milli>(now - last).count());
  last = now;
  const double secs = std::chrono::duration<double>(now - seg_start).count();
  if (secs < double(REXCVAR_GET(wwe13_live_ab_seconds)))
    return;
  std::vector<float> sorted = ms;
  std::sort(sorted.begin(), sorted.end());
  double sum = 0;
  u32 slow = 0, on_time = 0;
  for (float v : ms) {
    sum += v;
    slow += v > 33.4f;
    on_time += v <= 17.5f;  // one 60 Hz refresh (16.7 ms) plus timer slack
  }
  const size_t n = sorted.size();
  const u64 draws = g_live_ab_draws.exchange(0, std::memory_order_relaxed);
  BD_INFO("[ab] seg {} {}={} {:.1f} s: {} frames, {:.2f} fps, median {:.2f} ms, p99 {:.2f} ms, >33ms {}, "
          "<=17ms {:.1f} %, draws/frame {:.0f}",
          segment, arm_names[arm], on ? "on" : "off", secs, n, n / secs, sorted[n / 2],
          sorted[std::min(n - 1, n * 99 / 100)], slow, n ? 100.0 * on_time / n : 0.0,
          n ? double(draws) / n : 0.0);
  ++segment;
  if (on) {
    set_arm(arm, false);
    arm = (arm + 1) % arms.size();  // the next off segment is labelled with the arm that follows it
  } else {
    set_arm(arm, true);
  }
  on = !on;
  ms.clear();
  seg_start = now;
}
}  // namespace

void SvrOnFrame() {
  SvrWatchdogHeartbeat();
  LiveAbTick();
  const u64 frame = g_frame.fetch_add(1, std::memory_order_relaxed);
  ::svr::HookProfileTick(frame);
  ::svr::GuestProbeTick(frame);
  g_frame_index.store(frame, std::memory_order_relaxed);
  SvrFlushBatchedReadbacks();
  SvrProcessDeferredReadbacks(frame);
  g_surface_frame.store(frame, std::memory_order_relaxed);
  ReleaseIdleSurfaces(frame);
  {
    std::lock_guard lock(g_mutex);
    ReleaseIdleMirrors(frame);
  }
  LoadStatsTick();
  Wwe12QuitEntryTick(frame);
  Wwe12DisplayOnFrame();
  // Present rate in the log every 600 frames (pacing check).
  static auto last = std::chrono::steady_clock::now();
  if (frame % 600 == 0) {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    last = now;
    size_t surfaces, targets, mirrors;
    {
      std::lock_guard lock(g_mutex);
      surfaces = g_surfaces.size(), targets = g_targets.size(), mirrors = g_mirror_checks.size();
    }
    BD_INFO("SvR native: {:.1f} presents/s | live surfaces {}, targets {}, textures {}",
            600.0 / seconds, surfaces, targets, mirrors);
  }
}

u64 SvrFrameIndex() { return g_frame.load(std::memory_order_relaxed); }

// A GPU-written texture sampled through another texture header than the one the game resolved
// into: same base address, format and size (fetch dwords 1 and 2). WWE 2K15 renders a 64x64
// reflection cube per wrestler and samples it through copies of the resolved header, which were
// left unresolved (the 2D debug texture, so no cube in slot 11). Seen in issue #10's log (white
// attire on AMD); whether it is what turns attire white is not confirmed. Caller holds g_mutex.
static GuestTexture *TargetWithSameMemory(u32 texture_va) {
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  if (!ReadFetch(texture_va, fetch) || !fetch.base_address)
    return nullptr;
  for (auto &[va, target] : g_targets) {
    if (target.host && target.desc[0] == fetch.dword_1 && target.desc[1] == fetch.dword_2) {
      target.last_used = g_surface_frame.load(std::memory_order_relaxed);
      static u32 s_logged = 0;
      if (s_logged < 16 && ++s_logged)
        BD_INFO("SvR texture 0x{:08X}: sampled as target 0x{:08X} (same memory)", texture_va, va);
      return target.host;
    }
  }
  return nullptr;
}

// The game writes texture memory with the CPU whenever it likes (every Bink movie frame, menus
// reloading into reused memory), so a mirror is checked against its source once per frame it's
// used and rebuilt when the fetch constant or the bytes changed. A texture that can't be mirrored
// is remembered the same way, so it isn't retried (or logged) until it changes.
// Fast path (wwe13_draw_fast_path, default off): a texture that is not a GPU target and was already
// checked this frame resolves to the same mirror for the rest of the frame (the code below returns
// check.host without touching anything), so that answer is kept per thread, keyed by frame and by
// the g_targets count, and the lock and the two map lookups are skipped.
REXCVAR_DEFINE_BOOL(wwe13_draw_fast_path, false, "WWE13",
                    "Draw fast path: reuse this frame's texture lookups and the unchanged pixel-shader "
                    "constant block between draws (same picture)");

namespace {
struct ResolveCacheEntry {
  u32 va = 0;
  u64 frame = ~0ull, gen = ~0ull;
  GuestTexture *host = nullptr;
};
thread_local ResolveCacheEntry t_resolve_cache[256];
inline ResolveCacheEntry &ResolveCacheSlot(u32 va) { return t_resolve_cache[(va * 2654435761u) >> 24]; }
}  // namespace

GuestTexture *SvrResolveTexture(u32 texture_va) {
  WWE12_CPU_ZONE(kTextures);
  if (!texture_va)
    return nullptr;
  const u64 frame = g_frame.load(std::memory_order_relaxed);
  const bool fast = REXCVAR_GET(wwe13_draw_fast_path);
  if (fast) {
    const ResolveCacheEntry &e = ResolveCacheSlot(texture_va);
    if (e.va == texture_va && e.frame == frame &&
        e.gen == g_targets_gen.load(std::memory_order_relaxed))
      return e.host;
  }
  std::lock_guard lock(g_mutex);
  if (auto it = g_targets.find(texture_va);
      it != g_targets.end() && it->second.host && TargetCurrent(texture_va, it)) {
    it->second.last_used = g_surface_frame.load(std::memory_order_relaxed);
    return it->second.host;
  }
  MirrorCheck &check = g_mirror_checks[texture_va];
  if (check.frame == frame) {
    if (fast && check.host) {
      // Not a target (the lookup above missed or retired it) and already checked this frame.
      ResolveCacheEntry &e = ResolveCacheSlot(texture_va);
      e.va = texture_va, e.frame = frame, e.host = check.host;
      e.gen = g_targets_gen.load(std::memory_order_relaxed);
    }
    return check.host ? check.host : TargetWithSameMemory(texture_va);
  }
  NativeMirrorSource src;
  if (!GetNativeMirrorSource(texture_va, src)) {
    check.frame = frame;
    check.host = nullptr;
    return TargetWithSameMemory(texture_va);
  }
  u64 sample = 0;
  if (src.base_size + src.mips_size >= kSampledHashMin) {
    sample = SampleHash(src);
    if (check.frame != 0 && check.sample == sample && frame - check.full_frame < kFullHashFrames) {
      g_texture_sampled.fetch_add(1, std::memory_order_relaxed);
      check.frame = frame;
      return check.host ? check.host : TargetWithSameMemory(texture_va);
    }
  }
  const u64 hash = HashSource(src);
  if (check.frame != 0 && check.hash == hash) {
    check.frame = frame;
    check.sample = sample;
    check.full_frame = frame;
    return check.host ? check.host : TargetWithSameMemory(texture_va);
  }
  const auto build_start = std::chrono::steady_clock::now();
  check.host = GetOrCreateNativeMirror(texture_va, 0);
  g_mirror_builds.fetch_add(1, std::memory_order_relaxed);
  g_mirror_build_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - build_start)
                                  .count(),
                              std::memory_order_relaxed);
  check.hash = hash;
  check.frame = frame;
  check.sample = sample;
  check.full_frame = frame;
  return check.host ? check.host : TargetWithSameMemory(texture_va);
}

}  // namespace bd::gpu
