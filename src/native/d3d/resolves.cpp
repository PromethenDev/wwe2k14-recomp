// WP6 (decision 0006 stage 2): see resolves.h. Written for this project from the plan (WP6 section); SVR code unused.
// No guest memory is written; no EDRAM. Resolve classes follow what the nr copy-out path supports (nr_frontend CopyColor /
// CopyDepthSurface): RGBA8 (+ k_8 from RGBA8), fixed16 (k_16_16 / k_16_16_16_16 from the signed-normalised targets),
// depth to R32F. Everything else is counted by name ("resolve class unsupported") and gets a card if it ever occurs.
#include "resolves.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>

#include "native/nr/nr_backend.h"
#include "native/nr/nr_shared.h"
#include "capture.h"
#include "targets.h"
#include "textures.h"

namespace wwe2k14::d3d::resolves {
namespace {

namespace nb = nr::backend;
namespace xenos = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;

constexpr size_t kRing = 128;

struct Entry {
  ResolveRecord rec{};
  ResolveExtra ex{};
  bool used = false;
};
Entry g_ring[kRing];
uint64_t g_count = 0;

// Image currently registered for one destination base (predicated-tiling merge).
struct Group {
  uint32_t epoch = 0, handle = nb::kNone, pitch = 0, info = 0;
  Class cls = Class::kNone;
  std::vector<std::array<int32_t, 4>> rects;
  int32_t uni[4] = {0, 0, 0, 0};
};
std::map<uint32_t, Group> g_groups;

// Mode 1 (TranslateOnly): every nb::Snapshot* / RetainSnapshot / ReadColor / *SnapshotAvailable call is replaced by a
// local fake id (never nb::kNone, never released), so classification, grouping, merging and registration run as in mode 2.
// The *Available() probes are assumed true (init-time constants of the backend; reading them would touch `g`).
uint32_t g_fake_snap = 0x7F000000u;
uint32_t FakeSnap() { return ++g_fake_snap; }

// Card S3-27 (WWE2K14_D3D_TILE_DEDUPE=1): the per-tile resolves of a predicated-tiling scene all copy the whole scene
// (card M2c7: the scene is drawn once at full size), so tiles 2..N re-copy identical pixels. Reuse the snapshot of an
// identical earlier resolve (same tiling epoch, source handle, rect, class and conversion key) when no draw and no clear
// touched any target since (targets::WriteSeq) - same image, a second id (RetainSnapshot), no copy.
struct TileDup {
  bool valid = false;
  uint32_t epoch = 0, handle = 0, key = 0, snap = nb::kNone;
  int32_t rect[4] = {};
  Class cls = Class::kNone;
  uint64_t seq = 0, used = 0;
};
TileDup g_tdup[8];
uint64_t g_tdup_clock = 0, g_tdup_reused = 0, g_tdup_made = 0, g_tdup_cols = 0;
bool TileDedupeOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_TILE_DEDUPE");
    return e && std::strcmp(e, "1") == 0;
  }();
  return on;
}
// Card S3-66 (WWE2K14_D3D_TILE_EPOCH=1): bounded diagnostic + SV-0b runtime answer, printed only with the switch on
// (remove when the card closes). Per epoch: resolves inside the bracket / in the tail after EndTiling, distinct
// destinations of each, and writes (draw bind / applied clear = a possible reader) between resolves of one epoch.
struct EpochCensus {
  uint32_t epoch = 0, in = 0, tail = 0, n_in = 0, n_tail = 0;
  uint32_t dests_in[8] = {}, dests_tail[8] = {};
  uint64_t seq = 0;
};
EpochCensus g_ec;
uint64_t g_ec_epochs = 0, g_ec_in = 0, g_ec_tail = 0, g_ec_dests_in = 0, g_ec_dests_tail = 0, g_ec_dests_over = 0,
         g_ec_seq_breaks = 0, g_ec_widened = 0, g_ec_widen_pitch_skip = 0, g_ec_made = 0, g_ec_reused = 0, g_ec_tail_eps = 0, g_ec_in_max = 0,
         g_ec_tail_max = 0;
void EpochFold() {
  if (!g_ec.epoch) return;
  ++g_ec_epochs;
  g_ec_in += g_ec.in;
  g_ec_tail += g_ec.tail;
  g_ec_dests_in += g_ec.n_in;
  g_ec_dests_tail += g_ec.n_tail;
  if (g_ec.tail) ++g_ec_tail_eps;
  g_ec_in_max = std::max<uint64_t>(g_ec_in_max, g_ec.in);
  g_ec_tail_max = std::max<uint64_t>(g_ec_tail_max, g_ec.tail);
  g_ec = EpochCensus{};
}
void EpochNote(uint32_t epoch, bool tail, uint32_t dest, uint64_t seq) {
  if (epoch != g_ec.epoch) {
    EpochFold();
    g_ec.epoch = epoch;
    g_ec.seq = seq;
  } else if (seq != g_ec.seq) {
    ++g_ec_seq_breaks;
    g_ec.seq = seq;
  }
  uint32_t* d = tail ? g_ec.dests_tail : g_ec.dests_in;
  uint32_t& n = tail ? g_ec.n_tail : g_ec.n_in;
  ++(tail ? g_ec.tail : g_ec.in);
  for (uint32_t i = 0; i < n; ++i) {
    if (d[i] == dest) return;
  }
  if (n < 8) d[n++] = dest; else ++g_ec_dests_over;
}

template <class Make>
uint32_t TileDedupe(bool eligible, uint32_t epoch, uint32_t handle, const int32_t rect[4], Class cls, uint32_t key, Make make) {
  if (!eligible || !TileDedupeOn() || TranslateOnly()) return make();  // S3-66 review F3: TILE_EPOCH needs TILE_DEDUPE=1
  const uint64_t seq = targets::WriteSeq();
  for (TileDup& d : g_tdup) {
    if (d.valid && d.epoch != epoch) {  // an older scene: drop its hold
      nb::ReleaseSnapshot(d.snap);
      d = TileDup{};
    }
  }
  for (TileDup& d : g_tdup) {
    if (d.valid && d.handle == handle && d.cls == cls && d.key == key && d.seq == seq &&
        std::memcmp(d.rect, rect, sizeof(d.rect)) == 0) {
      const uint32_t id = nb::RetainSnapshot(d.snap);
      if (id != nb::kNone) {
        d.used = ++g_tdup_clock;
        ++g_tdup_reused;
        return id;
      }
    }
  }
  const uint32_t id = make();
  if (id == nb::kNone) return id;
  ++g_tdup_made;
  TileDup* slot = &g_tdup[0];
  for (TileDup& d : g_tdup) {
    if (!d.valid) { slot = &d; break; }
    if (d.used < slot->used) slot = &d;
  }
  if (slot->valid) nb::ReleaseSnapshot(slot->snap);
  const uint32_t held = nb::RetainSnapshot(id);
  *slot = TileDup{};
  if (held != nb::kNone) {
    slot->valid = true;
    slot->epoch = epoch;
    slot->handle = handle;
    slot->key = key;
    slot->snap = held;
    std::memcpy(slot->rect, rect, sizeof(slot->rect));
    slot->cls = cls;
    slot->seq = seq;
    slot->used = ++g_tdup_clock;
  }
  return id;
}

struct St {
  uint64_t resolves = 0, by_class[9] = {}, merged = 0, registered = 0, hashed = 0, hash_reads_failed = 0;
  uint64_t whole_declared = 0, whole_scene = 0, whole_clamped_dest = 0, tile_clear_kept = 0, empty_rect = 0, empty_noop = 0, empty_with_clear = 0, no_source = 0, clears_color = 0, clears_depth = 0,
           array_dest = 0, retain_fail = 0;
  uint64_t rect_grown_clamped = 0;
  uint64_t dest_point_differs = 0;  // card S3-17b (counted only, see OnResolve)
  std::map<std::string, uint64_t> unsupported;
} g_st;

// C1-N4: opt-in timing only. The normal path does not read clocks or format these counters.
enum class FineStage : uint8_t { kEvent, kSource, kSnapshot, kRegister, kReadbackHash, kClear, kCount };
enum class FineRegistrationPart : uint8_t { kGroupMerge, kGroupExtent, kRetainSnapshot, kTextureRegister, kCount };
constexpr uint64_t kFineWindowResolves = 512;
struct FineWindow {
  uint64_t events = 0;
  uint64_t by_class[9] = {};
  uint64_t wall_ns[uint32_t(FineStage::kCount)] = {};
  uint64_t cpu_ns[uint32_t(FineStage::kCount)] = {};
  uint64_t wall_max_ns[uint32_t(FineStage::kCount)] = {};
  uint64_t cpu_max_ns[uint32_t(FineStage::kCount)] = {};
  uint64_t calls[uint32_t(FineStage::kCount)] = {};
  uint64_t registration_wall_ns[uint32_t(FineRegistrationPart::kCount)] = {};
  uint64_t registration_cpu_ns[uint32_t(FineRegistrationPart::kCount)] = {};
  uint64_t registration_calls[uint32_t(FineRegistrationPart::kCount)] = {};
  uint32_t groups_peak = 0, group_rects_peak = 0;
} g_fine;

