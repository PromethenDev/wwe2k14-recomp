// WP6 (decision 0006 stage 2): D3D surface objects -> shadow host images (no EDRAM). Frozen interface (+ additive part).
// All functions run on the worker thread. Mode 2 (ShadowMode()==2): under the backend lock the worker holds per batch
// (do NOT take BackendLock() here: it is a plain mutex). Mode 1 (TranslateOnly()): NO nr::backend call is made at all;
// surface handles are local fakes (see EnsureHeight), clears are not run. Written for this project; method only from the plan (docs/reports/20261014-d3d-stage2-plan.md
// WP6), SVR code never copied.
//
// Source of truth: the DeviceState mirror (RB_SURFACE_INFO / RB_COLOR_INFO / RB_DEPTH_INFO at 0x2000..0x2005) and the
// surface object pointers; the surface object words (+24 / +28 / +36) are read through the optional guest reader below
// and are used for the height and as a cross-check only (counted), never as a second source for state.
#pragma once

#include <cstdint>
#include <string>

#include "device_state.h"
#include "shadow_types.h"

namespace wwe2k14::d3d::targets {

// ---------------------------------------------------------------- frozen part
// Shadow nb surface handles for the current binding (nr::backend::kNone if unbound / failed). false + why on failure.
bool BindForDraw(const DeviceState& s, uint32_t* color, uint32_t* depth, SurfaceDesc* cdesc, SurfaceDesc* ddesc,
                 std::string* why);
void OnSetRT(const EvHeader& e, const DeviceState& s);
void OnBeginTiling(const EvHeader& e, const DeviceState& s);
void OnEndTiling(const EvHeader& e);
void OnClear(const EvHeader& e, const DeviceState& s);

// ---------------------------------------------------------------- additive part (no frozen signature changed)
// Raw guest reader: copies `bytes` of guest memory at `guest_addr` (big-endian as stored) into dst; false = unreadable.
// Set once by whoever owns guest memory access (WP9 worker). Without it: surface heights come from viewport/scissor
// hints only, Clear rect lists and the BeginTiling colour/rects are treated as unread (counted).
using GuestReadFn = bool (*)(uint32_t guest_addr, void* dst, uint32_t bytes);
void SetGuestReader(GuestReadFn fn);

// Same as OnClear / OnBeginTiling but with the side data the event's pointers refer to, captured by WP9 at hook time
// (the pointers are only valid then; the frozen entry points above fall back to the guest reader, which may be stale).
//   OnClearData: rects_be = count * 16 bytes (D3DRECT x1,y1,x2,y2, big-endian int32) or null (= whole surface).
//   OnBeginTilingData: rects_be = args[1] * 16 bytes of tile D3DRECTs, color_be = 16 bytes (float4, big-endian) or null.
void OnClearData(const EvHeader& e, const DeviceState& s, const uint8_t* rects_be);
void OnBeginTilingData(const EvHeader& e, const DeviceState& s, const uint8_t* rects_be, const uint8_t* color_be);

// ---- used by resolves.cpp (WP6-internal) ----
struct Source {
  uint32_t handle = UINT32_MAX;  // nb surface handle
  SurfaceDesc desc{};
  uint64_t slot_id = 0;          // stable identity of the image slot (object + info words), survives height growth
  // Declared surface size from the surface object +36 (0 = unknown). desc.width is the EDRAM pitch (e.g. 560 for a
  // 512-wide surface); a NULL-rect (whole surface) resolve copies the declared size (card M2c2).
  uint32_t ext_w = 0, ext_h = 0;
  // Card M2c7: full scene extent of the slot's latest predicated tiling (0 = never tiled); a NULL-rect resolve of a
  // tiled scene copies the scene, not the declared tile.
  uint32_t scene_w = 0, scene_h = 0;
};
// Source surface of a resolve: depth if `depth`, else colour render target `index` (0..3). Grows the image so that
// `need_bottom` rows exist (0 = no requirement). false + why on failure.
bool ResolveSource(const DeviceState& s, bool depth, uint32_t index, uint32_t need_bottom, Source* out,
                   std::string* why);
// Clears recorded by resolves (clear-with-resolve) are applied before the next draw bind / clear / SetRT / tiling /
// intersecting resolve, so a following per-tile snapshot of the same scene still reads the un-cleared tiles.
struct DeferredClear {
  uint32_t handle = UINT32_MAX;
  bool depth = false, color = false, stencil = false;
  int32_t x = 0, y = 0, w = 0, h = 0;
  float rgba[4] = {0, 0, 0, 0};
  bool integer_view = false;
  float z = 1.0f;
  uint32_t stencil_value = 0;
  bool whole = false;   // card V1: a whole-surface clear (no rect list); the rect is the host image
  bool unread = false;  // card V1 round 2: the game passed a rect list that could not be read (coverage unknown)
};
void DeferClear(const DeferredClear& c);
bool PendingClearIntersects(uint32_t handle, int32_t x, int32_t y, int32_t w, int32_t h);
void FlushDeferredClears();
// Card S3-27: increments at every draw bind and every applied clear (any target): a resolve whose source saw no write
// since an earlier identical resolve reads identical pixels.
uint64_t WriteSeq();
// Predicated tiling bookkeeping: epoch increments at every BeginTiling (0 = none yet).
bool InTiling();
uint32_t TilingEpoch();

struct Stats {
  uint64_t binds = 0, bind_fail = 0, mrt_draws = 0, setrt = 0;
  uint64_t rt_created = 0, rt_grow = 0, rt_grow_lost = 0, rt_height_fallback = 0, rt_cap_fail = 0, rt_bytes = 0;
  uint64_t obj_read = 0, obj_info_match = 0, obj_info_mismatch = 0, obj_extent = 0;
  uint64_t clears_color = 0, clears_depth = 0, clears_mrt_skipped = 0, clear_rects_unread = 0;
  uint64_t rt_tile_widen = 0, rt_tile_taller = 0, tiling_begin = 0, tiling_end = 0, tiling_clear_color_unread = 0, tiling_extent_unread = 0;
  uint64_t rt_tile_resize_refused = 0;  // card S3-65: tiling resize kept the old image (backend refused the release)
  // Card G18b (G18d): depth images sized to the bound colour target (created or grown taller / recreated wider / covering
  // image refused: creation failed, the old or declared-extent image is used), and
  // draws whose framebuffer still had a depth attachment smaller than colour 0 (CHECK, expected 0; counted in draw.cpp).
  uint64_t depth_cover_create = 0, depth_cover_regrow = 0, depth_cover_refused = 0, fb_mismatch = 0;
};
const Stats& GetStats();
void CountFbMismatch();  // card G18b (G18d) CHECK
uint32_t DeclaredHeight(uint32_t handle);  // card G18b (G18d) diagnostic: the slot's +36 rows (0 = unknown)
// One "[d3d-shadow] rt ..." line (counts + live image list head).
void LogStats(const char* why);

// Card S3-66 (WWE2K14_D3D_TILE_EPOCH=1, default off): one copy per destination per tiling epoch. The epoch stays open
// after EndTiling ("tail") for the colour tile resolves EndTiling's body issues, until the next draw bind / clear /
// SetRT / BeginTiling. BeginTiling creates a slot without an image directly at the scene extent.
bool TileEpochOn();
bool InTilingTail();
void CloseTilingTail();  // Swap / resync (worker.cpp): no resolve of a later frame carries this epoch
uint32_t ResolveEpoch();  // TilingEpoch() inside the bracket or its tail, else 0 (maintained only with TILE_EPOCH on)
uint64_t SceneCreates();  // slots created at the scene extent by BeginTiling (TILE_EPOCH on)

// Card V1 (WWE2K14_D3D_RT_EVICT, default on; 0 = off; check; age): once per present (worker kSwap, mode 2). Under
// device-memory pressure releases render-target images unused for WWE2K14_D3D_RT_EVICT_AGE presents whose EDRAM range
// was fully overwritten by clears of other targets since (the console no longer has those pixels).
void OnFrameEnd();
// Card V1 CHECK (check modes only): a resolve COPY (not a clear-only / null / unsupported resolve) reads rect
// (x, y, w, h) of `src`; verifies flagged images (see targets.cpp).
void NoteResolveRead(const Source& src, int32_t x, int32_t y, uint32_t w, uint32_t h);

}  // namespace wwe2k14::d3d::targets
