// WP6 (decision 0006 stage 2): see targets.h. Written for this project from the plan (WP6 section) and the static
// findings listed in docs/reports/20261014-d3d-stage2-plan.md "## WP6"; SVR code is not used.
//
// Image model: one host image ("slot") per (surface object, RB_SURFACE_INFO word, RB_COLOR/DEPTH_INFO word, role). The
// same object with changed info words is a new image (no aliasing / ownership transfers, no EDRAM). Images live in the
// backend's surface table under keys with bit 63 set (nr's own keys never have it), so nr::backend::Surface() can never
// hand back an nr surface. Card S3-52/S3-65: a slot is the only owner of its image; replaced images are released through
// ReleaseSlotImage (nb::ReleaseSurface), which also keeps rt_bytes equal to the sum of the slots' bytes.
#include "capture.h"
#include "targets.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/logging.h>

#include "native/nr/nr_backend.h"
#include "native/nr/nr_shared.h"
#include "depth_cover_core.h"
#include "rt_evict_core.h"
#include "rt_real_cap_core.h"

namespace wwe2k14::d3d::targets {
namespace {

namespace nb = nr::backend;
namespace xenos = rex::graphics::xenos;

constexpr uint32_t kMaxDim = 8192;
constexpr uint32_t kFallbackHeight = 720;  // documented fallback when neither the object nor the state gives a height

// Xbox 360 D3DCLEAR_* (static inference: ClearF tests 0x10 for "Z value valid"; BeginTiling passes 63 = all):
// 0x1..0x8 = colour targets 0..3, 0x10 = depth, 0x20 = stencil.
constexpr uint32_t kClearTarget0 = 0x1, kClearTargets = 0xF, kClearDepth = 0x10, kClearStencil = 0x20;

inline uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline float BitsToFloat(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline uint32_t Align16(uint32_t v) { return (v + 15u) & ~15u; }
uint32_t EnvU32(const char* name, uint32_t def) {
  const char* v = std::getenv(name);
  return v && *v ? uint32_t(std::strtoul(v, nullptr, 10)) : def;
}

struct SlotKey {
  uint32_t object, info0, info1;
  uint8_t depth;
  bool operator<(const SlotKey& o) const {
    return std::tie(object, info0, info1, depth) < std::tie(o.object, o.info0, o.info1, o.depth);
  }
};
struct Slot {
  SlotKey key{};
  uint64_t id = 0;
  uint32_t width = 0, height = 0, handle = nb::kNone;
  uint32_t ext_w = 0, ext_h = 0;  // from surface object +36 (0 = unknown)
  // Card M2c7: predicated tiling draws the scene once at full size here; the surface object only declares one tile
  // (m2c7-a: 320x736 vs a 1280x720 scene). Scene extent of the slot's latest BeginTiling (0 = never tiled).
  uint32_t scene_w = 0, scene_h = 0;
  uint8_t samples = 1;        // host samples (the guest count unless capped)
  uint8_t guest_samples = 1;  // card S3-10: RB_SURFACE_INFO msaa_samples
  bool depth = false;
  nb::TargetFormat format = nb::TargetFormat::kRGBA8;
  uint32_t grown = 0;
  uint64_t bytes = 0;     // card S3-52: bytes counted in rt_bytes for the current image
  uint64_t real_bytes = 0;  // V19: trigger-heap allocations owned by the backend surface, including its aliases
  bool pending_read = false;
  uint64_t last_use = 0;  // card S3-52: steady-clock ms of the last Ensure (LRU eviction)
  // Card V1: write sequence (g_write_seq) and present count of the last use (Ensure / own clear), and the CHECK state.
  uint64_t last_seq = 0, last_present = 0;
  bool check_flag = false, check_drawn = false, check_sentinel = false;
  uint64_t check_flag_present = 0;
};

GuestReadFn g_reader = nullptr;
std::map<SlotKey, Slot> g_slots;
// A target the backend could not create is not requested again for kRtFailedRetry (V152 review 11-09: a permanent
// block would turn a short memory shortage into a lost target for the whole session; per-draw retries flooded the log
// with thousands of failures on cards that refuse the target).
struct RtFailedKey {
  std::string why;
  std::chrono::steady_clock::time_point at;
};
constexpr std::chrono::seconds kRtFailedRetry{3};
std::unordered_map<uint64_t, RtFailedKey> g_rt_failed_surface_keys;
std::vector<DeferredClear> g_deferred;
Stats g_stats;
bool g_tiling = false;
uint32_t g_epoch = 0;
// Card S3-66 (WWE2K14_D3D_TILE_EPOCH=1): the epoch stays open after EndTiling for the colour tile resolves its body
// issues (SV-0b: they follow the kEndTiling event); closed by the next draw bind / clear / SetRT / BeginTiling.
bool g_tail = false;
uint64_t g_scene_create = 0;  // card S3-66: new tiled slots created directly at the scene extent (no widen)
uint64_t g_log_counter = 0;
std::map<std::string, uint64_t> g_fail_reasons;

uint64_t g_write_seq = 0;  // card S3-27 (WriteSeq)

// ---- card V1: release render-target images that are provably dead, when device memory is under pressure ----
// WWE2K14_D3D_RT_EVICT (an image must always be idle for _AGE presents):
//   1 (default)  + its EDRAM footprint overwritten by full-coverage clears of other targets since its last use
//                + its surface object no longer describes the key (guest words +24/+28 changed: the game cannot bind
//                  that key again, so no later resolve can reach the image)
//   edram        idle + EDRAM only (round-1 rule)        age   idle only
//   0            off (as before)
//   check / check-edram / check-age   the same rules, but flag instead of release and verify on next real read.
struct EvictRule {
  bool on = true, edram = true, unreachable = true, check = false;
  const char* name = "1";
};
const EvictRule& Rule() {
  static const EvictRule r = [] {
    EvictRule x;
    const char* e = std::getenv("WWE2K14_D3D_RT_EVICT");
    const std::string v = e ? e : "";
    if (v == "0") x.on = false, x.name = "0";
    else if (v == "edram") x.unreachable = false, x.name = "edram";
    else if (v == "age") x.unreachable = x.edram = false, x.name = "age";
    else if (v == "check") x.check = true, x.name = "check";
    else if (v == "check-edram") x.check = true, x.unreachable = false, x.name = "check-edram";
    else if (v == "check-age") x.check = true, x.unreachable = x.edram = false, x.name = "check-age";
    return x;
  }();
  return r;
}
bool CheckMode() { return Rule().on && Rule().check; }
rt_evict::TileMark g_tile_mark[rt_evict::kTiles];  // latest known-coverage write (clear) of each EDRAM tile
uint64_t g_present = 0;
struct EvictStats {
  // round 3 (review F2): evicted_bytes / flagged_bytes = bytes in the trigger heap (nb::SurfaceLocalHeapBytes)
  uint64_t passes = 0, pressure = 0, idle = 0, uncovered = 0, evicted = 0, evicted_bytes = 0, refused = 0;
  uint64_t stamps = 0, stamp_skip_unknown = 0, stamp_skip_tiling = 0, stamp_skip_unread = 0, stamp_skip_partial = 0;
  uint64_t reachable = 0;  // idle + overwritten, refused: the surface object still describes the key
  uint64_t host_skip = 0;  // round 3: dead but not in heap 0 (U2-steered to host memory): releasing it cannot relieve
  uint64_t flagged = 0, flagged_bytes = 0, safe_clear = 0, draw_first = 0, read_clean = 0, read_stale = 0,
           read_definite = 0, read_unverified = 0, read_fail = 0;
  uint64_t last_usage = 0, last_budget = 0, last_rt_dev = 0;
} g_ev;
struct Slot;
std::unordered_map<uint32_t, Slot*> g_by_handle;  // card V1: image handle -> slot (stamping clears by handle)

uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }


uint64_t SlotId(const SlotKey& k) {
  uint64_t h = 1469598103934665603ull;
  h = Mix(h, k.object);
  h = Mix(h, k.info0);
  h = Mix(h, k.info1);
  h = Mix(h, k.depth);
  return h | (1ull << 63);
}
uint64_t SurfaceKey(const Slot& s, uint32_t height) { return Mix(s.id, (uint64_t(height) << 16) | s.width) | (1ull << 63); }

uint32_t RtCapScaleMilli() {
  static const uint32_t scale_milli = [] {
    const char* e = std::getenv("WWE2K14_D3D_SCENE_SCALE");
    const float scale = e ? std::strtof(e, nullptr) : 1.0f;
    return scale >= 0.4f && scale <= 3.0f ? uint32_t(std::lround(double(scale) * 1000.0)) : 1000u;
  }();
  return scale_milli;
}

uint64_t RtCapBytes() {
  static const uint64_t cap = rt_real_cap::ScaleCapBytes(
      uint64_t(EnvU32("WWE2K14_D3D_SHADOW_RT_MB", 2048)) << 20, RtCapScaleMilli());
  return cap;
}

void CountFail(const std::string& why) {
  std::string k = why.substr(0, 60);
  if (++g_fail_reasons[k] == 1) REXLOG_WARN("[d3d-shadow] rt first failure: {}", why);
}

// Surface object words +24 / +28 / +32 / +36 (static layout report + ClearF disassembly). false without a reader.
bool ReadObject(uint32_t object, uint32_t out[4]) {
  if (!g_reader || !object) return false;
  uint8_t raw[16];
  if (!g_reader(object + 24, raw, 16)) return false;
  for (int i = 0; i < 4; ++i) out[i] = Be32(raw + 4 * i);
  ++g_stats.obj_read;
  return true;
}

// Bottom of the viewport / window scissor in rows, from the mirror only (0 = no usable hint).
uint32_t HintBottom(const DeviceState& s) {
  const uint32_t vte = s.rs2200[6];  // PA_CL_VTE_CNTL: bit 2 y scale enable, bit 3 y offset enable
  if ((vte & 0xC) == 0xC) {
    const float scale = std::fabs(BitsToFloat(s.rs2100[0x11])), off = BitsToFloat(s.rs2100[0x12]);
    const float bottom = off + scale;
    if (std::isfinite(bottom) && bottom >= 1.0f && bottom <= float(kMaxDim)) return uint32_t(std::ceil(bottom));
  }
  const uint32_t win_br = (s.rs2000[18] >> 16) & 0x7FFF;  // window scissor BR y (mirror dword 18)
  if (win_br >= 1 && win_br <= kMaxDim) return win_br;
  return 0;
}

bool HostFormat(const DeviceState&, bool depth, uint32_t info1, nb::TargetFormat* out, std::string* why) {
  if (depth) {
    *out = ((info1 >> 16) & 1) ? nb::TargetFormat::kD24FS8 : nb::TargetFormat::kD24S8;
    return true;
  }
  auto cf = xenos::ColorRenderTargetFormat((info1 >> 16) & 0xF);
  if (cf == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) cf = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  if (!nr::shared::HostTargetFormat(cf, out)) {
    *why = "render target format " + std::to_string(uint32_t(cf));
    return false;
  }
  return true;
}

SurfaceDesc Describe(const Slot& sl, const DeviceState& s) {
  SurfaceDesc d{};
  d.object = sl.key.object;
  d.surface_info = sl.key.info0;
  d.color_or_depth_info = sl.key.info1;
  d.hiz = sl.depth ? s.rs2200[3] : 0;
  d.width = sl.width;
  d.height = sl.height;
  d.samples = sl.samples;
  d.guest_samples = sl.samples != sl.guest_samples ? sl.guest_samples : 0;
  d.depth = sl.depth;
  d.format = sl.format;
  return d;
}

// Mode 1 only: deterministic stand-in for nb::Surface (never equal to nb::kNone, outside any real handle range).
uint32_t FakeSurface(uint64_t key) {
  static std::map<uint64_t, uint32_t> fake;
  auto it = fake.find(key);
  if (it == fake.end()) it = fake.emplace(key, 0x40000000u + uint32_t(fake.size())).first;
  return it->second;
}

// Card S3-65 (RT widen leak): default on. WWE2K14_D3D_TILE_RESIZE=0 restores the pre-S3-65 behaviour (the tiling resize
// drops the slot's handle without releasing it, leaving the old image allocated and counted; mode 1 releases nothing).
bool TileResizeFix() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_TILE_RESIZE");
    return !(e && std::strcmp(e, "0") == 0);
  }();
  return on;
}

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint64_t g_rt_evicted = 0, g_rt_evicted_bytes = 0;
uint64_t g_rt_real_bytes = 0;
uint64_t g_rt_pending_retire_bytes = 0;
uint64_t g_rt_frame_evicted = 0, g_rt_frame_evicted_bytes = 0;
std::unordered_set<uint32_t> g_rt_retired_handles;