bool FineTimersOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_RESOLVE_FINE_TIMERS");
    return e && std::strcmp(e, "1") == 0;
  }();
  return on;
}

uint64_t FineWallNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t FineThreadCpuNowNs() {
#ifdef _WIN32
  FILETIME create, exit, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user)) return 0;
  auto to100 = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return (to100(kernel) + to100(user)) * 100;
#else
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

void FineAdd(FineStage stage, uint64_t wall_start, uint64_t cpu_start) {
  const uint32_t i = uint32_t(stage);
  const uint64_t wall_elapsed = FineWallNowNs() - wall_start;
  g_fine.wall_ns[i] += wall_elapsed;
  g_fine.wall_max_ns[i] = std::max(g_fine.wall_max_ns[i], wall_elapsed);
  const uint64_t cpu_end = FineThreadCpuNowNs();
  if (cpu_start && cpu_end >= cpu_start) {
    const uint64_t cpu_elapsed = cpu_end - cpu_start;
    g_fine.cpu_ns[i] += cpu_elapsed;
    g_fine.cpu_max_ns[i] = std::max(g_fine.cpu_max_ns[i], cpu_elapsed);
  }
  ++g_fine.calls[i];
}

class FineRegistrationPartScope {
 public:
  explicit FineRegistrationPartScope(FineRegistrationPart part) : part_(part), active_(FineTimersOn()) {
    if (active_) {
      wall_start_ = FineWallNowNs();
      cpu_start_ = FineThreadCpuNowNs();
    }
  }
  ~FineRegistrationPartScope() {
    if (!active_) return;
    const uint32_t i = uint32_t(part_);
    ++g_fine.registration_calls[i];
    g_fine.registration_wall_ns[i] += FineWallNowNs() - wall_start_;
    const uint64_t cpu_end = FineThreadCpuNowNs();
    if (cpu_start_ && cpu_end >= cpu_start_) g_fine.registration_cpu_ns[i] += cpu_end - cpu_start_;
  }
  FineRegistrationPartScope(const FineRegistrationPartScope&) = delete;
  FineRegistrationPartScope& operator=(const FineRegistrationPartScope&) = delete;

 private:
  FineRegistrationPart part_;
  bool active_;
  uint64_t wall_start_ = 0, cpu_start_ = 0;
};

void FineLogAndReset() {
  const uint64_t n = g_fine.events;
  if (!n) return;
  const textures::ResolveRegistrationFineStats texture_reg = textures::TakeResolveRegistrationFineStats();
  uint64_t sub_wall = 0, sub_cpu = 0;
  for (uint32_t i = uint32_t(FineStage::kSource); i < uint32_t(FineStage::kCount); ++i) {
    sub_wall += g_fine.wall_ns[i];
    sub_cpu += g_fine.cpu_ns[i];
  }
  const uint64_t residual_wall = g_fine.wall_ns[uint32_t(FineStage::kEvent)] > sub_wall
                                     ? g_fine.wall_ns[uint32_t(FineStage::kEvent)] - sub_wall : 0;
  const uint64_t residual_cpu = g_fine.cpu_ns[uint32_t(FineStage::kEvent)] > sub_cpu
                                    ? g_fine.cpu_ns[uint32_t(FineStage::kEvent)] - sub_cpu : 0;
  const double denom = double(n) * 1000.0;  // ns -> us per resolve
  const double tex_denom = double(n) * 1000.0;
  REXLOG_INFO("[d3d-resolve-fine] n={} wall_us/resolve={:.2f} cpu_us/resolve={:.2f} | "
              "wall_us source={:.2f} snapshot_queue={:.2f} registration={:.2f} readback_hash={:.2f} clear={:.2f} residual={:.2f} | "
              "cpu_us source={:.2f} snapshot_queue={:.2f} registration={:.2f} readback_hash={:.2f} clear={:.2f} residual={:.2f} | "
              "max_wall_us total={:.2f} source={:.2f} snapshot_queue={:.2f} registration={:.2f} readback_hash={:.2f} clear={:.2f} | "
              "max_cpu_us total={:.2f} source={:.2f} snapshot_queue={:.2f} registration={:.2f} readback_hash={:.2f} clear={:.2f} | "
              "calls source={} snapshot_queue={} registration={} readback_hash={} clear={} | "
              "class rgba8={} k8={} fixed16={} depth_r32f={} clear_only={} unsupported={} rgba4={} noop={} | "
              "reg_parts_wall_us group_merge={:.2f} group_extent={:.2f} retain={:.2f} texture={:.2f} "
              "reg_parts_cpu_us group_merge={:.2f} group_extent={:.2f} retain={:.2f} texture={:.2f} | "
              "reg_part_calls group_merge={} group_extent={} retain={} texture={} | "
              "texture_wall_us page_mark={:.2f} seed_invalidate={:.2f} resolve_metadata={:.2f} hide_old={:.2f} "
              "coverage={:.2f} overlap_lru={:.2f} memo_forget={:.2f} watch_seed={:.2f} trace={:.2f} | "
              "texture_cpu_us page_mark={:.2f} seed_invalidate={:.2f} resolve_metadata={:.2f} hide_old={:.2f} "
              "coverage={:.2f} overlap_lru={:.2f} memo_forget={:.2f} watch_seed={:.2f} trace={:.2f} | "
              "texture_calls page_mark={} seed_invalidate={} resolve_metadata={} hide_old={} coverage={} overlap_lru={} "
              "memo_forget={} watch_seed={} trace={} | "
              "sizes resolves={} fmemo={} seed={} coverage_cells={} pages_marked={} memo_scanned={} groups={} group_rects={}",
              n, g_fine.wall_ns[uint32_t(FineStage::kEvent)] / denom,
              g_fine.cpu_ns[uint32_t(FineStage::kEvent)] / denom,
              g_fine.wall_ns[uint32_t(FineStage::kSource)] / denom,
              g_fine.wall_ns[uint32_t(FineStage::kSnapshot)] / denom,
              g_fine.wall_ns[uint32_t(FineStage::kRegister)] / denom,
              g_fine.wall_ns[uint32_t(FineStage::kReadbackHash)] / denom,
              g_fine.wall_ns[uint32_t(FineStage::kClear)] / denom, residual_wall / denom,
              g_fine.cpu_ns[uint32_t(FineStage::kSource)] / denom,
              g_fine.cpu_ns[uint32_t(FineStage::kSnapshot)] / denom,
              g_fine.cpu_ns[uint32_t(FineStage::kRegister)] / denom,
              g_fine.cpu_ns[uint32_t(FineStage::kReadbackHash)] / denom,
              g_fine.cpu_ns[uint32_t(FineStage::kClear)] / denom, residual_cpu / denom,
              g_fine.wall_max_ns[uint32_t(FineStage::kEvent)] / 1000.0,
              g_fine.wall_max_ns[uint32_t(FineStage::kSource)] / 1000.0,
              g_fine.wall_max_ns[uint32_t(FineStage::kSnapshot)] / 1000.0,
              g_fine.wall_max_ns[uint32_t(FineStage::kRegister)] / 1000.0,
              g_fine.wall_max_ns[uint32_t(FineStage::kReadbackHash)] / 1000.0,
              g_fine.wall_max_ns[uint32_t(FineStage::kClear)] / 1000.0,
              g_fine.cpu_max_ns[uint32_t(FineStage::kEvent)] / 1000.0,
              g_fine.cpu_max_ns[uint32_t(FineStage::kSource)] / 1000.0,
              g_fine.cpu_max_ns[uint32_t(FineStage::kSnapshot)] / 1000.0,
              g_fine.cpu_max_ns[uint32_t(FineStage::kRegister)] / 1000.0,
              g_fine.cpu_max_ns[uint32_t(FineStage::kReadbackHash)] / 1000.0,
              g_fine.cpu_max_ns[uint32_t(FineStage::kClear)] / 1000.0,
              g_fine.calls[uint32_t(FineStage::kSource)], g_fine.calls[uint32_t(FineStage::kSnapshot)],
              g_fine.calls[uint32_t(FineStage::kRegister)], g_fine.calls[uint32_t(FineStage::kReadbackHash)],
              g_fine.calls[uint32_t(FineStage::kClear)],
              g_fine.by_class[uint32_t(Class::kRGBA8)], g_fine.by_class[uint32_t(Class::kK8)],
              g_fine.by_class[uint32_t(Class::kFixed16)], g_fine.by_class[uint32_t(Class::kDepthR32F)],
              g_fine.by_class[uint32_t(Class::kClearOnly)], g_fine.by_class[uint32_t(Class::kUnsupported)],
              g_fine.by_class[uint32_t(Class::kRGBA4)], g_fine.by_class[uint32_t(Class::kNoop)],
              g_fine.registration_wall_ns[uint32_t(FineRegistrationPart::kGroupMerge)] / denom,
              g_fine.registration_wall_ns[uint32_t(FineRegistrationPart::kGroupExtent)] / denom,
              g_fine.registration_wall_ns[uint32_t(FineRegistrationPart::kRetainSnapshot)] / denom,
              g_fine.registration_wall_ns[uint32_t(FineRegistrationPart::kTextureRegister)] / denom,
              g_fine.registration_cpu_ns[uint32_t(FineRegistrationPart::kGroupMerge)] / denom,
              g_fine.registration_cpu_ns[uint32_t(FineRegistrationPart::kGroupExtent)] / denom,
              g_fine.registration_cpu_ns[uint32_t(FineRegistrationPart::kRetainSnapshot)] / denom,
              g_fine.registration_cpu_ns[uint32_t(FineRegistrationPart::kTextureRegister)] / denom,
              g_fine.registration_calls[uint32_t(FineRegistrationPart::kGroupMerge)],
              g_fine.registration_calls[uint32_t(FineRegistrationPart::kGroupExtent)],
              g_fine.registration_calls[uint32_t(FineRegistrationPart::kRetainSnapshot)],
              g_fine.registration_calls[uint32_t(FineRegistrationPart::kTextureRegister)],
              texture_reg.wall_ns[uint32_t(textures::ResolveRegistrationFineStage::kPageMark)] / tex_denom,
              texture_reg.wall_ns[uint32_t(textures::ResolveRegistrationFineStage::kSeedInvalidate)] / tex_denom,
              texture_reg.wall_ns[uint32_t(textures::ResolveRegistrationFineStage::kResolveMetadata)] / tex_denom,
              texture_reg.wall_ns[uint32_t(textures::ResolveRegistrationFineStage::kHideOldScan)] / tex_denom,
              texture_reg.wall_ns[uint32_t(textures::ResolveRegistrationFineStage::kCoverage)] / tex_denom,
              texture_reg.wall_ns[uint32_t(textures::ResolveRegistrationFineStage::kOverlapLru)] / tex_denom,
              texture_reg.wall_ns[uint32_t(textures::ResolveRegistrationFineStage::kMemoForget)] / tex_denom,
              texture_reg.wall_ns[uint32_t(textures::ResolveRegistrationFineStage::kWatchSeed)] / tex_denom,
              texture_reg.wall_ns[uint32_t(textures::ResolveRegistrationFineStage::kTrace)] / tex_denom,
              texture_reg.cpu_ns[uint32_t(textures::ResolveRegistrationFineStage::kPageMark)] / tex_denom,
              texture_reg.cpu_ns[uint32_t(textures::ResolveRegistrationFineStage::kSeedInvalidate)] / tex_denom,
              texture_reg.cpu_ns[uint32_t(textures::ResolveRegistrationFineStage::kResolveMetadata)] / tex_denom,
              texture_reg.cpu_ns[uint32_t(textures::ResolveRegistrationFineStage::kHideOldScan)] / tex_denom,
              texture_reg.cpu_ns[uint32_t(textures::ResolveRegistrationFineStage::kCoverage)] / tex_denom,
              texture_reg.cpu_ns[uint32_t(textures::ResolveRegistrationFineStage::kOverlapLru)] / tex_denom,
              texture_reg.cpu_ns[uint32_t(textures::ResolveRegistrationFineStage::kMemoForget)] / tex_denom,
              texture_reg.cpu_ns[uint32_t(textures::ResolveRegistrationFineStage::kWatchSeed)] / tex_denom,
              texture_reg.cpu_ns[uint32_t(textures::ResolveRegistrationFineStage::kTrace)] / tex_denom,
              texture_reg.calls[uint32_t(textures::ResolveRegistrationFineStage::kPageMark)],
              texture_reg.calls[uint32_t(textures::ResolveRegistrationFineStage::kSeedInvalidate)],
              texture_reg.calls[uint32_t(textures::ResolveRegistrationFineStage::kResolveMetadata)],
              texture_reg.calls[uint32_t(textures::ResolveRegistrationFineStage::kHideOldScan)],
              texture_reg.calls[uint32_t(textures::ResolveRegistrationFineStage::kCoverage)],
              texture_reg.calls[uint32_t(textures::ResolveRegistrationFineStage::kOverlapLru)],
              texture_reg.calls[uint32_t(textures::ResolveRegistrationFineStage::kMemoForget)],
              texture_reg.calls[uint32_t(textures::ResolveRegistrationFineStage::kWatchSeed)],
              texture_reg.calls[uint32_t(textures::ResolveRegistrationFineStage::kTrace)],
              texture_reg.max_resolve_entries, texture_reg.max_frame_memo_entries, texture_reg.max_seed_canvases,
              texture_reg.max_coverage_cells, texture_reg.pages_marked, texture_reg.memo_entries_scanned,
              g_fine.groups_peak, g_fine.group_rects_peak);
  g_fine = FineWindow{};
}

