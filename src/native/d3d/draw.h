// WP7 (decision 0006 stage 2): DeviceState -> nr::backend::Draw translation. Frozen interface. Worker thread.
// Stub until WP7 lands (always returns false / "not implemented").
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <string>

#include "device_state.h"
#include "regview.h"
#include "shadow_types.h"
#include "wide_core.h"

namespace nr::backend {
struct Draw;
}

namespace wwe2k14::d3d {

// Side information about a translated draw that is not part of nb::Draw (diagnostics / compare).
struct DrawSide {
  uint32_t packet_addr = 0xFFFFFFFFu;
  uint32_t seq = 0;
  uint32_t vertex_bytes = 0;
  uint32_t index_count = 0;
  bool thinned = false;  // WWE2K14_D3D_CROWD_THIN left this member out: not a failure
  uint64_t vs_fnv = 0, ps_fnv = 0;
  uint32_t color_surface = 0, depth_surface = 0;  // shadow nb surface handles
  SurfaceDesc color_desc{}, depth_desc{};
  // --- additions (WP7) ---
  uint32_t prim = 0;                       // Xenos primitive type
  bool indexed = false;
  bool target_fail = false;                // targets::BindForDraw failed (surface handles kNone; draw still translated)
  bool texture_fail = false;               // at least one texture slot had no handle (binding kept with kNone)
  uint32_t texture_slots = 0;              // number of texture bindings
  uint64_t tex_content_key[32] = {};       // per binding (same order as Draw::textures): WP8 content key, 0 = unknown
  std::string note;                        // ids suffix as nr's audit ids (" f24", " earlyz", ...)
  // S3-3 vertex/index cache (draw-off mode only): entries whose bytes this draw uploads; DrawSubmitted() marks them
  // stored. Same VbCache::Entry* space for vertex (key tag 0) and index (key tag 1) entries.
  void* vb_commit[16] = {};
  uint32_t n_vb_commit = 0;
  bool cache_flush_sent = false;
};
// S3-3: the draw was handed to the backend (its cache uploads/flush will happen): commit the vertex/index-cache
// entries.
void DrawSubmitted(const DrawSide& side);

// Worker thread, called at kSwap after all draws of `frame` have been translated.
wide_core::FrameDrawStats TakeWideDrawStats(uint32_t frame);

// Lane diagnostic (default off): worker-thread local counters, logged every 300 frames.
namespace pd {
enum { kIxLookup, kIxConvert, kVbLookup, kVbCopy, kTexForSlot, kTexSampler, kConstCopy,
       kDoPre, kAcquire, kTranslate, kSubmit, kDrawSub, kWhole, kN };
inline const char* const kNames[kN] = {"ix_cache_lookup", "ix_convert", "vb_cache_lookup", "vb_copy(resize+swap)",
  "tex_ForSlot", "tex_sampler", "const_memcpy", "DoDraw_args_keys", "AcquireDraw", "TranslateDraw(inclusive)",
  "SubmitDraw(inclusive)", "DrawSubmitted", "DoDraw(inclusive)"};
inline bool On() { static const bool v = [] { const char* e = std::getenv("WWE2K14_D3D_PD_PROF"); return e && *e == '1' && !e[1]; }(); return v; }
inline uint64_t Now() { return On() ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) : 0; }
inline uint64_t g_ns[kN] = {}, g_cnt[kN] = {};
inline void Add(int k, uint64_t t0) { if (On()) { g_ns[k] += Now() - t0; ++g_cnt[k]; } }
struct WholeScope { uint64_t t0 = Now(); ~WholeScope() { Add(kWhole, t0); } };
}  // namespace pd

// Card S3-31 census (WWE2K14_D3D_PAIR_CENSUS=N, diagnostic, remove when settled): log the per-frame draw census by
// bound (colour, depth) pair every N frames; called by the worker at every Swap.
void PairCensusFrameEnd();
// U0b motion-vector census (WWE2K14_D3D_MV_CENSUS=N, diagnostic, remove when U0 is settled); at every Swap.
void MvCensusFrameEnd();
// Card S3-39: picks next frame's resolution set of dual-resolution surfaces (WWE2K14_D3D_SCALE_DYNAMIC); at every Swap.
void SceneScaleFrameEnd();
void LoadLockFrameEnd();  // card N4e

// Guest memory base (guest address A lives at base + A); set once by the capture/worker owner before the first
// TranslateDraw (hooks have it as `base`). Without it TranslateDraw fails with "no guest base".
void SetGuestBase(const uint8_t* guest_base);

// Fills `d` (from nb::AcquireDraw in S2-D, a local Draw in S2-T). false = skipped, `why` set.
bool TranslateDraw(const EvHeader& e, const RegView& rv, const DeviceState& s, nr::backend::Draw* d, DrawSide* side,
                   std::string* why);

// Card S3-67 (WWE2K14_D3D_LATE_LEARN=d3d|census, with WWE2K14_D3D_LATE_TEX=4): game thread, at each hooked D3D call's
// entry - notes the guest-kick count for event `seq` (census only; its first call installs the kick hook). Worker, at
// every kSwap: periodic [d3d-learn] line (the release learner resolves in late_upload::LearnAtRelease). Both no-ops in
// the default (cp) mode.
void LateLearnNoteCall(uint32_t seq);
void LateLearnFrameEnd();

// Card S3-44 diagnostic (FLASH_TRACE): skipped-draw reasons of the frame so far (cleared by the call).
std::string TakeFrameSkips();
}  // namespace wwe2k14::d3d