bool BoundSlot(const Slot& sl, const DeviceState& s) {
  const uint32_t info0 = s.rs2000[0];
  if (sl.depth) return s.depth == sl.key.object && info0 == sl.key.info0 && s.rs2000[2] == sl.key.info1;
  for (uint32_t i = 0; i < kMaxColorRt; ++i) {
    const uint32_t info1 = i == 0 ? s.rs2000[1] : s.rs2000[2 + i];
    if (s.color_rt[i] == sl.key.object && info0 == sl.key.info0 && info1 == sl.key.info1) return true;
  }
  return false;
}

// V19: query actual trigger-heap allocations in one backend synchronization. The optional new handle is returned
// separately so a failed cap check can release it while preserving the existing slot image.
bool RefreshRtRealBytes(uint32_t extra_handle = nb::kNone, nb::SurfaceMemoryInfo* extra_info = nullptr) {
  if (TranslateOnly()) return false;
  std::vector<uint32_t> handles;
  std::unordered_map<uint32_t, Slot*> owners;
  for (auto& kv : g_slots) {
    Slot& sl = kv.second;
    if (sl.handle == nb::kNone) continue;
    if (owners.emplace(sl.handle, &sl).second) handles.push_back(sl.handle);
  }
  for (uint32_t handle : g_rt_retired_handles)
    if (owners.find(handle) == owners.end()) handles.push_back(handle);
  if (extra_handle != nb::kNone && owners.find(extra_handle) == owners.end()) handles.push_back(extra_handle);
  std::vector<nb::SurfaceMemoryInfo> infos;
  if (!nb::GetSurfaceMemoryInfo(handles, &infos) || infos.size() != handles.size()) return false;

  for (auto& kv : g_slots) kv.second.real_bytes = 0, kv.second.pending_read = false;
  g_rt_real_bytes = 0, g_rt_pending_retire_bytes = 0;
  auto add_bytes = [](uint64_t* total, uint64_t bytes) {
    *total = bytes > std::numeric_limits<uint64_t>::max() - *total ?
             std::numeric_limits<uint64_t>::max() : *total + bytes;
  };
  nb::SurfaceMemoryInfo found_extra{};
  bool has_extra = false;
  for (size_t i = 0; i < handles.size(); ++i) {
    const uint32_t handle = handles[i];
    const nb::SurfaceMemoryInfo& info = infos[i];
    auto owner = owners.find(handle);
    if (owner != owners.end()) {
      owner->second->real_bytes = info.live ? info.local_heap_bytes : 0;
      owner->second->pending_read = info.live && info.pending_read;
      const uint64_t bytes = info.live ? info.local_heap_bytes : 0;
      g_rt_real_bytes = bytes > std::numeric_limits<uint64_t>::max() - g_rt_real_bytes ?
                        std::numeric_limits<uint64_t>::max() :
                        g_rt_real_bytes + bytes;
    } else if (g_rt_retired_handles.find(handle) != g_rt_retired_handles.end()) {
      if (info.live || info.retired) {
        add_bytes(&g_rt_real_bytes, info.local_heap_bytes);
        if (info.retired) add_bytes(&g_rt_pending_retire_bytes, info.local_heap_bytes);
      }
      if (!info.live && !info.retired) g_rt_retired_handles.erase(handle);
    }
    if (handle == extra_handle) found_extra = info, has_extra = true;
  }
  if (extra_info) *extra_info = has_extra ? found_extra : nb::SurfaceMemoryInfo{};
  return extra_handle == nb::kNone || has_extra;
}

// Card S3-52: frees the image of `sl` in the backend (waits for the GPU) and drops its pending clears. The slot keeps its
// key but has no handle; Ensure recreates the image if the game binds it again.
bool ReleaseSlotImage(Slot* sl) {
  if (sl->handle == nb::kNone) return false;
  if (TranslateOnly()) {
    // Card S3-65: mode 1 holds fake handles and no backend memory: no backend call, accounting only.
    if (!TileResizeFix()) return false;
    g_stats.rt_bytes -= std::min(g_stats.rt_bytes, sl->bytes);
    sl->bytes = 0;
    sl->handle = nb::kNone;
    sl->height = 0;
    return true;
  }
  const uint32_t h = sl->handle;
  if (!nb::ReleaseSurface(h)) return false;
  g_rt_retired_handles.insert(h);  // keep the allocation in the cap until its backend retire serial completes
  g_deferred.erase(std::remove_if(g_deferred.begin(), g_deferred.end(), [h](const DeferredClear& c) { return c.handle == h; }),
                   g_deferred.end());
  g_by_handle.erase(h);
  sl->check_flag = false;
  g_stats.rt_bytes -= std::min(g_stats.rt_bytes, sl->bytes);
  sl->bytes = 0;
  sl->real_bytes = 0;
  sl->pending_read = false;
  sl->handle = nb::kNone;
  sl->height = 0;
  return true;
}