class FineStageScope {
 public:
  explicit FineStageScope(FineStage stage) : stage_(stage), active_(FineTimersOn()) {
    if (active_) {
      wall_start_ = FineWallNowNs();
      cpu_start_ = FineThreadCpuNowNs();
    }
  }
  ~FineStageScope() { if (active_) FineAdd(stage_, wall_start_, cpu_start_); }
  FineStageScope(const FineStageScope&) = delete;
  FineStageScope& operator=(const FineStageScope&) = delete;

 private:
  FineStage stage_;
  bool active_;
  uint64_t wall_start_ = 0, cpu_start_ = 0;
};

class FineEventScope {
 public:
  explicit FineEventScope(const ResolveExtra& extra) : extra_(extra), active_(FineTimersOn()) {
    if (active_) {
      wall_start_ = FineWallNowNs();
      cpu_start_ = FineThreadCpuNowNs();
    }
  }
  ~FineEventScope() {
    if (!active_) return;
    FineAdd(FineStage::kEvent, wall_start_, cpu_start_);
    const uint32_t cls = uint32_t(extra_.cls);
    if (cls < 9) ++g_fine.by_class[cls];
    if (++g_fine.events == kFineWindowResolves) FineLogAndReset();
  }
  FineEventScope(const FineEventScope&) = delete;
  FineEventScope& operator=(const FineEventScope&) = delete;

 private:
  const ResolveExtra& extra_;
  bool active_;
  uint64_t wall_start_ = 0, cpu_start_ = 0;
};

uint32_t EnvU32(const char* name, uint32_t def) {
  const char* v = std::getenv(name);
  return v && *v ? uint32_t(std::strtoul(v, nullptr, 10)) : def;
}

void Unsupported(const std::string& what) {
  if (++g_st.unsupported[what] == 1) REXLOG_WARN("[d3d-shadow] resolve class unsupported: {}", what);
}

uint32_t Align8(uint32_t v) { return (v + 7u) & ~7u; }

bool Disjoint(const std::array<int32_t, 4>& a, const int32_t b[4]) {
  return a[2] <= b[0] || b[2] <= a[0] || a[3] <= b[1] || b[3] <= a[1];
}

const char* ClassName(Class c) {
  switch (c) {
    case Class::kNone: return "none";
    case Class::kRGBA8: return "rgba8";
    case Class::kK8: return "k8";
    case Class::kFixed16: return "fixed16";
    case Class::kDepthR32F: return "depth_r32f";
    case Class::kClearOnly: return "clear_only";
    case Class::kUnsupported: return "unsupported";
    case Class::kRGBA4: return "rgba4";
    case Class::kNoop: return "noop";
  }
  return "?";
}

// SDK-style destination extent (2D, not an array texture). bpp_log2 of the guest destination texel.
void DestExtent(const int32_t r[4], uint32_t pitch, uint32_t bpp_log2, uint32_t* start, uint32_t* length) {
  const uint32_t lo = tu::GetTiledAddressLowerBound2D(uint32_t(r[0]), uint32_t(r[1]), pitch, bpp_log2);
  const uint32_t hi = tu::GetTiledAddressUpperBound2D(uint32_t(r[2]), uint32_t(r[3]), pitch, bpp_log2);
  *start = lo;
  *length = hi > lo ? hi - lo : 0;
}

void MaybeLog() {
  static const uint32_t every = EnvU32("WWE2K14_D3D_SHADOW_LOG_EVERY", 2000);
  if (every && g_st.resolves % every == 0) LogStats("periodic");
}

}  // namespace

uint64_t HashRGBA8(const uint8_t* rgba, uint32_t w, uint32_t h) {
  return XXH3_64bits_withSeed(rgba, size_t(w) * h * 4, (uint64_t(w) << 32) | h);
}

