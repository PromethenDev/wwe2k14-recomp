// WP8 (decision 0006 stage 2): texture handles from fetch constants + guest memory; resolve images from host snapshots.
// Frozen interface (the three functions below) + additive WP8-owned extras (SetMemory, SetDrawShaders, Counters).
// Worker thread only (the write-watch callback is the one exception and touches only atomics).
//
// Source rule (decision 0006): the ONLY inputs are the DeviceState fetch constants and the guest memory they point
// to. A fetch whose base overlaps a destination recorded by RegisterResolveImage is served from that host snapshot,
// or refused - never from guest memory (in stage 2 that memory holds nr's own copy-out).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "device_state.h"
#include "shadow_types.h"

namespace nr::backend {
struct SamplerDesc;
struct ResolveCensusKey;
}
namespace nr::shared {
class TextureWatch;
}
namespace rex::graphics {
class Shader;
}
namespace rex::memory {
class Memory;
}
namespace wwe2k14::d3d::quality { struct SamplingQualityInfo; }

namespace wwe2k14::d3d::textures {

// Q: enabled once by the worker BEFORE processing any event when an option is
// active. Purely derived metadata; never changes the authoritative texture path.
void EnableSamplingQualityMetadata();
quality::SamplingQualityInfo GetSamplingQualityInfo(uint32_t handle);

// Diagnostics-only metadata counts (registry sizes, fail-closed reasons, history). Used by the
// 120swap diagnostics only; never an input to the authoritative texture path.
struct SamplingQualityMetadataStats {
  uint32_t live = 0, peak = 0, invalid_causes = 0, history_records = 0, orphan_records = 0;
  bool history_overflow = false;
};
SamplingQualityMetadataStats GetSamplingQualityMetadataStats();

// Diagnostics-only fetch-constant facts for one slot: the raw six words plus the fetch-claimed
// geometry/levels decoded through the SDK's own fetch decoding. Never an input to the
// authoritative texture path, GetSamplingQualityInfo or eligibility.
struct SamplingQualityFetchInfo {
  bool valid = false;
  std::array<uint32_t, 6> words{};
  uint32_t width = 0, height = 0, format = 0, base = 0, mip = 0, min_level = 0, max_level = 0,
           first_level = 0, claimed_levels = 0;
};
SamplingQualityFetchInfo GetSamplingQualityFetchInfo(const DeviceState& ds, uint32_t slot);

// Handle for fetch slot `slot` of the current state; nr::backend::kNone + why on failure (counted).
// `dimension` is the shader contract's value (ShaderEntry::texture_dims, XenosRecomp TextureDimension: 1 2D, 3 cube;
// 0 1D / 2 3D are refused), as WP7 passes it. `content_key`: guest textures = low 64
// bits of the XXH3-128 build hash (cache key + every guest byte the build reads); resolve-sourced = (1<<63)|entry
// sequence (compare those through ResolveRecord::hash, not this). ShadowMode()==1 (translate only) runs every check
// and the hash but makes no backend call and returns the reserved handle 0.
uint32_t ForSlot(const DeviceState& s, uint32_t slot, uint32_t dimension, nr::backend::SamplerDesc* sampler,
                 uint64_t* content_key, std::string* why);
// Process-lifetime 1x1 RGBA8 white texture for the opt-in WWE2K14_D3D_SHADOWS=0 receiver substitutions.
// Translate-only mode returns the same reserved fake handle 0 used by ForSlot; no backend call is made there.
uint32_t LitShadowTexture(std::string* why);
// Record that destination range [phys_base, phys_base+extent) is now represented by host snapshot `snapshot`.
// Replaces every recorded entry the range overlaps. OWNERSHIP: WP8 takes over `snapshot` (WP6 passes a
// RetainSnapshot'ed id) and calls nr::backend::ReleaseSnapshot on it when the entry is replaced/dropped.
// The range is usable only when src_rect left/top are 0 (the image then starts at the destination origin).
// Snapshot image contract (assumption, to confirm with WP6): src_rect = {left, top, right, bottom} of the resolved
// region; the image is (right-left) x (bottom-top) texels, top row first, in the layout of the destination format:
// k_8_8_8_8 / k_8 -> RGBA8 (R/B as the source, swap bit applied here via swizzle); k_16_16(_16_16) -> 16-bit UNORM
// of the resolve LUT values; depth copies -> R32F of the Float20e4 value.
void RegisterResolveImage(uint32_t phys_base, uint32_t extent, const ResolveDesc& r, uint32_t snapshot,
                          const nr::backend::ResolveCensusKey& census_key);
// Per-frame housekeeping: frame counter (cache stamps), LRU, periodic counter log line.
void OnWorkerFrameEnd();

// ---- WP8 extras (additive) ----
// Optional: guest memory object (default: the kernel's, looked up on first use). Needed for the write watch.
void SetMemory(rex::memory::Memory* memory);
// RR2 offline replay: a recorded write-watch invalidation (the same MarkWritten the live watch callback does).
void ReplayMarkWritten(uint32_t start, uint32_t length);
// Record-then-replay (worker thread): the live resolve registry in start order - entry extent, its latest registration,
// its snapshot id (borrowed) and census key. Images made before a recording window live only here.
void ForEachResolveEntry(const std::function<void(uint32_t start, uint32_t length, const ResolveDesc& desc,
                                                  uint32_t snapshot, const nr::backend::ResolveCensusKey& key)>& cb);
// Optional, WP7 before the ForSlot calls of a draw: the SDK shaders whose fetch instructions may override the fetch
// constant's filters (nr::shared::SamplerFromFetch). Null = fetch constant only.
void SetDrawShaders(const rex::graphics::Shader* vs, const rex::graphics::Shader* ps);

struct Counters {
  uint64_t hits = 0;              // cache hits (incl. memo hits)
  uint64_t memo_hits = 0;         // subset of hits served by the per-slot memo
  uint64_t misses = 0;            // texture builds (first sight, or rebuilt after a content change)
  uint64_t rebuilds = 0;          // subset of misses: an existing entry whose guest bytes changed
  uint64_t upload_bytes = 0;      // host texel bytes handed to the backend (CreateTexture)
  uint64_t resolve_binds = 0;     // binds served from a resolve snapshot
  uint64_t refusals = 0;          // ForSlot returned kNone (all causes)
  uint64_t refused_cap = 0;       // ... because the shadow texture cap was full
  uint64_t refused_resolve = 0;   // ... resolve overlap not servable (class / written / geometry)
  uint64_t resolves_registered = 0, resolves_unusable = 0, resolve_written = 0, resolves_composed = 0, resolve_crops = 0, resolve_cubes = 0, resolve_mipped = 0,
           mip_levels_clamped = 0, resolve_stale_dropped = 0, resolve_seeded = 0, resolve_stale_written_dropped = 0, resolve_watch_kept = 0, resolve_overlap_bypassed = 0, resolve_materialized = 0, resolve_plain_seeded = 0, resolve_assembled = 0;
  uint64_t evictions = 0;
  uint32_t live = 0, live_high = 0;  // shadow textures currently alive / high-water mark
  uint32_t cap = 0;
  uint64_t refused_bytes = 0;  // ... because the shadow texture byte cap (WWE2K14_D3D_SHADOW_TEX_MB) was full
  uint64_t live_bytes = 0, byte_cap = 0;
};
const Counters& GetCounters();
// C1-N5: internal registration breakdown collected only when
// WWE2K14_D3D_RESOLVE_FINE_TIMERS=1. Read and reset at the same 512-resolve
// boundary as the outer resolve timer.
enum class ResolveRegistrationFineStage : uint8_t {
  kPageMark,
  kSeedInvalidate,
  kResolveMetadata,
  kHideOldScan,
  kCoverage,
  kOverlapLru,
  kMemoForget,
  kWatchSeed,
  kTrace,
  kCount,
};
struct ResolveRegistrationFineStats {
  static constexpr std::size_t kStageCount = static_cast<std::size_t>(ResolveRegistrationFineStage::kCount);
  std::array<uint64_t, kStageCount> wall_ns{};
  std::array<uint64_t, kStageCount> cpu_ns{};
  std::array<uint64_t, kStageCount> calls{};
  uint64_t pages_marked = 0;
  uint64_t memo_entries_scanned = 0;
  uint32_t max_resolve_entries = 0;
  uint32_t max_frame_memo_entries = 0;
  uint32_t max_seed_canvases = 0;
  uint32_t max_coverage_cells = 0;
};
ResolveRegistrationFineStats TakeResolveRegistrationFineStats();
// Card S3-44 diagnostic (FLASH_TRACE): this frame's non-memo texture binds as text (cleared by the call).
std::string TakeFrameBinds();
// Card S3-44: a guest texture at this base address changed its bytes after being built (worker or CP thread).
bool IsDynamicBase(uint32_t start);
bool FetchBaseRange(const DeviceState& ds, uint32_t slot, uint32_t* start, uint32_t* length, uint32_t* format,
                    uint32_t* width);

// S3-3: the texture layer's guest-write watch and memory (shared with the D3D vertex cache); null before first use.
nr::shared::TextureWatch* SharedWatch();

// Card S3-17 (draw-off): the guest CPU is about to read [base, base+length) (OnCpuVisibility, under the backend lock,
// after the worker caught up). For every live resolve entry overlapping it whose bytes can be reproduced (k_8_8_8_8,
// k_8, 4444 from an RGBA8 snapshot, image at the destination origin): read the snapshot back, convert to the guest
// layout and hand the bytes inside the range to `write(start, bytes)` (one call per entry; untouched texels carry
// guest memory's current bytes). Everything else is listed in `refused`.
struct CpuVisResult {
  uint32_t entries = 0, served = 0;
  uint64_t texels = 0, bytes = 0;
  std::vector<std::string> refused;
};
CpuVisResult WriteBackResolves(uint32_t base, uint32_t length,
                               const std::function<void(uint32_t, const std::vector<uint8_t>&)>& write);
rex::memory::Memory* SharedMemory();
}  // namespace wwe2k14::d3d::textures