// Card S3-52: the game creates new render targets per arena / mode and never revisits most of the old ones; without a
// release the byte budget fills after ~3-4 matches and every later target is refused (s398/s402/s403: all draws of
// whole scenes dropped as "bad surfaces" after ~15 min). Frees the least recently used targets idle for >= 3 s until
// `need` more bytes fit under 80 % of the budget.
void EvictIdle(const Slot* keep, uint64_t need) {
  const uint64_t now = NowMs();
  std::vector<Slot*> idle;
  for (auto& kv : g_slots) {
    Slot& o = kv.second;
    if (&o == keep || o.handle == nb::kNone || now - o.last_use < 3000) continue;
    idle.push_back(&o);
  }
  std::sort(idle.begin(), idle.end(), [](const Slot* a, const Slot* b) { return a->last_use < b->last_use; });
  uint64_t freed = 0;
  uint32_t n = 0;
  for (Slot* o : idle) {
    if (g_stats.rt_bytes + need <= RtCapBytes() / 5 * 4) break;  // free down to 80 %: one batch (one GPU wait) per scene change
    const uint64_t b = o->bytes;
    ReleaseSlotImage(o);
    if (o->handle == nb::kNone) freed += b, ++n;
  }
  if (n) {
    g_rt_evicted += n;
    g_rt_evicted_bytes += freed;
    REXLOG_INFO("[d3d-shadow] rt evicted {} idle targets ({} MB; total {} / {} MB), budget {} / {} MB", n, freed >> 20,
                g_rt_evicted, g_rt_evicted_bytes >> 20, g_stats.rt_bytes >> 20, RtCapBytes() >> 20);
  }
}

// V19: use backend allocation sizes, excluding the currently bound targets, lazy resolve reads, and anything used
// during the last three seconds. ReleaseSurface repeats the pending-read check after synchronizing the record thread.
void EvictRealIdle(const DeviceState& state, const Slot* keep, uint64_t incoming, bool* waited_for_retire) {
  const uint64_t now = NowMs();
  std::vector<rt_real_cap::Candidate> candidates;
  std::unordered_map<uint64_t, Slot*> by_token;
  for (auto& kv : g_slots) {
    Slot& sl = kv.second;
    if (&sl == keep || sl.handle == nb::kNone) continue;
    const uint64_t token = sl.handle;
    if (!by_token.emplace(token, &sl).second) continue;
    candidates.push_back({token, sl.last_use, sl.real_bytes, BoundSlot(sl, state), sl.pending_read});
  }
  const rt_real_cap::Plan plan = rt_real_cap::MakePlan(g_rt_real_bytes, incoming, RtCapBytes(), now, 3000, candidates);
  bool released = false;
  for (uint64_t token : plan.victims) {
    auto it = by_token.find(token);
    if (it == by_token.end()) continue;
    Slot* victim = it->second;
    const uint64_t bytes = victim->real_bytes;
    if (!ReleaseSlotImage(victim)) continue;
    released = true;
    ++g_rt_evicted;
    g_rt_evicted_bytes += bytes;
    ++g_rt_frame_evicted;
    g_rt_frame_evicted_bytes += bytes;
  }
  const bool measured = !released || RefreshRtRealBytes();
  if (!measured || !rt_real_cap::PendingRetireCanSatisfy(g_rt_real_bytes, incoming, RtCapBytes(),
                                                         g_rt_pending_retire_bytes,
                                                         waited_for_retire && *waited_for_retire))
    return;

  // ReleaseSurface serializes this call on the backend record thread and tags each image with
  // the active list's retire serial. One FinishGpuWork drains that list once for this whole LRU
  // batch; the following memory query reaps the completed surfaces and remeasures real bytes.
  if (waited_for_retire) *waited_for_retire = true;
  const uint64_t before_bytes = g_rt_real_bytes;
  const uint64_t pending_bytes = g_rt_pending_retire_bytes;
  const auto wait_start = std::chrono::steady_clock::now();
  nb::FinishGpuWork();
  const bool refreshed = RefreshRtRealBytes();
  const uint64_t wait_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - wait_start).count());
  REXLOG_INFO("[d3d-shadow] rt-real-cap retire-wait frame={} pending={} MB before={} MB after={} MB incoming={} MB cap={} MB wait_ms={} measured={}",
              g_present, pending_bytes >> 20, before_bytes >> 20, g_rt_real_bytes >> 20, incoming >> 20,
              RtCapBytes() >> 20, wait_ms, refreshed ? 1 : 0);
}

// Card G18b (G18d): `cover` = the depth image is sized to the bound colour target (may exceed the declared extent; a
// replacement gets exactly `need` rows); `replace` = create a new image even if the current one has enough rows (it is
// narrower than sl->width). The old image is released only after the new one exists.
bool EnsureHeight(const DeviceState& state, Slot* sl, uint32_t need, std::string* why, bool cover = false, bool replace = false) {
  sl->last_use = NowMs();
  sl->last_seq = g_write_seq;  // card V1
  sl->last_present = g_present;
  need = std::min(Align16(std::max(need, 16u)), kMaxDim);
  if (sl->handle != nb::kNone && sl->height >= need && !replace) return true;
  if (sl->handle != nb::kNone && sl->ext_h && !cover) {  // declared surface size: never grow, callers clamp their rects
    return true;
  }
  uint32_t nh = need;
  if (sl->handle != nb::kNone && !cover) nh = std::min(std::max(need, Align16(sl->height + sl->height / 4)), kMaxDim);
  const uint64_t surface_key = SurfaceKey(*sl, nh);
  auto failed_surface = g_rt_failed_surface_keys.find(surface_key);
  if (failed_surface != g_rt_failed_surface_keys.end()) {
    if (std::chrono::steady_clock::now() - failed_surface->second.at < kRtFailedRetry) {
      *why = failed_surface->second.why;
      return false;
    }
    g_rt_failed_surface_keys.erase(failed_surface);  // try again: the shortage may be over
  }
  const uint64_t bytes = uint64_t(sl->width) * nh * sl->samples * std::max(1u, nb::TargetBytesPerPixel(sl->format));
  const bool real_cap = RefreshRtRealBytes();
  bool waited_for_retire = false;
  if (real_cap) {
    EvictRealIdle(state, sl, bytes, &waited_for_retire);  // logical size is a conservative pre-allocation floor
    if (bytes > RtCapBytes() || g_rt_real_bytes > RtCapBytes() - bytes) {
      ++g_stats.rt_cap_fail;
      *why = "shadow render target real-byte cap (WWE2K14_D3D_SHADOW_RT_MB)";
      return false;
    }
  } else {
    if (g_stats.rt_bytes + bytes > RtCapBytes()) EvictIdle(sl, bytes);
    if (g_stats.rt_bytes + bytes > RtCapBytes()) {
      ++g_stats.rt_cap_fail;
      *why = "shadow render target byte cap (WWE2K14_D3D_SHADOW_RT_MB)";
      return false;
    }
  }
  std::string err;
  // Mode 1: the worker must not touch the backend (nb::Surface push_backs g->surfaces under the record thread). A fake
  // handle per surface key keeps the real call's dedupe (same key -> same handle); values are not nr handles.
  const uint32_t h = TranslateOnly() ? FakeSurface(surface_key)
                                     : nb::Surface(surface_key, sl->width, nh, sl->depth, sl->format, sl->samples, &err);
  if (h == nb::kNone) {
    *why = "Surface(): " + err;
    g_rt_failed_surface_keys[surface_key] = {*why, std::chrono::steady_clock::now()};
    return false;
  }
  if (h == sl->handle) return true;  // Surface() deduplicated the requested image; it already has this slot's accounting.
  auto release_uncommitted = [&] {
    if (TranslateOnly()) return;
    if (nb::ReleaseSurface(h)) g_rt_retired_handles.insert(h);
    else REXLOG_WARN("[d3d-shadow] rt-real-cap could not release uncommitted surface {}", h);
    RefreshRtRealBytes();
  };

  nb::SurfaceMemoryInfo created_info{};
  if (real_cap) {
    const bool measured = RefreshRtRealBytes(h, &created_info);
    if (!measured || !created_info.live) {
      release_uncommitted();
      *why = "could not measure new render target allocation";
      if (measured) g_rt_failed_surface_keys[surface_key] = {*why, std::chrono::steady_clock::now()};
      return false;
    }
    EvictRealIdle(state, sl, created_info.local_heap_bytes, &waited_for_retire);
    if (created_info.local_heap_bytes > RtCapBytes() ||
        g_rt_real_bytes > RtCapBytes() - created_info.local_heap_bytes) {
      release_uncommitted();
      ++g_stats.rt_cap_fail;
      *why = "new render target exceeds real-byte cap";
      return false;
    }
  }
  if (sl->handle != nb::kNone) {
    // Card S3-52: the old, smaller image is no longer used by this slot: free it (its content is not carried over).
    ++g_stats.rt_grow;
    ++sl->grown;
    // The backend can only copy inside one surface: the old content is not carried over.
    ++g_stats.rt_grow_lost;
    REXLOG_WARN("[d3d-shadow] rt grow obj={:08X} {}x{} -> {}x{} samples={} (content of the old image not carried over)",
                sl->key.object, sl->width, sl->height, sl->width, nh, sl->samples);
    if (!ReleaseSlotImage(sl)) {
      release_uncommitted();
      *why = "old render target is still referenced by a pending read";
      return false;
    }
  }
  g_stats.rt_bytes += bytes;
  sl->bytes = bytes;
  sl->real_bytes = real_cap ? created_info.local_heap_bytes : 0;
  sl->pending_read = false;
  if (real_cap)
    g_rt_real_bytes = created_info.local_heap_bytes > std::numeric_limits<uint64_t>::max() - g_rt_real_bytes ?
                      std::numeric_limits<uint64_t>::max() :
                      g_rt_real_bytes + created_info.local_heap_bytes;
  ++g_stats.rt_created;
  sl->handle = h;
  sl->height = nh;
  if (!TranslateOnly()) g_by_handle[h] = sl;  // card V1
  return true;
}