const ResolveRecord* Last() {
  if (!g_count) return nullptr;
  return &g_ring[(g_count - 1) % kRing].rec;
}
const ResolveExtra* LastExtra() {
  if (!g_count) return nullptr;
  return &g_ring[(g_count - 1) % kRing].ex;
}
const ResolveRecord* FindByPacket(uint32_t packet_addr, const ResolveExtra** extra) {
  const uint64_t n = std::min<uint64_t>(g_count, kRing);
  for (uint64_t i = 0; i < n; ++i) {
    const Entry& e = g_ring[(g_count - 1 - i) % kRing];
    if (e.rec.packet_addr == packet_addr) {
      if (extra) *extra = &e.ex;
      return &e.rec;
    }
  }
  return nullptr;
}

// Card S3-44 diagnostic (WWE2K14_NR_FLASH_TRACE=1): compact list of this frame's resolves, taken at each Swap.
std::string g_frame_resolves;
std::string TakeFrameResolves() {
  std::string v;
  v.swap(g_frame_resolves);
  return v;
}
void NoteFrameResolve(const ResolveExtra& ex, uint32_t snapshot) {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_NR_FLASH_TRACE");
    return v && *v == '1';
  }();
  if (!on) return;
  char b[96];
  std::snprintf(b, sizeof(b), " %X:%ux%u:c%d:s%d%s%s", ex.dest_base, ex.width, ex.height, int(ex.cls),
                snapshot == nb::kNone ? -1 : int(snapshot), ex.merged ? ":m" : "", ex.flushed_clear ? ":pc" : "");
  if (g_frame_resolves.size() < 4000) g_frame_resolves += b;
}