bool DepthCoverOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_D3D_DEPTH_COVER");
    return !(v && *v == '0');
  }();
  return on;
}

// Finds or creates the slot for the bound colour target `index` / depth surface and makes sure `need_bottom` rows
// (and the hints) exist.
// Card S3-66: scene_w / scene_h (BeginTiling with TILE_EPOCH on) = create a slot that has no image yet at that extent.
Slot* Ensure(const DeviceState& s, bool depth, uint32_t index, uint32_t need_bottom, std::string* why,
             uint32_t scene_w = 0, uint32_t scene_h = 0) {
  const uint32_t object = depth ? s.depth : s.color_rt[index & 3];
  const uint32_t info0 = s.rs2000[0];
  const uint32_t info1 = depth ? s.rs2000[2] : (index == 0 ? s.rs2000[1] : s.rs2000[2 + (index & 3)]);
  SlotKey key{object, info0, info1, uint8_t(depth)};
  auto it = g_slots.find(key);
  if (it == g_slots.end()) {
    Slot sl;
    sl.key = key;
    sl.id = SlotId(key);
    sl.depth = depth;
    const uint32_t samples_log2 = (info0 >> 16) & 3;
    if (samples_log2 > 2) {
      *why = "MSAA code 3";
      return nullptr;
    }
    sl.guest_samples = uint8_t(1u << samples_log2);
    // Card S3-10 (owner option, picture change): WWE2K14_D3D_AA_CAP=2 renders the game's 4x MSAA surfaces with 2 host
    // samples (all of them, latched at start). Resolves see both counts (SurfaceDesc::guest_samples).
    static const uint32_t aa_cap = [] {
      const char* e = std::getenv("WWE2K14_D3D_AA_CAP");
      // r3 (owner 2026-10-18 late: measure AA off; lane aa1): '1' = 1 host sample (no MSAA) for the game's 2x/4x surfaces.
      return e && *e == '2' ? 2u : e && *e == '1' ? 1u : 0u;
    }();
    sl.samples = aa_cap && sl.guest_samples > aa_cap ? uint8_t(aa_cap) : sl.guest_samples;
    sl.width = info0 & 0x3FFF;
    if (!sl.width || sl.width > kMaxDim) {
      *why = "surface pitch " + std::to_string(sl.width);
      return nullptr;
    }
    if (!HostFormat(s, depth, info1, &sl.format, why)) return nullptr;
    uint32_t w[4];
    if (ReadObject(object, w)) {
      // Cross-check (counted, not an input): +28 must equal the mirrored RB_COLOR_INFO/RB_DEPTH_INFO, +24 the
      // RB_SURFACE_INFO (the depth object's +24 only matters when there is no colour target).
      if (w[1] == info1) ++g_stats.obj_info_match; else ++g_stats.obj_info_mismatch;
      if (g_stats.obj_info_mismatch && g_stats.obj_info_mismatch <= 8 && w[1] != info1) {
        REXLOG_WARN("[d3d-shadow] rt obj={:08X} +28={:08X} mirror info={:08X} (mirror used)", object, w[1], info1);
      }
      if ((!depth || !s.color_rt[0]) && w[0] != info0 && g_stats.obj_info_mismatch <= 8) {
        REXLOG_WARN("[d3d-shadow] rt obj={:08X} +24={:08X} mirror RB_SURFACE_INFO={:08X}", object, w[0], info0);
      }
      // Static finding (ClearF 0x82CB8A70 default rect): +36 bits 18-31 = width-1, bits 3-17 = height-1.
      const uint32_t ew = ((w[3] >> 18) & 0x3FFF) + 1, eh = ((w[3] >> 3) & 0x7FFF) + 1;
      if (ew >= 8 && eh >= 8 && ew <= kMaxDim && eh <= kMaxDim) {
        sl.ext_w = ew;
        sl.ext_h = eh;
        ++g_stats.obj_extent;
      }
    }
    it = g_slots.emplace(key, sl).first;
  }
  Slot* sl = &it->second;
  uint32_t need = need_bottom;
  if (sl->ext_h) {
    need = sl->ext_h;  // declared size is authoritative
  } else {
    need = std::max(need, HintBottom(s));
    if (!need && sl->handle == nb::kNone) {
      need = kFallbackHeight;
      ++g_stats.rt_height_fallback;
    }
  }
  if (!need) need = sl->height;
  if (sl->handle == nb::kNone && scene_w && scene_h && scene_w <= kMaxDim && scene_h <= kMaxDim &&
      (scene_w > sl->width || scene_h > need)) {  // card S3-66: one image at the scene extent, not tile + widen
    sl->width = std::max(sl->width, (scene_w + 7) & ~7u);
    need = std::max(need, scene_h);
    ++g_scene_create;
  }
  // Card G18b (G18d): a depth image bound with colour target 0 must cover the colour image (the framebuffer extent).
  // The depth slot shares the colour's pitch (key info0) but its rows come from the depth object's declared extent
  // (+36) or the 720-row fallback: the attire composite (colour 2080x1024) bound a 2080x720 depth image -> invalid
  // framebuffer, AMD dropped colour rows >= 720. The slot key includes the colour pitch / MSAA (RB_SURFACE_INFO), so the
  // larger image only serves bindings with this pitch; rows past the declared extent are host-only (the console has
  // other EDRAM there, which the host never models). The EDRAM footprint (Geo, StampClear) keeps the declared extent.
  // Default on; WWE2K14_D3D_DEPTH_COVER=0 restores the declared-extent sizing.
  bool cover = false;
  const uint32_t declared_need = need;
  uint32_t declared_w = sl->width;
  uint32_t cover_from = nb::kNone;  // the handle a new covering image replaces / kNone (none, or nothing new yet)
  bool cover_new = false;           // a covering image is created below (fresh slot) or was created here (grown)
  std::vector<DeferredClear> pend;  // deferred clears of the replaced image, re-queued on the new one
  const Slot* cs = nullptr;
  depth_cover::Plan p;
  if (depth && s.color_rt[0] && DepthCoverOn()) {
    auto ci = g_slots.find(SlotKey{s.color_rt[0], s.rs2000[0], s.rs2000[1], 0});
    if (ci != g_slots.end() && ci->second.handle != nb::kNone) {
      cs = &ci->second;
      p = depth_cover::PlanCover(sl->width, sl->handle != nb::kNone ? sl->height : 0, need, cs->width, cs->height);
      cover = p.cover;
    }
  }
  if (cover) {
    const uint32_t old_w = sl->width, old_h = sl->handle != nb::kNone ? sl->height : 0;
    const bool grow = sl->handle != nb::kNone && (p.recreate || sl->height < std::min(Align16(p.need_h), kMaxDim));
    bool counted = false;
    if (sl->handle == nb::kNone) {  // fresh image: created at the covering size by the EnsureHeight below
      ++g_stats.depth_cover_create;
      counted = cover_new = true;
      sl->width = p.width;
      need = p.need_h;
    } else if (grow) {
      // Review rv1: create the covering image first, release the old one after; if creation fails keep the old image
      // (the pre-G18d framebuffer, counted as refused).
      const uint32_t old = sl->handle;
      for (const DeferredClear& c : g_deferred)
        if (c.handle == old) pend.push_back(c);
      sl->width = p.width;
      std::string gerr;
      if (EnsureHeight(s, sl, p.need_h, &gerr, true, p.recreate) && sl->handle != old) {
        ++(p.recreate ? g_stats.depth_cover_regrow : g_stats.depth_cover_create);
        counted = cover_new = true;
        cover_from = old;
        need = p.need_h;
      } else {
        sl->width = declared_w;
        ++g_stats.depth_cover_refused;
        cover = false;
        pend.clear();
        if (g_stats.depth_cover_refused <= 8)
          REXLOG_WARN("[g18d-cover] depth obj={:08X} {}x{}: covering image {}x{} refused ({}); old image kept",
                      sl->key.object, old_w, old_h, p.width, Align16(p.need_h), gerr.empty() ? "same image" : gerr);
      }
    } else {
      need = p.need_h;  // the image already covers the colour target
    }
    if (counted) {
      const uint64_t n = g_stats.depth_cover_create + g_stats.depth_cover_regrow;
      if (n <= 16 || (n & (n - 1)) == 0) {
        REXLOG_INFO("[g18d-cover] #{} depth obj={:08X} {}x{} (declared {}x{}) -> {}x{} for colour obj={:08X} {}x{}{}", n,
                    sl->key.object, old_w, old_h, sl->ext_w, sl->ext_h, p.width, Align16(p.need_h), cs->key.object,
                    cs->width, cs->height, p.recreate ? " (recreated: old depth content not carried over)" : "");
      }
    }
  }
  const uint32_t before = sl->handle;
  if (!EnsureHeight(s, sl, need, why, cover)) {
    if (!cover || before != nb::kNone) return nullptr;
    // A fresh covering image was refused (byte cap / Surface error): fall back to the declared-extent image.
    sl->width = declared_w;
    ++g_stats.depth_cover_refused;
    cover = cover_new = false;
    if (!EnsureHeight(s, sl, declared_need, why)) return nullptr;
  }
  if (cover_new && sl->handle != nb::kNone && sl->handle != cover_from) {
    // Review rv1 (finding 9): nb::Surface leaves a new image's content undefined (no clear at creation,
    // nr_backend.cpp Surface). The rows past the declared extent are never cleared by the game's declared-rect
    // clears: clear the whole covering image once (to the replaced image's pending depth clear value if there was one,
    // else far depth / stencil 0), then re-queue the replaced image's pending clears on it.
    DeferredClear init;
    init.handle = sl->handle;
    init.depth = init.stencil = true;
    init.w = int32_t(sl->width);
    init.h = int32_t(sl->height);
    for (const DeferredClear& c : pend) {
      if (c.depth) init.z = c.z;
      if (c.stencil) init.stencil_value = c.stencil_value;
    }
    ++g_write_seq;  // new content: no earlier resolve of this slot matches (card S3-27 dedupe)
    std::string err;
    if (!TranslateOnly() &&
        !nb::ClearDepthStencil(init.handle, 0, 0, init.w, init.h, true, init.z, true, init.stencil_value, &err))
      REXLOG_WARN("[g18d-cover] initial clear of depth obj={:08X} failed: {}", sl->key.object, err);
    for (DeferredClear c : pend) {
      c.handle = sl->handle;
      g_deferred.push_back(c);
    }
  }
  return sl;
}

struct Rect { int32_t x0, y0, x1, y1; };

// Clamp to the image; false if empty.
bool ClampRect(const Slot& sl, Rect* r) {
  r->x0 = std::max(r->x0, 0);
  r->y0 = std::max(r->y0, 0);
  r->x1 = std::min<int32_t>(r->x1, int32_t(sl.width));
  r->y1 = std::min<int32_t>(r->y1, int32_t(sl.height));
  return r->x1 > r->x0 && r->y1 > r->y0;
}

void StampClear(const DeferredClear& c);  // card V1
void RunClear(const DeferredClear& c) {
  ++g_write_seq;
  if (TranslateOnly()) return;
  StampClear(c);  // nothing to clear: fake handles, and nb::Clear* would record on the shared command list
  std::string err;
  if (c.color) {
    if (!nb::ClearColor(c.handle, c.x, c.y, c.w, c.h, c.rgba, c.integer_view, &err)) {
      REXLOG_WARN("[d3d-shadow] rt clear colour failed: {}", err);
    }
  }
  if (c.depth || c.stencil) {
    if (!nb::ClearDepthStencil(c.handle, c.x, c.y, c.w, c.h, c.depth, c.z, c.stencil, c.stencil_value, &err)) {
      REXLOG_WARN("[d3d-shadow] rt clear depth failed: {}", err);
    }
  }
}

// ---- card V1 ----
// EDRAM footprint of a slot: base/pitch/MSAA/format from its key (the mirrored RB_SURFACE_INFO and RB_COLOR_INFO /
// RB_DEPTH_INFO words, registers.h:672-674,741,840), rows from the declared extent (surface object +36) or, unknown, the
// host image height (>= the rows the game used: a larger footprint is harder to prove overwritten).
rt_evict::Geometry Geo(const Slot& sl) {
  const uint32_t info0 = sl.key.info0, info1 = sl.key.info1;
  const bool is64 = !sl.depth && xenos::IsColorRenderTargetFormat64bpp(xenos::ColorRenderTargetFormat((info1 >> 16) & 0xF));
  const uint32_t pitch = info0 & 0x3FFF;
  return rt_evict::MakeGeometry(info1 & 0x7FF, pitch, (info0 >> 16) & 3, is64, sl.ext_w ? sl.ext_w : pitch,
                                sl.ext_h ? sl.ext_h : sl.height);
}

// Applied clear of a known rect: on the console the cleared tiles of this target's EDRAM range now hold the clear
// value, whatever another target kept there. Counted only when the write coverage is known: outside predicated tiling
// a rect (or a whole-surface clear of a surface with a declared extent); inside tiling / on tiled-scene images only
// whole-surface clears (the tiling clear, BeginTiling ClearF 63: every tile pass clears the declared surface).
// Depth: only depth AND stencil (a depth-only clear keeps the stencil bits of the shared word).
void StampClear(const DeferredClear& c) {
  if (!Rule().on) return;
  auto it = g_by_handle.find(c.handle);
  if (it == g_by_handle.end() || it->second->handle != c.handle) return;
  Slot& sl = *it->second;
  if (sl.check_flag && CheckMode() && c.x <= 0 && c.y <= 0 && c.x + c.w >= int32_t(sl.width) &&
      c.y + c.h >= int32_t(sl.height) && (sl.depth ? (c.depth && c.stencil) : c.color)) {
    ++g_ev.safe_clear;  // the whole image is rewritten before any read: evicting it was invisible
    sl.check_flag = false;
  }
  if (sl.depth ? !(c.depth && c.stencil) : !c.color) return;
  if (c.unread) {  // round 2 (review F1): a rect list that could not be read covers an unknown area
    ++g_ev.stamp_skip_unread;
    return;
  }
  if (c.whole && (!sl.ext_w || !sl.ext_h)) {
    ++g_ev.stamp_skip_unknown;
    return;
  }
  // A tiled scene's host image is the scene, its EDRAM range one tile: rect coordinates do not map (only whole clears).
  if ((g_tiling || sl.scene_w) && !c.whole) {
    ++g_ev.stamp_skip_tiling;
    return;
  }
  // Round 2 (review F1): only a clear whose own rect covers the writer's whole extent counts (declared +36 size, or
  // pitch x host height when unknown - the host height is >= the rows the game uses). Partial clears never count.
  const int32_t ew = int32_t(sl.ext_w ? sl.ext_w : (sl.key.info0 & 0x3FFF)), eh = int32_t(sl.ext_h ? sl.ext_h : sl.height);
  if (!c.whole && !rt_evict::ClearCoversExtent(c.x, c.y, c.w, c.h, ew, eh)) {
    ++g_ev.stamp_skip_partial;
    return;
  }
  const rt_evict::Geometry geo = Geo(sl);
  const uint64_t seq = g_write_seq;
  rt_evict::MarkWrite(geo, 0, 0, ew, eh, seq, g_tile_mark);
  sl.last_seq = seq;  // its own write: only LATER writes by other targets overwrite its pixels
  ++g_ev.stamps;
}

// Round 2 (review F2): true while the slot's surface object in guest memory still carries the key's info words, i.e.
// the game can still bind this key and resolve from it. +28 = the colour/depth info word, +24 = RB_SURFACE_INFO
// (compared for colour only; a depth object's +24 is not authoritative, see Ensure). No reader = assume reachable;
// unreadable object memory = not reachable.
bool ObjectDescribes(const Slot& sl) {
  if (!g_reader) return true;
  uint8_t raw[8];
  if (!sl.key.object || !g_reader(sl.key.object + 24, raw, 8)) return false;  // ReadObject's layout, not counted
  return Be32(raw + 4) == sl.key.info1 && (sl.depth || Be32(raw) == sl.key.info0);
}