void OnResolve(const EvHeader& e, const DeviceState& s, const ResolveDesc& r) {
  // GAP-mem1 (WWE2K14_NR_MEM_LEAN bit 1): only the newest record's snapshot has a reader (Last(), at the swap). Older
  // records keep their metadata but stop pinning an image; up to 128 full-screen images otherwise stay held.
  static const bool lean_ring = (nb::MemLeanMode() & nb::kMemLeanRing) != 0;
  if (lean_ring && !TranslateOnly() && g_count) {
    Entry& newest = g_ring[(g_count - 1) % kRing];
    if (newest.used && newest.rec.snapshot != nb::kNone) {
      nb::ReleaseSnapshot(newest.rec.snapshot);
      newest.rec.snapshot = nb::kNone;
    }
  }
  Entry& ent = g_ring[g_count % kRing];
  FineEventScope fine_event(ent.ex);
  ++g_st.resolves;
  MaybeLog();
  if (!TranslateOnly() && ent.used && ent.rec.snapshot != nb::kNone) nb::ReleaseSnapshot(ent.rec.snapshot);  // oldest record falls out
  ent = Entry{};
  ent.used = true;
  ent.rec.seq = e.seq;
  ent.rec.packet_addr = e.packet_addr;
  ent.rec.desc = r;
  ent.rec.snapshot = nb::kNone;
  ent.rec.hash = 0;
  ent.ex.seq = e.seq;
  ent.ex.packet_addr = e.packet_addr;
  ++g_count;
  ResolveRecord& rec = ent.rec;
  ResolveExtra& ex = ent.ex;
  nb::ResolveCensusKey census_key{};
  auto done = [&](Class c) {
    ex.cls = c;
    ++g_st.by_class[uint8_t(c)];
    if (nb::ResolveCensusEnabled()) {
      census_key.dest_base = ex.dest_base;
      census_key.dest_format = (ex.dest_info >> 7) & 0x3F;
      if (!census_key.width) census_key.width = ex.width;
      if (!census_key.height) census_key.height = ex.height;
      census_key.resolve_class = uint32_t(c);
      census_key.valid = true;
      nb::ResolveCensusResolve(census_key);
    }
  };

  // RB_COPY_* (0x2318..0x2323) as the resolve worker left them; the mirror block is the fallback.
  uint32_t rb[12];
  std::memcpy(rb, r.rb_copy, sizeof(rb));
  bool all_zero = true;
  for (uint32_t v : rb) all_zero = all_zero && v == 0;
  if (all_zero) std::memcpy(rb, s.rs2300 + 24, sizeof(rb));
  const uint32_t ctrl = rb[0];
  const uint32_t src_sel = all_zero ? (r.flags & 7) : (ctrl & 7);
  const bool depth = src_sel >= 4;
  const uint32_t copy_cmd = (ctrl >> 20) & 3, sample_select = (ctrl >> 4) & 7;
  const bool clear_color = (ctrl >> 8) & 1, clear_depth = (ctrl >> 9) & 1;
  const uint32_t dest_base = rb[1], dest_pitch = rb[2] & 0x3FFF, dest_info = rb[3];
  const uint32_t dest_fmt = (dest_info >> 7) & 0x3F, dest_array = (dest_info >> 3) & 1;
  const int32_t exp_bias = int32_t(dest_info << 10) >> 26;
  ex.dest_base = dest_base;
  ex.dest_pitch = dest_pitch;
  ex.dest_info = dest_info;
  if (nb::ResolveCensusEnabled()) {
    census_key.dest_base = dest_base;
    census_key.dest_format = dest_fmt;
    census_key.frame = e.frame;
    census_key.valid = true;
  }
  ex.tiling_epoch = targets::TileEpochOn() ? targets::ResolveEpoch()  // card S3-66: + the tail after EndTiling
                                           : targets::InTiling() ? targets::TilingEpoch() : 0;
  if (targets::TileEpochOn() && ex.tiling_epoch) EpochNote(ex.tiling_epoch, !targets::InTiling(), dest_base, targets::WriteSeq());

  // ---- source image and rectangle
  int32_t x0 = r.src_rect[0], y0 = r.src_rect[1], x1 = r.src_rect[2], y1 = r.src_rect[3];
  const bool whole = x1 <= x0 || y1 <= y0;
  if (!whole) {
    x0 = std::max(x0, 0) & ~7;
    y0 = std::max(y0, 0) & ~7;
    x1 = int32_t(Align8(uint32_t(std::max(x1, 0))));
    y1 = int32_t(Align8(uint32_t(std::max(y1, 0))));
  }
  targets::Source src;
  std::string why;
  bool source_ok = false;
  {
    FineStageScope fine_source(FineStage::kSource);
    source_ok = targets::ResolveSource(s, depth, depth ? 0 : src_sel, whole ? 0 : uint32_t(y1), &src, &why);
  }
  if (!source_ok) {
    ++g_st.no_source;
    Unsupported("no source image: " + why);
    done(Class::kUnsupported);
    return;
  }
  const uint32_t W = src.desc.width, H = src.desc.height;
  if (whole) {
    // Card M2c2: the whole surface is its declared size (surface object +36), not the EDRAM pitch: a 512-wide target
    // has W = 560, and a 560-wide snapshot never matched the 512-wide fetch of the destination ("width 512 vs 560").
    x0 = y0 = 0;
    // Card M2c7: a tiled scene's slot copies the scene extent (BeginTiling), not the one-tile declared size.
    const uint32_t dw = src.scene_w ? src.scene_w : src.ext_w, dh = src.scene_w ? src.scene_h : src.ext_h;
    x1 = int32_t(dw ? std::min(dw, W) : W);
    y1 = int32_t(dh ? std::min(dh, H) : H);
    if (dw && dw < W) ++g_st.whole_declared;
    if (src.scene_w) ++g_st.whole_scene;
    // Card S3-71 (WWE2K14_D3D_RESOLVE_CLAMP_DEST=1, default off): a NULL-rect resolve outside predicated tiling copies
    // at most the destination D3D programmed into RB_COPY_DEST_PITCH (pitch x copy_dest_height = the destination level).
    // s371 comptrace: the attire mip chain (1D085000 1024x512, 1D005000 512x256, 1CFE5000 256x128) is resolved with a
    // NULL rect from the 2048x1024 compositing surface; taken as 2048x1024 the entries were unusable ("rect beyond the
    // pitch") and their 2048x1024 extents superseded the base level 1D285000, so the S3-17 CPU-visibility writeback had
    // nothing to serve to the game's XGUntileSurface reads (Heenan's DXT1 atlas compressed from zeros).
    static const bool clamp_dest = [] {
      const char* v = std::getenv("WWE2K14_D3D_RESOLVE_CLAMP_DEST");
      return v && std::strcmp(v, "1") == 0;
    }();
    const uint32_t cd_pitch = rb[2] & 0x3FFF, cd_h = (rb[2] >> 16) & 0x3FFF;
    if (clamp_dest && !src.scene_w && cd_pitch && cd_h && (x1 > int32_t(cd_pitch) || y1 > int32_t(cd_h))) {
      static uint32_t logged = 0;
      if (logged++ < 32)
        REXLOG_INFO("[d3d-shadow] resolve clamp-dest frame={} dest={:08X} whole {}x{} -> {}x{} (RB_COPY_DEST_PITCH)",
                    e.frame, dest_base, x1, y1, std::min(x1, int32_t(cd_pitch)), std::min(y1, int32_t(cd_h)));
      x1 = std::min(x1, int32_t(cd_pitch));
      y1 = std::min(y1, int32_t(cd_h));
      ++g_st.whole_clamped_dest;
    }
  }
  const int32_t x_max = int32_t(W & ~7u ? (W & ~7u) : W);
  if (x1 > x_max || y1 > int32_t(H)) ++g_st.rect_grown_clamped;
  x1 = std::min(x1, x_max);
  y1 = std::min(y1, int32_t(H));
  ex.samples = src.desc.samples;
  if (nb::ResolveCensusEnabled()) {
    census_key.source_handle = src.handle;
    census_key.source_target = src.slot_id;
    census_key.msaa = src.desc.samples;
    census_key.frame = e.frame;
  }
  if (x1 <= x0 || y1 <= y0) {
    // The clamped rect is empty: the requested region lies outside the bound source. nr no-ops this
    // (nr_frontend.cpp:3473-3477 empty region; :3693 zero-extent clear), so no copy and no clear are issued.
    ++g_st.empty_rect;
    ++g_st.empty_noop;
    if (clear_color || clear_depth) ++g_st.empty_with_clear;
    done(Class::kNoop);
    return;
  }
  ex.rect[0] = x0; ex.rect[1] = y0; ex.rect[2] = x1; ex.rect[3] = y1;
  ex.width = uint32_t(x1 - x0);
  ex.height = uint32_t(y1 - y0);
  std::memcpy(ex.union_rect, ex.rect, sizeof(ex.rect));
  // Card M2c8: the per-tile resolves of a tiled scene (NULL rect -> the whole scene, drawn once here) must all read
  // the drawn scene: the clear-with-resolve of the first tile stays deferred (applied at the next draw bind / clear /
  // SetRT / tiling, targets.h), it is not flushed by the following tile resolves of the same scene (m2c8-a frame 6000:
  // first scene copy correct, the next eight black).
  const bool tiled_whole = whole && src.scene_w != 0;
  if (tiled_whole) ++g_st.tile_clear_kept;
  if (!tiled_whole && targets::PendingClearIntersects(src.handle, x0, y0, int32_t(ex.width), int32_t(ex.height))) {
    ex.flushed_clear = true;
    targets::FlushDeferredClears();
  }

  // Card S3-17b (PARKED, not applied): D3D Resolve pDestPoint differing from the source rect origin is only counted.
  // Applying it as a destination offset was refuted for the CAS portrait (predicated tiling, S3-17c) and is unverified
  // for the 512x512 composites into 1 MB (= 512x512x4) destinations in menus/entrances/matches (s317r-a: 42 such
  // resolves, e.g. point 96,56) - an offset there would overflow the destination; the previous placement has the
  // m2c/s45 compare evidence (median 0 %). Reopen with a CP-side cross-check of those copies (RB_COPY_DEST_BASE,
  // window offset) showing where the hardware writes them.
  // Card S3-40 (WWE2K14_D3D_DEST_POINT=1): outside predicated tiling, place the copy at pDestPoint (destination rect =
  // source rect translated by dst_point - source origin) for the registry / extent; the snapshot is unchanged.
  int32_t dest_dx = 0, dest_dy = 0;
  if (r.has_dst_point) {
    const int32_t ox = whole ? 0 : r.src_rect[0], oy = whole ? 0 : r.src_rect[1];
    if (r.dst_point[0] != ox || r.dst_point[1] != oy) {
      ++g_st.dest_point_differs;
      static const bool apply_point = EnvU32("WWE2K14_D3D_DEST_POINT", 0) == 1;
      if (apply_point && !targets::InTiling() && !whole) {  // s353: whole-surface 512x512 composites at point 128,0 crashed
        dest_dx = r.dst_point[0] - ox;
        dest_dy = r.dst_point[1] - oy;
      }
      static const uint32_t ptrace = EnvU32("WWE2K14_D3D_DEST_POINT_TRACE", 0);
      static uint32_t plines = 0;
      if (ptrace && plines < 60) {
        ++plines;
        REXLOG_INFO("[d3d-dptrace] frame={} dest={:08X} pitch={} info={:08X} src_rect={},{},{},{} whole={} point={},{} "
                    "tiling={} -> dx={} dy={}", e.frame, dest_base, dest_pitch, dest_info, r.src_rect[0], r.src_rect[1],
                    r.src_rect[2], r.src_rect[3], whole, r.dst_point[0], r.dst_point[1], targets::InTiling(), dest_dx,
                    dest_dy);
      }
    }
  }

  // ---- copy
  Class cls = Class::kClearOnly;
  uint32_t snap = nb::kNone;
  uint32_t bpp_log2 = 2;
  std::string err;
  if (copy_cmd > 1) {
    if (copy_cmd == 2) Unsupported("copy command 2 (constant fill)");  // 3 = null copy: clear only
    cls = copy_cmd == 2 ? Class::kUnsupported : Class::kClearOnly;
  } else if (exp_bias != 0) {
    Unsupported("dest exp bias");
    cls = Class::kUnsupported;
  } else if (dest_array) {
    ++g_st.array_dest;
    Unsupported("array/3D destination");
    cls = Class::kUnsupported;
  } else {
    // Card V1 CHECK (round 2, review F4): only a real source-reading copy verifies a flagged image (no-op otherwise).
    targets::NoteResolveRead(src, x0, y0, ex.width, ex.height);
    const uint32_t samples = src.desc.samples;
    const bool averaged = samples == 1 || sample_select == uint32_t(xenos::CopySampleSelect::k0123) ||
                          (samples == 2 && sample_select == uint32_t(xenos::CopySampleSelect::k01));
    const nb::TargetFormat tf = src.desc.format;
    // Merge with the previous tile of the same predicated-tiling epoch?
    auto git = [&] {
      FineRegistrationPartScope fine_group_merge(FineRegistrationPart::kGroupMerge);
      return g_groups.find(dest_base);
    }();
    int32_t snap_rect[4] = {x0, y0, x1, y1};
    // Card S3-27b: a per-tile resolve (explicit tile column) of a predicated-tiling scene. The scene is drawn once at
    // full width and the tiles of one scene are resolved back to back (no draw/clear between, WriteSeq), so every tile
    // reads final pixels: copy the full scene width at the first tile and let the later tiles (whose merged union only
    // grows to that width, try_merge) reuse it through TileDedupe instead of re-copying 1, 2, 3 ... tile widths.
    // PARKED (S85/S86: 2/2 UHD runs froze with it, lane A clean; see card S3-27): opt-in WWE2K14_D3D_TILE_DEDUPE_COLS=1.
    static const bool cols_on = [] {
      const char* e = std::getenv("WWE2K14_D3D_TILE_DEDUPE_COLS");
      return e && std::strcmp(e, "1") == 0;
    }();
    const bool tiled_cols = !targets::TileEpochOn() && cols_on && TileDedupeOn() && !whole && ex.tiling_epoch &&
                            src.scene_w && targets::InTiling() && x0 >= 0 && x1 <= int32_t(src.scene_w);
    if (tiled_cols) {
      snap_rect[0] = 0;
      snap_rect[2] = std::min<int32_t>(int32_t(src.scene_w), x_max);
      if (snap_rect[2] <= snap_rect[0]) snap_rect[2] = x1;
      ++g_tdup_cols;
    }
    // Card S3-66 (WWE2K14_D3D_TILE_EPOCH=1, refines COLS: needs WWE2K14_D3D_TILE_DEDUPE=1 and _COLS=1, review F3): a
    // column tile of a tiled scene inside the bracket (spans the scene height) copies the whole scene width once; the
    // later tiles of the epoch reuse it through TileDedupe while no write happened in between (WriteSeq). Review F1/F2:
    // no row widen (no known user: the CAS portrait strips are NULL-rect resolves outside the bracket, S3-17 s317b-c),
    // and the column widen only when the destination pitch holds the widened width (else the registered extent would
    // run past a narrower destination). Tail resolves (after EndTiling) are only tagged with the epoch (SV-0b).
    const int32_t sw = int32_t(src.scene_w), sh = int32_t(src.scene_h);
    const bool in_scene = x0 >= 0 && y0 >= 0 && x1 <= int32_t(Align8(src.scene_w)) && y1 <= int32_t(Align8(src.scene_h));
    const bool spans_h = y0 == 0 && y1 >= (sh & ~7);
    const int32_t widen_x1 = std::max(x1, std::min<int32_t>(sw, x_max));
    const bool epoch_cand = targets::TileEpochOn() && cols_on && TileDedupeOn() && !whole && ex.tiling_epoch &&
                            targets::InTiling() && sw && sh && in_scene && spans_h;
    const bool epoch_tile = epoch_cand && dest_pitch >= uint32_t(widen_x1);
    if (epoch_cand && !epoch_tile) ++g_ec_widen_pitch_skip;
    if (epoch_tile) {
      snap_rect[0] = 0;
      snap_rect[2] = widen_x1;
      ++g_ec_widened;
    }
    const bool dedupe_ok = tiled_whole || tiled_cols || epoch_tile;
    const uint64_t ec_made0 = g_tdup_made, ec_reused0 = g_tdup_reused;  // card S3-66 diagnostic
    bool merged = false;
    auto try_merge = [&](Class c) {
      FineRegistrationPartScope fine_group_merge(FineRegistrationPart::kGroupMerge);
      if (git == g_groups.end()) return;
      Group& g = git->second;
      if (!ex.tiling_epoch || g.epoch != ex.tiling_epoch || g.handle != src.handle || g.pitch != dest_pitch ||
          g.info != dest_info || g.cls != c) {
        return;
      }
      const int32_t tile_rect[4] = {x0, y0, x1, y1};  // card S3-27b: the tile itself (snap_rect may be widened)
      for (const auto& q : g.rects) {
        if (!Disjoint(q, tile_rect)) return;
      }
      snap_rect[0] = std::min(g.uni[0], snap_rect[0]);
      snap_rect[1] = std::min(g.uni[1], snap_rect[1]);
      snap_rect[2] = std::max(g.uni[2], snap_rect[2]);
      snap_rect[3] = std::max(g.uni[3], snap_rect[3]);
      merged = true;
    };
    auto snap_w = [&] { return uint32_t(snap_rect[2] - snap_rect[0]); };
    auto snap_h = [&] { return uint32_t(snap_rect[3] - snap_rect[1]); };
    if (depth) {
      if (!TranslateOnly() && !nb::DepthSnapshotAvailable()) {
        Unsupported("depth snapshot pipeline unavailable");
        cls = Class::kUnsupported;
      } else if (sample_select > 3 && samples > 1) {
        Unsupported("depth sample select " + std::to_string(sample_select));
        cls = Class::kUnsupported;
      } else {
        cls = Class::kDepthR32F;
        try_merge(cls);
        if (nb::ResolveCensusEnabled()) {
          census_key.resolve_class = uint32_t(cls);
          census_key.width = snap_w();
          census_key.height = snap_h();
        }
        nb::ResolveParams p{};
        p.w = snap_w();
        p.h = snap_h();
        p.x0 = uint32_t(snap_rect[0]);
        p.y0 = uint32_t(snap_rect[1]);
        p.select = samples > 1 ? sample_select : 0;
        // Card S3-10 (AA cap option): a guest 4x surface rendered with 2 host samples - guest samples 0,1 / 2,3 read
        // host sample 0 / 1 (approximation of the option; exact only with the cap off).
        if (src.desc.guest_samples == 4 && samples == 2 && p.select <= 3) p.select >>= 1;
        p.ms = samples == 4 ? 2 : samples == 2 ? 1 : 0;
        p.flags = src.desc.format == nb::TargetFormat::kD24FS8 ? 0x100u : 0u;
        if (NativeDepth()) p.flags |= 0x200u;  // card S3-22: raw host depth (no float24 truncation)
        // Card S3-27 diagnostic (WWE2K14_D3D_DEPTH_TRACE=N: log every depth resolve of every N-th frame; remove when
        // the depth dedupe question is settled).
        static const uint32_t dtrace = EnvU32("WWE2K14_D3D_DEPTH_TRACE", 0);
        if (dtrace && e.frame % dtrace == 0) {
          REXLOG_INFO("[d3d-dtrace] frame={} dest={:08X} handle={} rect={},{},{},{} whole={} scene_w={} epoch={} intiling={} "
                      "sel={} ms={} merged={} seq={} samples={}",
                      e.frame, dest_base, src.handle, snap_rect[0], snap_rect[1], snap_rect[2], snap_rect[3], whole,
                      src.scene_w, ex.tiling_epoch, targets::InTiling(), p.select, p.ms, merged, targets::WriteSeq(),
                      samples);
        }
        p.dst_start = dest_base;
        if (TranslateOnly()) {
          snap = FakeSnap();
        } else {
          nb::ResolveCensusScope census_scope(census_key);
          FineStageScope fine_snapshot(FineStage::kSnapshot);
          snap = TileDedupe(dedupe_ok, ex.tiling_epoch, src.handle, snap_rect, cls,
                            p.select | p.ms << 8 | p.flags << 16,
                            [&] { return nb::SnapshotDepthR32F(src.handle, p, &err); });
        }
        bpp_log2 = 2;
      }
    } else if (!averaged) {
      Unsupported("sample select " + std::to_string(sample_select) + " on " + std::to_string(samples) + "x");
      cls = Class::kUnsupported;
    } else if (tf == nb::TargetFormat::kRGBA8 &&
               (dest_fmt == uint32_t(xenos::ColorFormat::k_8_8_8_8) ||
                dest_fmt == uint32_t(xenos::ColorFormat::k_8_8_8_8_A) || dest_fmt == uint32_t(xenos::ColorFormat::k_8) ||
                dest_fmt == uint32_t(xenos::ColorFormat::k_4_4_4_4))) {
      // A 4:4:4:4 destination still snapshots the source's RGBA8 image; only the destination texel is 2 bytes
      // (bpp_log2 = 1), as nr's bpp = 2 / bpp_log2 = 1 (nr_frontend.cpp:3955, :3990).
      cls = dest_fmt == uint32_t(xenos::ColorFormat::k_8)          ? Class::kK8
            : dest_fmt == uint32_t(xenos::ColorFormat::k_4_4_4_4) ? Class::kRGBA4
                                                                  : Class::kRGBA8;
      bpp_log2 = cls == Class::kK8 ? 0 : cls == Class::kRGBA4 ? 1 : 2;
      try_merge(cls);
      if (nb::ResolveCensusEnabled()) {
        census_key.resolve_class = uint32_t(cls);
        census_key.width = snap_w();
        census_key.height = snap_h();
      }
      if (TranslateOnly()) {
        snap = FakeSnap();
      } else {
        nb::ResolveCensusScope census_scope(census_key);
        FineStageScope fine_snapshot(FineStage::kSnapshot);
        snap = TileDedupe(dedupe_ok, ex.tiling_epoch, src.handle, snap_rect, cls, 0, [&] {
          return nb::SnapshotColor(src.handle, uint32_t(snap_rect[0]), uint32_t(snap_rect[1]), snap_w(), snap_h(), &err);
        });
      }
    } else if ((tf == nb::TargetFormat::kRG16S && dest_fmt == uint32_t(xenos::ColorFormat::k_16_16)) ||
               (tf == nb::TargetFormat::kRGBA16S && dest_fmt == uint32_t(xenos::ColorFormat::k_16_16_16_16))) {
      if (!TranslateOnly() && !nb::Fixed16SnapshotAvailable()) {
        Unsupported("fixed16 snapshot pipeline unavailable");
        cls = Class::kUnsupported;
      } else {
        cls = Class::kFixed16;
        const bool wide = tf == nb::TargetFormat::kRGBA16S;
        bpp_log2 = wide ? 3 : 2;
        try_merge(cls);
        if (nb::ResolveCensusEnabled()) {
          census_key.resolve_class = uint32_t(cls);
          census_key.width = snap_w();
          census_key.height = snap_h();
        }
        nb::ResolveParams p{};
        p.w = snap_w();
        p.h = snap_h();
        p.src_bpp = wide ? 8 : 4;
        p.bpp_log2 = bpp_log2;
        p.flags = 2;
        p.dst_start = dest_base;
        if (TranslateOnly()) {
          snap = FakeSnap();
        } else {
          nb::ResolveCensusScope census_scope(census_key);
          FineStageScope fine_snapshot(FineStage::kSnapshot);
          snap = TileDedupe(dedupe_ok, ex.tiling_epoch, src.handle, snap_rect, cls, p.src_bpp, [&] {
            return nb::SnapshotFixed16(src.handle, uint32_t(snap_rect[0]), uint32_t(snap_rect[1]), p, &err);
          });
        }
      }
    } else {
      Unsupported("src host format " + std::to_string(uint32_t(tf)) + " -> dest ColorFormat " + std::to_string(dest_fmt));
      cls = Class::kUnsupported;
    }
    if (targets::TileEpochOn() && ex.tiling_epoch) {
      g_ec_made += g_tdup_made - ec_made0;
      g_ec_reused += g_tdup_reused - ec_reused0;
    }
    if (cls != Class::kClearOnly && cls != Class::kUnsupported) {
      if (snap == nb::kNone) {
        Unsupported(std::string(ClassName(cls)) + " snapshot failed: " + err);
        cls = Class::kUnsupported;
      } else {
        std::memcpy(ex.union_rect, snap_rect, sizeof(snap_rect));
        ex.merged = merged;
        if (merged) ++g_st.merged;
        FineStageScope fine_register(FineStage::kRegister);
        int32_t dest_rect[4]{};
        uint32_t held = nb::kNone;
        {
          FineRegistrationPartScope fine_group_extent(FineRegistrationPart::kGroupExtent);
          Group& g = g_groups[dest_base];
          if (!merged) {
            g = Group{};
            g.epoch = ex.tiling_epoch;
            g.handle = src.handle;
            g.pitch = dest_pitch;
            g.info = dest_info;
            g.cls = cls;
          }
          g.rects.push_back({x0, y0, x1, y1});
          std::memcpy(g.uni, snap_rect, sizeof(snap_rect));
          if (FineTimersOn()) {
            g_fine.groups_peak = std::max<uint32_t>(g_fine.groups_peak, uint32_t(g_groups.size()));
            g_fine.group_rects_peak = std::max<uint32_t>(g_fine.group_rects_peak, uint32_t(g.rects.size()));
          }
          rec.snapshot = snap;
          // Registration for WP8: destination extent start/length as the SDK computes them for the union rect.
          dest_rect[0] = snap_rect[0] + dest_dx;
          dest_rect[1] = snap_rect[1] + dest_dy;
          dest_rect[2] = snap_rect[2] + dest_dx;
          dest_rect[3] = snap_rect[3] + dest_dy;
          if (dest_rect[0] < 0 || dest_rect[1] < 0) std::memcpy(dest_rect, snap_rect, sizeof(dest_rect));  // S3-40 guard
          DestExtent(dest_rect, dest_pitch, bpp_log2, &ex.extent_start, &ex.extent_length);
          ex.extent_start += dest_base;
        }
        {
          FineRegistrationPartScope fine_retain(FineRegistrationPart::kRetainSnapshot);
          held = TranslateOnly() ? FakeSnap() : nb::RetainSnapshot(snap);  // WP8 owns this second id (releases it when replaced)
        }
        if (held == nb::kNone || !ex.extent_length) {
          ++g_st.retain_fail;
        } else {
          ResolveDesc reg = r;
          for (int i = 0; i < 4; ++i) reg.src_rect[i] = dest_rect[i];  // S3-40: destination placement (== snap_rect unless DEST_POINT)
          std::memcpy(reg.rb_copy, rb, sizeof(rb));
          nb::ResolveCensusScope census_scope(census_key);
          {
            FineRegistrationPartScope fine_texture_register(FineRegistrationPart::kTextureRegister);
            textures::RegisterResolveImage(ex.extent_start, ex.extent_length, reg, held, census_key);
          }
          ex.registered = true;
          ++g_st.registered;
        }
      }
    }
    // ---- Card M2c7 diagnostic (WWE2K14_D3D_TRACE_EVERY=N, _TRACE_FROM=F; off by default; remove when settled): on
    // frames F, F+N, ... log every colour copy with a hash of its source region, to line up with nr's "[trace] N"
    // lines of the same frame (ReadColor averages MSAA the same way on both sides).
    {
      static const uint32_t tr_every = EnvU32("WWE2K14_D3D_TRACE_EVERY", 0), tr_from = EnvU32("WWE2K14_D3D_TRACE_FROM", 0);
      static const uint32_t tr_until = EnvU32("WWE2K14_D3D_TRACE_UNTIL", UINT32_MAX);
      static uint32_t tr_frame = UINT32_MAX, tr_idx = 0;
      if (tr_every && !TranslateOnly() && !depth && e.frame >= tr_from && e.frame <= tr_until &&
          (e.frame - tr_from) % tr_every == 0) {
        FineStageScope fine_readback(FineStage::kReadbackHash);
        if (tr_frame != e.frame) tr_frame = e.frame, tr_idx = 0;
        std::vector<uint8_t> px;
        std::string rerr;
        nb::ResolveCensusScope census_scope(census_key);
        const bool rd = nb::ReadColor(src.handle, uint32_t(x0), uint32_t(y0), ex.width, ex.height, px, &rerr);
        // Card M2c8: also save large RGBA8 copies of traced frames as PPM (RGB) to compare passes visually.
        if (rd && tf == nb::TargetFormat::kRGBA8 && ex.width >= 640 && px.size() >= size_t(ex.width) * ex.height * 4) {
          const char* o = std::getenv("WWE2K14_NR_OUT");
          char name[256];
          std::snprintf(name, sizeof(name), "%s/trace-S-f%u-i%u.ppm", o && *o ? o : "nr-out", e.frame, tr_idx);
          if (FILE* f = std::fopen(name, "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", ex.width, ex.height);
            for (size_t i = 0; i < size_t(ex.width) * ex.height; ++i) std::fwrite(&px[i * 4], 1, 3, f);
            std::fclose(f);
          }
        }
        REXLOG_INFO("[trace] S frame={} i={} ext={:08X} dest={:08X} {}x{} at {},{} dfmt={} tf={} cls={} hash={:016X}{}",
                    e.frame, tr_idx++, ex.extent_start, dest_base, ex.width, ex.height, x0, y0, dest_fmt, uint32_t(tf),
                    ClassName(cls), rd ? XXH3_64bits(px.data(), px.size()) : 0, rd ? "" : " READ FAILED: " + rerr);
      }
    }
    // ---- verification hash (sampled; RGBA8 class only; GPU readback, so off unless asked)
    static const uint32_t check_n = EnvU32("WWE2K14_D3D_SHADOW_RESOLVE_CHECK", 0);
    static uint32_t checked_frame = UINT32_MAX, checked_in_frame = 0;
    if (check_n && !TranslateOnly() && cls == Class::kRGBA8 && e.frame % 60 == 0) {
      if (checked_frame != e.frame) {
        checked_frame = e.frame;
        checked_in_frame = 0;
      }
      if (checked_in_frame < check_n && ex.width * ex.height <= (4u << 20)) {
        FineStageScope fine_readback(FineStage::kReadbackHash);
        ++checked_in_frame;
        std::vector<uint8_t> px;
        std::string rerr;
        nb::ResolveCensusScope census_scope(census_key);
        if (nb::ReadColor(src.handle, uint32_t(x0), uint32_t(y0), ex.width, ex.height, px, &rerr)) {
          rec.hash = HashRGBA8(px.data(), ex.width, ex.height);
          ex.hashed = true;
          ++g_st.hashed;
        } else {
          ++g_st.hash_reads_failed;
        }
      }
    }
  }
  done(cls);
  NoteFrameResolve(ex, rec.snapshot);  // card S3-44 diagnostic
  if (rec.snapshot != nb::kNone && ex.width >= 160) nb::FlashTapSnapshot(rec.snapshot, ex.dest_base);

  // ---- clear with resolve: the resolved rect, after the copy (deferred, see targets.h)
  if (clear_color || clear_depth) {
    FineStageScope fine_clear(FineStage::kClear);
    const int32_t cw = int32_t(ex.width), ch = int32_t(ex.height);
    if (clear_color) {
      targets::Source cs;
      std::string w;
      if (targets::ResolveSource(s, false, 0, 0, &cs, &w)) {
        const uint32_t info1 = s.rs2000[1];
        auto cf = xenos::ColorRenderTargetFormat((info1 >> 16) & 0xF);
        if (cf == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) cf = xenos::ColorRenderTargetFormat::k_8_8_8_8;
        targets::DeferredClear c;
        c.handle = cs.handle;
        c.color = true;
        c.x = x0; c.y = y0; c.w = cw; c.h = ch;
        std::string cw_why;
        if (nr::shared::ColorClearValue(cf, rb[6], rb[7], c.rgba, &c.integer_view, &cw_why)) {
          targets::DeferClear(c);
          ++g_st.clears_color;
        } else {
          Unsupported("resolve colour clear: " + cw_why);
        }
      } else {
        Unsupported("resolve colour clear without colour target: " + w);
      }
    }
    if (clear_depth) {
      targets::Source ds;
      std::string w;
      if (targets::ResolveSource(s, true, 0, 0, &ds, &w)) {
        targets::DeferredClear c;
        c.handle = ds.handle;
        c.depth = c.stencil = true;
        c.x = x0; c.y = y0; c.w = cw; c.h = ch;
        c.z = nr::shared::DepthClearZ((s.rs2000[2] >> 16) & 1, rb[5]);
        c.stencil_value = rb[5] & 0xFF;
        targets::DeferClear(c);
        ++g_st.clears_depth;
      } else {
        Unsupported("resolve depth clear without depth surface: " + w);
      }
    }
  }
}