// Every 30 presents: under device-memory pressure, release (or, CHECK, flag) every slot image that was not used for
// WWE2K14_D3D_RT_EVICT_AGE presents (default 600) and whose whole EDRAM footprint was overwritten by clears of other
// targets since its last use (mode age: the idle condition only).
void EvictDead() {
  const EvictRule& rule = Rule();
  if (!rule.on || TranslateOnly() || g_present % 30 != 0) return;
  static const uint32_t age = EnvU32("WWE2K14_D3D_RT_EVICT_AGE", 600), pct = EnvU32("WWE2K14_D3D_RT_EVICT_PCT", 75),
                        rt_pct = EnvU32("WWE2K14_D3D_RT_EVICT_RT_PCT", 25);
  // Round 3 (780M DEVICE_LOST report, out/worker-reports/20261027-gh/v1fix-780m.md): only images placed in heap 0 (the
  // heap the trigger measures) are released; WWE2K14_D3D_RT_EVICT_HOST=1 = round 2 (any placement).
  static const bool any_placement = EnvU32("WWE2K14_D3D_RT_EVICT_HOST", 0) != 0;
  ++g_ev.passes;
  uint64_t usage = 0, budget = 0;
  if (!nb::LocalHeapBudget(&usage, &budget)) return;
  g_ev.last_usage = usage;
  g_ev.last_budget = budget;
  // Round 2 (review F3): the RT arm uses the backend's real device-local RT allocations (= [nr-mem-owners] rt).
  const uint64_t rt_dev = nb::SurfaceDeviceLocalBytes();
  g_ev.last_rt_dev = rt_dev;
  if (!rt_evict::Pressure(usage, budget, rt_dev, pct, rt_pct)) return;
  ++g_ev.pressure;
  uint32_t n = 0, idle = 0, uncovered = 0, refused = 0, reachable = 0, host_skip = 0;
  uint64_t bytes = 0;
  for (auto& kv : g_slots) {
    Slot& o = kv.second;
    if (o.handle == nb::kNone || o.check_flag || g_present - o.last_present < age) continue;
    ++idle;
    if (rule.edram && !rt_evict::FootprintOverwritten(Geo(o), g_tile_mark, o.last_seq)) {
      ++uncovered;
      continue;
    }
    if (rule.unreachable && ObjectDescribes(o)) {
      ++reachable;
      continue;
    }
    // Review F2: the counted bytes are the image's bytes in the trigger heap (what releasing it frees there).
    const uint64_t b = nb::SurfaceLocalHeapBytes(o.handle);
    if (!rt_evict::PlacementRelieves(b, any_placement)) {
      ++host_skip;
      continue;
    }
    if (CheckMode()) {
      // Keep the image; RGBA8 colour images get a sentinel (no game writes it: 1,254,1,1) so a later resolve can tell
      // whether it read pixels that were never rewritten (= pixels eviction would have lost).
      o.check_flag = true;
      o.check_drawn = false;
      o.check_flag_present = g_present;
      o.check_sentinel = false;
      if (!o.depth && o.format == nb::TargetFormat::kRGBA8) {
        static const float kSentinel[4] = {1.0f / 255.0f, 254.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f};
        std::string err;
        o.check_sentinel = nb::ClearColor(o.handle, 0, 0, int32_t(o.width), int32_t(o.height), kSentinel, false, &err);
      }
      ++g_ev.flagged;
      g_ev.flagged_bytes += b;
      ++n, bytes += b;
      continue;
    }
    ReleaseSlotImage(&o);
    if (o.handle == nb::kNone) {
      ++n, bytes += b;
    } else {
      ++refused;
    }
  }
  g_ev.idle += idle;
  g_ev.uncovered += uncovered;
  g_ev.refused += refused;
  g_ev.reachable += reachable;
  g_ev.host_skip += host_skip;
  if (!CheckMode()) g_ev.evicted += n, g_ev.evicted_bytes += bytes;
  if (n) ++g_write_seq;  // nothing may match a resolve recorded before this point (card S3-27 dedupe)
  if (n || refused) {
    REXLOG_INFO("[d3d-shadow] rt-evict mode={} present={} heap0={}/{}MB rt_dev={}MB idle={} uncovered={} reachable={} "
                "host_skip={} {}={} ({} MB) refused={} total_evicted={} ({} MB) flagged={}",
                rule.name, g_present, usage >> 20, budget >> 20, rt_dev >> 20, idle, uncovered, reachable, host_skip,
                CheckMode() ? "flagged" : "evicted", n, bytes >> 20, refused, g_ev.evicted,
                g_ev.evicted_bytes >> 20, g_ev.flagged);
  }
}

// Immediate clear of colour target 0 and/or the depth surface over `rects` (empty = whole image).
void ApplyClear(const DeviceState& s, bool want_color, const float rgba[4], bool want_depth, float z,
                bool want_stencil, uint32_t stencil, std::vector<Rect> rects, uint32_t need_bottom,
                bool rects_unread = false) {
  uint32_t bottom = need_bottom;
  for (const Rect& r : rects) bottom = std::max<uint32_t>(bottom, uint32_t(std::max(r.y1, 0)));
  std::string why;
  Slot* sc = nullptr;
  Slot* sd = nullptr;
  if (want_color && s.color_rt[0]) {
    sc = Ensure(s, false, 0, bottom, &why);
    if (!sc) CountFail(why);
  } else if ((want_depth || want_stencil) && s.depth && s.color_rt[0] && DepthCoverOn()) {
    // Review rv1: a depth-only clear with colour 0 bound: the colour image must exist first so the depth image is
    // created at its covering size and this clear lands on it (not on a 720-row image replaced by the next draw).
    if (!Ensure(s, false, 0, 0, &why)) CountFail(why);
  }
  if ((want_depth || want_stencil) && s.depth) {
    sd = Ensure(s, true, 0, bottom, &why);
    if (!sd) CountFail(why);
  }
  for (int pass = 0; pass < 2; ++pass) {
    Slot* sl = pass == 0 ? sc : sd;
    if (!sl) continue;
    std::vector<Rect> rs = rects;
    if (rs.empty()) rs.push_back({0, 0, int32_t(sl->width), int32_t(sl->height)});
    for (Rect r : rs) {
      if (!ClampRect(*sl, &r)) continue;
      DeferredClear c;
      c.handle = sl->handle;
      c.x = r.x0;
      c.y = r.y0;
      c.w = r.x1 - r.x0;
      c.h = r.y1 - r.y0;
      c.whole = rects.empty() && !rects_unread;  // card V1: whole surface (no rect list)
      c.unread = rects_unread;                   // card V1 round 2: rect list given but not readable
      if (pass == 0) {
        c.color = true;
        std::memcpy(c.rgba, rgba, sizeof(c.rgba));
        ++g_stats.clears_color;
      } else {
        c.depth = want_depth;
        c.stencil = want_stencil;
        c.z = z;
        c.stencil_value = stencil & 0xFF;
        ++g_stats.clears_depth;
      }
      RunClear(c);
    }
  }
}

std::vector<Rect> ParseRects(const uint8_t* be, uint32_t count) {
  std::vector<Rect> out;
  if (!be) return out;
  count = std::min(count, 64u);
  for (uint32_t i = 0; i < count; ++i) {
    out.push_back({int32_t(Be32(be + 16 * i)), int32_t(Be32(be + 16 * i + 4)), int32_t(Be32(be + 16 * i + 8)),
                   int32_t(Be32(be + 16 * i + 12))});
  }
  return out;
}

void MaybeLog() {
  static const uint32_t every = EnvU32("WWE2K14_D3D_SHADOW_LOG_EVERY", 20000);
  if (every && ++g_log_counter % every == 0) LogStats("periodic");
}

}  // namespace

uint64_t WriteSeq() { return g_write_seq; }

// ------------------------------------------------------------------------------------------------ public

void SetGuestReader(GuestReadFn fn) { g_reader = fn; }
const Stats& GetStats() { return g_stats; }
void CountFbMismatch() { ++g_stats.fb_mismatch; }
uint32_t DeclaredHeight(uint32_t handle) {
  auto it = g_by_handle.find(handle);
  return it != g_by_handle.end() && it->second->handle == handle ? it->second->ext_h : 0;
}
bool InTiling() { return g_tiling; }
uint32_t TilingEpoch() { return g_epoch; }
bool TileEpochOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_TILE_EPOCH");
    return e && std::strcmp(e, "1") == 0;
  }();
  return on;
}
bool InTilingTail() { return g_tail; }
void CloseTilingTail() { g_tail = false; }  // S3-66 review F5: Swap / resync end the tail too
uint32_t ResolveEpoch() { return g_tiling || g_tail ? g_epoch : 0; }
uint64_t SceneCreates() { return g_scene_create; }

void DeferClear(const DeferredClear& c) { g_deferred.push_back(c); }
bool PendingClearIntersects(uint32_t handle, int32_t x, int32_t y, int32_t w, int32_t h) {
  for (const DeferredClear& c : g_deferred) {
    if (c.handle == handle && x < c.x + c.w && c.x < x + w && y < c.y + c.h && c.y < y + h) return true;
  }
  return false;
}
void FlushDeferredClears() {
  if (g_deferred.empty()) return;
  std::vector<DeferredClear> v;
  v.swap(g_deferred);
  for (const DeferredClear& c : v) {
    if (c.color) ++g_stats.clears_color;
    if (c.depth || c.stencil) ++g_stats.clears_depth;
    RunClear(c);
  }
}