void LogStats(const char* why) {
  REXLOG_INFO(
      "[d3d-shadow] resolve ({}) n={} rgba8={} k8={} fixed16={} depth_r32f={} clear_only={} unsupported={} rgba4={} "
      "noop={} merged={} registered={} hashed={} hash_read_fail={} no_source={} whole_declared={} whole_scene={} whole_clamped_dest={} tile_clear_kept={} empty_rect={} empty_noop={} "
      "empty_with_clear={} array_dest={} retain_fail={} rect_clamped={} clear_c={} clear_d={} dest_point_differs={} "
       "tile_dedupe made={} reused={} cols={}",
      why, g_st.resolves, g_st.by_class[1], g_st.by_class[2], g_st.by_class[3], g_st.by_class[4], g_st.by_class[5],
      g_st.by_class[6], g_st.by_class[7], g_st.by_class[8], g_st.merged, g_st.registered, g_st.hashed,
      g_st.hash_reads_failed, g_st.no_source, g_st.whole_declared, g_st.whole_scene, g_st.whole_clamped_dest, g_st.tile_clear_kept, g_st.empty_rect, g_st.empty_noop, g_st.empty_with_clear,
      g_st.array_dest, g_st.retain_fail, g_st.rect_grown_clamped, g_st.clears_color, g_st.clears_depth,
       g_st.dest_point_differs, g_tdup_made, g_tdup_reused, g_tdup_cols);
  for (const auto& kv : g_st.unsupported) REXLOG_INFO("[d3d-shadow] resolve unsupported x{}: {}", kv.second, kv.first);
  if (targets::TileEpochOn()) {  // card S3-66 diagnostic (SV-0b runtime answer)
    const double ep = g_ec_epochs ? double(g_ec_epochs) : 1.0;
    REXLOG_INFO("[d3d-shadow] tile_epoch ({}) epochs={} in/ep={:.2f} tail/ep={:.2f} dests_in/ep={:.2f} dests_tail/ep={:.2f} "
                "tail_epochs={} in_max={} tail_max={} dests_over={} seq_breaks={} widened={} made={} reused={} "
                "widen_pitch_skip={} scene_create={}",
                why, g_ec_epochs, g_ec_in / ep, g_ec_tail / ep, g_ec_dests_in / ep, g_ec_dests_tail / ep, g_ec_tail_eps,
                g_ec_in_max, g_ec_tail_max, g_ec_dests_over, g_ec_seq_breaks, g_ec_widened, g_ec_made, g_ec_reused,
                g_ec_widen_pitch_skip, targets::SceneCreates());
  }
  targets::LogStats(why);
}