bool ResolveSource(const DeviceState& s, bool depth, uint32_t index, uint32_t need_bottom, Source* out,
                   std::string* why) {
  if (depth ? !s.depth : !s.color_rt[index & 3]) {
    *why = depth ? "no depth surface bound" : "no colour target " + std::to_string(index) + " bound";
    return false;
  }
  if (!depth && index > 3) {
    *why = "source select " + std::to_string(index);
    return false;
  }
  Slot* sl = Ensure(s, depth, index, need_bottom, why);
  if (!sl) return false;
  out->handle = sl->handle;
  out->desc = Describe(*sl, s);
  out->slot_id = sl->id;
  out->ext_w = sl->ext_w;
  out->ext_h = sl->ext_h;
  out->scene_w = sl->scene_w;
  out->scene_h = sl->scene_h;
  return true;
}

bool BindForDraw(const DeviceState& s, uint32_t* color, uint32_t* depth, SurfaceDesc* cdesc, SurfaceDesc* ddesc,
                 std::string* why) {
  g_tail = false;  // card S3-66
  FlushDeferredClears();
  ++g_write_seq;
  MaybeLog();
  ++g_stats.binds;
  if (color) *color = nb::kNone;
  if (depth) *depth = nb::kNone;
  if (cdesc) *cdesc = SurfaceDesc{};
  if (ddesc) *ddesc = SurfaceDesc{};
  if (s.color_rt[1] || s.color_rt[2] || s.color_rt[3]) ++g_stats.mrt_draws;
  bool ok = true;
  std::string w;
  auto note_draw = [](Slot* sl) {  // card V1 CHECK: a flagged image is drawn into before any full clear
    if (sl->check_flag && !sl->check_drawn) {
      sl->check_drawn = true;
      ++g_ev.draw_first;
    }
  };
  if (s.color_rt[0]) {
    if (Slot* sl = Ensure(s, false, 0, 0, &w)) {
      note_draw(sl);
      if (color) *color = sl->handle;
      if (cdesc) *cdesc = Describe(*sl, s);
    } else {
      ok = false;
      CountFail(w);
    }
  }
  if (ok && s.depth) {
    if (Slot* sl = Ensure(s, true, 0, 0, &w)) {
      note_draw(sl);
      if (depth) *depth = sl->handle;
      if (ddesc) *ddesc = Describe(*sl, s);
    } else {
      ok = false;
      CountFail(w);
    }
  }
  if (!ok) {
    ++g_stats.bind_fail;
    if (color) *color = nb::kNone;
    if (depth) *depth = nb::kNone;
    if (why) *why = w;
  }
  return ok;
}

void OnSetRT(const EvHeader&, const DeviceState&) {
  g_tail = false;  // card S3-66
  FlushDeferredClears();
  ++g_stats.setrt;
}

void OnBeginTiling(const EvHeader& e, const DeviceState& s) {
  // Frozen entry: the rect list / colour pointers in args are read from guest memory now (may be stale, see header).
  std::vector<uint8_t> rects, color;
  if (g_reader) {
    const uint32_t count = std::min(e.args[1], 64u);
    if (count && e.args[2]) {
      rects.resize(size_t(count) * 16);
      if (!g_reader(e.args[2], rects.data(), uint32_t(rects.size()))) rects.clear();
    }
    if (e.args[3]) {
      color.resize(16);
      if (!g_reader(e.args[3], color.data(), 16)) color.clear();
    }
  }
  OnBeginTilingData(e, s, rects.empty() ? nullptr : rects.data(), color.empty() ? nullptr : color.data());
}

void OnBeginTilingData(const EvHeader& e, const DeviceState& s, const uint8_t* rects_be, const uint8_t* color_be) {
  g_tail = false;  // card S3-66
  FlushDeferredClears();
  ++g_stats.tiling_begin;
  g_tiling = true;
  ++g_epoch;
  // BeginTiling(dev, r4 flags (bit0 = skip the first clear), r5 tile count, r6 D3DRECT tiles, r7 float4* colour,
  //             f1 Z, r9 stencil) - recomp 85:29952; scene extent = max x2 / max y2 over the tiles (dev+13556/+13560).
  const uint32_t flags = e.args[0];
  uint32_t max_x = 0, max_y = 0;
  for (const Rect& r : ParseRects(rects_be, e.args[1])) {
    max_x = std::max<uint32_t>(max_x, uint32_t(std::max(r.x1, 0)));
    max_y = std::max<uint32_t>(max_y, uint32_t(std::max(r.y1, 0)));
  }
  if (!max_y) ++g_stats.tiling_extent_unread;
  // Make sure the scene images exist at the full size before the first draw binds them (card M2c7: also the full
  // width - the bound surface object declares one tile, the scene is max x2 wide).
  std::string why;
  for (int d = 0; d < 2; ++d) {
    if (d ? !s.depth : !s.color_rt[0]) continue;
    Slot* sl = TileEpochOn() ? Ensure(s, d != 0, 0, max_y, &why, max_x, max_y)  // card S3-66
                             : Ensure(s, d != 0, 0, max_y, &why);
    if (!sl) {
      CountFail(why);
      continue;
    }
    if (max_x && max_y) {
      sl->scene_w = max_x;
      sl->scene_h = max_y;
    }
    const bool wider = max_x > sl->width && max_x <= kMaxDim;
    // Card S3-17c: a scene TALLER than the image (CAS portrait: 736x1280 in four 736x320 tiles; the surface declares
    // one 320-row tile and a declared size never grows in Ensure) is recreated at the scene height too, as the M2c7
    // widen does for side-by-side tiles. Without it the scene is clipped to the first tile and every tile resolve copies
    // that tile (s317b-c: CP copies at four bases, D3D source image 800x320).
    const bool taller = max_y > sl->height && max_y <= kMaxDim;
    if ((wider || taller) && TileResizeFix()) {
      // Card S3-65: release the old image first so the slot stays the only owner of its image and of its counted bytes
      // (before: the handle was dropped without a release, one orphaned image per widen that EvictIdle can never reach).
      const uint32_t keep_h = sl->height;  // ReleaseSlotImage zeroes it
      ReleaseSlotImage(sl);
      if (sl->handle != nb::kNone) {  // backend refused: keep the old image, do not orphan it; the next BeginTiling retries
        ++g_stats.rt_tile_resize_refused;
        continue;
      }
      if (wider) {
        sl->width = (max_x + 7) & ~7u;
        ++g_stats.rt_tile_widen;
      }
      if (taller) ++g_stats.rt_tile_taller;
      if (!EnsureHeight(s, sl, std::max<uint32_t>(keep_h, max_y), &why)) CountFail(why);
    } else if (wider || taller) {
      // WWE2K14_D3D_TILE_RESIZE=0: pre-S3-65 path (the old image stays allocated and counted).
      if (wider) {
        sl->width = (max_x + 7) & ~7u;
        ++g_stats.rt_tile_widen;
      }
      if (taller) ++g_stats.rt_tile_taller;
      sl->handle = nb::kNone;
      if (!EnsureHeight(s, sl, std::max<uint32_t>(sl->height, max_y), &why)) CountFail(why);
    }
  }
  if (flags & 1) return;  // D3DTILING_SKIP_FIRST_CLEAR
  // The library clears with ClearF(flags 63: all targets + Z + stencil, rect {0,0,maxW,maxH}, colour float4*, Z, stencil).
  // Per-tile widths/heights equal the scene here because the image is drawn once at full size (plan WP6).
  const float z = BitsToFloat(e.args[7]);
  float rgba[4] = {0, 0, 0, 0};
  bool have_color = false;
  if (color_be) {
    for (int i = 0; i < 4; ++i) rgba[i] = BitsToFloat(Be32(color_be + 4 * i));
    have_color = true;
  } else {
    ++g_stats.tiling_clear_color_unread;
  }
  ApplyClear(s, have_color, rgba, true, z, true, e.args[5], {}, max_y);
}

void OnEndTiling(const EvHeader&) {
  ++g_stats.tiling_end;
  g_tiling = false;  // the per-tile Resolves that follow (or precede, depending on capture order) merge by epoch
  if (TileEpochOn()) g_tail = true;  // card S3-66: the colour tile resolves of EndTiling's body follow this event
}

void OnClear(const EvHeader& e, const DeviceState& s) {
  std::vector<uint8_t> rects;
  if (e.args[0] && e.args[1]) {
    if (g_reader) {
      rects.resize(size_t(std::min(e.args[0], 64u)) * 16);
      if (!g_reader(e.args[1], rects.data(), uint32_t(rects.size()))) rects.clear();
    }
    if (rects.empty()) ++g_stats.clear_rects_unread;
  }
  OnClearData(e, s, rects.empty() ? nullptr : rects.data());
}