void CensusFrameEnd(uint64_t frame) {
  if (!nb::ResolveCensusEnabled()) return;
  const nb::ResolveCensusFrame f = nb::TakeResolveCensusFrame(frame);
  REXLOG_INFO("[s51-census-frame] frame={} images={} copies={} blits={} compute={} msaa_resolves={} barriers={} consumers={}", frame,
              f.images, f.copies, f.blits, f.compute, f.msaa_resolves, f.barriers, f.consumers);

  using TopKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t, uint32_t>;
  static std::map<TopKey, nb::ResolveCensusRow> top;
  static auto last = std::chrono::steady_clock::now();
  static auto top_start = last;
  static uint32_t top_windows = 0;
  static uint64_t top_frames = 0;
  ++top_frames;
  const auto now = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(now - last).count();
  if (elapsed < 1.0) return;
  last = now;
  const std::vector<nb::ResolveCensusRow> rows = nb::TakeResolveCensusWindow();
  nb::ResolveCensusCounts total{};
  for (const nb::ResolveCensusRow& row : rows) {
    if (!row.key.valid) {
      total = row.counts;
      continue;
    }
    const nb::ResolveCensusCounts& c = row.counts;
    const nb::ResolveCensusKey& k = row.key;
    const TopKey tk{k.dest_base, k.dest_format, k.width, k.height, k.resolve_class, k.source_handle, k.source_target, k.msaa};
    if (top.size() >= 8192 && !top.count(tk)) continue;
    auto it = top.emplace(tk, nb::ResolveCensusRow{k, {}}).first;
    nb::ResolveCensusCounts& d = it->second.counts;
    d.resolves += c.resolves;
    d.images += c.images;
    d.copies += c.copies;
    d.blits += c.blits;
    d.compute += c.compute;
    d.msaa_resolves += c.msaa_resolves;
    d.bytes_copied += c.bytes_copied;
    d.guest_writebacks += c.guest_writebacks;
    d.guest_writeback_bytes += c.guest_writeback_bytes;
    d.cpu_readbacks += c.cpu_readbacks;
    d.cpu_readback_bytes += c.cpu_readback_bytes;
    d.consumers += c.consumers;
    d.barriers += c.barriers;
    for (const auto& site : c.copy_sites) {
      nb::ResolveCensusSiteCounts& dst = d.copy_sites[site.first];
      dst.calls += site.second.calls;
      dst.bytes += site.second.bytes;
    }
    for (const auto& fetch : c.fetches) {
      nb::ResolveCensusFetchCounts& dst = d.fetches[fetch.first];
      dst.binds += fetch.second.binds;
      dst.rebuilds += fetch.second.rebuilds;
      dst.guest_detile_bytes += fetch.second.guest_detile_bytes;
      dst.guest_upload_bytes += fetch.second.guest_upload_bytes;
      dst.resolve_paste_bytes += fetch.second.resolve_paste_bytes;
    }
  }
  const double rate = 1.0 / elapsed;
  REXLOG_INFO("[s51-census] window_s={:.2f} resolves/s={:.1f} host_images_created/s={:.1f} copies+blits+compute_passes/s={:.1f} "
              "copies/s={:.1f} blits/s={:.1f} compute/s={:.1f} msaa_resolves/s={:.1f} bytes_copied/s={:.0f} "
              "guest_writebacks/s={:.2f} CPU_readbacks/s={:.2f} barriers/s={:.1f}",
              elapsed, total.resolves * rate, total.images * rate, (total.copies + total.blits + total.compute) * rate,
              total.copies * rate, total.blits * rate, total.compute * rate, total.msaa_resolves * rate,
              total.bytes_copied * rate, total.guest_writebacks * rate, total.cpu_readbacks * rate, total.barriers * rate);
  if (++top_windows < 10) return;
  top_windows = 0;
  const double top_elapsed = std::chrono::duration<double>(now - top_start).count();
  const double frames = double(top_frames);
  top_start = now;
  top_frames = 0;
  std::vector<const nb::ResolveCensusRow*> ranked;
  ranked.reserve(top.size());
  for (const auto& entry : top) ranked.push_back(&entry.second);
  std::sort(ranked.begin(), ranked.end(), [](const auto* a, const auto* b) {
    const auto& x = a->counts;
    const auto& y = b->counts;
    const uint64_t xo = x.images + x.copies + x.blits + x.compute + x.msaa_resolves;
    const uint64_t yo = y.images + y.copies + y.blits + y.compute + y.msaa_resolves;
    return x.resolves != y.resolves ? x.resolves > y.resolves : xo > yo;
  });
  const double top_rate = top_elapsed > 0.0 ? 1.0 / top_elapsed : 0.0;
  for (size_t i = 0; i < ranked.size() && i < 30; ++i) {
    const nb::ResolveCensusRow& row = *ranked[i];
    const nb::ResolveCensusKey& k = row.key;
    const nb::ResolveCensusCounts& c = row.counts;
    REXLOG_INFO("[s51-census-top] rank={} window_s={:.2f} dest={:08X} fmt={} {}x{} class={} src_handle={} src_target={:016X} msaa={} "
                "resolves/s={:.2f} host_images/s={:.2f} copies+blits+compute/s={:.2f} bytes_copied/s={:.0f} "
                "guest_writebacks/s={:.2f} CPU_readbacks/s={:.2f} consumers={} barriers/s={:.2f}",
                i + 1, top_elapsed, k.dest_base, k.dest_format, k.width, k.height, ClassName(Class(k.resolve_class)),
                k.source_handle, k.source_target, k.msaa, c.resolves * top_rate, c.images * top_rate,
                (c.copies + c.blits + c.compute) * top_rate, c.bytes_copied * top_rate, c.guest_writebacks * top_rate,
                c.cpu_readbacks * top_rate, c.consumers, c.barriers * top_rate);
    for (const auto& site : c.copy_sites) {
      REXLOG_INFO("[s51-census-site] dest={:08X} {}x{} class={} line={} calls/s={:.2f} bytes/s={:.0f}", k.dest_base,
                  k.width, k.height, ClassName(Class(k.resolve_class)), site.first, site.second.calls * top_rate,
                  site.second.bytes * top_rate);
    }
    for (const auto& fetch : c.fetches) {
      const nb::ResolveCensusFetchKey& fk = fetch.first;
      const nb::ResolveCensusFetchCounts& fc = fetch.second;
      REXLOG_INFO("[s51-census-fetch] dest={:08X} class={} fetch={:08X} fmt={} {}x{} mip_max={} tiled={} binds/s={:.2f} "
                  "rebuilds/s={:.2f} rebuilds/frame={:.3f} guest_detile_B/s={:.0f} guest_upload_B/s={:.0f} "
                  "resolve_paste_B/s={:.0f}",
                  k.dest_base, ClassName(Class(k.resolve_class)), fk.base, fk.format, fk.width, fk.height, fk.mip_max,
                  fk.tiled, fc.binds * top_rate, fc.rebuilds * top_rate, frames > 0 ? fc.rebuilds / frames : 0.0,
                  fc.guest_detile_bytes * top_rate, fc.guest_upload_bytes * top_rate, fc.resolve_paste_bytes * top_rate);
    }
  }
  top.clear();
}

}  // namespace wwe2k14::d3d::resolves