void OnClearData(const EvHeader& e, const DeviceState& s, const uint8_t* rects_be) {
  g_tail = false;  // card S3-66
  FlushDeferredClears();
  MaybeLog();
  // Clear(dev, r4 count, r5 pRects, r6 flags, r7 D3DCOLOR, f1 Z, r9 stencil): args[0..] = r4.. , args[7] = f1 bits.
  const uint32_t count = e.args[0], flags = e.args[2], d3dcolor = e.args[3];
  if (flags & (kClearTargets & ~kClearTarget0)) ++g_stats.clears_mrt_skipped;
  const bool color = (flags & kClearTarget0) != 0, depth = (flags & kClearDepth) != 0,
             stencil = (flags & kClearStencil) != 0;
  if (!color && !depth && !stencil) return;
  float rgba[4] = {float((d3dcolor >> 16) & 0xFF) / 255.0f, float((d3dcolor >> 8) & 0xFF) / 255.0f,
                   float(d3dcolor & 0xFF) / 255.0f, float((d3dcolor >> 24) & 0xFF) / 255.0f};
  // count == 0 (or no readable list while count != 0, counted by OnClear) = whole image.
  std::vector<Rect> rects = count ? ParseRects(rects_be, count) : std::vector<Rect>{};
  // Card V1 round 2: count != 0 without a readable list is cleared whole (as before) but never counts as coverage.
  const bool unread = count && !rects_be;
  ApplyClear(s, color, rgba, depth, BitsToFloat(e.args[7]), stencil, e.args[5], std::move(rects), 0, unread);
}

// Card V1: frame boundary (worker, kSwap, mode 2, backend lock held).
void OnFrameEnd() {
  ++g_present;
  EvictDead();
  if (g_rt_frame_evicted) {
    REXLOG_INFO("[d3d-shadow] rt-real-cap frame={} evicted={} bytes={} MB live={} MB cap={} MB scale={:.3f}", g_present,
                g_rt_frame_evicted, g_rt_frame_evicted_bytes >> 20, g_rt_real_bytes >> 20, RtCapBytes() >> 20,
                double(RtCapScaleMilli()) / 1000.0);
    g_rt_frame_evicted = 0;
    g_rt_frame_evicted_bytes = 0;
  }
}

// Card V1 CHECK: a resolve reads rect (x, y, w, h) of `src`. For an image flagged as dead, decide whether eviction
// would have been visible: RGBA8 colour = read the rect back and count sentinel pixels (pixels nobody rewrote since the
// flag, i.e. pixels eviction would have lost); other formats: definite if nothing drew or fully cleared it since the
// flag, otherwise unverified. One verdict per flag; at most 256 readbacks per run (then unverified).
void NoteResolveRead(const Source& src, int32_t x, int32_t y, uint32_t w, uint32_t h) {
  if (!CheckMode() || TranslateOnly()) return;
  auto it = g_by_handle.find(src.handle);
  if (it == g_by_handle.end() || it->second->handle != src.handle || !it->second->check_flag) return;
  Slot& sl = *it->second;
  sl.check_flag = false;
  static uint32_t reads = 0, logged = 0;
  uint64_t stale = 0;
  bool verified = false;
  if (sl.check_sentinel && w && h && reads < 256) {
    ++reads;
    std::string err;
    const uint32_t id = nb::SnapshotColor(sl.handle, uint32_t(std::max(x, 0)), uint32_t(std::max(y, 0)), w, h, &err);
    std::vector<uint8_t> px;
    if (id != nb::kNone && nb::ReadSnapshot(id, w, h, px, &err) && px.size() >= size_t(w) * h * 4) {
      for (size_t i = 0; i + 3 < px.size(); i += 4)
        stale += px[i] == 1 && px[i + 1] == 254 && px[i + 2] == 1 && px[i + 3] == 1;
      verified = true;
    } else {
      ++g_ev.read_fail;
    }
    if (id != nb::kNone) nb::ReleaseSnapshot(id);
  }
  const char* verdict;
  if (verified) {
    verdict = stale ? "STALE" : "clean";
    ++(stale ? g_ev.read_stale : g_ev.read_clean);
  } else if (!sl.check_drawn) {
    verdict = "DEFINITE";  // read with no draw and no full clear since the flag: eviction would change what it copies
    ++g_ev.read_definite;
  } else {
    verdict = "unverified";
    ++g_ev.read_unverified;
  }
  if ((stale || !verified) && logged < 64) {
    ++logged;
    REXLOG_WARN("[d3d-shadow] rt-evict-check {} obj={:08X} info={:08X}/{:08X} {} {}x{} rect={},{} {}x{} stale_px={} "
                "drawn={} flagged_at={} now={}",
                verdict, sl.key.object, sl.key.info0, sl.key.info1, sl.depth ? "depth" : "colour", sl.width, sl.height, x,
                y, w, h, stale, sl.check_drawn ? 1 : 0, sl.check_flag_present, g_present);
  }
}

void LogStats(const char* why) {
  const Stats& t = g_stats;
  // Card S3-65 bounded diagnostic (remove when the card closes): bytes held by slots vs the counted total.
  uint64_t live_sum = 0, flag_pending = 0;
  for (const auto& kv : g_slots) {
    live_sum += kv.second.bytes;
    flag_pending += kv.second.check_flag;  // card V1 CHECK: flagged, not read or fully cleared since
  }
  const int64_t gap = int64_t(t.rt_bytes) - int64_t(live_sum);
  REXLOG_INFO(
      "[d3d-shadow] rt ({}) binds={} fail={} mrt_draws={} setrt={} images={} created={} bytes={}MB grow={} lost={} "
      "fallback_h={} capfail={} obj_read={} info_match={} mismatch={} extent={} clear_c={} clear_d={} clear_mrt_skip={} "
      "clear_rects_unread={} tiling={}/{} tile_widen={} tile_taller={} tile_clear_color_unread={} tile_extent_unread={} "
      "live_sum={}MB gap={} resize_refused={} depth_cover={}/{}/{} fb_mismatch={}",
      why, t.binds, t.bind_fail, t.mrt_draws, t.setrt, g_slots.size(), t.rt_created, t.rt_bytes >> 20, t.rt_grow,
      t.rt_grow_lost, t.rt_height_fallback, t.rt_cap_fail, t.obj_read, t.obj_info_match, t.obj_info_mismatch,
      t.obj_extent, t.clears_color, t.clears_depth, t.clears_mrt_skipped, t.clear_rects_unread, t.tiling_begin,
      t.tiling_end, t.rt_tile_widen, t.rt_tile_taller, t.tiling_clear_color_unread, t.tiling_extent_unread,
      live_sum >> 20, gap, t.rt_tile_resize_refused, t.depth_cover_create, t.depth_cover_regrow, t.depth_cover_refused,
      t.fb_mismatch);
  if (ShadowMode() == 2) REXLOG_INFO("[d3d-shadow] heap ({}) {}", why, nb::HeapBudgets());
  // Issue plan step 5: what fills graphics memory (rare: new 256 MB highs only, at most 16 lines).
  if (std::string kinds = nb::MemKindsOnGrowth(); !kinds.empty()) REXLOG_INFO("[nr-mem-kinds] ({}) {}", why, kinds);
  // Card V1: eviction / CHECK counters (mismatches = stale + definite; expected 0).
  REXLOG_INFO("[d3d-shadow] rt-evict ({}) mode={} present={} passes={} pressure={} heap0={}/{}MB rt_dev={}MB idle={} "
              "uncovered={} reachable={} host_skip={} evicted={} ({} MB) refused={} stamps={} skip_unknown={} skip_tiling={} "
              "skip_unread={} skip_partial={} check: flagged={} ({} MB) pending={} safe_clear={} draw_first={} clean={} "
              "stale={} definite={} unverified={} read_fail={} mismatches={} unresolved={}",
              why, Rule().name, g_present, g_ev.passes, g_ev.pressure, g_ev.last_usage >> 20, g_ev.last_budget >> 20,
              g_ev.last_rt_dev >> 20, g_ev.idle, g_ev.uncovered, g_ev.reachable, g_ev.host_skip, g_ev.evicted, g_ev.evicted_bytes >> 20,
              g_ev.refused, g_ev.stamps, g_ev.stamp_skip_unknown, g_ev.stamp_skip_tiling, g_ev.stamp_skip_unread,
              g_ev.stamp_skip_partial, g_ev.flagged, g_ev.flagged_bytes >> 20, flag_pending, g_ev.safe_clear,
              g_ev.draw_first, g_ev.read_clean, g_ev.read_stale, g_ev.read_definite, g_ev.read_unverified,
              g_ev.read_fail, g_ev.read_stale + g_ev.read_definite, g_ev.read_unverified + g_ev.read_fail);
  for (const auto& kv : g_fail_reasons) REXLOG_INFO("[d3d-shadow] rt fail x{}: {}", kv.second, kv.first);
}

}  // namespace wwe2k14::d3d::targets
