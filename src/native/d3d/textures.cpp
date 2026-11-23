// WP8 (decision 0006 stage 2): textures. See textures.h for the contract.
//
// Method: the fetch constant (device mirror, per stage) decides the host texture exactly as nr's TextureFor does for
// a guest texture; the decode helpers are nr::shared (WP0), called not copied. Differences by design:
//  * input = DeviceState::fetch (never a RegisterFile); guest bytes = physical memory the fetch points at;
//  * ordinary guest textures stay on the existing path; CanvasOn destination seeds use an isolated GPU detile pool;
//  * a fetch overlapping a recorded resolve destination binds WP6's host snapshot or is refused, never guest memory;
//  * own TextureWatch instance + own invalidation callback (the SDK delivers every write to every callback).
#include "textures.h"
#include "quality_lifetime.h"

#ifdef WWE2K14_HAS_NR

#include <algorithm>
#include <chrono>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <tuple>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <rex/logging.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/system/kernel_state.h>

#include "../nr/nr_backend.h"
#include "../nr/nr_shared.h"
#include "capture.h"
#include "dest_images.h"
#include "glitch_detectors.h"
#include "late_upload.h"
#include "seed_lazy.h"
#include "rr_mem.h"

namespace wwe2k14::d3d::textures {
namespace {

namespace nb = nr::backend;
namespace sh = nr::shared;
using rex::graphics::texture_util::TextureGuestLayout;
using rex::graphics::xenos::xe_gpu_texture_fetch_t;
namespace xenos = rex::graphics::xenos;
using TexKey = sh::TextureKeys::TextureKey;

constexpr uint32_t kPhysLimit = 0x20000000u;
constexpr uint32_t kMaxResolveEntries = 2048;  // card S3-55: 256 evicted 6-man wrestler textures (LRU below)
constexpr uint64_t kSeedCanvasCacheBytes = 32ull << 20;
constexpr size_t kSeedCanvasCacheEntries = 64;
constexpr size_t kMaxResolveRectHistory = 64;
constexpr uint64_t kSeedLazyMaskBudget = 32ull << 20;
constexpr size_t kSeedLazyCopyRectLimit = 65536;

struct SeedCoverage;

struct Entry {
  uint32_t handle = nb::kNone;
  std::array<uint32_t, 4> range{};  // base start/end, mip start/end (physical)
  XXH128_hash_t hash{};
  uint64_t frame = 0;      // frame that last validated / built it
  uint64_t last_use = 0;
  uint64_t gen_sum = 0;
  bool gen_valid = false;
  uint64_t content = 0;
  uint64_t bytes = 0;      // host bytes counted against the byte cap (card M2b2-B)
  uint32_t res_start = 0;  // resolve-derived: owning resolve entry (start, seq); 0 seq = guest texture
  uint64_t res_seq = 0;
  uint32_t persistent_res_start = 0;
  uint64_t persistent_lineage = 0;
  // Card S3-69 5b/5c: persistent destination image (mip chain / cube): resolve entry seq per level / face it holds.
  std::array<uint64_t, 16> pseq{};
  uint32_t pseq_n = 0;
};

struct Res {
  struct RectWrite {
    std::array<int32_t, 4> rect{};
    uint64_t seq = 0;
  };
  uint32_t start = 0, length = 0;
  uint64_t seq = 0;
  uint64_t lineage_id = 0;  // stable only while compatible resolve registrations remain live
  uint64_t last_use = 0;  // card S3-55: res_seq at registration or at the last fetch that overlapped it (LRU cap)
  uint64_t mem_last_use_frame = 0;  // WWE2K14_NR_MEM_OWNERS idle age, in worker presents
  uint32_t snapshot = nb::kNone;  // the reference WP6 handed over: released when this entry is dropped
  int kind = -1;                  // 0 rgba8, 1 k_8, 2 float24 depth, 3 fixed16x4, 4 fixed16x2,
                                  // 5 rgba8 source -> 4444 (2-byte) destination; -1 unusable
  std::string why_unusable;
  uint32_t w = 0, h = 0, pitch_aligned = 0, endian = 0, dest_format = 0;
  int32_t resolve_rect[4] = {0, 0, 0, 0};  // this registration's destination pixels; canvas snapshot coordinates match
  bool swap_rb = false;
  std::array<uint32_t, 4> range{};
  uint64_t gen_sum = 0;
  uint64_t sample_hash = 0;  // card S3-33b: 64 sampled guest dwords of the destination at registration
  bool materialized = false;  // card S3-33e: the snapshot's bytes were written to guest memory
  std::vector<std::string> derived;  // cache_ keys of textures made from the snapshot
  uint64_t reg_frame = 0;            // card S3-41 trace: s.frame at registration
  nb::ResolveCensusKey census_key{};  // S5-1 per-destination transfer attribution
  ResolveDesc reg_desc{};             // record-then-replay: the registration (re-registered offline)
  // Card M2c3: destination identity and composition. A resolve whose rect does not start at (0,0), or that leaves
  // part of an earlier compatible resolve to the same destination visible, is copied into a canvas image of the
  // destination (pitch x height) at its rect: the canvas is the shadow's model of that memory. cov = 8x8-pixel cells
  // written by some resolve (canvas only; a plain entry covers (0,0)-(w,h)).
  uint32_t dest_base = 0, info = 0;
  uint32_t dest_texture = 0, dest_h = 0;  // card S3-69: Resolve r6 (destination texture object), RB_COPY_DEST_PITCH height
  bool canvas = false;
  std::vector<RectWrite> resolve_rect_history;  // Resolve-into catch-up source rectangles; newest resolve is last
  bool resolve_rect_history_overflow = false;  // older consumers must rebuild once their sequence predates history
  uint32_t cov_cols = 0;
  std::vector<uint8_t> cov;
  // Card S3-69 5a: the canvas was seeded from the guest bytes of the whole destination at registration (never cleared);
  // its uncovered cells read as the guest memory's (their only writer). Unseeded canvases keep the M2c6 bind path.
  bool seeded = false;
  bool seed_pending = false;  // lazy state: the original guest seed is not yet observable/materialized
  uint32_t seed_source = 0, seed_source_bytes = 0;
  uint64_t seed_source_gen = 0;
  std::shared_ptr<SeedCoverage> seed_coverage;
  bool Covers(uint32_t w2, uint32_t h2) const {
    if (!canvas) return w2 <= w && h2 <= h;
    if (w2 > w || h2 > h) return false;
    for (uint32_t cy = 0; cy < (h2 + 7) / 8; ++cy)
      for (uint32_t cx = 0; cx < (w2 + 7) / 8; ++cx)
        if (!cov[size_t(cy) * cov_cols + cx]) return false;
    return true;
  }
  // Only cells the rect covers completely (Luna review: a partial edge cell must not count as written).
  void Mark(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    for (int32_t cy = (std::max(0, y0) + 7) / 8; cy < y1 / 8 && uint32_t(cy) < (h + 7) / 8; ++cy)
      for (int32_t cx = (std::max(0, x0) + 7) / 8; cx < x1 / 8 && uint32_t(cx) < cov_cols; ++cx)
        cov[size_t(cy) * cov_cols + uint32_t(cx)] = 1;
  }
};

struct Memo {
  uint32_t fetch[6] = {};
  uint64_t frame = ~0ull, epoch = 0;
  uint32_t dimension = 0;
  uint32_t obj = 0;  // card S3-69 5e: bound texture object the memoized result was made for
  const void *vs = nullptr, *ps = nullptr;
  uint32_t handle = nb::kNone;
  uint32_t resolve_base = 0;
  nb::ResolveCensusKey census_key{};
  uint64_t content = 0;
  nb::SamplerDesc sampler;
  bool valid = false;
};

// Card S3-12: frame-wide lookup memo. Key = the 6 fetch-constant dwords + shader dimension + bound VS/PS (the sampler
// depends on them); value = what ForSlot returned. Valid for the current frame only (as the cache's own once-per-frame
// revalidation); entries are forgotten when their handle is released (DropEntry / DropResolve) or a resolve is
// registered over their guest range. Replaces the per-slot memo's global epoch, which every texture creation and
// every resolve (~90 per entrance frame) invalidated.
struct FKey {
  uint32_t fetch[6];
  uint32_t dimension;
  uint32_t obj;  // card S3-69 5e: bound texture object (0 when the object-identity switches are off)
  const void* vs;
  const void* ps;
  bool operator==(const FKey& o) const { return std::memcmp(this, &o, sizeof(FKey)) == 0; }
};
struct FKeyHash {
  size_t operator()(const FKey& k) const { return size_t(XXH3_64bits(&k, sizeof(FKey))); }
};
struct FVal {
  uint32_t handle = nb::kNone;
  uint32_t resolve_base = 0;
  nb::ResolveCensusKey census_key{};
  uint64_t content = 0;
  nb::SamplerDesc sampler;
  std::array<uint32_t, 4> range{};
};


// Optional cache of the prior composed canvas for one exact destination object. A write-watch generation over the
// complete detile source range must remain unchanged; resolved-cell coverage is carried with the host image so only
// newly resolved rectangles are overlaid on reuse.
struct SeedCanvas {
  uint32_t snapshot = nb::kNone;  // one retained snapshot id, independent of any live Res entry
  uint32_t dest_span = 0, info = 0, kind = -1, pitch = 0, dest_h = 0, w = 0, h = 0;
  uint32_t source = 0, source_bytes = 0;
  uint64_t source_gen = 0, last_use = 0, length = 0;
  uint32_t start = 0, cov_cols = 0;
  std::vector<uint8_t> cov;
  uint64_t image_bytes = 0;
};

using QualityUpload = quality::QualityUploadRecord;
struct QualityState {
  quality::RangeWriteHistory writes;
  quality::ResolvePageMap resolved;
  quality::QualityUploadRegistry uploads;
  bool valid = true;
  uint32_t invalid_causes = 0;  // bit1: live-cap Put failure, bit2: invalid resolve range
};

struct State {
  rex::memory::Memory* memory = nullptr;
  std::unordered_map<FKey, FVal, FKeyHash> fmemo;  // card S3-12
  uint64_t fmemo_frame = ~0ull, fmemo_hits = 0, fmemo_forgets = 0;
  std::unique_ptr<sh::TextureWatch> watch;
  void* watch_handle = nullptr;
  XXH3_state_t* hash_state = nullptr;
  std::unordered_map<std::string, Entry> cache;
  // B1-a (PERFSTAB step 2, 11-17): cache keys of persistent entries by (persistent_res_start, persistent_lineage), so
  // DropResolve finds a lineage's entries without walking the whole cache. Maintained by CachePut / IdxRemove at every
  // insert and erase of `cache`. WWE2K14_D3D_PERSIST_INDEX: 1 (default) index, 0 old full walk, 2 CHECK (both, compare).
  std::map<std::pair<uint32_t, uint64_t>, std::set<std::string>> persist_idx;
  int persist_mode = -1;
  uint64_t persist_ns = 0, persist_drops = 0, persist_check_bad = 0, persist_check_n = 0, persist_scan_ns = 0;
  std::map<uint32_t, Res> resolves;  // by start, non-overlapping
  std::map<std::pair<uint32_t, uint32_t>, SeedCanvas> seed_canvases;  // (destination base, D3D texture object)
  uint64_t seed_canvas_bytes = 0;
  Memo memo[kFetchSlots];
  const rex::graphics::Shader *vs = nullptr, *ps = nullptr;
  uint64_t frame = 0, epoch = 1, res_seq = 0;
  uint32_t cap = 1024;
  uint64_t byte_cap = 256ull << 20;  // WWE2K14_D3D_SHADOW_TEX_MB (card M2b2-B)
  bool ignore_resolve_write = false;
  Counters c;
  std::map<std::string, uint64_t> hist;  // refusal classes
  uint64_t logged_frame = 0;
  Counters last_logged;
  std::unordered_map<std::string, uint32_t> rebuild_count;  // card S3-44: guest-texture content changes per cache key
  std::set<std::string> dyn_keys;                           // ... keys changed at least twice (re-hashed every frame)
  std::string flash_binds;  // card S3-44 diagnostic: this frame's non-memo texture binds (FLASH_TRACE)
  uint64_t resolve_into_checks = 0, resolve_into_mismatches = 0;
  uint64_t resolve_into_covered_mismatches = 0, resolve_into_uncovered_mismatches = 0;
  uint64_t resolve_into_covered_texels = 0, resolve_into_uncovered_texels = 0, resolve_into_bytes_saved = 0;
  struct SeedLazyStats {
    uint64_t created = 0, skipped = 0;
    uint64_t materialized_bind = 0, materialized_writeback = 0, materialized_propagation = 0;
    uint64_t would_skip = 0, would_materialize = 0, dirty_refused = 0, dirty_propagation = 0, backup_fallback = 0;
    uint64_t checks = 0, mismatch = 0, check_rate_skipped = 0, check_failed = 0;
  } seed_lazy;
  uint64_t seed_mask_bytes = 0, seed_lazy_log_frame = 0;
  uint32_t lit_shadow_texture = nb::kNone;
  bool lit_shadow_texture_attempted = false;
  std::unique_ptr<QualityState> quality;  // absent (no histories/maps) when options are off
};

struct SeedCoverage {
  State* owner = nullptr;
  seed_lazy::PixelCoverage pixels;
  uint64_t accounted_bytes = 0;
  SeedCoverage(uint32_t w, uint32_t h) : pixels(w, h), accounted_bytes(pixels.storage_bytes()) {}
  ~SeedCoverage() {
    if (owner) owner->seed_mask_bytes -= std::min(owner->seed_mask_bytes, accounted_bytes);
  }
};

ResolveRegistrationFineStats g_resolve_registration_fine;

bool ResolveRegistrationFineTimersOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_RESOLVE_FINE_TIMERS");
    return e && std::strcmp(e, "1") == 0;
  }();
  return on;
}
uint64_t ResolveRegistrationFineWallNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint64_t ResolveRegistrationFineCpuNowNs() {
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
class ResolveRegistrationFineScope {
 public:
  explicit ResolveRegistrationFineScope(ResolveRegistrationFineStage stage)
      : stage_(stage), active_(ResolveRegistrationFineTimersOn()) {
    if (active_) {
      wall_start_ = ResolveRegistrationFineWallNowNs();
      cpu_start_ = ResolveRegistrationFineCpuNowNs();
    }
  }
  ~ResolveRegistrationFineScope() {
    if (!active_) return;
    const size_t i = static_cast<size_t>(stage_);
    ++g_resolve_registration_fine.calls[i];
    g_resolve_registration_fine.wall_ns[i] += ResolveRegistrationFineWallNowNs() - wall_start_;
    const uint64_t cpu_end = ResolveRegistrationFineCpuNowNs();
    if (cpu_start_ && cpu_end >= cpu_start_) g_resolve_registration_fine.cpu_ns[i] += cpu_end - cpu_start_;
  }
  ResolveRegistrationFineScope(const ResolveRegistrationFineScope&) = delete;
  ResolveRegistrationFineScope& operator=(const ResolveRegistrationFineScope&) = delete;

 private:
  ResolveRegistrationFineStage stage_;
  bool active_;
  uint64_t wall_start_ = 0, cpu_start_ = 0;
};

void ResolveRegistrationFineNoteMemoScan(size_t entries) {
  if (!ResolveRegistrationFineTimersOn()) return;
  g_resolve_registration_fine.memo_entries_scanned += entries;
  g_resolve_registration_fine.max_frame_memo_entries =
      std::max<uint32_t>(g_resolve_registration_fine.max_frame_memo_entries, uint32_t(entries));
}

std::mutex g_dyn_bases_mu;
std::set<uint32_t> g_dyn_bases;
bool FlashTraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_NR_FLASH_TRACE");
    return v && *v == '1';
  }();
  return on;
}

State& S() {
  static State* s = [] {
    auto* st = new State();  // never freed: the write-watch callback may fire during shutdown
    if (const char* e = std::getenv("WWE2K14_D3D_TEX_CAP")) {
      const long v = std::strtol(e, nullptr, 10);
      if (v >= 16 && v <= 4096) st->cap = uint32_t(v);
    }
    if (const char* e = std::getenv("WWE2K14_D3D_TEX_RESOLVE_WRITE")) st->ignore_resolve_write = std::strcmp(e, "ignore") == 0;
    if (const char* e = std::getenv("WWE2K14_D3D_SHADOW_TEX_MB")) {
      const long v = std::strtol(e, nullptr, 10);
      if (v >= 1 && v <= 65536) st->byte_cap = uint64_t(v) << 20;
    }
    st->c.cap = st->cap;
    st->c.byte_cap = st->byte_cap;
    return st;
  }();
  return *s;
}

int SeedLazyMode() {
  static const int mode = [] {
    const char* e = std::getenv("WWE2K14_D3D_SEED_LAZY");
    if (!e) return 0;
    const long v = std::strtol(e, nullptr, 10);
    return v >= 0 && v <= 2 ? int(v) : 0;
  }();
  return mode;
}

uint64_t SeedLazyABFrames() {
  static const uint64_t frames = [] {
    const char* e = std::getenv("WWE2K14_D3D_SEED_LAZY_AB");
    if (!e || !*e) return uint64_t(0);
    char* end = nullptr;
    const unsigned long long v = std::strtoull(e, &end, 10);
    return end != e && !*end && v > 0 ? uint64_t(v) : uint64_t(0);
  }();
  return frames;
}

bool SeedLazyDiagnosticsOn() { return SeedLazyMode() != 0 || SeedLazyABFrames() != 0; }
bool SeedLazyCheckMode() { return SeedLazyMode() == 2 && SeedLazyABFrames() == 0; }

// The AB override is self-contained: even frame blocks use eager seeding, odd blocks defer it.
int SeedLazyModeAtFrame(uint64_t frame) {
  const uint64_t block = SeedLazyABFrames();
  if (block) return ((frame / block) & 1) ? 1 : 0;
  return SeedLazyMode();
}

std::shared_ptr<SeedCoverage> MakeSeedCoverage(State& s, uint32_t w, uint32_t h) {
  try {
    auto coverage = std::make_shared<SeedCoverage>(w, h);
    if (!coverage->accounted_bytes || s.seed_mask_bytes + coverage->accounted_bytes > kSeedLazyMaskBudget) return {};
    coverage->owner = &s;
    s.seed_mask_bytes += coverage->accounted_bytes;
    return coverage;
  } catch (...) {
    return {};
  }
}

std::atomic<sh::TextureWatch*> g_watch_ptr{nullptr};

// The SDK calls every registered callback for a write to a watched page; ours only advances generations / clears
// armed bits (atomics), and answers the same range as nr's callback so the unwatch window is the touched pages.
std::pair<uint32_t, uint32_t> WatchThunk(void*, uint32_t start, uint32_t length, bool) {
  rr::NoteInvalidate(start, length);  // RR1: replay marks the same pages written
  if (sh::TextureWatch* w = g_watch_ptr.load(std::memory_order_acquire)) w->MarkWritten(start, length, false);
  if (!length || start >= kPhysLimit) return {0u, UINT32_MAX};
  const uint32_t first = start >> 12, last = uint32_t(std::min<uint64_t>(uint64_t(start) + length - 1, kPhysLimit - 1) >> 12);
  return {first << 12, (last - first + 1) << 12};
}

void EnsureRegistered(void*) {
  State& s = S();
  if (!s.watch_handle && s.memory) s.watch_handle = s.memory->RegisterPhysicalMemoryInvalidationCallback(&WatchThunk, nullptr);
}

bool EnsureInit() {
  State& s = S();
  if (!s.memory) {
    auto* ks = rex::system::kernel_state();
    if (ks) s.memory = ks->memory();
  }
  if (!s.memory) return false;
  if (!s.watch) {
    s.watch = std::make_unique<sh::TextureWatch>();
    s.watch->SetEnsureRegistered(&EnsureRegistered, nullptr);
    g_watch_ptr.store(s.watch.get(), std::memory_order_release);
  }
  if (!s.hash_state) s.hash_state = XXH3_createState();
  return s.hash_state != nullptr;
}

// Arms [start, start+len) unless every page already carries the one-shot bit (re-arming is a protect syscall).
void ArmIfNeeded(State& s, uint32_t start, uint32_t len) {
  if (!len || start >= kPhysLimit) return;
  const uint32_t first = start >> 12, last = uint32_t(std::min<uint64_t>(uint64_t(start) + len - 1, kPhysLimit - 1) >> 12);
  for (uint32_t pg = first; pg <= last; ++pg) {
    if (!s.watch->Armed(pg)) {
      s.watch->Arm(s.memory, first << 12, size_t(last - first + 1) << 12);
      return;
    }
  }
}

uint32_t Refuse(State& s, const std::string& cls, const std::string& msg, std::string* why, bool resolve = false) {
  ++s.c.refusals;
  if (resolve) ++s.c.refused_resolve;
  ++s.hist[cls];
  if (why) *why = msg;
  return nb::kNone;
}

void BackendRelease(uint32_t handle) {
  State& s = S();
  if (s.quality && handle != nb::kNone) s.quality->uploads.Erase(handle);
  if (!TranslateOnly() && handle != nb::kNone) nb::ReleaseTexture(handle);
}

void FMemoForgetHandle(State& s, uint32_t handle) {
  if (s.fmemo.empty() || handle == nb::kNone) return;
  ResolveRegistrationFineScope fine_memo(ResolveRegistrationFineStage::kMemoForget);
  ResolveRegistrationFineNoteMemoScan(s.fmemo.size());
  for (auto it = s.fmemo.begin(); it != s.fmemo.end();) {
    if (it->second.handle == handle) it = s.fmemo.erase(it), ++s.fmemo_forgets;
    else ++it;
  }
}
void FMemoForgetRange(State& s, uint32_t a, uint32_t b) {
  if (s.fmemo.empty() || b <= a) return;
  ResolveRegistrationFineScope fine_memo(ResolveRegistrationFineStage::kMemoForget);
  ResolveRegistrationFineNoteMemoScan(s.fmemo.size());
  for (auto it = s.fmemo.begin(); it != s.fmemo.end();) {
    const auto& r = it->second.range;
    const bool hit = (r[1] > r[0] && r[0] < b && a < r[1]) || (r[3] > r[2] && r[2] < b && a < r[3]);
    if (hit) it = s.fmemo.erase(it), ++s.fmemo_forgets;
    else ++it;
  }
}

int PersistIndexMode(State& s) {
  if (s.persist_mode < 0) {
    s.persist_mode = 1;
    if (const char* e = std::getenv("WWE2K14_D3D_PERSIST_INDEX"); e && *e) s.persist_mode = std::atoi(e);
    if (s.persist_mode != 1) REXLOG_INFO("[d3d-tex] persistent index mode {} (B1-a; 0 old walk, 2 CHECK)", s.persist_mode);
  }
  return s.persist_mode;
}

void IdxRemove(State& s, const std::string& key, const Entry& e) {
  if (!e.persistent_lineage) return;  // the walk matched lineage == r.lineage_id >= 1: lineage 0 never matches
  auto it = s.persist_idx.find({e.persistent_res_start, e.persistent_lineage});
  if (it == s.persist_idx.end()) return;
  it->second.erase(key);
  if (it->second.empty()) s.persist_idx.erase(it);
}

void CachePut(State& s, const std::string& key, const Entry& ne) {
  auto old = s.cache.find(key);
  if (old != s.cache.end()) IdxRemove(s, key, old->second);
  s.cache[key] = ne;
  if (ne.persistent_lineage) s.persist_idx[{ne.persistent_res_start, ne.persistent_lineage}].insert(key);
}

void DropEntry(State& s, std::unordered_map<std::string, Entry>::iterator it) {
  IdxRemove(s, it->first, it->second);
  FMemoForgetHandle(s, it->second.handle);
  if (it->second.res_seq) {
    auto r = s.resolves.find(it->second.res_start);
    if (r != s.resolves.end() && r->second.seq == it->second.res_seq) {
      auto& d = r->second.derived;
      d.erase(std::remove(d.begin(), d.end(), it->first), d.end());
    }
  }
  BackendRelease(it->second.handle);
  if (s.c.live) --s.c.live;
  s.c.live_bytes -= std::min(s.c.live_bytes, it->second.bytes);
  s.cache.erase(it);
  ++s.epoch;
}

// Card S3-33b: fingerprint of a destination's guest bytes (64 dwords spread over the extent). The write watch also
// fires without a content change (allocation / protection events when the game recycles memory between matches); a
// changed fingerprint means the game really wrote the memory.
uint64_t SampleHash(State& s, uint32_t start, uint32_t length) {
  if (!s.memory || length < 4) return 0;
  uint64_t h = 1469598103934665603ull;
  const uint32_t n = 64, step = std::max<uint32_t>(4, (length / n) & ~3u);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t off = std::min(i * step, (length - 4) & ~3u);
    uint32_t v;
    rr::NotePhys(start + off, 4);
    std::memcpy(&v, s.memory->TranslatePhysical<const uint8_t*>(start + off), 4);
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}
// Card S3-69 diagnostic: zero dwords among 256 sampled over [start, start+length) (never-written memory vs data).
uint32_t SampleZeros(State& s, uint32_t start, uint32_t length) {
  if (!s.memory || length < 4) return 0;
  uint32_t z = 0;
  const uint32_t n = 256, step = std::max<uint32_t>(4, (length / n) & ~3u);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t off = std::min(i * step, (length - 4) & ~3u);
    uint32_t v;
    rr::NotePhys(start + off, 4);
    std::memcpy(&v, s.memory->TranslatePhysical<const uint8_t*>(start + off), 4);
    z += v == 0;
  }
  return z;
}
// Card S3-69: bytes of the whole destination (pitch x height rows, 32-row aligned) a resolve of this kind writes into.
uint32_t DestSpan(int kind, uint32_t pitch_aligned, uint32_t dest_h) {
  const uint32_t bpp = kind == 1 ? 1 : kind == 5 ? 2 : kind == 3 ? 8 : 4;
  const uint64_t b = uint64_t(pitch_aligned) * ((dest_h + 31) & ~31u) * bpp;
  return uint32_t(std::min<uint64_t>(b, 64ull << 20));
}
// Card S3-69 5a: a destination of this kind/endian can be seeded from guest memory. RGBA8 also supports the
// copy-destination R/B swap: SeedCanvasBytes inverts that swap in the host texels before the resolve's adjusted fetch
// swizzle is applied. The other supported kinds retain the no-swap restriction from the M2c6 path.
bool SeedableRes(int kind, bool swap_rb, uint32_t endian) {
  return endian <= 3 && (kind == 0 || (!swap_rb && (kind == 3 || kind == 4)));
}
// The resolved image uses a fetch swizzle with R/B selectors exchanged when swap_rb is set. For guest-memory seed
// texels, invert that selector adjustment by swapping R/B in the decoded RGBA8 bytes. This makes adjusted-swizzle(seed)
// equal the ordinary fetch-swizzle(guest-memory) for uncovered texels.
void SeedCanvasBytes(int kind, bool swap_rb, std::vector<uint8_t>* bytes) {
  if (!swap_rb || kind != 0) return;
  for (size_t i = 0; i + 3 < bytes->size(); i += 4) std::swap((*bytes)[i], (*bytes)[i + 2]);
}
// The existing compute detile applies the Xenos endian permutation to each output dword. Pre-permute the bounded
// uploaded RGBA8 source as E^-1 * RB * E so the shader's E produces the same bytes as SeedCanvasBytes(E(source)).
// This is a linear channel permutation only; the tiling address walk remains exclusively in the GPU detile shader.
void SeedCanvasSourceBytes(int kind, bool swap_rb, uint32_t endian, std::vector<uint8_t>* bytes) {
  if (!swap_rb || kind != 0) return;
  auto endian_permute = [endian](uint8_t* p) {
    switch (endian) {
      case 1: std::swap(p[0], p[1]); std::swap(p[2], p[3]); break;
      case 2: std::swap(p[0], p[3]); std::swap(p[1], p[2]); break;
      case 3: std::swap(p[0], p[2]); std::swap(p[1], p[3]); break;
      default: break;
    }
  };
  for (size_t i = 0; i + 3 < bytes->size(); i += 4) {
    uint8_t* p = bytes->data() + i;
    endian_permute(p);
    std::swap(p[0], p[2]);
    endian_permute(p);  // the four supported dword permutations are self-inverse
  }
}
// Card S3-69 5a: a synthetic texture key describing the whole destination (pitch_aligned x dest_h) in the resolve's
// own format, for detiling its guest bytes exactly as a fetch of the copy-out would. The pitch is in 32-texel units
// (as GetGuestTextureLayout's parameter and the fetch key's field). false when no plan exists (never for seedable
// kinds); *key is left invalid.
bool CanvasSeedKey(uint32_t dest_base, int kind, uint32_t endian, uint32_t pitch_aligned, uint32_t w, uint32_t h,
                   TexKey* key, sh::TexFormatPlan* plan) {
  key->MakeInvalid();
  key->base_page = dest_base >> 12;
  key->dimension = xenos::DataDimension::k2DOrStacked;
  key->width_minus_1 = w - 1;
  key->height_minus_1 = h - 1;
  key->depth_or_array_size_minus_1 = 0;
  key->tiled = 1;
  key->pitch = pitch_aligned / 32;
  key->mip_max_level = 0;
  key->format = kind == 3 ? xenos::TextureFormat::k_16_16_16_16 : kind == 4 ? xenos::TextureFormat::k_16_16
                                                                             : xenos::TextureFormat::k_8_8_8_8;
  key->endianness = xenos::Endian(endian);
  key->is_valid = 1;
  xe_gpu_texture_fetch_t fetch = {};
  std::string why;
  return sh::PlanTextureFormat(fetch, *key, plan, &why);
}
bool CanvasSeedSource(State& s, const Res& r, uint32_t w, uint32_t h, TexKey* key, sh::TexFormatPlan* plan,
                      TextureGuestLayout* layout, sh::LevelGeom* geom, uint32_t* source, uint32_t* source_bytes,
                      uint64_t* source_gen) {
  if (!s.memory || !SeedableRes(r.kind, r.swap_rb, r.endian) ||
      !CanvasSeedKey(r.dest_base, r.kind, r.endian, r.pitch_aligned, w, h, key, plan)) return false;
  *layout = key->GetGuestLayout();
  *geom = sh::ComputeLevelGeom(*key, *layout, *plan, 0);
  const uint64_t end = uint64_t(geom->address) + layout->base.level_data_extent_bytes;
  if (!layout->base.level_data_extent_bytes || end > kPhysLimit) return false;
  *source = geom->address;
  *source_bytes = layout->base.level_data_extent_bytes;
  ArmIfNeeded(s, *source, *source_bytes);
  const std::array<uint32_t, 4> range{*source, *source + *source_bytes, 0, 0};
  *source_gen = s.watch->GenSum(range);
  return true;
}
using SeedCanvasKey = std::pair<uint32_t, uint32_t>;
void DropSeedCanvas(State& s, std::map<SeedCanvasKey, SeedCanvas>::iterator it) {
  if (it->second.snapshot != nb::kNone) nb::ReleaseSnapshot(it->second.snapshot);
  s.seed_canvas_bytes -= std::min(s.seed_canvas_bytes, it->second.image_bytes);
  s.seed_canvases.erase(it);
  dest::C().seed_cache_bytes = s.seed_canvas_bytes;
  ++dest::C().canvas_cache_evict;
}
bool SeedCacheDescriptorMatches(const SeedCanvas& c, const Res& r) {
  return r.dest_texture && c.info == r.info && c.kind == r.kind && c.pitch == r.pitch_aligned && c.dest_h == r.dest_h;
}
bool ByteRangesOverlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
  return a1 > a0 && b1 > b0 && a0 < b1 && b0 < a1;
}
// A different D3D destination object (or incompatible resolve layout) writing any part of a cached destination makes
// the old composed image ineligible. Same-object, same-layout resolves update that image and remain cacheable.
void InvalidateSeedCanvases(State& s, const Res& incoming, uint32_t phys_base, uint32_t extent, bool same_object_update) {
  if (!dest::CanvasReuseOn() || s.seed_canvases.empty()) return;
  const SeedCanvasKey key{incoming.dest_base, incoming.dest_texture};
  const uint64_t write_lo = phys_base, write_hi = uint64_t(phys_base) + extent;
  const uint32_t new_span = incoming.kind >= 0 ? DestSpan(incoming.kind, incoming.pitch_aligned, incoming.dest_h) : 0;
  const uint64_t dest_lo = incoming.dest_base, dest_hi = dest_lo + new_span;
  for (auto it = s.seed_canvases.begin(); it != s.seed_canvases.end();) {
    const SeedCanvas& c = it->second;
    const uint64_t cache_lo = it->first.first, cache_hi = cache_lo + c.dest_span;
    const bool overlaps = ByteRangesOverlap(cache_lo, cache_hi, write_lo, write_hi) ||
                          ByteRangesOverlap(cache_lo, cache_hi, dest_lo, dest_hi);
    const bool same = same_object_update && it->first == key && SeedCacheDescriptorMatches(c, incoming);
    if (same || (!overlaps && it->first.first != incoming.dest_base)) {
      ++it;
    } else {
      auto drop = it++;
      DropSeedCanvas(s, drop);
    }
  }
}
// Keep one extra reference to each retained canvas, bounded by 32 MiB and 64 destinations. The Res still owns its own
// reference; cache eviction only drops this extra lifetime. `source_gen` is the write-watch baseline for the complete
// tiled guest range from which the seeded image was decoded, not the host canvas after resolve rectangles are overlaid.
void RememberSeedCanvas(State& s, const Res& r, uint32_t source, uint32_t source_bytes, uint64_t source_gen) {
  if (!dest::CanvasReuseOn() || !r.dest_texture || !r.canvas || !r.seeded || r.seed_pending || r.snapshot == nb::kNone ||
      !source_bytes || r.dest_base >= kPhysLimit) return;
  const uint64_t image_bytes = uint64_t(r.w) * r.h * (r.kind == 3 ? 8u : 4u);
  if (!image_bytes || image_bytes > kSeedCanvasCacheBytes) return;
  const uint32_t retained = nb::RetainSnapshot(r.snapshot);
  if (retained == nb::kNone) return;
  const SeedCanvasKey key{r.dest_base, r.dest_texture};
  auto old = s.seed_canvases.find(key);
  if (old != s.seed_canvases.end()) DropSeedCanvas(s, old);
  SeedCanvas c;
  c.snapshot = retained;
  c.dest_span = DestSpan(r.kind, r.pitch_aligned, r.dest_h);
  c.info = r.info;
  c.kind = r.kind;
  c.pitch = r.pitch_aligned;
  c.dest_h = r.dest_h;
  c.w = r.w;
  c.h = r.h;
  c.source = source;
  c.source_bytes = source_bytes;
  c.source_gen = source_gen;
  c.last_use = s.frame;
  c.length = r.length;
  c.start = r.start;
  c.cov_cols = r.cov_cols;
  c.cov = r.cov;
  c.image_bytes = image_bytes;
  s.seed_canvas_bytes += image_bytes;
  s.seed_canvases.emplace(key, std::move(c));
  while (s.seed_canvas_bytes > kSeedCanvasCacheBytes || s.seed_canvases.size() > kSeedCanvasCacheEntries) {
    auto victim = s.seed_canvases.end();
    for (auto it = s.seed_canvases.begin(); it != s.seed_canvases.end(); ++it) {
      if (it->first == key) continue;
      if (victim == s.seed_canvases.end() || it->second.last_use < victim->second.last_use) victim = it;
    }
    if (victim == s.seed_canvases.end()) break;
    DropSeedCanvas(s, victim);
  }
  dest::C().seed_cache_bytes = s.seed_canvas_bytes;
}
void UpdateSeedCanvasMetadata(State& s, const Res& r) {
  const SeedCanvasKey key{r.dest_base, r.dest_texture};
  auto it = s.seed_canvases.find(key);
  if (it == s.seed_canvases.end() || !SeedCacheDescriptorMatches(it->second, r) ||
      it->second.w != r.w || it->second.h != r.h) return;
  SeedCanvas& c = it->second;  // its retained id names the same shared image just updated by the live Res
  c.length = r.length;
  c.start = r.start;
  c.cov_cols = r.cov_cols;
  c.cov = r.cov;
  c.last_use = s.frame;
}
// Card S3-69 5a: XXH3 over the 8x8 cells of a tight detiled image (`bytes` = row_w x max_h texels of bpb bytes each)
// that the coverage map leaves uncovered, clipped to (max_w, max_h). Both the seeded canvas bytes (row_w = canvas
// width) and the fetch's own decode (row_w = fetch width) are hashed through it for the same cells. `st` must be a
// created XXH3 state; the caller resets and digests it.
uint64_t CellHash(const std::vector<uint8_t>& bytes, uint32_t row_w, uint32_t max_w, uint32_t max_h, uint32_t bpb,
                  const std::vector<uint8_t>& cov, uint32_t cov_cols, XXH3_state_t* st, bool swap_rb = false) {
  for (uint32_t cy = 0; cy < (max_h + 7) / 8; ++cy) {
    for (uint32_t cx = 0; cx < (max_w + 7) / 8; ++cx) {
      if (cov[size_t(cy) * cov_cols + cx]) continue;
      const uint32_t cw = std::min(8u, max_w - cx * 8), ch = std::min(8u, max_h - cy * 8);
      for (uint32_t i = 0; i < ch; ++i) {
        const uint8_t* row = bytes.data() + (size_t(cy) * 8 + i) * row_w * bpb + size_t(cx) * 8 * bpb;
        if (swap_rb && bpb == 4) {
          for (uint32_t x = 0; x < cw; ++x) {
            uint8_t px[4];
            std::memcpy(px, row + size_t(x) * 4, sizeof(px));
            std::swap(px[0], px[2]);
            XXH3_64bits_update(st, px, sizeof(px));
          }
        } else {
          XXH3_64bits_update(st, row, cw * bpb);
        }
      }
    }
  }
  return XXH3_64bits_digest(st);
}

// Card S3-33e (WWE2K14_D3D_MATERIALIZE=1): draw-off never writes resolve output to guest memory, so a guest-byte
// fallback over a resolve-written region reads junk (s337/s341/s342). Before such a fallback, write the overlapping
// entries' snapshot bytes into guest memory once (GPU readback, as the S3-17 CPU-visible path), then read normally.
bool MaterializeOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_MATERIALIZE");
    return e && std::strcmp(e, "1") == 0;
  }();
  return on;
}
bool StaleSampleOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_STALE_SAMPLE");
    return e && std::strcmp(e, "1") == 0;
  }();
  return on;
}

// Card S3-41 diagnostic (WWE2K14_D3D_RES_TRACE=1): kind-5 (4444 destination) entries of >= 256 pixels wide -
// registrations and drops with the reason, to find the writer of the assembled k_4_4_4_4 texture's base level.
bool ResTraceOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_RES_TRACE");
    return e && std::strcmp(e, "1") == 0;
  }();
  return on;
}
bool ResTraced(const Res& r) { return ResTraceOn() && r.kind == 5 && r.w >= 256; }
int ResolveIntoMode() {
  static const int mode = [] {
    const char* e = std::getenv("WWE2K14_D3D_RESOLVE_INTO_TEX");
    if (!e) return 0;
    const int v = int(std::strtol(e, nullptr, 10));
    return v == 1 || v == 2 ? v : 0;
  }();
  return mode;
}
bool ResolveIntoOn() { return ResolveIntoMode() != 0; }
bool ResolveIntoCheckOn() { return ResolveIntoMode() == 2; }

const Res::RectWrite* ResolveRectCovering(const Res& r, uint32_t x, uint32_t y) {
  for (auto it = r.resolve_rect_history.rbegin(); it != r.resolve_rect_history.rend(); ++it) {
    const auto& q = it->rect;
    if (int32_t(x) >= q[0] && int32_t(y) >= q[1] && int32_t(x) < q[2] && int32_t(y) < q[3]) return &*it;
  }
  const auto& q = r.resolve_rect;
  if (int32_t(x) >= q[0] && int32_t(y) >= q[1] && int32_t(x) < q[2] && int32_t(y) < q[3]) return nullptr;
  return nullptr;
}

void LogResolveIntoFirstDifference(const char* path, const TexKey& key, uint32_t base, uint32_t width,
                                   uint32_t height, uint32_t level, uint32_t x, uint32_t y, uint32_t bpp,
                                   const uint8_t* old_texel, const uint8_t* new_texel, const Res& resolve,
                                   uint64_t persistent_seq) {
  using Pair = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t>;
  static std::set<Pair> logged_pairs;
  const Pair pair{base, uint32_t(key.format), width, height, uint32_t(key.mip_max_level), level,
                  resolve.start, resolve.seq};
  if (!logged_pairs.insert(pair).second) return;

  uint64_t old_integer = 0, new_integer = 0;
  std::memcpy(&old_integer, old_texel, std::min<uint32_t>(bpp, sizeof(old_integer)));
  std::memcpy(&new_integer, new_texel, std::min<uint32_t>(bpp, sizeof(new_integer)));
  const Res::RectWrite* covering = ResolveRectCovering(resolve, x, y);
  const auto& rect = covering ? covering->rect : std::array<int32_t, 4>{resolve.resolve_rect[0],
                                                                         resolve.resolve_rect[1],
                                                                         resolve.resolve_rect[2],
                                                                         resolve.resolve_rect[3]};
  const bool rect_covers = int32_t(x) >= rect[0] && int32_t(y) >= rect[1] &&
                           int32_t(x) < rect[2] && int32_t(y) < rect[3];
  const char* ordering = resolve.seq > persistent_seq ? "old-newer" :
                         resolve.seq < persistent_seq ? "old-older" : "same";
  REXLOG_WARN("[d3d-resolve-into-first] path={} fetch_base={:08X} fetch_fmt={} fetch_size={}x{} mip_max={} "
              "level={} xy={},{} bpp={} old_integer=0x{:016X} new_integer=0x{:016X} "
              "resolve_rect={},{},{},{} rect_covers_xy={} covering_rect_seq={} resolve_seq={} "
              "persistent_last_update_seq={} old_snapshot_order={}",
              path, base, uint32_t(key.format), width, height, uint32_t(key.mip_max_level), level, x, y, bpp,
              old_integer, new_integer, rect[0], rect[1], rect[2], rect[3], rect_covers,
              covering ? covering->seq : resolve.seq, resolve.seq, persistent_seq, ordering);
}

// Card S3-33c-select-silhouettes stage 0 diagnostic (WWE2K14_D3D_BIND_TRACE=1; remove when settled): which texture
// (guest bytes or resolve-derived, content id) the >= 256x256 fetches bind, and guest hits whose bytes changed although
// the write watch reported them clean.
bool BindTraceOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_BIND_TRACE");
    return e && std::strcmp(e, "1") == 0;
  }();
  return on;
}

// Card S3-69 B2-fix3: bounded trace-only cross-check of the bytes behind the texture handle returned to the draw
// against a fresh detile of the current DeviceState fetch's guest address. ReadTexture reads the live host image once;
// this never feeds a renderer decision.
void TraceBoundContent(State& s, const uint32_t* fw, uint32_t slot, uint32_t handle, const char* event,
                       uint32_t previous_obj, uint32_t bound_obj) {
  if (!dest::TraceOn()) return;
  static uint32_t bypass_checks = 0, memo_object_checks = 0;
  uint32_t& checks = std::strcmp(event, "obj_bypass") == 0 ? bypass_checks : memo_object_checks;
  if (checks >= 8) return;
  ++checks;

  xe_gpu_texture_fetch_t fetch = {};
  std::memcpy(&fetch, fw, sizeof(fetch));
  TexKey key;
  uint8_t signs = 0;
  sh::TextureKeys::BindingInfoFromFetchConstant(fetch, key, &signs);
  sh::TexFormatPlan plan;
  std::string status = "ok";
  const bool valid_key = key.is_valid;
  const bool valid_plan = valid_key && sh::PlanTextureFormat(fetch, key, &plan, &status);
  const uint32_t width = valid_key ? key.GetWidth() : 0;
  const uint32_t height = valid_key ? key.GetHeight() : 0;
  const uint32_t base = valid_key ? uint32_t(key.base_page) << 12 : 0;
  uint64_t guest_hash = 0, host_hash = 0;
  std::vector<uint8_t> guest, host;
  bool guest_ok = false, host_ok = false;
  if (!valid_key) {
    status = "invalid-fetch";
  } else if (!valid_plan) {
    status = "format-plan:" + status;
  } else if (!s.memory || !width || !height) {
    status = "no-memory-or-size";
  } else {
    const TextureGuestLayout layout = key.GetGuestLayout();
    const sh::LevelGeom g0 = sh::ComputeLevelGeom(key, layout, plan, 0);
    const uint64_t guest_end = uint64_t(g0.address) + layout.base.level_data_extent_bytes;
    if (!layout.base.level_data_extent_bytes || guest_end > kPhysLimit) {
      status = "guest-range";
    } else {
      nb::LevelData level;
      guest_ok = sh::DetileLevelCpu(key, plan, g0, 0, 0,
                                    s.memory->TranslatePhysical<const uint8_t*>(g0.address), &level);
      if (guest_ok && g0.lw == width && g0.lh == height) {
        guest = std::move(level.bytes);
        guest_hash = XXH3_64bits(guest.data(), guest.size());
      } else {
        guest_ok = false;
        status = "guest-detile";
      }
      std::string read_error;
      host_ok = handle != nb::kNone && nb::ReadTexture(handle, plan.format, width, height, host, &read_error);
      if (host_ok) host_hash = XXH3_64bits(host.data(), host.size());
      else if (status == "ok") status = "host-readback:" + read_error;
    }
  }
  const bool compared = guest_ok && host_ok && guest.size() == host.size();
  const char* match = compared ? (guest == host ? "true" : "false") : "unknown";
  if (compared && guest != host && status == "ok") status = "byte-mismatch";
  REXLOG_INFO("[d3d-dest] bind-content frame={} event={} match={} status={} slot={} base={:08X} fmt{} {}x{} level=0 "
              "obj={:08X}->{:08X} handle={} bytes={} guest={:016x} host={:016x}",
              s.frame, event, match, status, slot, base, valid_key ? uint32_t(key.format) : UINT32_MAX, width, height,
              previous_obj, bound_obj, handle, compared ? guest.size() : 0, guest_hash, host_hash);
}

void DropResolve(State& s, std::map<uint32_t, Res>::iterator it, const char* reason = "") {
  Res& r = it->second;
  if (dest::TraceOn()) dest::NoteDrop(r.dest_base, r.seq, s.frame, reason);  // card S3-69 diagnostic
  if (ResTraced(r)) {
    REXLOG_INFO("[d3d-rtrace] drop frame={} {:08X}+{:X} seq={} reg_frame={} {}x{} canvas={} derived={} reason={}", s.frame,
                r.start, r.length, r.seq, r.reg_frame, r.w, r.h, r.canvas, r.derived.size(), reason);
  }
  for (const std::string& k : r.derived) {
    auto e = s.cache.find(k);
    if (e == s.cache.end()) continue;
    FMemoForgetHandle(s, e->second.handle);
    BackendRelease(e->second.handle);
    if (s.c.live) --s.c.live;
    s.c.live_bytes -= std::min(s.c.live_bytes, e->second.bytes);
    IdxRemove(s, e->first, e->second);
    s.cache.erase(e);
  }
  {
    const int mode = PersistIndexMode(s);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> doomed;
    if (mode != 0) {
      auto ix = s.persist_idx.find({r.start, r.lineage_id});
      if (ix != s.persist_idx.end()) doomed.assign(ix->second.begin(), ix->second.end());
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (mode != 1) {
      std::vector<std::string> walk;
      for (const auto& kv : s.cache)
        if (kv.second.persistent_res_start == r.start && kv.second.persistent_lineage == r.lineage_id) walk.push_back(kv.first);
      s.persist_scan_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t1).count());
      if (mode == 2) {
        std::sort(walk.begin(), walk.end());
        ++s.persist_check_n;
        if (walk != doomed) {  // doomed comes from a std::set: already sorted
          if (++s.persist_check_bad <= 8)
            REXLOG_WARN("[d3d-tex] persistent index CHECK mismatch start={:08X} lineage={} index={} walk={}", r.start,
                        r.lineage_id, doomed.size(), walk.size());
          doomed = walk;  // CHECK keeps the old behaviour
        }
      } else {
        doomed = std::move(walk);
      }
    }
    s.persist_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    ++s.persist_drops;
    for (const std::string& k : doomed) {
      auto e = s.cache.find(k);
      if (e != s.cache.end()) DropEntry(s, e);
    }
  }
  if (!TranslateOnly() && r.snapshot != nb::kNone) nb::ReleaseSnapshot(r.snapshot);  // the id WP6 handed over (RetainSnapshot'ed by WP6)
  s.resolves.erase(it);
  ++s.epoch;
}

// Frees room for one more shadow texture: releases the least recently used entries not used this frame.
bool MakeRoom(State& s) {
  if (s.c.live < s.cap) return true;
  std::vector<std::pair<uint64_t, std::string>> old;
  for (const auto& kv : s.cache) {
    if (kv.second.last_use < s.frame) old.emplace_back(kv.second.last_use, kv.first);
  }
  if (old.empty()) return false;
  const size_t want = std::min<size_t>(old.size(), std::max<size_t>(32, s.cap / 16));
  std::partial_sort(old.begin(), old.begin() + want, old.end());
  for (size_t i = 0; i < want; ++i) {
    auto it = s.cache.find(old[i].second);
    if (it != s.cache.end()) {
      DropEntry(s, it);
      ++s.c.evictions;
    }
  }
  return s.c.live < s.cap;
}

// Card M2b2-B: room for `need` more host bytes under WWE2K14_D3D_SHADOW_TEX_MB. Evicts LRU entries not used this frame;
// false (and counted) when still over the cap.
bool MakeByteRoom(State& s, uint64_t need) {
  if (s.c.live_bytes + need <= s.byte_cap) return true;
  std::vector<std::pair<uint64_t, std::string>> old;
  for (const auto& kv : s.cache) {
    if (kv.second.last_use < s.frame) old.emplace_back(kv.second.last_use, kv.first);
  }
  std::sort(old.begin(), old.end());
  for (const auto& o : old) {
    if (s.c.live_bytes + need <= s.byte_cap) break;
    auto it = s.cache.find(o.second);
    if (it != s.cache.end()) {
      DropEntry(s, it);
      ++s.c.evictions;
    }
  }
  if (s.c.live_bytes + need <= s.byte_cap) return true;
  ++s.c.refused_bytes;
  return false;
}

void NoteCreated(State& s) {
  ++s.c.live;
  s.c.live_high = std::max(s.c.live_high, s.c.live);
}

// Resolve entries overlapping [a, b). Entries never overlap each other, so one backwards walk finds them all.
std::vector<Res*> Overlapping(State& s, uint32_t a, uint32_t b) {
  std::vector<Res*> out;
  if (b <= a) return out;
  auto it = s.resolves.lower_bound(b);  // first start >= b
  while (it != s.resolves.begin()) {
    --it;
    if (uint64_t(it->second.start) + it->second.length <= a) break;
    out.push_back(&it->second);
  }
  return out;
}

std::string FmtSize(uint32_t fmt, uint32_t w, uint32_t h) {
  char b[48];
  std::snprintf(b, sizeof(b), "fmt%u %ux%u", fmt, w, h);
  return b;
}

// S3-3 perf probe: texture sub-step times (ms/frame logged every 300 frames by OnWorkerFrameEnd).
struct TxProf { uint64_t ns[6] = {}, n[6] = {}, bytes_hashed = 0; } g_txp;
uint32_t seed_trace_checks = 0;  // B2-perf2: first eight GPU seed outputs compared with CPU detile in DEST_TRACE mode
const char* const kTxName[6] = {"forslot_total", "revalidate_hash", "new_hash", "cpu_detile", "upload_create", "resolve_derived"};
inline uint64_t TxNow() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct TxT {
  int k; uint64_t t = TxNow();
  explicit TxT(int kk) : k(kk) {}
  ~TxT() { g_txp.ns[k] += TxNow() - t; ++g_txp.n[k]; }
};

// Card M2c5: bytes per texel a resolve of this kind writes when fetched as `fmt` (0 = this kind cannot produce `fmt`;
// same pairs as the single-image check in ForSlot).
uint32_t KindBpt(int kind, xenos::TextureFormat fmt, xenos::Endian endian) {
  switch (kind) {
    case 0: return fmt == xenos::TextureFormat::k_8_8_8_8 ? 4 : 0;
    case 1: return fmt == xenos::TextureFormat::k_8 && endian != xenos::Endian::k16in32 ? 1 : 0;
    case 2: return fmt == xenos::TextureFormat::k_24_8_FLOAT ? 4 : 0;
    case 3: return fmt == xenos::TextureFormat::k_16_16_16_16 ? 8 : 0;
    case 4: return fmt == xenos::TextureFormat::k_16_16 ? 4 : 0;
    case 5: return fmt == xenos::TextureFormat::k_4_4_4_4 ? 2 : 0;
  }
  return 0;
}
// Host swizzle for a fetch of resolve entry r (the kind's channel layout, R/B swap of the copy).
void ResSwizzle(const Res& r, uint32_t fetch_swizzle, uint8_t sw[4]) {
  for (int c = 0; c < 4; ++c) {
    uint8_t v = uint8_t((fetch_swizzle >> (3 * c)) & 7);
    if (r.kind == 1) {
      if (v <= 3) v = r.swap_rb ? 2 : 0;  // one channel: byte 0 of the (R/B swapped) texel
    } else if (r.kind == 2) {
      if (v >= 1 && v <= 3) v = 0;
    } else if (r.kind == 4) {
      if (v >= 2 && v <= 3) v = 1;
    } else if (r.swap_rb && (v == 0 || v == 2)) {
      v = uint8_t(2 - v);
    }
    sw[c] = v;
  }
}
nb::TexFormat KindTexFormat(int kind) {
  return kind == 2 ? nb::TexFormat::kR32F : kind == 3 ? nb::TexFormat::kRGBA16 : kind == 4 ? nb::TexFormat::kRG16
                                                                                         : nb::TexFormat::kRGBA8;
}

enum class SeedReason : uint8_t { kBind, kWriteBack, kPropagation };
seed_lazy::Observer SeedObserver(SeedReason reason) {
  switch (reason) {
    case SeedReason::kBind: return seed_lazy::Observer::kUncoveredBind;
    case SeedReason::kWriteBack: return seed_lazy::Observer::kWriteBack;
    case SeedReason::kPropagation: return seed_lazy::Observer::kPropagation;
  }
  return seed_lazy::Observer::kNone;
}
void CountSeedMaterialized(State& s, SeedReason reason) {
  if (reason == SeedReason::kBind) ++s.seed_lazy.materialized_bind;
  else if (reason == SeedReason::kWriteBack) ++s.seed_lazy.materialized_writeback;
  else ++s.seed_lazy.materialized_propagation;
}

struct SeedInput {
  TexKey key;
  sh::TexFormatPlan plan;
  TextureGuestLayout layout;
  sh::LevelGeom geom{};
  uint32_t source = 0, source_bytes = 0;
  uint64_t source_gen = 0;
  const uint8_t* raw = nullptr;
  const uint8_t* gpu = nullptr;
  std::vector<uint8_t> transformed;
};

bool PrepareSeedInput(State& s, const Res& r, SeedInput* input) {
  if (!input || !CanvasSeedSource(s, r, r.w, r.h, &input->key, &input->plan, &input->layout, &input->geom,
                                 &input->source, &input->source_bytes, &input->source_gen)) return false;
  if (!s.memory || input->source != r.seed_source || input->source_bytes != r.seed_source_bytes) return false;
  input->raw = s.memory->TranslatePhysical<const uint8_t*>(input->geom.address);
  rr::NotePhys(input->geom.address, input->source_bytes);
  input->gpu = input->raw;
  if (r.swap_rb && r.kind == 0) {
    input->transformed.assign(input->raw, input->raw + input->source_bytes);
    SeedCanvasSourceBytes(r.kind, r.swap_rb, r.endian, &input->transformed);
    input->gpu = input->transformed.data();
  }
  return true;
}

bool SeedSnapshotFromInput(State& s, Res& r, uint32_t target, std::string* error) {
  SeedInput input;
  if (!PrepareSeedInput(s, r, &input)) {
    if (error) *error = "seed source unavailable or changed layout";
    return false;
  }
  const nb::GpuLevel gpu_params = sh::MakeGpuLevel(input.key, input.plan, input.geom, 0, 0);
  nb::SeedDetileStats gpu_stats;
  std::string err;
  const bool detiled = !nb::ShaderFormatIsDxil() &&
                       nb::DetileSnapshotGpu(target, gpu_params, input.source_bytes, input.gpu, nullptr, &gpu_stats, &err);
  dest::C().seed_upload_ns += gpu_stats.upload_ns;
  dest::C().seed_upload_calls += gpu_stats.padded_upload_bytes != 0;
  dest::C().seed_upload_bytes += gpu_stats.source_bytes;
  dest::C().seed_upload_padded_bytes += gpu_stats.padded_upload_bytes;
  dest::C().seed_gpu_submit_ns += gpu_stats.dispatch_submit_ns;
  dest::C().seed_gpu_calls += gpu_stats.dispatch_submit_ns != 0;
  if (detiled) return true;

  std::vector<nb::LevelData> level(1);
  const uint64_t start = TxNow();
  const bool decoded = sh::DetileLevelCpu(input.key, input.plan, input.geom, 0, 0, input.raw, &level[0]);
  dest::C().seed_detile_ns += TxNow() - start;
  ++dest::C().seed_detile_calls;
  if (decoded) SeedCanvasBytes(r.kind, r.swap_rb, &level[0].bytes);
  if (!decoded || !nb::FillSnapshotRows(target, r.w, r.h, level[0].bytes, &err)) {
    if (error) *error = err.empty() ? "CPU seed detile failed" : err;
    return false;
  }
  return true;
}

uint64_t SeedHash(const uint8_t* bytes, const seed_lazy::PixelCoverage& coverage, uint32_t w, uint32_t h,
                  uint32_t pixel_bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      if (coverage.Covered(x, y)) continue;
      const uint8_t* p = bytes + (size_t(y) * w + x) * pixel_bytes;
      for (uint32_t c = 0; c < pixel_bytes; ++c) hash = (hash ^ p[c]) * 1099511628211ull;
    }
  }
  return hash;
}

bool SeedCheckRateAllowed() {
  static auto last = std::chrono::steady_clock::time_point::min();
  const auto now = std::chrono::steady_clock::now();
  if (last != std::chrono::steady_clock::time_point::min() && now - last < std::chrono::milliseconds(250)) return false;
  last = now;
  return true;
}

bool CheckSeedCanvas(State& s, Res& r) {
  auto fail = [&](const char* status) {
    ++s.seed_lazy.checks;
    ++s.seed_lazy.mismatch;
    REXLOG_INFO("[d3d-seed-check] CHECK frame={} seq={} base={:08X} status={} checks={} mismatch={}",
                s.frame, r.seq, r.dest_base, status, s.seed_lazy.checks, s.seed_lazy.mismatch);
    return false;
  };
  if (!r.seed_coverage || TranslateOnly()) return fail("no-coverage-or-backend");
  SeedInput input;
  std::vector<nb::LevelData> expected(1);
  if (!PrepareSeedInput(s, r, &input) ||
      !sh::DetileLevelCpu(input.key, input.plan, input.geom, 0, 0, input.raw, &expected[0])) return fail("cpu-detile");
  SeedCanvasBytes(r.kind, r.swap_rb, &expected[0].bytes);
  const uint32_t pixel_bytes = input.plan.bpb;
  const size_t expected_size = size_t(r.w) * r.h * pixel_bytes;
  if (!pixel_bytes || expected[0].bytes.size() < expected_size) return fail("expected-size");

  nb::TextureDesc desc;
  desc.width = r.w;
  desc.height = r.h;
  desc.format = KindTexFormat(r.kind);
  for (uint32_t i = 0; i < 4; ++i) desc.swizzle[i] = uint8_t(i);
  std::string err;
  const uint32_t view = nb::TextureFromSnapshot(r.snapshot, desc, &err);
  if (view == nb::kNone) return fail("snapshot-view");
  std::vector<uint8_t> actual;
  const bool read = nb::ReadTexture(view, desc.format, r.w, r.h, actual, &err);
  nb::ReleaseTexture(view);
  if (!read || actual.size() < expected_size) return fail("readback");
  const uint64_t expected_hash = SeedHash(expected[0].bytes.data(), r.seed_coverage->pixels, r.w, r.h, pixel_bytes);
  const uint64_t actual_hash = SeedHash(actual.data(), r.seed_coverage->pixels, r.w, r.h, pixel_bytes);
  const bool match = expected_hash == actual_hash;
  ++s.seed_lazy.checks;
  if (!match) ++s.seed_lazy.mismatch;
  REXLOG_INFO("[d3d-seed-check] CHECK frame={} seq={} base={:08X} seeded_hash={:016x} eager_hash={:016x} checks={} mismatch={} status={}",
              s.frame, r.seq, r.dest_base, expected_hash, actual_hash, s.seed_lazy.checks, s.seed_lazy.mismatch,
              match ? "match" : "mismatch");
  return match;
}

enum class SeedEnsureResult : uint8_t { kNotPending, kSkipped, kDone, kDirtyBind, kFailed };
SeedEnsureResult EnsureSeedMaterialized(State& s, Res& r, SeedReason reason) {
  if (!r.seed_pending) return SeedEnsureResult::kNotPending;
  const bool watch_clean = s.ignore_resolve_write || s.watch->Clean(r.range, true, r.gen_sum);
  const seed_lazy::Action action = seed_lazy::Decide(true, watch_clean, SeedObserver(reason));
  if (action == seed_lazy::Action::kSkip) {
    ++s.seed_lazy.would_skip;
    return SeedEnsureResult::kSkipped;
  }
  if (action == seed_lazy::Action::kRefuseDirtyBind) {
    ++s.seed_lazy.dirty_refused;
    return SeedEnsureResult::kDirtyBind;
  }
  if (!watch_clean) ++s.seed_lazy.dirty_propagation;
  ++s.seed_lazy.would_materialize;
  if (SeedLazyCheckMode()) {
    if (watch_clean && SeedCheckRateAllowed()) {  // dirty: the eager image holds older bytes by design
      if (!CheckSeedCanvas(s, r)) {
        ++s.seed_lazy.check_failed;
      }
    } else {
      ++s.seed_lazy.check_rate_skipped;
    }
    r.seed_pending = false;
    r.seed_coverage.reset();
    return SeedEnsureResult::kDone;
  }
  if (!r.seed_coverage || r.snapshot == nb::kNone) return SeedEnsureResult::kFailed;
  std::string err;
  const uint32_t temp = nb::NewSnapshotLike(r.snapshot, r.w, r.h, &err, false);
  if (temp == nb::kNone) return SeedEnsureResult::kFailed;
  bool ok = SeedSnapshotFromInput(s, r, temp, &err);
  std::vector<seed_lazy::Rect> rects;
  if (ok) ok = r.seed_coverage->pixels.UncoveredRects(&rects, kSeedLazyCopyRectLimit);
  if (ok) {
    for (const seed_lazy::Rect& rect : rects) {
      if (!nb::CopySnapshotRegion(r.snapshot, rect.x0, rect.y0, temp, rect.x0, rect.y0,
                                  rect.x1 - rect.x0, rect.y1 - rect.y0, &err)) {
        ok = false;
        break;
      }
    }
  }
  nb::ReleaseSnapshot(temp);
  if (!ok) return SeedEnsureResult::kFailed;
  r.seeded = true;  // also when resolves already cover every pixel: same bind path as an eager seed
  r.seed_pending = false;
  r.seed_coverage.reset();
  if (!rects.empty()) {
    CountSeedMaterialized(s, reason);
    if (r.seed_source_bytes) RememberSeedCanvas(s, r, r.seed_source, r.seed_source_bytes, r.seed_source_gen);
  } else {
    ++s.seed_lazy.would_skip;
  }
  return SeedEnsureResult::kDone;
}

}  // namespace

void SetMemory(rex::memory::Memory* memory) { S().memory = memory; }
void ForEachResolveEntry(const std::function<void(uint32_t, uint32_t, const ResolveDesc&, uint32_t,
                                                  const nb::ResolveCensusKey&)>& cb) {
  State& s = S();
  for (const auto& kv : s.resolves)
    if (kv.second.snapshot != nb::kNone) cb(kv.second.start, kv.second.length, kv.second.reg_desc, kv.second.snapshot,
                                            kv.second.census_key);
}
void ReplayMarkWritten(uint32_t start, uint32_t length) {
  if (!EnsureInit()) return;
  if (sh::TextureWatch* w = g_watch_ptr.load(std::memory_order_acquire)) w->MarkWritten(start, length, false);
}

void EnableSamplingQualityMetadata() {
  State& s = S();
  if (!s.quality) s.quality = std::make_unique<QualityState>();
}

quality::SamplingQualityInfo GetSamplingQualityInfo(uint32_t handle) {
  State& s = S();
  quality::SamplingQualityInfo info;
  if (!s.quality || !s.quality->valid || s.quality->uploads.overflowed() || TranslateOnly()) return info;
  QualityState& q = *s.quality;
  const QualityUpload* u = q.uploads.Find(handle);
  if (!u) return info;
  info = u->info;
  if (!info.ordinary_guest) return info;
  const quality::RangeEndpoints range{u->range[0], u->range[1], u->range[2], u->range[3]};
  info.resolve_target |= q.resolved.OverlapsEndpoints(range);
  quality::RangeRecord original;
  if (!q.writes.Find(u->range[0], &original) || !original.observed) {
    info.known = false;
    return info;
  }
  // EVERY binding rechecks the live watch, even when ForSlot returned a memo.
  // Do not refresh this baseline after a same-content rehash or cache reload.
  if (!s.watch || original.range != range ||
      !s.watch->Clean(u->range, true, original.first_generation))
    q.writes.MarkDirty(u->range[0]);
  info.cpu_updated |= q.writes.IsExcluded(u->range[0]) || IsDynamicBase(u->range[0]);
  info.late_upload |= late_upload::IsLateBase(u->range[0]);
  return info;
}

SamplingQualityMetadataStats GetSamplingQualityMetadataStats() {
  State& s = S();
  SamplingQualityMetadataStats stats;
  if (!s.quality) return stats;
  QualityState& q = *s.quality;
  stats.live = uint32_t(q.uploads.size());
  stats.peak = uint32_t(q.uploads.peak());
  stats.invalid_causes = q.invalid_causes;
  stats.history_records = q.writes.size();
  stats.history_overflow = q.writes.overflowed();
  // Read-only registry iteration against the texture cache's handles (diagnostics only).
  for (const auto& kv : q.uploads.records()) {
    bool live = false;
    for (const auto& e : s.cache) {
      if (e.second.handle == kv.first) {
        live = true;
        break;
      }
    }
    if (!live) ++stats.orphan_records;
  }
  return stats;
}
SamplingQualityFetchInfo GetSamplingQualityFetchInfo(const DeviceState& ds, uint32_t slot) {
  SamplingQualityFetchInfo info;
  if (slot >= kFetchSlots) return info;
  std::memcpy(info.words.data(), &ds.fetch[slot * 6], sizeof(info.words));
  xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, &ds.fetch[slot * 6], sizeof(fetch));
  TexKey key;
  uint8_t signs = 0;
  sh::TextureKeys::BindingInfoFromFetchConstant(fetch, key, &signs);
  if (!key.is_valid) return info;
  uint32_t min_level = 0, max_level = 0;
  rex::graphics::texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr,
                                                                 nullptr, nullptr, &min_level, &max_level);
  info.width = key.GetWidth();
  info.height = key.GetHeight();
  info.format = uint32_t(key.format);
  info.base = uint32_t(key.base_page) << 12;
  info.mip = uint32_t(key.mip_page) << 12;
  info.min_level = min_level;
  info.max_level = max_level;
  info.first_level = key.base_page ? 0u : 1u;
  info.claimed_levels = max_level >= info.first_level ? max_level - info.first_level + 1 : 0;
  info.valid = true;
  return info;
}
nr::shared::TextureWatch* SharedWatch() { return EnsureInit() ? S().watch.get() : nullptr; }
rex::memory::Memory* SharedMemory() { return EnsureInit() ? S().memory : nullptr; }
void SetDrawShaders(const rex::graphics::Shader* vs, const rex::graphics::Shader* ps) {
  S().vs = vs;
  S().ps = ps;
}
uint32_t LitShadowTexture(std::string* why) {
  if (TranslateOnly()) return 0;
  State& s = S();
  if (s.lit_shadow_texture != nb::kNone) return s.lit_shadow_texture;
  if (s.lit_shadow_texture_attempted) {
    if (why) *why = "prior 1x1 lit texture creation failed";
    return nb::kNone;
  }
  s.lit_shadow_texture_attempted = true;
  nb::TextureDesc desc;
  desc.width = desc.height = desc.levels = 1;
  desc.format = nb::TexFormat::kRGBA8;
  nb::LevelData level;
  level.width = level.height = 1;
  level.bytes = {255, 255, 255, 255};
  std::string error;
  s.lit_shadow_texture = nb::CreateTexture(desc, std::vector<nb::LevelData>{std::move(level)}, &error);
  if (s.lit_shadow_texture == nb::kNone) {
    if (why) *why = error.empty() ? "backend returned no texture handle" : error;
    return nb::kNone;
  }
  return s.lit_shadow_texture;
}
// Card S3-44 diagnostic (FLASH_TRACE): base-level guest range and format of the texture in fetch slot `slot`.
bool FetchBaseRange(const DeviceState& ds, uint32_t slot, uint32_t* start, uint32_t* length, uint32_t* format,
                    uint32_t* width) {
  if (slot >= kFetchSlots) return false;
  xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, &ds.fetch[slot * 6], sizeof(fetch));
  TexKey key;
  uint8_t signs = 0;
  sh::TextureKeys::BindingInfoFromFetchConstant(fetch, key, &signs);
  if (!key.is_valid) return false;
  const TextureGuestLayout layout = key.GetGuestLayout();
  *start = uint32_t(key.base_page) << 12;
  *length = layout.base.level_data_extent_bytes;
  *format = uint32_t(key.format);
  *width = key.GetWidth();
  return *length && uint64_t(*start) + *length <= kPhysLimit;
}
bool IsDynamicBase(uint32_t start) {
  std::lock_guard<std::mutex> lk(g_dyn_bases_mu);
  return g_dyn_bases.count(start) != 0;
}
std::string TakeFrameBinds() {
  std::string v;
  v.swap(S().flash_binds);
  return v;
}
const Counters& GetCounters() { return S().c; }
ResolveRegistrationFineStats TakeResolveRegistrationFineStats() {
  ResolveRegistrationFineStats out = g_resolve_registration_fine;
  g_resolve_registration_fine = {};
  return out;
}

void RegisterResolveImage(uint32_t phys_base, uint32_t extent, const ResolveDesc& r, uint32_t snapshot,
                          const nb::ResolveCensusKey& census_key) {
  State& s = S();
  // History intentionally survives stale resolve-entry removal and CPU memory
  // reuse. A page ever used as a destination cannot regain static eligibility.
  if (s.quality) {
    ResolveRegistrationFineScope fine_page_mark(ResolveRegistrationFineStage::kPageMark);
    if (ResolveRegistrationFineTimersOn() && extent && uint64_t(phys_base) + extent <= quality::ResolvePageMap::kGuestBytes) {
      const uint32_t first = phys_base >> quality::ResolvePageMap::kPageShift;
      const uint32_t last = uint32_t((uint64_t(phys_base) + extent - 1u) >> quality::ResolvePageMap::kPageShift);
      g_resolve_registration_fine.pages_marked += uint64_t(last) - first + 1u;
    }
    if (!s.quality->resolved.MarkRange(phys_base, extent)) {
      s.quality->valid = false;
      s.quality->invalid_causes |= 2;
    }
  }
  if (!EnsureInit()) {
    if (!TranslateOnly() && snapshot != nb::kNone) nb::ReleaseSnapshot(snapshot);  // ownership was handed over; nothing can use it
    return;
  }
  ++s.c.resolves_registered;

  Res e;
  e.snapshot = snapshot;
  e.census_key = census_key;
  e.reg_desc = r;
  e.seq = ++s.res_seq;
  e.last_use = e.seq;
  e.mem_last_use_frame = s.frame;
  const uint32_t control = r.rb_copy[0], info = r.rb_copy[3], pitch = r.rb_copy[2] & 0x3FFF;
  const uint32_t dest_h = (r.rb_copy[2] >> 16) & 0x3FFF;  // RB_COPY_DEST_PITCH.copy_dest_height
  const uint32_t src_select = control & 7;
  e.dest_base = r.rb_copy[1];
  e.info = info;
  e.dest_texture = r.dest_texture;
  e.dest_h = dest_h;
  e.dest_format = (info >> 7) & 0x3F;
  e.endian = info & 7;
  e.swap_rb = ((info >> 24) & 1) != 0;
  e.pitch_aligned = (pitch + 31) & ~31u;
  const int32_t x0 = r.src_rect[0], y0 = r.src_rect[1], x1 = r.src_rect[2], y1 = r.src_rect[3];
  e.resolve_rect[0] = x0;
  e.resolve_rect[1] = y0;
  e.resolve_rect[2] = x1;
  e.resolve_rect[3] = y1;
  const int32_t rw = x1 - x0, rh = y1 - y0;
  e.w = rw > 0 ? uint32_t(rw) : 0;
  e.h = rh > 0 ? uint32_t(rh) : 0;

  const auto df = xenos::ColorFormat((info >> 7) & 0x3F);
  if (snapshot == nb::kNone) {
    e.why_unusable = "no snapshot";
  } else if (phys_base & 4095 && (!(x0 | y0) || x1 > int32_t(e.pitch_aligned))) {
    e.why_unusable = "destination not page aligned";
  } else if (x1 > int32_t(e.pitch_aligned) && ((x0 | y0) & 31 || rw > int32_t(e.pitch_aligned))) {
    e.why_unusable = "rect beyond the pitch, not tile aligned";
  } else if (e.dest_base & 4095) {
    e.why_unusable = "destination base not page aligned";
  } else if (!e.w || !e.h || x0 < 0 || y0 < 0) {
    e.why_unusable = "empty source rect";
  } else if (src_select >= 4) {
    e.kind = 2;
  } else if (df == xenos::ColorFormat::k_8_8_8_8) {
    e.kind = 0;
  } else if (df == xenos::ColorFormat::k_8) {
    e.kind = 1;
  } else if (df == xenos::ColorFormat::k_16_16_16_16) {
    e.kind = 3;
  } else if (df == xenos::ColorFormat::k_16_16) {
    e.kind = 4;
  } else if (df == xenos::ColorFormat::k_4_4_4_4) {
    e.kind = 5;  // 4444 destination; the snapshot is still the RGBA8 image (derives like kind 0)
  } else {
    e.why_unusable = "dest format " + std::to_string(e.dest_format);
  }
  // Card M2c3: an earlier usable resolve to the same destination (base, pitch, format/endian/swap, kind).
  Res* old = nullptr;
  {
    ResolveRegistrationFineScope fine_metadata(ResolveRegistrationFineStage::kResolveMetadata);
    auto oit = s.resolves.find(e.dest_base);
    if (e.kind >= 0 && oit != s.resolves.end() && oit->second.kind == e.kind && oit->second.dest_base == e.dest_base &&
        oit->second.pitch_aligned == e.pitch_aligned && oit->second.info == info && oit->second.snapshot != nb::kNone) {
      old = &oit->second;
    }
    e.lineage_id = old ? old->lineage_id : e.seq;
    if (ResolveIntoOn()) {
      if (old) {
        if (old->canvas && !old->resolve_rect_history.empty()) {
          e.resolve_rect_history = old->resolve_rect_history;
          e.resolve_rect_history_overflow = old->resolve_rect_history_overflow;
        } else {
          e.resolve_rect_history.push_back({{0, 0, int32_t(old->w), int32_t(old->h)}, old->seq});
        }
      }
      e.resolve_rect_history.push_back({{x0, y0, x1, y1}, e.seq});
      if (e.resolve_rect_history.size() > kMaxResolveRectHistory) {
        const size_t excess = e.resolve_rect_history.size() - kMaxResolveRectHistory;
        e.resolve_rect_history.erase(e.resolve_rect_history.begin(), e.resolve_rect_history.begin() + excess);
        e.resolve_rect_history_overflow = true;
      }
    }
  }
  // A plain entry suffices when the rect starts at the origin and hides everything the earlier one covered.
  bool hides_old = false;
  {
    ResolveRegistrationFineScope fine_hide_old(ResolveRegistrationFineStage::kHideOldScan);
    hides_old = !old || (old->canvas ? [&] {
      for (uint32_t cy = 0; cy < (old->h + 7) / 8; ++cy)
        for (uint32_t cx = 0; cx < old->cov_cols; ++cx)
          if (old->cov[size_t(cy) * old->cov_cols + cx] && (int32_t(cx * 8) >= x1 || int32_t(cy * 8) >= y1)) return false;
      return true;
    }() : (int32_t(old->w) <= x1 && int32_t(old->h) <= y1));
  }
  uint64_t end = uint64_t(phys_base) + extent;
  // Card M2c4: a rect reaching beyond the destination pitch (cube faces rendered side by side, m2c4-b: rect 64i,0-
  // 64i+64,64, pitch 64, extent = face i) is not a sub-rect of a pitch-wide texture: its tile-aligned origin makes the
  // written memory start at the SDK extent start with the destination pitch, so it is a plain image of that memory.
  const bool wide = x1 > int32_t(e.pitch_aligned);
  // Card S3-69 5a (WWE2K14_D3D_DEST_IMAGES=1 + WWE2K14_D3D_DEST_CANVAS=1): a resolve at the origin that does not rewrite
  // its whole destination (pitch x RB_COPY_DEST_PITCH height, both derived from the destination texture header,
  // static-0c) composes into one host image of the whole destination (created seeded from the destination's guest
  // bytes, written in place by later resolves to it) instead of a plain image of the rect: a later fetch of the
  // destination texture binds that image (s33c-l1: 48x48 / 32x32 rects at the origin of fmt6 1024x512 destinations
  // were refused "width 1024 > 48").
  bool forced = false;
  if (dest::CanvasOn() && e.kind >= 0 && !wide && !(x0 | y0) && hides_old && dest_h && pitch &&
      SeedableRes(e.kind, e.swap_rb, e.endian)) {
    const bool whole_dest = x1 + 32 > int32_t(e.pitch_aligned) && y1 >= int32_t(dest_h);
    if (!whole_dest) {
      const uint64_t bytes = uint64_t(e.pitch_aligned) * ((std::max<uint32_t>(dest_h, uint32_t(y1)) + 7) & ~7u) *
                             (e.kind == 3 ? 8u : 4u);
      if (bytes <= (32ull << 20)) forced = true;
      else ++dest::C().canvas_too_big;
    }
  }
  const bool same_object_update = e.kind >= 0 && !wide &&
      ((x0 | y0) != 0 || !hides_old || forced) && SeedableRes(e.kind, e.swap_rb, e.endian);
  {
    ResolveRegistrationFineScope fine_seed_invalidate(ResolveRegistrationFineStage::kSeedInvalidate);
    InvalidateSeedCanvases(s, e, phys_base, extent, same_object_update);
  }
  uint32_t seed_source = 0, seed_source_bytes = 0;
  uint64_t seed_source_gen = 0;
  bool seed_source_valid = false, seed_canvas_reused_image = false;
  if (e.kind >= 0 && !wide && ((x0 | y0) != 0 || !hides_old || forced)) {
    // ---- compose into a canvas
    const uint32_t cw = std::max<uint32_t>(e.pitch_aligned, uint32_t(x1));
    uint32_t ch = std::max<uint32_t>(dest_h, uint32_t(y1));
    if (old) ch = std::max(ch, old->h);
    ch = (ch + 7) & ~7u;
    uint32_t canvas = nb::kNone;
    bool reuse = false;
    bool reuse_cached = false;
    SeedCanvas* cached_seed = nullptr;
    std::string err;
    if (old && old->canvas && old->w >= cw && old->h >= ch) {
      canvas = old->snapshot;  // taken over below
      reuse = true;
      seed_canvas_reused_image = true;
      e.seeded = old->seeded;  // card S3-69 5a: the canvas image (and its seed state) is carried over
      e.seed_pending = old->seed_pending;
      e.seed_source = old->seed_source;
      e.seed_source_bytes = old->seed_source_bytes;
      e.seed_source_gen = old->seed_source_gen;
      e.seed_coverage = old->seed_coverage;
      if (dest::CanvasReuseOn() && e.dest_texture) {
        auto cit = s.seed_canvases.find({e.dest_base, e.dest_texture});
        if (cit != s.seed_canvases.end() && SeedCacheDescriptorMatches(cit->second, e) &&
            cit->second.w == old->w && cit->second.h == old->h) {
          seed_source = cit->second.source;
          seed_source_bytes = cit->second.source_bytes;
          seed_source_gen = cit->second.source_gen;
          seed_source_valid = true;
        }
      }
    } else if (!old && !TranslateOnly() && dest::CanvasReuseOn() && e.dest_texture) {
      // The retained image is the same D3D destination object and includes its previous resolve coverage. Reuse only
      // when the complete guest detile source remains write-watch clean; then the current rect is copied over it.
      const SeedCanvasKey cache_key{e.dest_base, e.dest_texture};
      auto cit = s.seed_canvases.find(cache_key);
      if (cit != s.seed_canvases.end()) {
        SeedCanvas& c = cit->second;
        TexKey dkey;
        sh::TexFormatPlan dplan;
        TextureGuestLayout dlayout;
        sh::LevelGeom dg0{};
        uint32_t src = 0, src_bytes = 0;
        uint64_t src_gen = 0;
        const bool source_ok = SeedCacheDescriptorMatches(c, e) && c.w >= cw && c.h >= ch &&
                               CanvasSeedSource(s, e, c.w, c.h, &dkey, &dplan, &dlayout, &dg0,
                                                &src, &src_bytes, &src_gen) &&
                               src == c.source && src_bytes == c.source_bytes && src_gen == c.source_gen;
        if (source_ok) {
          canvas = nb::RetainSnapshot(c.snapshot);
          if (canvas != nb::kNone) {
            reuse = true;
            reuse_cached = true;
            seed_canvas_reused_image = true;
            cached_seed = &c;
            e.seeded = true;
            seed_source = c.source;
            seed_source_bytes = c.source_bytes;
            seed_source_gen = c.source_gen;
            seed_source_valid = true;
            c.last_use = s.frame;
            end = std::max<uint64_t>(end, uint64_t(c.start) + c.length);
          }
        }
        if (!reuse) {
          ++dest::C().canvas_cache_miss;
          auto drop = cit;
          DropSeedCanvas(s, drop);
        }
      }
      if (reuse) ++dest::C().canvas_cache_reuse;
    }
    // If a pending canvas must be copied into a larger image, make its original seed concrete first. This preserves
    // the old image's seed in the copied area even if guest bytes changed after that seed was created.
    bool old_seed_failed = false;
    if (old && old->canvas && old->seed_pending && !reuse) {
      // A failure leaves undefined texels in the old image: fail the compose rather than carry them as covered.
      old_seed_failed = EnsureSeedMaterialized(s, *old, SeedReason::kPropagation) == SeedEnsureResult::kFailed;
    }
    const bool eager_for_old_seed_extension = old && old->canvas && old->seeded && !reuse;
    if (!reuse && TranslateOnly()) {
      canvas = snapshot;  // mode 1: ids are local fakes; nothing is copied
    } else if (!reuse) {
      // Card S3-69 5a: a new destination image is seeded at creation from the detiled guest bytes of the whole
      // destination (texels no resolve wrote read as the guest memory's, their only writer); it is never cleared
      // (the old path keeps WWE2K14_D3D_CANVAS_CLEAR's rule for M2c3).
      canvas = nb::NewSnapshotLike(snapshot, cw, ch, &err, false);
      if (canvas != nb::kNone) {
        ++dest::C().canvas_new;
        if (forced) ++dest::C().canvas_forced;
        bool seeded = false;
        if (dest::CanvasOn() && s.memory && pitch && cw <= 8192 && ch <= dest_h && ch <= 8192 &&
            uint64_t(cw) * ch * (e.kind == 3 ? 8u : 4u) <= (32ull << 20)) {
          // SeedableRes is implied (the canvas only exists under CanvasOn via the forced / compose conditions, and
          // e.kind/swap/endian passed there); re-checked cheaply to keep the seed self-contained.
          TexKey dkey;
          sh::TexFormatPlan dplan;
          TextureGuestLayout dlayout;
          sh::LevelGeom dg0{};
          uint32_t src = 0, src_bytes = 0;
          uint64_t src_gen = 0;
          const bool cache_on = dest::CanvasReuseOn() && e.dest_texture;
          if (CanvasSeedSource(s, e, cw, ch, &dkey, &dplan, &dlayout, &dg0,
                               &src, &src_bytes, &src_gen)) {
            const int lazy_mode = nb::ShaderFormatIsDxil() ? 0 : SeedLazyModeAtFrame(s.frame);
            const bool lazy_candidate = lazy_mode == 2 || (lazy_mode == 1 && !eager_for_old_seed_extension);
            bool tracked = false;
            bool defer_seed = false;
            if (lazy_mode != 0) ++s.seed_lazy.created;
            if (lazy_candidate) {
              auto coverage = MakeSeedCoverage(s, cw, ch);
              if (coverage) {
                tracked = true;
                e.seed_coverage = std::move(coverage);
                e.seed_source = src;
                e.seed_source_bytes = src_bytes;
                e.seed_source_gen = src_gen;
                e.seed_pending = true;  // mode 2 keeps this bookkeeping while the old eager pixels remain present
                seed_source = src;
                seed_source_bytes = src_bytes;
                seed_source_gen = src_gen;
                seed_source_valid = true;
                defer_seed = lazy_mode == 1;
              } else {
                ++s.seed_lazy.backup_fallback;
              }
            }
            if (defer_seed) {
              ++s.seed_lazy.skipped;
            } else {
            const uint8_t* source_ptr = s.memory->TranslatePhysical<const uint8_t*>(dg0.address);
            rr::NotePhys(dg0.address, src_bytes);
            std::vector<uint8_t> upload_source;
            uint64_t transform_ns = 0;
            if (e.swap_rb && e.kind == 0) {
              const uint64_t transform_start = TxNow();
              upload_source.assign(source_ptr, source_ptr + src_bytes);
              SeedCanvasSourceBytes(e.kind, e.swap_rb, e.endian, &upload_source);
              transform_ns = TxNow() - transform_start;
              source_ptr = upload_source.data();
              dest::C().seed_source_transform_ns += transform_ns;
              ++dest::C().seed_source_transform_calls;
            }
            const bool trace_seed = dest::TraceOn() && seed_trace_checks < 8;
            if (trace_seed) ++seed_trace_checks;
            std::vector<uint8_t> gpu_level;
            nb::SeedDetileStats gpu_stats;
            const nb::GpuLevel gpu_params = sh::MakeGpuLevel(dkey, dplan, dg0, 0, 0);
            // Director 2026-10-19 (review): GPU seed only on the Vulkan backend (D3D12 keeps main's behaviour).
            const bool detiled = !nb::ShaderFormatIsDxil() &&
                                 nb::DetileSnapshotGpu(canvas, gpu_params, src_bytes, source_ptr,
                                                       trace_seed ? &gpu_level : nullptr, &gpu_stats, &err);
            dest::C().seed_upload_ns += gpu_stats.upload_ns;
            dest::C().seed_upload_calls += gpu_stats.padded_upload_bytes != 0;
            dest::C().seed_upload_bytes += gpu_stats.source_bytes;
            dest::C().seed_upload_padded_bytes += gpu_stats.padded_upload_bytes;
            dest::C().seed_gpu_submit_ns += gpu_stats.dispatch_submit_ns;
            dest::C().seed_gpu_calls += gpu_stats.dispatch_submit_ns != 0;
            if (trace_seed) {
              std::vector<nb::LevelData> cpu_level(1);
              const uint64_t cpu_start = TxNow();
              const bool cpu_ok = sh::DetileLevelCpu(dkey, dplan, dg0, 0, 0,
                                                     s.memory->TranslatePhysical<const uint8_t*>(dg0.address),
                                                     &cpu_level[0]);
              dest::C().seed_trace_cpu_ns += TxNow() - cpu_start;
              ++dest::C().seed_trace_cpu_calls;
              if (cpu_ok) SeedCanvasBytes(e.kind, e.swap_rb, &cpu_level[0].bytes);
              const bool compared = detiled && cpu_ok && gpu_level.size() == cpu_level[0].bytes.size();
              const bool match = compared && gpu_level == cpu_level[0].bytes;
              const uint64_t gpu_hash = gpu_level.empty() ? 0 : XXH3_64bits(gpu_level.data(), gpu_level.size());
              const uint64_t cpu_hash = cpu_ok ? XXH3_64bits(cpu_level[0].bytes.data(), cpu_level[0].bytes.size()) : 0;
              REXLOG_INFO("[d3d-dest] seed-gpu-hash frame={} match={} status={} fmt{} {}x{} base={:08X}+{:X} kind={} "
                          "swap_rb={} bytes={} gpu={:016x} cpu={:016x}",
                          s.frame, compared ? (match ? "true" : "false") : "unknown", err, uint32_t(dkey.format), cw,
                          ch, src, src_bytes, e.kind, e.swap_rb, compared ? gpu_level.size() : 0, gpu_hash, cpu_hash);
            }
            if (!detiled) {  // Director 2026-10-19 (review SHOULD-FIX 1): CPU fallback = main's seed path
              std::vector<nb::LevelData> dlevel(1);
              const uint64_t cpu_start = TxNow();
              if (sh::DetileLevelCpu(dkey, dplan, dg0, 0, 0, s.memory->TranslatePhysical<const uint8_t*>(dg0.address),
                                     &dlevel[0])) {
                SeedCanvasBytes(e.kind, e.swap_rb, &dlevel[0].bytes);
                if (nb::FillSnapshotRows(canvas, cw, ch, dlevel[0].bytes, &err)) seeded = true;
              }
              dest::C().seed_detile_ns += TxNow() - cpu_start;
            }
            if (detiled) {
              seeded = true;
              if (cache_on) {
                const std::array<uint32_t, 4> source_range{src, src + src_bytes, 0, 0};
                if (s.watch->GenSum(source_range) == src_gen) {
                  seed_source = src;
                  seed_source_bytes = src_bytes;
                  seed_source_gen = src_gen;
                  seed_source_valid = true;
                }
              }
            }
            }
            if (!seeded && !defer_seed) {
              e.seed_pending = false;
              e.seed_coverage.reset();
            }
            if (tracked && lazy_mode == 2 && !seeded) {
              e.seed_pending = false;
              e.seed_coverage.reset();
            }
          }
        }
        e.seeded = seeded;
        if (seeded) ++dest::C().canvas_seeded;
      }
    }
    if (reuse) ++dest::C().canvas_inplace;
    bool ok = canvas != nb::kNone && !old_seed_failed;
    if (old_seed_failed) err = "pending seed of the old canvas failed";
    e.canvas = true;
    e.w = reuse ? (old ? old->w : cached_seed->w) : cw;
    e.h = reuse ? (old ? old->h : cached_seed->h) : ch;
    {
      ResolveRegistrationFineScope fine_coverage(ResolveRegistrationFineStage::kCoverage);
      e.cov_cols = (e.w + 7) / 8;
      e.cov.assign(size_t(e.cov_cols) * ((e.h + 7) / 8), 0);
      if (ResolveRegistrationFineTimersOn()) {
        g_resolve_registration_fine.max_coverage_cells =
            std::max<uint32_t>(g_resolve_registration_fine.max_coverage_cells, uint32_t(e.cov.size()));
      }
      if (ok && old && !reuse) {
        // Bring the earlier content over (its image is old->w x old->h at the origin).
        if (!TranslateOnly() && !nb::CopySnapshotRegion(canvas, 0, 0, old->snapshot, 0, 0, std::min(old->w, e.w),
                                                        std::min(old->h, e.h), &err)) {
          ok = false;
        }
      }
      if (ok && old && !reuse && e.seed_coverage) {
        // CopySnapshotRegion above replaces this entire old-image rectangle after creating the new seed. Preserve
        // every copied pixel, including partial resolve edges and any seed from the older canvas.
        e.seed_coverage->pixels.MarkRect(0, 0, int32_t(std::min(old->w, e.w)), int32_t(std::min(old->h, e.h)));
      }
      if (ok && old) {
        if (old->canvas) {
          for (uint32_t cy = 0; cy < std::min((old->h + 7) / 8, (e.h + 7) / 8); ++cy)
            for (uint32_t cx = 0; cx < std::min(old->cov_cols, e.cov_cols); ++cx)
              if (old->cov[size_t(cy) * old->cov_cols + cx]) e.cov[size_t(cy) * e.cov_cols + cx] = 1;
        } else {
          e.Mark(0, 0, int32_t(old->w), int32_t(old->h));
        }
        end = std::max<uint64_t>(end, uint64_t(old->start) + old->length);
      }
      if (ok && reuse_cached && cached_seed && cached_seed->cov_cols == e.cov_cols &&
          cached_seed->cov.size() == e.cov.size()) {
        e.cov = cached_seed->cov;
      }
    }
    if (ok && !TranslateOnly() &&
        !nb::CopySnapshotRegion(canvas, uint32_t(x0), uint32_t(y0), snapshot, 0, 0, uint32_t(rw), uint32_t(rh), &err)) {
      ok = false;
    }
    if (ok) {
      {
        ResolveRegistrationFineScope fine_coverage(ResolveRegistrationFineStage::kCoverage);
        e.Mark(x0, y0, x1, y1);
        if (e.seed_coverage) e.seed_coverage->pixels.MarkRect(x0, y0, x1, y1);
      }
      if (!TranslateOnly() && snapshot != nb::kNone) nb::ReleaseSnapshot(snapshot);  // its pixels now live in the canvas
      e.snapshot = canvas;
      if (reuse && old) old->snapshot = nb::kNone;  // the canvas moves to the new entry (DropResolve must not release it)
      ++s.c.resolves_composed;
    } else {
      if (!TranslateOnly() && canvas != nb::kNone &&
          (reuse_cached || (!reuse && canvas != snapshot))) nb::ReleaseSnapshot(canvas);
      e.kind = -1;
      e.canvas = false;
      e.why_unusable = "compose failed: " + err;
    }
  }
  e.start = e.kind >= 0 && e.canvas ? e.dest_base : phys_base;
  e.length = uint32_t(std::max<uint64_t>(end, e.start) - e.start);
  e.range = {e.start, e.start + e.length, 0, 0};
  // Card S3-69 5a: a seeded canvas models the whole destination; a CPU write anywhere in it (not only inside the
  // written extent) must be seen at the fetch, so the watch covers the whole destination. e.length keeps the written
  // extent (the overlap/supersede logic relies on it).
  if (dest::CanvasOn() && e.kind >= 0 && e.canvas && (e.seeded || e.seed_pending) && e.dest_base) {
    const uint32_t span = DestSpan(e.kind, e.pitch_aligned, dest_h);
    if (span > e.length) e.range[1] = e.start + span;
  }
  if (e.kind < 0) {
    ++s.c.resolves_unusable;
    ResolveRegistrationFineScope fine_metadata(ResolveRegistrationFineStage::kResolveMetadata);
    ++s.hist["resolve registered unusable: " + e.why_unusable];
  }
  // Overlapping entries are superseded: this resolve rewrote that memory (in the game's model; a composed canvas
  // carries the earlier pixels of its own destination along).
  {
    ResolveRegistrationFineScope fine_overlap_lru(ResolveRegistrationFineStage::kOverlapLru);
    for (Res* o : Overlapping(s, e.start, e.start + std::max(e.length, 1u))) {
      auto it = s.resolves.find(o->start);
      if (it != s.resolves.end()) DropResolve(s, it, "superseded");
    }
    if (s.resolves.size() >= kMaxResolveEntries) {
      // Card S3-55: drop the least recently used entry (registered or fetched longest ago), not the lowest address -
      // begin() evicted the frame's own scene colour / depth resolves (low addresses) before the DOF composite fetched
      // them in busy 6-man frames; oldest-registered instead evicted the match-long wrestler textures (fx1).
      auto victim = s.resolves.begin();
      for (auto it = s.resolves.begin(); it != s.resolves.end(); ++it)
        if (it->second.last_use < victim->second.last_use) victim = it;
      DropResolve(s, victim, "entry cap");
    }
  }
  // Arm BEFORE the baseline: a CPU write into the destination later (the game reusing the memory) must be seen.
  {
    ResolveRegistrationFineScope fine_watch_seed(ResolveRegistrationFineStage::kWatchSeed);
    if (s.memory && e.length) {
      // For a 5a canvas the armed range is the whole destination (see the range extension above); for every other
      // entry it is the written extent (identical to the old behavior).
      const uint32_t span = e.range[1] - e.range[0];
      ArmIfNeeded(s, e.start, span);
      e.gen_sum = s.watch->GenSum(e.range);
      if (StaleSampleOn()) e.sample_hash = SampleHash(s, e.start, span);  // card S3-33b
    }
    if (seed_source_valid) {
      if (seed_canvas_reused_image) UpdateSeedCanvasMetadata(s, e);
      else RememberSeedCanvas(s, e, seed_source, seed_source_bytes, seed_source_gen);
    }
  }
  const uint32_t key = e.start;
  e.reg_frame = s.frame;
  {
    ResolveRegistrationFineScope fine_trace(ResolveRegistrationFineStage::kTrace);
    static std::set<std::tuple<uint32_t, int, uint32_t, uint32_t>> rseen;  // S3-41: first registration per identity
    if (ResTraceOn() && !ResTraced(e) && rseen.size() < 3000 && rseen.insert({e.dest_base, e.kind, e.w, e.h}).second) {
      REXLOG_INFO("[d3d-rtrace] first frame={} {:08X}+{:X} kind={} {}x{} dest_base={:08X} pitch={} fmt={}", s.frame, e.start,
                  e.length, e.kind, e.w, e.h, e.dest_base, e.pitch_aligned, e.dest_format);
    }
    if (ResTraced(e)) {
      REXLOG_INFO("[d3d-rtrace] reg frame={} {:08X}+{:X} seq={} dest_base={:08X} rect={},{}-{},{} pitch={} dest_h={} canvas={} "
                  "sample_hash={:016x}", s.frame, e.start, e.length, e.seq, e.dest_base, x0, y0, x1, y1, e.pitch_aligned,
                  dest_h, e.canvas, e.sample_hash);
    }
  }
  FMemoForgetRange(s, e.start, e.start + std::max(e.length, 1u));  // card S3-12: lookups over this memory change
  {
    ResolveRegistrationFineScope fine_trace(ResolveRegistrationFineStage::kTrace);
    if (dest::TraceOn() && s.memory) {  // card S3-69 diagnostic: registration history of this destination
      dest::RegRec rr;
      rr.frame = s.frame;
      rr.seq = e.seq;
      rr.rect[0] = x0, rr.rect[1] = y0, rr.rect[2] = x1, rr.rect[3] = y1;
      rr.pitch = pitch;
      rr.dest_h = dest_h;
      rr.info = info;
      rr.start = e.start;
      rr.length = e.length;
      rr.dest_texture = r.dest_texture;
      rr.kind = e.kind;
      rr.canvas = e.canvas;
      rr.has_point = r.has_dst_point;
      rr.point[0] = r.dst_point[0], rr.point[1] = r.dst_point[1];
      const uint32_t span = DestSpan(e.kind, e.pitch_aligned, std::max<uint32_t>(dest_h, uint32_t(std::max(y1, 0))));
      if (span && uint64_t(e.dest_base) + span <= kPhysLimit) {
        rr.dest_fp = SampleHash(s, e.dest_base, span);
        rr.dest_zero = SampleZeros(s, e.dest_base, span);
      }
      dest::NoteRegistration(e.dest_base, rr);
    }
  }
  {
    ResolveRegistrationFineScope fine_metadata(ResolveRegistrationFineStage::kResolveMetadata);
    s.resolves[key] = std::move(e);
  }
  if (ResolveRegistrationFineTimersOn()) {
    g_resolve_registration_fine.max_resolve_entries =
        std::max<uint32_t>(g_resolve_registration_fine.max_resolve_entries, uint32_t(s.resolves.size()));
    g_resolve_registration_fine.max_frame_memo_entries =
        std::max<uint32_t>(g_resolve_registration_fine.max_frame_memo_entries, uint32_t(s.fmemo.size()));
    g_resolve_registration_fine.max_seed_canvases =
        std::max<uint32_t>(g_resolve_registration_fine.max_seed_canvases, uint32_t(s.seed_canvases.size()));
  }
  ++s.epoch;
}

uint32_t ForSlot(const DeviceState& ds, uint32_t slot, uint32_t dimension, nb::SamplerDesc* sampler,
                 uint64_t* content_key, std::string* why) {
  TxT tx_total(0);
  State& s = S();
  const std::string sl = "slot " + std::to_string(slot) + " ";
  if (slot >= kFetchSlots) return Refuse(s, "slot out of range", sl + "out of range", why);
  if (!EnsureInit()) return Refuse(s, "no guest memory", sl + "no guest memory object", why);
  // Card S3-69 5e: the bound texture object decides object-identity handling below; 0 when the switches that read it
  // are off (then the memo/FKey values keep their memset-zero state and nothing changes).
  const uint32_t bound_obj = (dest::ObjBypassOn() || dest::CountOn()) && slot < kMaxTextureStages ? ds.texture_ptr[slot] : 0;
  const uint32_t* fw = &ds.fetch[slot * 6];
  nb::ResolveCensusFetchKey memo_fetch_key{};
  bool memo_fetch_valid = false;
  if (nb::ResolveCensusEnabled()) {
    xe_gpu_texture_fetch_t memo_fetch{};
    std::memcpy(&memo_fetch, fw, sizeof(memo_fetch));
    TexKey memo_key;
    uint8_t memo_signs = 0;
    sh::TextureKeys::BindingInfoFromFetchConstant(memo_fetch, memo_key, &memo_signs);
    if (memo_key.is_valid) {
      memo_fetch_key = {uint32_t(memo_key.base_page) << 12, uint32_t(memo_key.format), memo_key.GetWidth(),
                        memo_key.GetHeight(), uint32_t(memo_key.mip_max_level), uint32_t(memo_key.tiled)};
      memo_fetch_valid = true;
    }
  }
  bool trace_obj_changed = false;
  uint32_t trace_previous_obj = 0;
  if (dest::TraceOn() && bound_obj) {
    xe_gpu_texture_fetch_t tfetch = {};
    std::memcpy(&tfetch, fw, sizeof(tfetch));
    TexKey tkey;
    uint8_t tsigns = 0;
    sh::TextureKeys::BindingInfoFromFetchConstant(tfetch, tkey, &tsigns);
    if (tkey.is_valid) {
      const uint64_t address_slot = (uint64_t(uint32_t(tkey.base_page) << 12) << 32) | slot;
      static std::unordered_map<uint64_t, uint32_t> last_bound_obj;
      auto it = last_bound_obj.find(address_slot);
      if (it != last_bound_obj.end()) {
        trace_previous_obj = it->second;
        trace_obj_changed = trace_previous_obj != bound_obj;
        it->second = bound_obj;
      } else if (last_bound_obj.size() < 8192) {
        last_bound_obj.emplace(address_slot, bound_obj);
      }
    }
  }
  bool trace_object_bypassed = false;

  // Per-slot memo: byte-identical fetch constant (+ shaders) in the same frame with nothing released since.
  Memo& m = s.memo[slot];
  const bool slot_memo_match = m.valid && m.frame == s.frame && m.epoch == s.epoch && m.dimension == dimension &&
                               m.vs == s.vs && m.ps == s.ps && m.obj == bound_obj &&
                               std::memcmp(m.fetch, fw, sizeof(m.fetch)) == 0;
  if (slot_memo_match && !ResolveIntoCheckOn()) {
    if (trace_obj_changed) TraceBoundContent(s, fw, slot, m.handle, "memo_obj_change", trace_previous_obj, bound_obj);
    if (m.resolve_base) {
      nb::ResolveCensusConsumer(m.resolve_base);
      if (memo_fetch_valid) nb::ResolveCensusFetchBind(m.census_key, memo_fetch_key, s.frame, false, 0, 0, 0);
    }
    if (sampler) *sampler = m.sampler;
    if (content_key) *content_key = m.content;
    ++s.c.hits;
    ++s.c.memo_hits;
    return m.handle;
  }
  // CHECK re-enters the resolve cache below for a per-slot memo hit so it validates the same handle mode 1 serves.

  // Card S3-12: frame memo (WWE2K14_D3D_TEX_FMEMO=0 turns it off for an A/B).
  static const bool fmemo_on = [] {
    const char* e = std::getenv("WWE2K14_D3D_TEX_FMEMO");
    return !(e && *e == '0');
  }();
  if (fmemo_on && s.fmemo_frame != s.frame) {
    s.fmemo.clear();
    s.fmemo_frame = s.frame;
  }
  FKey fk;
  std::memset(&fk, 0, sizeof(fk));
  std::memcpy(fk.fetch, fw, sizeof(fk.fetch));
  fk.dimension = dimension;
  fk.obj = bound_obj;
  fk.vs = s.vs;
  fk.ps = s.ps;
  if (fmemo_on && !slot_memo_match) {
    auto fit = s.fmemo.find(fk);
    if (fit != s.fmemo.end() && !ResolveIntoCheckOn()) {
      const FVal& v = fit->second;
      if (trace_obj_changed) TraceBoundContent(s, fw, slot, v.handle, "memo_obj_change", trace_previous_obj, bound_obj);
      if (v.resolve_base) {
        nb::ResolveCensusConsumer(v.resolve_base);
        if (memo_fetch_valid) nb::ResolveCensusFetchBind(v.census_key, memo_fetch_key, s.frame, false, 0, 0, 0);
      }
      if (sampler) *sampler = v.sampler;
      if (content_key) *content_key = v.content;
      ++s.c.hits;
      ++s.fmemo_hits;
      return v.handle;
    }
  }

  xe_gpu_texture_fetch_t fetch;
  static_assert(sizeof(fetch) == 24, "fetch constant is 6 dwords");
  std::memcpy(&fetch, fw, sizeof(fetch));
  TexKey key;
  uint8_t signs = 0;
  sh::TextureKeys::BindingInfoFromFetchConstant(fetch, key, &signs);
  if (!key.is_valid) return Refuse(s, "invalid fetch constant", sl + "invalid fetch constant", why);
  const uint32_t base_start = uint32_t(key.base_page) << 12;
  // `dimension` = ShaderEntry::texture_dims value (XenosRecomp TextureDimension: 0 1D, 1 2D, 2 3D, 3 cube), as WP7 passes.
  if (dimension != 1 && dimension != 3) {
    glitch::FetchDimensionMismatch(glitch::CurrentDraw(), slot, base_start, uint32_t(key.format),
                                   uint32_t(key.dimension), dimension, "unsupported_shader_contract_dimension");
    return Refuse(s, "shader texture dimension", sl + "shader texture dimension " + std::to_string(dimension) + " not supported", why);
  }
  const bool want_cube = dimension == 3;
  const bool cube = key.dimension == xenos::DataDimension::kCube;
  if (!(cube || (key.dimension == xenos::DataDimension::k2DOrStacked && key.GetDepthOrArraySize() == 1))) {
    glitch::FetchDimensionMismatch(glitch::CurrentDraw(), slot, base_start, uint32_t(key.format),
                                   uint32_t(key.dimension), dimension, "unsupported_fetch_dimension");
    return Refuse(s, "dimension", sl + "dimension " + std::to_string(uint32_t(key.dimension)) + " depth " +
                                       std::to_string(key.GetDepthOrArraySize()), why);
  }
  if (want_cube != cube) {
    glitch::FetchDimensionMismatch(glitch::CurrentDraw(), slot, base_start, uint32_t(key.format),
                                   uint32_t(key.dimension), dimension, "fetch_vs_shader_contract");
    return Refuse(s, "shader/texture dimension mismatch", sl + "shader/texture dimension mismatch", why);
  }
  sh::TexFormatPlan plan;
  std::string plan_why;
  if (!sh::PlanTextureFormat(fetch, key, &plan, &plan_why)) {
    return Refuse(s, "format: " + plan_why, sl + plan_why, why);
  }
  const uint32_t width = key.GetWidth(), height = key.GetHeight();
  const TextureGuestLayout layout = key.GetGuestLayout();
  const uint32_t base_span = layout.base.level_data_extent_bytes;
  const uint32_t mip_span = layout.mips_total_extent_bytes;
  const uint32_t mip_start = uint32_t(key.mip_page) << 12;
  if (uint64_t(base_start) + base_span > kPhysLimit || uint64_t(mip_start) + mip_span > kPhysLimit) {
    return Refuse(s, "address out of physical range", sl + "texture address outside physical memory", why);
  }
  const std::array<uint32_t, 4> range = {base_start, base_start + base_span, mip_start, mip_start + mip_span};
  const xe_gpu_texture_fetch_t fetch_key = sh::TextureKeyFetch(fetch, key, true);
  std::string persistent_fetch_key;
  if (ResolveIntoOn()) {
    persistent_fetch_key = "I";
    persistent_fetch_key.append(reinterpret_cast<const char*>(&fetch_key), sizeof(fetch_key));
  }
  const nb::ResolveCensusFetchKey census_fetch_key{base_start, uint32_t(key.format), width, height,
                                                   uint32_t(key.mip_max_level), uint32_t(key.tiled)};
  bool census_fetch_rebuilt = false;
  uint64_t census_guest_detile_bytes = 0, census_guest_upload_bytes = 0, census_resolve_paste_bytes = 0;

  nb::SamplerDesc samp = sh::SamplerFromFetch(fetch, slot, s.vs, s.ps);
  char bpath = 'G';  // S3-33c-select-silhouettes diagnostic: G guest, g base only, A assembled, R/S/M/C resolve-derived
  nb::ResolveCensusKey bind_census_key{};
  auto finish = [&](uint32_t handle, uint64_t content) {
    if (s.quality && handle != nb::kNone && !TranslateOnly() && !s.quality->uploads.Find(handle)) {
      // Only the actual normal guest upload below supplies positive residency.
      // Other image paths (including g/A and every resolve-derived path) fail
      // closed; bpath is a texture-layer provenance fact, not a hash-bit guess.
      QualityUpload u;
      u.range = range;
      u.info.resolve_target = bpath != 'G';
      if (!s.quality->uploads.Put(handle, u)) {
        s.quality->valid = false;
        s.quality->invalid_causes |= 1;
      }
    }
    const uint32_t resolve_base = bpath == 'R' && (content & (1ull << 63)) ? base_start : 0;
    if (resolve_base) {
      nb::ResolveCensusConsumer(resolve_base);
      nb::ResolveCensusFetchBind(bind_census_key, census_fetch_key, s.frame, census_fetch_rebuilt,
                                 census_guest_detile_bytes, census_guest_upload_bytes, census_resolve_paste_bytes);
    }
    if (trace_object_bypassed) {
      trace_object_bypassed = false;
      TraceBoundContent(s, fw, slot, handle, "obj_bypass", trace_previous_obj, bound_obj);
    }
    if (BindTraceOn() && width >= 256 && height >= 256) {
      static std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, char>, std::pair<uint64_t, uint32_t>> bseen;
      static uint32_t blines = 0;
      char p = bpath;
      if (p == 'R') p = (content >> 62) & 1 ? 'C' : (content >> 61) & 1 ? 'M' : (content >> 60) & 1 ? 'S' : 'R';
      auto& v = bseen[{base_start, uint32_t(key.format), width, height, p}];
      if ((v.second == 0 || v.first != content) && v.second < 20 && blines < 4000) {
        ++v.second;
        ++blines;
        v.first = content;
        REXLOG_INFO("[d3d-btrace] bind frame={} {} base={:08X}+{:X} mip={:08X}+{:X} fmt{} {}x{} mips={} tiled={} slot={} "
                    "content={:016x} handle={} res_entries={}", s.frame, p, base_start, range[1] - range[0], mip_start,
                    range[3] - range[2], uint32_t(key.format), width, height, uint32_t(key.mip_max_level),
                    bool(key.tiled), slot, content, handle, s.resolves.size());
      }
    }
    if (FlashTraceOn() && s.flash_binds.size() < 3000) {  // card S3-44 diagnostic
      char b[80];
      std::snprintf(b, sizeof(b), " %X:%ux%u:f%u:h%u:c%04X", base_start, width, height, uint32_t(key.format), handle,
                    uint32_t(content & 0xFFFF));
      s.flash_binds += b;
    }
    m.valid = true;
    std::memcpy(m.fetch, fw, sizeof(m.fetch));
    m.frame = s.frame;
    m.epoch = s.epoch;
    m.dimension = dimension;
    m.obj = bound_obj;
    m.vs = s.vs;
    m.ps = s.ps;
    m.handle = handle;
    m.resolve_base = resolve_base;
    m.census_key = bind_census_key;
    m.content = content;
    m.sampler = samp;
    if (fmemo_on) {
      FVal& v = s.fmemo[fk];
      v.handle = handle;
      v.resolve_base = resolve_base;
      v.census_key = bind_census_key;
      v.content = content;
      v.sampler = samp;
      v.range = range;
    }
    if (sampler) *sampler = samp;
    if (content_key) *content_key = content;
    return handle;
  };

  bool base_only = false;  // card S3-33d: guest texture without its (resolve-written) mip levels
  Res* asm_l1 = nullptr;   // card S3-33h: guest base level + this entry's snapshot as mip level 1 (2 levels)
  // ---- resolve destinations: snapshot or refusal, never guest memory ----
  {
    std::vector<Res*> ov = Overlapping(s, range[0], range[1]);
    for (Res* o : Overlapping(s, range[2], range[3])) {
      if (std::find(ov.begin(), ov.end(), o) == ov.end()) ov.push_back(o);
    }
     for (Res* o : ov) {
       o->last_use = s.res_seq;  // card S3-55: fetched entries stay resident under the entry cap
       o->mem_last_use_frame = s.frame;
     }
    if (dest::CountOn()) {  // card S3-69 (SV-6x question): is the bound texture object the resolve's destination object?
      for (const Res* o : ov) {
        if (o->start != base_start) {
          if (bound_obj && o->dest_texture) ++(bound_obj == o->dest_texture ? dest::C().obj_part_eq : dest::C().obj_part_ne);
          continue;
        }
        if (!bound_obj || !o->dest_texture) {
          ++dest::C().obj_null;
          ++(!bound_obj ? dest::C().obj_fetch_null : dest::C().obj_dest_null);
        } else if (bound_obj == o->dest_texture) {
          ++dest::C().obj_eq;
        } else {
          ++dest::C().obj_ne;
        }
      }
    }
    // Card M2c5: an entry that cannot have produced what this fetch reads is stale - a resolve writes tiled texels of
    // its own destination format, so a linear fetch or a format no overlapping resolve writes means the memory was
    // rewritten since (m2c4-a: DXT/k_8 linear textures over old 1280x720 resolve extents). Drop them (memory semantics:
    // the last writer wins) and read the fetch as a guest texture.
    // Luna M2c5 review: only when the fetch can certainly not read resolve output - a linear fetch or a block-compressed
    // format (a resolve writes tiled texels of a colour/depth format); a re-interpreting tiled fetch stays a refusal.
    // Card S3-33 (WWE2K14_D3D_STALE_DROP=1): an overlapping entry whose destination the game wrote after the resolve
    // (write watch generation moved) is stale - memory semantics, the last writer wins: drop it and let the fetch read
    // the game's bytes, instead of refusing every later fetch over it (lane A s332-long / s333-base: refusals 1.5 k ->
    // millions from the second match on, wrestlers missing). In draw-off the backend writes guest memory only for
    // CPU-visible resolves (S3-17), whose bytes are the resolve's own pixels.
    static const bool stale_drop = [] {
      const char* e = std::getenv("WWE2K14_D3D_STALE_DROP");
      return e && std::strcmp(e, "1") == 0;
    }();
    if ((stale_drop || StaleSampleOn()) && !ov.empty() && !s.ignore_resolve_write) {
      std::vector<uint32_t> stale;
      for (Res* o : ov) {
        if (!o->length || s.watch->Clean(o->range, true, o->gen_sum)) continue;
        // Card S3-33b: watch hit without a content change -> not a write by the game: keep the entry, new baseline.
        // For a 5a canvas the sampled and re-armed span is the whole destination (range[1] - range[0] > length);
        // for every other entry it equals the written extent (identical to the old behavior).
        if (StaleSampleOn() && SampleHash(s, o->start, o->range[1] - o->range[0]) == o->sample_hash) {
          ArmIfNeeded(s, o->start, o->range[1] - o->range[0]);
          o->gen_sum = s.watch->GenSum(o->range);
          ++s.c.resolve_watch_kept;
          continue;
        }
        stale.push_back(o->start);
      }
      if (!stale.empty()) {
        for (uint32_t st : stale) {
          auto it = s.resolves.find(st);
          if (it != s.resolves.end()) {
            glitch::StaleResolveFallback(glitch::CurrentDraw(), slot, base_start, uint32_t(key.format), width,
                                         height, it->second.start, it->second.seq, "guest_write");
            DropResolve(s, it, "stale written");
            ++s.c.resolve_stale_written_dropped;
          }
        }
        ov = Overlapping(s, range[0], range[1]);
        for (Res* o : Overlapping(s, range[2], range[3])) {
          if (std::find(ov.begin(), ov.end(), o) == ov.end()) ov.push_back(o);
        }
      }
    }
    // Card S3-33c (WWE2K14_D3D_OVERLAP_BYPASS=1): a fetch that only straddles resolve destinations (no entry starts at
    // its base; s336-otrace: 64x64 at 16967000 over entries 16964000 and 16968000) is not a resolve output - read
    // the game's bytes for this fetch; the entries stay for their own fetches.
    static const bool overlap_bypass = [] {
      const char* e = std::getenv("WWE2K14_D3D_OVERLAP_BYPASS");
      return e && std::strcmp(e, "1") == 0;
    }();
    // Card S3-33e: write the entries' snapshot bytes to guest memory (once per entry) so a guest read is correct.
    auto materialize = [&](const std::vector<Res*>& es) -> bool {
      for (const Res* o : es) {
        if ((o->kind != 0 && o->kind != 1 && o->kind != 5) || o->start != o->dest_base || o->snapshot == nb::kNone) return false;
      }
      for (Res* o : es) {
        if (o->materialized) continue;
        const CpuVisResult res = WriteBackResolves(o->start, o->length, [&](uint32_t start, const std::vector<uint8_t>& bytes) {
          std::memcpy(s.memory->TranslatePhysical<uint8_t*>(start), bytes.data(), bytes.size());
          s.watch->MarkWritten(start, uint32_t(bytes.size()), true);  // guest textures over it rebuild (host write, as GuestWrite)
        });
        if (!res.refused.empty() || !res.served) return false;
        ++s.c.resolve_materialized;
      }
      for (Res* o : es) {  // the guest bytes now equal the snapshot: new baselines, the entry stays valid
        o->materialized = true;
        ArmIfNeeded(s, o->start, o->length);
        o->gen_sum = s.watch->GenSum(o->range);
        if (StaleSampleOn()) o->sample_hash = SampleHash(s, o->start, o->length);
      }
      return true;
    };
    // Card S3-69 5e (WWE2K14_D3D_DEST_OBJ_BYPASS=1): the bound texture object is none of the overlapping entries'
    // destination objects (Resolve r6 == bound object at every entry-start fetch with a mirrored pointer, dest1/dest3:
    // 0 mismatches): this fetch reads another texture that reuses the memory (class B: 64x64 match-1 entries under
    // new 64x64 textures before match 2). Read its own texture; entries stay. Applies at an entry's base too (a fetch
    // of the destination object itself is excluded by obj == dest_texture).
    if (dest::ObjBypassOn() && !ov.empty()) {
      bool other = bound_obj != 0;
      for (const Res* o : ov) other = other && o->dest_texture && o->dest_texture != bound_obj;
      if (other) {
        ov.clear();
        ++dest::C().obj_bypass;
        trace_object_bypassed = true;
      }
    }
    if (overlap_bypass && ov.size() >= 2) {  // a fetch inside ONE entry is a crop of it (kept)
      bool at_base = false;
      for (const Res* o : ov) at_base = at_base || o->start == base_start;
      if (!at_base && (!MaterializeOn() || materialize(ov))) {
        ov.clear();
        ++s.c.resolve_overlap_bypassed;
      }
    }
    // Card S3-33d: entries that touch only the mip tail (s340-ptrace: 1024x512 k_4_4_4_4 crowd atlas, base 1EC6D000 not
    // resolved, one 512x256 entry at its mip address 1EC1D000) - the base level is the game's own bytes: read them.
    // PARKED (s341/s342: Undertaker mips / third-match entrance junk): opt-in WWE2K14_D3D_MIPTAIL_BYPASS=1.
    static const bool miptail_bypass = [] {
      const char* e = std::getenv("WWE2K14_D3D_MIPTAIL_BYPASS");
      return e && std::strcmp(e, "1") == 0;
    }();
    // Card S3-33f (WWE2K14_D3D_MIPTAIL_4444=1): only the k_4_4_4_4 atlas case of s340-ptrace (every overlapping entry
    // rgba4-class, kind 5) - materialized, full mip chain from guest; any other format keeps the refusal.
    static const bool miptail_4444 = [] {
      const char* e = std::getenv("WWE2K14_D3D_MIPTAIL_4444");
      return e && std::strcmp(e, "1") == 0;
    }();
    // Card S3-33h (WWE2K14_D3D_ASSEMBLE_L1=1; s351: the game never wrote the entry and level 2 has no writer): a
    // k_4_4_4_4 fetch whose only overlapping entry is its level 1 (kind 5, at the mip address, base untouched) is
    // assembled from the guest base level + that snapshot as level 1, with the chain clamped to two levels.
    static const bool assemble_l1 = [] {
      const char* e = std::getenv("WWE2K14_D3D_ASSEMBLE_L1");
      return e && std::strcmp(e, "1") == 0;
    }();
    if (assemble_l1 && ov.size() == 1 && key.format == xenos::TextureFormat::k_4_4_4_4 && key.mip_max_level >= 1 &&
        !cube && dimension == 1 && key.base_page && ov[0]->kind == 5 && ov[0]->start == mip_start &&
        ov[0]->snapshot != nb::kNone && !(uint64_t(ov[0]->start) < range[1] && uint64_t(ov[0]->start) + ov[0]->length > range[0]) &&
        ov[0]->w >= std::max(1u, width >> 1) && ov[0]->h >= std::max(1u, height >> 1)) {
      asm_l1 = ov[0];
      if (ResTraceOn()) {
        REXLOG_INFO("[d3d-rtrace] assemble frame={} base={:08X}+{:X} mip={:08X} {}x{} mips={} entry seq={} reg_frame={} {}x{} "
                    "base_fp={:016x} entry_fp_now={:016x} entry_fp_reg={:016x}", s.frame, base_start, range[1] - range[0],
                    mip_start, width, height, uint32_t(key.mip_max_level), ov[0]->seq, ov[0]->reg_frame, ov[0]->w, ov[0]->h,
                    SampleHash(s, base_start, uint32_t(range[1] - range[0])), SampleHash(s, ov[0]->start, ov[0]->length),
                    ov[0]->sample_hash);
      }
      ov.clear();
      ++s.c.resolve_overlap_bypassed;
    }
    bool m4444 = miptail_4444 && MaterializeOn() && key.format == xenos::TextureFormat::k_4_4_4_4 && !ov.empty();
    for (const Res* o : ov) m4444 = m4444 && o->kind == 5;
    if ((miptail_bypass || m4444) && !ov.empty() && range[1] > range[0]) {  // S3-33d PARKED (s341-s343: Undertaker junk)
      bool touches_base = false;
      for (const Res* o : ov) {
        touches_base = touches_base || (uint64_t(o->start) < range[1] && uint64_t(o->start) + o->length > range[0]);
      }
      if (!touches_base && MaterializeOn() && (m4444 || miptail_bypass)) {
        if (materialize(ov)) {  // card S3-33e: resolve-written mip levels are now in guest memory too
          ov.clear();
          ++s.c.resolve_overlap_bypassed;
        }
      } else if (!touches_base) {
        // The mip levels came from resolves the backend never wrote to guest memory: use the base level only.
        ov.clear();
        base_only = key.mip_max_level > 0;
        ++s.c.resolve_overlap_bypassed;
      }
    }
    const rex::graphics::FormatInfo* finfo = rex::graphics::FormatInfo::Get(key.format);
    const bool never_resolved = !key.tiled || (finfo && finfo->type == rex::graphics::FormatType::kCompressed);
    if (!ov.empty() && never_resolved) {
      bool any = false;
      for (const Res* o : ov) {
        if (key.tiled && (o->kind >= 0 ? KindBpt(o->kind, key.format, key.endianness) != 0
                                       : uint32_t(key.format) == o->dest_format)) {
          any = true;
        }
      }
      if (!any) {
        std::vector<uint32_t> starts;
        for (const Res* o : ov) starts.push_back(o->start);
        for (uint32_t st : starts) {
          auto it = s.resolves.find(st);
          if (it != s.resolves.end()) {
            const Res& stale_resolve = it->second;
            glitch::FetchFormatMismatch(glitch::CurrentDraw(), slot, base_start, uint32_t(key.format),
                                        uint32_t(stale_resolve.kind), stale_resolve.dest_format, width, height,
                                        "fetch_vs_resolve_format");
            glitch::StaleResolveFallback(glitch::CurrentDraw(), slot, base_start, uint32_t(key.format), width,
                                         height, stale_resolve.start, stale_resolve.seq, "format_mismatch");
            DropResolve(s, it, "stale format");
            ++s.c.resolve_stale_dropped;
          }
        }
        ov.clear();
      }
    }
    if (!ov.empty()) {
      bpath = 'R';
      Res& r = *ov[0];
      nb::ResolveCensusKey census_key = r.census_key;
      census_key.frame = s.frame;
      bind_census_key = census_key;
      nb::ResolveCensusScope census_scope(census_key);
      const std::string cls = "resolve sample class unsupported: " + FmtSize(uint32_t(key.format), width, height);
      auto refuse = [&](const std::string& detail) {
        // Card S3-35 stage 0 diagnostic (WWE2K14_D3D_FETCH_TRACE=1; one line per unique (ps, slot, format, base),
        // <= 200; remove once the Undertaker fetch is identified).
        static const bool ftrace = [] {
          const char* e = std::getenv("WWE2K14_D3D_FETCH_TRACE");
          return e && *e == '1';
        }();
        static std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>> fseen;
        const uint64_t psh = s.ps ? s.ps->ucode_data_hash() : 0, vsh = s.vs ? s.vs->ucode_data_hash() : 0;
        if (ftrace && fseen.size() < 200 && fseen.insert({psh, slot, uint32_t(key.format), base_start}).second) {
          std::string es;
          for (const Res* o : ov) {
            es += fmt::format(" [{:08X}+{:X} seq={} kind={} {}x{} pitch={} canvas={} mat={} fp_changed={}]", o->start, o->length,
                              o->seq, o->kind, o->w, o->h, o->pitch_aligned, o->canvas, o->materialized,
                              StaleSampleOn() ? int(SampleHash(s, o->start, o->length) != o->sample_hash) : -1);
          }
          // Also: has the game written the fetched base range itself since the overlapping entries were registered?
          const uint64_t base_fp = SampleHash(s, base_start, base_span);
          REXLOG_INFO("[d3d-ftrace] frame={} ps={:016x} vs={:016x} slot={} fmt{} {}x{} dim={} mips={} base={:08X}+{:X} "
                      "mip={:08X}+{:X} base_fp={:016x} -> {} |{}", s.frame, psh, vsh, slot, uint32_t(key.format), width, height,
                      dimension, uint32_t(key.mip_max_level), base_start, base_span, mip_start, mip_span, base_fp, detail, es);
        }
        if (dest::TraceOn()) {  // card S3-69 diagnostic: what the refused fetch overlaps and that memory's history
          std::string dcls = detail.substr(0, detail.find_first_of("0123456789"));
          if (dest::TraceRefusalOnce(base_start, uint32_t(key.format), width, height, dcls)) {
            std::string es;
            for (const Res* o : ov) {
              uint64_t ffp = 0, ff = 0;
              const bool hf = dest::FirstFingerprint(o->dest_base, &ffp, &ff);
              const uint32_t span = DestSpan(o->kind, o->pitch_aligned, std::max(o->dest_h, o->h));
              const bool sp = span && uint64_t(o->dest_base) + span <= kPhysLimit;
              es += fmt::format("\n    entry {:08X}+{:X} seq={} reg_f={} kind={} {}x{} pitch={} dest_h={} canvas={} cov={} swap={} "
                                "endian={} obj={:08X} clean={} dest_fp_first={}{:016x}@f{} dest_fp_now={:016x} dest_zero_now={} hist:{}",
                                o->start, o->length, o->seq, o->reg_frame, o->kind, o->w, o->h, o->pitch_aligned, o->dest_h,
                                o->canvas, o->canvas ? [&] { size_t n = 0; for (uint8_t c : o->cov) n += c; return n; }() : 0,
                                o->swap_rb, o->endian, o->dest_texture, s.watch->Clean(o->range, true, o->gen_sum),
                                hf ? "" : "?", ffp, ff, sp ? SampleHash(s, o->dest_base, span) : 0,
                                sp ? SampleZeros(s, o->dest_base, span) : 0, dest::History(o->dest_base));
            }
            REXLOG_INFO("[d3d-dest] refuse frame={} ps={:016x} vs={:016x} slot={} obj={:08X} fmt{} {}x{} tiled={} mips={} endian={} "
                        "swz={:03o} signed={} cube={} base={:08X}+{:X} mip={:08X}+{:X} pitch_bytes={} base_fp={:016x} base_zero={}/256 "
                        "-> {}{}", s.frame, s.ps ? s.ps->ucode_data_hash() : 0, s.vs ? s.vs->ucode_data_hash() : 0, slot,
                        slot < kMaxTextureStages ? ds.texture_ptr[slot] : 0, uint32_t(key.format), width, height, bool(key.tiled),
                        uint32_t(key.mip_max_level), uint32_t(key.endianness), uint32_t(fetch.swizzle), plan.any_signed, cube,
                        base_start, base_span, mip_start, mip_span, layout.base.row_pitch_bytes,
                        SampleHash(s, base_start, base_span), SampleZeros(s, base_start, base_span), detail, es);
          }
        }
        return Refuse(s, cls + " (" + detail + ")", sl + "resolve sample class unsupported (" + detail + ") " +
                                                      FmtSize(uint32_t(key.format), width, height), why, true);
      };
      if (cube && key.mip_max_level == 0 && key.tiled && dimension == 3) {
        // Card M2c4: a cube whose faces were each written by one resolve (m2c4-b: 64x64 faces rendered side by side,
        // one resolve per face, extent = face). Face f lives at base + f * array_slice_stride (texture_util layout).
        const uint32_t stride = layout.base.array_slice_stride_bytes;
        Res* faces[6] = {};
        int found = 0;
        std::string bad;
        for (uint32_t f = 0; f < 6 && bad.empty(); ++f) {
          auto fit = s.resolves.find(base_start + f * stride);
          if (fit == s.resolves.end()) continue;
          Res& fr = fit->second;
          if (fr.kind < 0) bad = "face " + std::to_string(f) + ": " + fr.why_unusable;
          else if (fr.kind != 0 || key.format != xenos::TextureFormat::k_8_8_8_8) bad = "face format";
          else if (fr.canvas || fr.w < width || fr.h < height) bad = "face size";
          else if (layout.base.row_pitch_bytes != fr.pitch_aligned * 4) bad = "face pitch";
          else if (uint32_t(key.endianness) != fr.endian) bad = "face endian";
          else if (f && faces[0] && fr.swap_rb != faces[0]->swap_rb) bad = "faces differ in R/B swap";
          else if (!s.ignore_resolve_write && !s.watch->Clean(fr.range, true, fr.gen_sum)) bad = "face written since the resolve";
          else faces[f] = &fr, ++found;
        }
        if (!bad.empty()) return refuse("cube " + bad);
        if (found != 6) return refuse("cube faces resolved " + std::to_string(found) + " of 6");
        if (plan.any_signed) return refuse("signed fetch");
        uint8_t sw[4];
        for (int c = 0; c < 4; ++c) {
          uint8_t v = uint8_t((fetch.swizzle >> (3 * c)) & 7);
          if (faces[0]->swap_rb && (v == 0 || v == 2)) v = uint8_t(2 - v);
          sw[c] = v;
        }
        char kb[160];
        std::snprintf(kb, sizeof(kb), "C%llu.%llu.%llu.%llu.%llu.%llu:%ux%u:%u%u%u%u",
                      (unsigned long long)faces[0]->seq, (unsigned long long)faces[1]->seq,
                      (unsigned long long)faces[2]->seq, (unsigned long long)faces[3]->seq,
                      (unsigned long long)faces[4]->seq, (unsigned long long)faces[5]->seq, width, height, sw[0], sw[1],
                      sw[2], sw[3]);
        // Card S3-69 5c (WWE2K14_D3D_DEST_CUBE=1): one persistent cube image per destination (base, size, swizzle);
        // faces whose resolve entry changed are copied into it in place instead of building a new cube image.
        char pkb[64];
        std::snprintf(pkb, sizeof(pkb), "Q%08X:%ux%u:%u%u%u%u", base_start, width, height, sw[0], sw[1], sw[2], sw[3]);
        const std::string ckey = dest::CubeOn() ? std::string(pkb) : std::string(kb);
        uint64_t seqs[6];
        for (int f = 0; f < 6; ++f) seqs[f] = faces[f]->seq;
        auto cit = s.cache.find(ckey);
        if (cit != s.cache.end() && dest::CubeOn() && cit->second.pseq_n == 6) {
          Entry& pe = cit->second;
          bool ok = true, changed = false;
          std::string err;
          for (uint32_t f = 0; f < 6 && ok; ++f) {
            if (pe.pseq[f] == seqs[f]) continue;
            if (!TranslateOnly() && !nb::CopySnapshotToTextureLayer(pe.handle, faces[f]->snapshot, 0, f, width, height, &err)) {
              ok = false;
              break;
            }
            pe.pseq[f] = seqs[f];
            changed = true;
            ++dest::C().cube_faces_copied;
          }
          if (ok) {
            if (changed) {
              pe.content = (1ull << 63) | (1ull << 62) | (XXH3_64bits(seqs, sizeof(seqs)) >> 2);
              ++dest::C().cube_inplace;
            }
            pe.last_use = s.frame;
            ++s.c.hits;
            ++s.c.resolve_binds;
            return finish(pe.handle, pe.content);
          }
          DropEntry(s, cit);  // could not update in place: build a new one below
          cit = s.cache.end();
        }
        if (cit != s.cache.end()) {
          cit->second.last_use = s.frame;
          ++s.c.hits;
          ++s.c.resolve_binds;
          return finish(cit->second.handle, cit->second.content);
        }
        if (!MakeRoom(s)) {
          ++s.c.refused_cap;
          return Refuse(s, "texture cap full", sl + "shadow texture cap full", why);
        }
        const uint64_t bytes = uint64_t(width) * height * 4 * 6;
        if (!MakeByteRoom(s, bytes)) {
          return Refuse(s, "texture byte cap full", sl + "shadow texture byte cap (WWE2K14_D3D_SHADOW_TEX_MB)", why);
        }
        uint32_t handle = 0;
        if (!TranslateOnly()) {
          nb::TextureDesc desc;
          desc.width = width;
          desc.height = height;
          desc.format = nb::TexFormat::kRGBA8;
          desc.cube = true;
          std::memcpy(desc.swizzle, sw, 4);
          uint32_t ids[6];
          for (int f = 0; f < 6; ++f) ids[f] = faces[f]->snapshot;
          std::string err;
          { TxT tx(5); handle = nb::TextureCubeFromSnapshots(ids, desc, &err); }
          if (handle == nb::kNone) return refuse("backend: " + err);
        }
        Entry ne;
        ne.bytes = bytes;
        ne.handle = handle;
        ne.range = range;
        ne.frame = ne.last_use = s.frame;
        ne.content = (1ull << 63) | (1ull << 62) | (XXH3_64bits(seqs, sizeof(seqs)) >> 2);
        if (dest::CubeOn()) {  // 5c: persistent, not tied to the face entries (LRU / byte cap release it)
          for (int f = 0; f < 6; ++f) ne.pseq[f] = seqs[f];
          ne.pseq_n = 6;
          ++dest::C().cube_new;
        } else {
          ne.res_start = faces[0]->start;
          ne.res_seq = faces[0]->seq;
        }
        CachePut(s, ckey, ne);
        s.c.live_bytes += ne.bytes;
        if (!dest::CubeOn()) for (Res* fr : faces) fr->derived.push_back(ckey);  // dropping any face drops the cube
        NoteCreated(s);
        ++s.c.resolve_binds;
        ++s.c.resolve_cubes;
        ++s.c.misses;
        return finish(handle, ne.content);
      }
      if (!cube && key.mip_max_level > 0 && key.tiled && dimension == 1 && !plan.any_signed) {
        // Card M2c5: a mipmapped texture whose levels were each written by a resolve (m2c3-a overlap sample: 4444
        // 1024x512 base + 512x256 / 256x128 levels as separate resolves). Level n lives where texture_util's guest
        // layout puts it (sh::ComputeLevelGeom); packed mip-tail levels are not resolve images: the texture is built
        // with the leading levels found (counted as clamped).
        Res* lv[16] = {};
        uint32_t nlev = 0;
        const uint32_t max_level = std::min<uint32_t>(key.mip_max_level, 15);
        for (uint32_t level = 0; level <= max_level; ++level) {
          uint32_t addr, row_pitch;
          if (level == 0) {
            addr = base_start;
            row_pitch = layout.base.row_pitch_bytes;
          } else {
            if (layout.packed_level != UINT32_MAX && level >= layout.packed_level) break;
            const sh::LevelGeom g = sh::ComputeLevelGeom(key, layout, plan, level);
            addr = g.address;
            row_pitch = g.lay->row_pitch_bytes;
          }
          auto lit = s.resolves.find(addr);
          if (lit == s.resolves.end()) break;
          Res& e = lit->second;
          const uint32_t lw = std::max(1u, width >> level), lh = std::max(1u, height >> level);
          const uint32_t bpt = e.kind >= 0 ? KindBpt(e.kind, key.format, key.endianness) : 0;
          if (e.kind >= 0 && !bpt) {
            glitch::FetchFormatMismatch(glitch::CurrentDraw(), slot, base_start, uint32_t(key.format),
                                        uint32_t(e.kind), e.dest_format, width, height, "mip_fetch_vs_resolve");
          }
          if (!bpt || uint32_t(key.endianness) != e.endian || e.w < lw || e.h < lh ||
              (e.canvas && !e.Covers(lw, lh)) || row_pitch != e.pitch_aligned * bpt ||
              (level && (e.kind != lv[0]->kind || e.swap_rb != lv[0]->swap_rb)) ||
              (!s.ignore_resolve_write && !s.watch->Clean(e.range, true, e.gen_sum))) {
            break;
          }
          lv[nlev++] = &e;
        }
        if (nlev >= 1) {
          if (nlev <= max_level) ++s.c.mip_levels_clamped;
          uint8_t sw[4];
          ResSwizzle(*lv[0], fetch.swizzle, sw);
          uint64_t seqs[16];
          for (uint32_t i = 0; i < nlev; ++i) seqs[i] = lv[i]->seq;
          uint32_t ids[16] = {};
          for (uint32_t i = 0; i < nlev; ++i) ids[i] = lv[i]->snapshot;
          nb::TextureDesc desc;
          desc.width = width;
          desc.height = height;
          desc.format = KindTexFormat(lv[0]->kind);
          std::memcpy(desc.swizzle, sw, 4);
          uint64_t chain_bytes = 0;
          for (uint32_t i = 0; i < nlev; ++i)
            chain_bytes += uint64_t(std::max(1u, width >> i)) * std::max(1u, height >> i) *
                           (lv[0]->kind == 3 ? 8u : 4u);
          const uint64_t h64 = XXH3_64bits(seqs, sizeof(uint64_t) * nlev);
          char kb[96];
          std::snprintf(kb, sizeof(kb), "M%016llx.%u:%ux%u:%u%u%u%u", (unsigned long long)h64, nlev, width, height, sw[0],
                        sw[1], sw[2], sw[3]);
          // Card S3-69 5b (WWE2K14_D3D_DEST_MIPS=1): one persistent mip image per destination chain (base, levels, size,
          // kind, swizzle); levels whose resolve entry changed are copied into it in place.
          char pkb[80];
          std::snprintf(pkb, sizeof(pkb), "P%08X.%u.%d:%ux%u:%u%u%u%u", base_start, nlev, lv[0]->kind, width, height, sw[0],
                        sw[1], sw[2], sw[3]);
          const bool persistent_mips = dest::MipsOn() || ResolveIntoOn();
          const std::string ckey = persistent_mips ? std::string(pkb) : std::string(kb);
          auto cit = s.cache.find(ckey);
          auto check_mips = [&](uint32_t persistent_handle, const Entry& persistent_entry) {
            if (!ResolveIntoCheckOn() || TranslateOnly()) return;
            std::string check_error;
            const uint32_t old_handle = nb::TextureFromSnapshotLevels(ids, nlev, desc, &check_error);
            ++s.resolve_into_checks;
            uint64_t covered_texels = 0;
            bool mismatch = old_handle == nb::kNone;
            for (uint32_t i = 0; i < nlev && old_handle != nb::kNone; ++i) {
              const uint32_t lw = std::max(1u, width >> i), lh = std::max(1u, height >> i);
              std::vector<uint8_t> expected, actual;
              std::string read_error;
              const bool old_ok = nb::ReadTextureLevel(old_handle, desc.format, i, lw, lh, expected, &read_error);
              const bool new_ok = nb::ReadTextureLevel(persistent_handle, desc.format, i, lw, lh, actual, &read_error);
              if (!old_ok || !new_ok || expected.size() != actual.size()) {
                mismatch = true;
                continue;
              }
              covered_texels += uint64_t(lw) * lh;
              const uint32_t bpp = lv[0]->kind == 3 ? 8u : 4u;
              for (size_t p = 0; p + bpp <= actual.size(); p += bpp) {
                if (std::memcmp(expected.data() + p, actual.data() + p, bpp) != 0) {
                  mismatch = true;
                  ++s.resolve_into_covered_mismatches;
                  const uint32_t texel = uint32_t(p / bpp);
                  LogResolveIntoFirstDifference("mips", key, base_start, width, height, i, texel % lw,
                                                texel / lw, bpp, expected.data() + p, actual.data() + p,
                                                *lv[i], i < persistent_entry.pseq_n ? persistent_entry.pseq[i] : 0);
                }
              }
            }
            if (old_handle != nb::kNone) nb::ReleaseTexture(old_handle);
            s.resolve_into_mismatches += mismatch;
            s.resolve_into_covered_texels += covered_texels;
          };
          if (cit != s.cache.end() && persistent_mips && cit->second.pseq_n == nlev) {
            Entry& pe = cit->second;
            bool ok = true, changed = false;
            uint64_t changed_bytes = 0;
            std::string err;
            for (uint32_t i = 0; i < nlev && ok; ++i) {
              if (pe.pseq[i] == seqs[i]) continue;
              if (!TranslateOnly() && !nb::CopySnapshotToTextureLevel(pe.handle, lv[i]->snapshot, i, std::max(1u, width >> i),
                                                                       std::max(1u, height >> i), &err)) {
                ok = false;
                break;
              }
              pe.pseq[i] = seqs[i];
              changed = true;
              const uint32_t lw = std::max(1u, width >> i), lh = std::max(1u, height >> i);
              changed_bytes += uint64_t(lw) * lh * (lv[0]->kind == 3 ? 8u : 4u);
              ++dest::C().mip_levels_copied;
            }
            if (ok) {
              if (changed) {
                pe.content = (1ull << 63) | (1ull << 61) | (h64 >> 3);
                ++dest::C().mip_inplace;
              }
              if (changed) {
                census_resolve_paste_bytes += changed_bytes;
                s.resolve_into_bytes_saved += chain_bytes > changed_bytes ? chain_bytes - changed_bytes : 0;
              }
              pe.last_use = s.frame;
              ++s.c.hits;
              ++s.c.resolve_binds;
              check_mips(pe.handle, pe);
              return finish(pe.handle, pe.content);
            }
            DropEntry(s, cit);  // could not update in place: build a new one below
            cit = s.cache.end();
          }
          if (cit != s.cache.end()) {
            cit->second.last_use = s.frame;
            ++s.c.hits;
            ++s.c.resolve_binds;
            check_mips(cit->second.handle, cit->second);
            return finish(cit->second.handle, cit->second.content);
          }
          if (!MakeRoom(s)) {
            ++s.c.refused_cap;
            return Refuse(s, "texture cap full", sl + "shadow texture cap full", why);
          }
          const uint64_t bytes = uint64_t(width) * height * (lv[0]->kind == 3 ? 8u : 4u) * 4 / 3;
          if (!MakeByteRoom(s, bytes)) {
            return Refuse(s, "texture byte cap full", sl + "shadow texture byte cap (WWE2K14_D3D_SHADOW_TEX_MB)", why);
          }
          uint32_t handle = 0;
          if (!TranslateOnly()) {
            std::string err;
            { TxT tx(5); handle = nb::TextureFromSnapshotLevels(ids, nlev, desc, &err); }
            if (handle == nb::kNone) return refuse("backend: " + err);
            census_fetch_rebuilt = true;
            census_resolve_paste_bytes += chain_bytes;
          }
          Entry ne;
          ne.bytes = bytes;
          ne.handle = handle;
          ne.range = range;
          ne.frame = ne.last_use = s.frame;
          ne.content = (1ull << 63) | (1ull << 61) | (h64 >> 3);
          if (persistent_mips) {  // persistent chain: update the changed snapshot levels in place
            for (uint32_t i = 0; i < nlev; ++i) ne.pseq[i] = seqs[i];
            ne.pseq_n = nlev;
            ++dest::C().mip_new;
          } else {
            ne.res_start = lv[0]->start;
            ne.res_seq = lv[0]->seq;
          }
          CachePut(s, ckey, ne);
          s.c.live_bytes += ne.bytes;
          if (!persistent_mips) for (uint32_t i = 0; i < nlev; ++i) lv[i]->derived.push_back(ckey);  // old sequence cache
          NoteCreated(s);
          ++s.c.resolve_binds;
          ++s.c.resolve_mipped;
          ++s.c.misses;
          check_mips(handle, ne);
          return finish(handle, ne.content);
        }
      }
      if (ov.size() > 1) {
        // Card S3-33c diagnostic (WWE2K14_D3D_OVERLAP_TRACE=1; at most 40 lines; remove when settled).
        static const bool otrace = [] {
          const char* e = std::getenv("WWE2K14_D3D_OVERLAP_TRACE");
          return e && *e == '1';
        }();
        static int olines = 0;
        if (otrace && olines < 40) {
          ++olines;
          std::string es;
          for (const Res* o : ov) {
            es += fmt::format(" [{:08X}+{:X} seq={} kind={} {}x{} canvas={} dest={:08X} clean={}]", o->start, o->length,
                              o->seq, o->kind, o->w, o->h, o->canvas, o->dest_base,
                              s.watch->Clean(o->range, true, o->gen_sum));
          }
          REXLOG_INFO("[d3d-otrace] frame={} fetch fmt{} {}x{} dim={} mips={} tiled={} base={:08X}+{:X} mip={:08X}+{:X} res_seq={} |{}",
                      s.frame, uint32_t(key.format), width, height, dimension, uint32_t(key.mip_max_level), bool(key.tiled),
                      base_start, base_span, mip_start, mip_span, s.res_seq, es);
        }
        return refuse("overlaps several resolves");
      }
      if (r.kind < 0) return refuse("resolve: " + r.why_unusable);
      if (r.start != base_start || cube || key.mip_max_level != 0 || !key.tiled || base_start == 0 ||
          (mip_span && range[3] > range[2] && range[2] != range[0])) {
        {  // Card S3-33d diagnostic (WWE2K14_D3D_OVERLAP_TRACE=1; <= 40 lines; remove when settled)
          static const bool ptrace = [] {
            const char* e = std::getenv("WWE2K14_D3D_OVERLAP_TRACE");
            return e && *e == '1';
          }();
          static int plines = 0;
          if (ptrace && plines < 40 && uint32_t(key.format) == 15) {
            ++plines;
            std::string es;
            for (const Res* o : ov) {
              es += fmt::format(" [{:08X}+{:X} seq={} kind={} {}x{} canvas={} pitch={}]", o->start, o->length, o->seq, o->kind,
                                o->w, o->h, o->canvas, o->pitch_aligned);
            }
            REXLOG_INFO("[d3d-ptrace] frame={} fetch fmt{} {}x{} mips={} tiled={} base={:08X}+{:X} mip={:08X}+{:X} |{}",
                        s.frame, uint32_t(key.format), width, height, uint32_t(key.mip_max_level), bool(key.tiled),
                        base_start, base_span, mip_start, mip_span, es);
          }
        }
        return refuse("partial overlap / not a single-level tiled fetch at the destination base");
      }
      // Kind vs fetch format (as nr's ReuseReject and per-kind fetch checks).
      uint32_t bpt = 4;
      bool ok = false;
      switch (r.kind) {
        case 0: ok = key.format == xenos::TextureFormat::k_8_8_8_8; break;
        case 1: ok = key.format == xenos::TextureFormat::k_8 && key.endianness != xenos::Endian::k16in32; bpt = 1; break;
        case 2: ok = key.format == xenos::TextureFormat::k_24_8_FLOAT; break;
        case 3: ok = key.format == xenos::TextureFormat::k_16_16_16_16; bpt = 8; break;
        case 4: ok = key.format == xenos::TextureFormat::k_16_16; break;
        case 5: ok = key.format == xenos::TextureFormat::k_4_4_4_4; bpt = 2; break;
      }
      if (!ok) {
        glitch::FetchFormatMismatch(glitch::CurrentDraw(), slot, base_start, uint32_t(key.format), uint32_t(r.kind),
                                    r.dest_format, width, height, "fetch_vs_resolve_kind");
        return refuse("format " + std::to_string(uint32_t(key.format)) + " vs resolve kind " + std::to_string(r.kind));
      }
      if (plan.any_signed) return refuse("signed fetch");
      // Card M2c3: same pitch (checked below) = same texel layout, so a fetch no larger than the resolved image takes
      // its top-left region (a canvas must have every 8x8 cell of that region written by some resolve).
      // Card S3-33g (WWE2K14_D3D_SEED_PLAIN=1): a plain entry at the fetch base that is smaller than the fetch (s345:
      // 80x80 resolve at the origin of a 1024x512 atlas, same pitch) - seed the guest bytes and paste the entry's image
      // at (0,0), like the M2c6 partial canvas.
      static const bool seed_plain = [] {
        const char* e = std::getenv("WWE2K14_D3D_SEED_PLAIN");
        return e && std::strcmp(e, "1") == 0;
      }();
      const bool plain_seed = seed_plain && !r.canvas && (width > r.w || height > r.h);
      if (width > r.w && !plain_seed) return refuse("width " + std::to_string(width) + " > " + std::to_string(r.w));
      if (r.canvas && r.seed_pending && width <= r.w && height <= r.h) {
        const bool uncovered_request = !r.Covers(width, height);
        if (!uncovered_request) {
          ++s.seed_lazy.would_skip;
        } else {
          const SeedEnsureResult ensured = EnsureSeedMaterialized(s, r, SeedReason::kBind);
          if (ensured == SeedEnsureResult::kDirtyBind) {
            ++s.c.resolve_written;
            return refuse("destination written since the resolve (watch)");
          }
        }
      }
      // Card S3-69 5a: a destination image (canvas) seeded from the guest bytes binds where no resolve has landed yet
      // (its texels there read as the guest memory's, their only writer) instead of the refusal; unseeded canvases and
      // plain entries keep the M2c6 guest seed below.
      const bool dest_uncovered = dest::CanvasOn() && r.canvas && r.seeded && width <= r.w && height <= r.h &&
                                  !r.Covers(width, height);
      if (((r.canvas && !r.Covers(width, height)) || plain_seed) && !dest_uncovered) {
        // Card M2c6: memory semantics for a partly resolved destination - texels no resolve wrote are the guest
        // memory's (their only writer); the resolved cells are pasted over it from the canvas. Only where the canvas
        // texels equal what a fetch of the copy-out reads (no R/B swap; kinds whose snapshot is the fetch's host
        // format - checked again by the backend), single level, 2D.
        const bool seedable = !r.swap_rb && (r.kind == 0 || r.kind == 3 || r.kind == 4) && key.mip_max_level == 0 &&
                              !cube && key.base_page;
        if (!seedable) return refuse("region not covered by resolves");
        if (uint32_t(key.endianness) != r.endian) return refuse("endian");
        if (layout.base.row_pitch_bytes != r.pitch_aligned * bpt) return refuse("pitch");
        if (height > r.h && !plain_seed) return refuse("height " + std::to_string(height) + " > " + std::to_string(r.h));
        if (!s.ignore_resolve_write && !s.watch->Clean(r.range, true, r.gen_sum)) {
          ++s.c.resolve_written;
          return refuse("destination written since the resolve (watch)");
        }
        auto seed_rects = [&](bool count_plain) {
          std::vector<int32_t> rects;
          const uint32_t cols = plain_seed ? 0 : (width + 7) / 8, rows = plain_seed ? 0 : (height + 7) / 8;
          if (plain_seed) {
            rects = {0, 0, int32_t(std::min(r.w, width)), int32_t(std::min(r.h, height))};
            if (count_plain) ++s.c.resolve_plain_seeded;
          }
          for (uint32_t cy = 0; cy < rows; ++cy) {
            for (uint32_t cx = 0; cx < cols;) {
              if (!r.cov[size_t(cy) * r.cov_cols + cx]) {
                ++cx;
                continue;
              }
              uint32_t ex = cx;
              while (ex < cols && r.cov[size_t(cy) * r.cov_cols + ex]) ++ex;
              const int32_t x0 = int32_t(cx * 8), x1 = int32_t(std::min(ex * 8, width));
              const int32_t y0 = int32_t(cy * 8), y1 = int32_t(std::min(cy * 8 + 8, height));
              bool merged = false;
              for (size_t i = 0; i < rects.size(); i += 4) {
                if (rects[i] == x0 && rects[i + 2] == x1 && rects[i + 3] == y0) {
                  rects[i + 3] = y1;
                  merged = true;
                  break;
                }
              }
              if (!merged) rects.insert(rects.end(), {x0, y0, x1, y1});
              cx = ex;
            }
          }
          return rects;
        };
        auto rect_bytes = [&](const std::vector<int32_t>& rects) {
          uint64_t bytes = 0;
          for (size_t i = 0; i + 3 < rects.size(); i += 4) {
            const uint32_t rw = uint32_t(std::max(0, rects[i + 2] - rects[i]));
            const uint32_t rh = uint32_t(std::max(0, rects[i + 3] - rects[i + 1]));
            bytes += uint64_t(rw) * rh * plan.bpb;
          }
          return bytes;
        };
        auto resolve_update_rects = [&](uint64_t last_update_seq) {
          if (!r.canvas) {
            const int32_t x1 = int32_t(std::min(r.w, width));
            const int32_t y1 = int32_t(std::min(r.h, height));
            if (!x1 || !y1) return std::vector<int32_t>{};
            return std::vector<int32_t>{0, 0, x1, y1};
          }
          std::vector<int32_t> rects;
          for (const Res::RectWrite& write : r.resolve_rect_history) {
            if (write.seq <= last_update_seq) continue;
            const int32_t x0 = std::clamp(write.rect[0], 0, int32_t(width));
            const int32_t y0 = std::clamp(write.rect[1], 0, int32_t(height));
            const int32_t x1 = std::clamp(write.rect[2], x0, int32_t(width));
            const int32_t y1 = std::clamp(write.rect[3], y0, int32_t(height));
            if (x1 > x0 && y1 > y0) rects.insert(rects.end(), {x0, y0, x1, y1});
          }
          return rects;
        };
        auto covered_bytes = [&]() {
          if (plain_seed) return uint64_t(std::min(r.w, width)) * std::min(r.h, height) * plan.bpb;
          uint64_t bytes = 0;
          for (uint32_t cy = 0; cy < (height + 7) / 8; ++cy) {
            for (uint32_t cx = 0; cx < (width + 7) / 8; ++cx) {
              if (!r.cov[size_t(cy) * r.cov_cols + cx]) continue;
              bytes += uint64_t(std::min(8u, width - cx * 8)) * std::min(8u, height - cy * 8) * plan.bpb;
            }
          }
          return bytes;
        };
        nb::TextureDesc seed_desc;
        seed_desc.width = width;
        seed_desc.height = height;
        seed_desc.format = plan.format;
        sh::TextureSwizzle(fetch, plan, seed_desc.swizzle);
        auto check_seed = [&](uint32_t new_handle, uint64_t persistent_last_update_seq) {
          if (!ResolveIntoCheckOn() || TranslateOnly()) return;
          const std::vector<int32_t> ref_rects = seed_rects(false);
          std::vector<uint8_t> covered(size_t(width) * height, 0);
          for (size_t i = 0; i + 3 < ref_rects.size(); i += 4) {
            for (int32_t y = ref_rects[i + 1]; y < ref_rects[i + 3]; ++y)
              std::fill(covered.begin() + size_t(y) * width + ref_rects[i],
                        covered.begin() + size_t(y) * width + ref_rects[i + 2], uint8_t(1));
          }
          std::vector<nb::LevelData> old_levels(1);
          const sh::LevelGeom old_geom = sh::ComputeLevelGeom(key, layout, plan, 0);
          std::string error;
          uint32_t old_handle = nb::kNone;
          bool mismatch = false;
          uint64_t covered_texels = 0, uncovered_texels = 0;
          if (dest_uncovered) {
            uint8_t old_swizzle[4];
            ResSwizzle(r, fetch.swizzle, old_swizzle);
            nb::TextureDesc old_desc = seed_desc;
            std::memcpy(old_desc.swizzle, old_swizzle, sizeof(old_swizzle));
            const bool crop = width < r.w || height < r.h;
            old_handle = crop ? nb::TextureFromSnapshotCrop(r.snapshot, old_desc, &error)
                              : nb::TextureFromSnapshot(r.snapshot, old_desc, &error);
            mismatch = old_handle == nb::kNone;
          } else {
            if (!sh::DetileLevelCpu(key, plan, old_geom, 0, 0,
                                    s.memory->TranslatePhysical<const uint8_t*>(old_geom.address), &old_levels[0])) {
              mismatch = true;
            } else {
              old_handle = nb::CreateTexture(seed_desc, old_levels, &error);
              if (old_handle == nb::kNone ||
                  !nb::CopySnapshotRectsToTexture(old_handle, r.snapshot, ref_rects.data(),
                                                   uint32_t(ref_rects.size() / 4), &error)) {
                mismatch = true;
              }
            }
          }
          if (old_handle != nb::kNone && !mismatch) {
            std::vector<uint8_t> old_bytes, new_bytes;
            const bool old_ok = nb::ReadTexture(old_handle, plan.format, width, height, old_bytes, &error);
            const bool new_ok = nb::ReadTexture(new_handle, plan.format, width, height, new_bytes, &error);
            if (!old_ok || !new_ok || old_bytes.size() != new_bytes.size()) {
              mismatch = true;
            } else {
              for (size_t px = 0; px < size_t(width) * height; ++px) {
                if (covered[px]) ++covered_texels; else ++uncovered_texels;
                const size_t at = px * plan.bpb;
                if (std::memcmp(old_bytes.data() + at, new_bytes.data() + at, plan.bpb) != 0) {
                  mismatch = true;
                  if (covered[px]) ++s.resolve_into_covered_mismatches;
                  else ++s.resolve_into_uncovered_mismatches;
                  LogResolveIntoFirstDifference("seed", key, base_start, width, height, 0,
                                                uint32_t(px % width), uint32_t(px / width), plan.bpb,
                                                old_bytes.data() + at, new_bytes.data() + at, r,
                                                persistent_last_update_seq);
                }
              }
            }
          }
          if (old_handle != nb::kNone) nb::ReleaseTexture(old_handle);
          ++s.resolve_into_checks;
          s.resolve_into_mismatches += mismatch;
          s.resolve_into_covered_texels += covered_texels;
          s.resolve_into_uncovered_texels += uncovered_texels;
        };
        char kb[96];
        std::snprintf(kb, sizeof(kb), "S%llu:%ux%u", (unsigned long long)r.seq, width, height);
        const std::string ckey = ResolveIntoOn() ? persistent_fetch_key : std::string(kb);
        auto cit = s.cache.find(ckey);
        if (cit != s.cache.end()) {
          if (ResolveIntoOn()) {
            Entry& pe = cit->second;
            if (s.watch->Clean(pe.range, pe.gen_valid, pe.gen_sum) && pe.pseq_n == 1) {
              const bool same_lineage = pe.persistent_res_start == r.start && pe.persistent_lineage == r.lineage_id;
              const bool history_gap = r.canvas &&
                  (r.resolve_rect_history.empty() ||
                   (r.resolve_rect_history_overflow && pe.pseq[0] < r.resolve_rect_history.front().seq));
              if (!same_lineage || pe.pseq[0] > r.seq || history_gap) {
                DropEntry(s, cit);  // resolve was replaced or the bounded history no longer reaches this texture
              } else if (pe.pseq[0] < r.seq) {
                const std::vector<int32_t> update = resolve_update_rects(pe.pseq[0]);
                std::string error;
                if (!update.empty() &&
                    !nb::CopySnapshotRectsToTexture(pe.handle, r.snapshot, update.data(), uint32_t(update.size() / 4), &error)) {
                  DropEntry(s, cit);
                } else {
                  pe.pseq[0] = r.seq;
                  pe.content = (1ull << 63) | r.seq;
                  pe.last_use = s.frame;
                  const uint64_t updated_bytes = rect_bytes(update);
                  census_resolve_paste_bytes += updated_bytes;
                  const uint64_t full_paste = covered_bytes();
                  s.resolve_into_bytes_saved += pe.bytes + (full_paste > updated_bytes ? full_paste - updated_bytes : 0);
                  ++s.c.hits;
                  ++s.c.resolve_binds;
                  check_seed(pe.handle, pe.pseq[0]);
                  return finish(pe.handle, pe.content);
                }
              } else {
                pe.last_use = s.frame;
                ++s.c.hits;
                ++s.c.resolve_binds;
                check_seed(pe.handle, pe.pseq[0]);
                return finish(pe.handle, pe.content);
              }
            } else {
              DropEntry(s, cit);  // changed guest range: rebuild from its current, write-watch-validated bytes
            }
            cit = s.cache.end();
          }
        }
        if (!ResolveIntoOn() && cit != s.cache.end()) {
          if (s.watch->Clean(cit->second.range, cit->second.gen_valid, cit->second.gen_sum)) {
            cit->second.last_use = s.frame;
            ++s.c.hits;
            ++s.c.resolve_binds;
            return finish(cit->second.handle, cit->second.content);
          }
          DropEntry(s, cit);  // guest texels changed: rebuild
        }
        if (!MakeRoom(s)) {
          ++s.c.refused_cap;
          return Refuse(s, "texture cap full", sl + "shadow texture cap full", why);
        }
        // Covered cell runs inside the fetched region, merged vertically.
        std::vector<int32_t> rects = seed_rects(true);
        if (rects.size() / 4 > 1024) return refuse("too many covered runs");
        ArmIfNeeded(s, base_start, base_span);
        Entry ne;
        ne.range = range;
        ne.gen_sum = s.watch->GenSum(range);
        ne.gen_valid = true;
        const sh::LevelGeom g0 = sh::ComputeLevelGeom(key, layout, plan, 0);
        std::vector<nb::LevelData> levels(1);
        if (!sh::DetileLevelCpu(key, plan, g0, 0, 0, s.memory->TranslatePhysical<const uint8_t*>(g0.address), &levels[0])) {
          return refuse("seed detile");
        }
        const uint64_t bytes = levels[0].bytes.size();
        if (!MakeByteRoom(s, bytes)) {
          return Refuse(s, "texture byte cap full", sl + "shadow texture byte cap (WWE2K14_D3D_SHADOW_TEX_MB)", why);
        }
        uint32_t handle = 0;
        if (!TranslateOnly()) {
          std::string err;
          handle = nb::CreateTexture(seed_desc, levels, &err);
          if (handle == nb::kNone) return refuse("seed upload: " + err);
          if (!nb::CopySnapshotRectsToTexture(handle, r.snapshot, rects.data(), uint32_t(rects.size() / 4), &err)) {
            nb::ReleaseTexture(handle);
            return refuse("seed paste: " + err);
          }
          census_fetch_rebuilt = true;
          census_guest_detile_bytes = bytes;
          census_guest_upload_bytes = bytes;
          for (size_t i = 0; i + 3 < rects.size(); i += 4) {
            const uint32_t rw = uint32_t(std::max(0, rects[i + 2] - rects[i]));
            const uint32_t rh = uint32_t(std::max(0, rects[i + 3] - rects[i + 1]));
            census_resolve_paste_bytes += uint64_t(rw) * rh * plan.bpb;
          }
          check_seed(handle, r.seq);
        }
        ne.handle = handle;
        ne.bytes = bytes;
        ne.frame = ne.last_use = s.frame;
        ne.content = (1ull << 63) | (1ull << 60) | r.seq;
          if (ResolveIntoOn()) {
            ne.pseq[0] = r.seq;
            ne.pseq_n = 1;
            ne.persistent_res_start = r.start;
            ne.persistent_lineage = r.lineage_id;
          } else {
          ne.res_start = r.start;
          ne.res_seq = r.seq;
        }
        CachePut(s, ckey, ne);
        s.c.live_bytes += bytes;
        s.c.upload_bytes += bytes;
        if (!ResolveIntoOn()) r.derived.push_back(ckey);
        NoteCreated(s);
        ++s.c.resolve_binds;
        ++s.c.resolve_seeded;
        ++s.c.misses;
        return finish(handle, ne.content);
      }
      if (uint32_t(key.endianness) != r.endian) return refuse("endian");
      if (layout.base.row_pitch_bytes != r.pitch_aligned * bpt) return refuse("pitch");
      // Card S3-69 5a: a destination image models the whole destination (its size, not the written extent, bounds it).
      if (base_span > r.length && !(dest::CanvasOn() && r.canvas && width <= r.w && height <= r.h)) return refuse("extent");
      if (height > r.h) return refuse("height " + std::to_string(height) + " > " + std::to_string(r.h));
      if (!s.ignore_resolve_write && !s.watch->Clean(r.range, true, r.gen_sum)) {
        // Either the game wrote the memory since the resolve (then the entry is stale), or the SDK's own resolve
        // write fired the callback (then it is not). Counted, named, never guessed.
        ++s.c.resolve_written;
        return refuse("destination written since the resolve (watch)");
      }
      // Cached derived texture for (resolve entry, desc).
      const bool crop = width < r.w || height < r.h;
      uint8_t sw[4];
      for (int c = 0; c < 4; ++c) {
        uint8_t v = uint8_t((fetch.swizzle >> (3 * c)) & 7);
        if (r.kind == 1) {
          if (v <= 3) v = r.swap_rb ? 2 : 0;  // one channel: byte 0 of the (R/B swapped) texel
        } else if (r.kind == 2) {
          if (v >= 1 && v <= 3) v = 0;
        } else if (r.kind == 4) {
          if (v >= 2 && v <= 3) v = 1;
        } else if (r.swap_rb && (v == 0 || v == 2)) {
          v = uint8_t(2 - v);
        }
        sw[c] = v;
      }
      if (dest_uncovered) {
        ++dest::C().uncovered_binds;
        static uint32_t ulines[2] = {};
        const bool seed_swap_rb = r.swap_rb && r.kind == 0;
        const uint32_t ui = seed_swap_rb ? 1u : 0u;
        if (dest::TraceOn() && ulines[ui] < 8) {
          // Card S3-69 5a evidence (deciding metric): the canvas texels of the uncovered cells must equal what this
          // fetch's own decode of the guest bytes reads (the M2c6 path) - detile the fetch size with the fetch key and
          // the canvas size with the seed key, hash the uncovered 8x8 cells of both, and compare.
          ++ulines[ui];
          const sh::LevelGeom fg0 = sh::ComputeLevelGeom(key, layout, plan, 0);
          std::vector<nb::LevelData> frows(1);
          const bool fok =
              sh::DetileLevelCpu(key, plan, fg0, 0, 0, s.memory->TranslatePhysical<const uint8_t*>(fg0.address), &frows[0]);
          TexKey dkey;
          sh::TexFormatPlan dplan;
          const bool dok = CanvasSeedKey(r.dest_base, r.kind, r.endian, r.pitch_aligned, r.w, r.h, &dkey, &dplan);
          std::vector<nb::LevelData> drows;
          bool size_ok = false;
          uint64_t fetch_cells = 0, canvas_cells = 0;
          bool match = false;
          if (fok && dok) {
            const sh::LevelGeom dg0 = sh::ComputeLevelGeom(dkey, dkey.GetGuestLayout(), dplan, 0);
            drows.resize(1);
            if (sh::DetileLevelCpu(dkey, dplan, dg0, 0, 0, s.memory->TranslatePhysical<const uint8_t*>(dg0.address),
                                   &drows[0]) &&
                drows[0].bytes.size() >= size_t(r.w) * r.h * dplan.bpb) {
              size_ok = true;
              if (!s.hash_state) s.hash_state = XXH3_createState();
              XXH3_64bits_reset(s.hash_state);
              fetch_cells = CellHash(frows[0].bytes, width, width, height, bpt, r.cov, r.cov_cols, s.hash_state,
                                     seed_swap_rb);
              XXH3_64bits_reset(s.hash_state);
              canvas_cells = CellHash(drows[0].bytes, r.w, width, height, dplan.bpb, r.cov, r.cov_cols, s.hash_state,
                                      seed_swap_rb);
              match = fetch_cells == canvas_cells;
            }
          }
          size_t n = 0;
          for (uint8_t c : r.cov) n += c;
          REXLOG_INFO("[d3d-dest] seed-hash frame={} match={} fmt{} {}x{} base={:08X}+{:X} entry {:08X}+{:X} seq={} {}x{} "
                      "swap_rb={} "
                      "cov={}/{} obj={:08X}/{:08X} seed={:016x} now={:016x} detile={}{}{} hist:{}", s.frame, match,
                      uint32_t(key.format), width, height, base_start, base_span, r.start, r.length, r.seq, r.w, r.h,
                      r.swap_rb, n,
                      r.cov.size(), bound_obj, r.dest_texture, canvas_cells, fetch_cells, fok ? 'f' : '-', dok ? 'd' : '-',
                      size_ok ? 's' : '-', dest::History(r.dest_base));
        }
      }
      char kb[96];
      std::snprintf(kb, sizeof(kb), "R%llu:%ux%u:%u%u%u%u", (unsigned long long)r.seq, width, height, sw[0], sw[1], sw[2], sw[3]);
      const std::string ckey = kb;
      auto it = s.cache.find(ckey);
      if (it != s.cache.end()) {
        it->second.last_use = s.frame;
        ++s.c.hits;
        ++s.c.resolve_binds;
        return finish(it->second.handle, it->second.content);
      }
      if (!MakeRoom(s)) {
        ++s.c.refused_cap;
        return Refuse(s, "texture cap full", sl + "shadow texture cap full", why);
      }
      if (!MakeByteRoom(s, uint64_t(width) * height * (r.kind == 3 ? 8u : 4u))) {
        return Refuse(s, "texture byte cap full", sl + "shadow texture byte cap (WWE2K14_D3D_SHADOW_TEX_MB)", why);
      }
      uint32_t handle = 0;
      if (!TranslateOnly()) {
        nb::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = r.kind == 2 ? nb::TexFormat::kR32F : r.kind == 3 ? nb::TexFormat::kRGBA16
                    : r.kind == 4 ? nb::TexFormat::kRG16 : nb::TexFormat::kRGBA8;
        std::memcpy(desc.swizzle, sw, 4);
        std::string err;
        { TxT tx(5); handle = crop ? nb::TextureFromSnapshotCrop(r.snapshot, desc, &err) : nb::TextureFromSnapshot(r.snapshot, desc, &err); }
        if (handle == nb::kNone) return refuse("backend: " + err);
        census_fetch_rebuilt = true;
        if (crop) ++s.c.resolve_crops;
      }
      Entry ne;
      ne.bytes = uint64_t(width) * height * (r.kind == 3 ? 8u : 4u);
      ne.handle = handle;
      ne.range = range;
      ne.frame = ne.last_use = s.frame;
      ne.content = (1ull << 63) | r.seq;
      ne.res_start = r.start;
      ne.res_seq = r.seq;
      CachePut(s, ckey, ne);
      s.c.live_bytes += ne.bytes;
      r.derived.push_back(ckey);
      NoteCreated(s);
      ++s.c.resolve_binds;
      ++s.c.misses;
      return finish(handle, ne.content);
    }
  }

  // ---- guest texture ----
  std::string ckey = (asm_l1 ? "A" : base_only ? "g" : "G") +
                     std::string(reinterpret_cast<const char*>(&fetch_key), sizeof(fetch_key));  // "R..." = resolve-derived; "g" = base level only (S3-33d); "A" = base + snapshot level 1 (S3-33h)
  if (asm_l1) ckey += std::string(reinterpret_cast<const char*>(&asm_l1->seq), sizeof(asm_l1->seq));
  bpath = asm_l1 ? 'A' : base_only ? 'g' : 'G';
  const glitch::DrawContext& glitch_draw = glitch::CurrentDraw();
  if ((bpath == 'G' || bpath == 'g' || bpath == 'A') &&
      glitch::ClaimUnwrittenTextureProbe(glitch_draw, base_start, mip_start, mip_span, uint32_t(key.format), width,
                                         height, dimension)) {
    const uint32_t sampled_base = base_span >= 4 ? 256u : 0u;
    const uint32_t sampled_mip = mip_span >= 4 ? 256u : 0u;
    const uint32_t zero_dwords = SampleZeros(s, base_start, base_span) + SampleZeros(s, mip_start, mip_span);
    const uint32_t sampled_dwords = sampled_base + sampled_mip;
    if (sampled_dwords && zero_dwords == sampled_dwords) {
      glitch::UnwrittenTextureCandidate(glitch_draw, slot, base_start, base_span + mip_span, mip_start, mip_span,
                                        uint32_t(key.format), width, height, dimension, zero_dwords,
                                        sampled_dwords);
    }
  }
  auto it = s.cache.find(ckey);
  bool forced_hit = false;  // card S3-44a
  if (it != s.cache.end()) {
    Entry& e = it->second;
    bool same = true;
    if (e.frame != s.frame && BindTraceOn() && width >= 256 && height >= 256 &&
        s.watch->Clean(e.range, e.gen_valid, e.gen_sum)) {
      // S3-33c-select-silhouettes diagnostic: verify a watch-trusted hit by hash (each key at most every 30 frames).
      static std::unordered_map<std::string, uint64_t> vlast;
      static uint32_t vlines = 0;
      uint64_t& lf = vlast[ckey];
      if (s.frame >= lf + 30) {
        lf = s.frame;
        const XXH128_hash_t h = sh::TextureHash(s.hash_state, ckey, key, layout, s.memory);
        if (!XXH128_isEqual(h, e.hash) && vlines < 400) {
          ++vlines;
          REXLOG_INFO("[d3d-btrace] watch-miss frame={} base={:08X}+{:X} fmt{} {}x{} mips={} built_frame={} old={:016x} "
                      "now={:016x}", s.frame, base_start, range[1] - range[0], uint32_t(key.format), width, height,
                      uint32_t(key.mip_max_level), e.frame, e.content, h.low64);
        }
      }
    }
    // Card S3-44a (WWE2K14_D3D_LATE_TEX=4, sub-switch WWE2K14_D3D_LATE4_HITS=0 = off): a hit on a learned late base in
    // a new worker frame means the game has not rewritten it YET (movie plane sets alternate per present; g06 8623 /
    // 8627 showed the set's content from two presents earlier). Rebuild it so the upload sits in the unsubmitted list
    // and registers with late_upload, which re-reads the guest bytes at the present release.
    if (e.frame != s.frame && late_upload::HitsOn() && late_upload::IsLateBase(base_start)) {
      same = false;
      forced_hit = true;
    } else if (e.frame != s.frame) {
      // Card S3-44 (WWE2K14_D3D_DYN_REHASH=1): a texture whose bytes changed twice already (video frames, s362: the
      // second buffer of a double-buffered movie kept its old content although the game rewrote it - the write watch
      // missed it) is re-hashed at its first use in every frame instead of trusting the watch.
      static const bool dyn_rehash = [] {
        const char* v = std::getenv("WWE2K14_D3D_DYN_REHASH");
        return v && *v == '1';
      }();
      const bool force = dyn_rehash && s.dyn_keys.count(ckey) != 0;
      if (!force && s.watch->Clean(e.range, e.gen_valid, e.gen_sum)) {
        // generation sum unchanged and every page still armed: no write touched it since the hash
      } else {
        ArmIfNeeded(s, base_start, base_span);
        ArmIfNeeded(s, mip_start, mip_span);
        const uint64_t gsum = s.watch->GenSum(e.range);
        {
          TxT tx(1);
          g_txp.bytes_hashed += layout.base.level_data_extent_bytes + layout.mips_total_extent_bytes;
          same = XXH128_isEqual(sh::TextureHash(s.hash_state, ckey, key, layout, s.memory), e.hash);
        }
        if (same) {
          e.gen_sum = gsum;
          e.gen_valid = true;
        }
      }
    }
    if (same) {
      e.frame = e.last_use = s.frame;
      ++s.c.hits;
      return finish(e.handle, e.content);
    }
    ++s.c.rebuilds;
    if (++s.rebuild_count[ckey] >= 2) s.dyn_keys.insert(ckey);  // card S3-44
    {
      std::lock_guard<std::mutex> lk(g_dyn_bases_mu);
      g_dyn_bases.insert(base_start);  // card S3-44: content changed since first build (late-write candidates)
    }
    DropEntry(s, it);
  }

  // Miss: arm -> generation baseline -> hash -> read bytes (never the reverse, as nr).
  if (!MakeRoom(s)) {
    ++s.c.refused_cap;
    return Refuse(s, "texture cap full", sl + "shadow texture cap full", why);
  }
  ArmIfNeeded(s, base_start, base_span);
  ArmIfNeeded(s, mip_start, mip_span);
  Entry ne;
  ne.range = range;
  ne.gen_sum = s.watch->GenSum(range);
  ne.gen_valid = true;
  if (s.quality) {
    const quality::RangeEndpoints observed{range[0], range[1], range[2], range[3]};
    s.quality->writes.Observe(range[0], observed, ne.gen_sum);
  }
  {
    TxT tx(2);
    ne.hash = sh::TextureHash(s.hash_state, ckey, key, layout, s.memory);
  }
  ne.content = ne.hash.low64;

  const uint32_t faces = cube ? 6 : 1;
  const uint32_t first_level = key.base_page ? 0 : 1;
  std::vector<nb::LevelData> levels;
  uint64_t bytes = 0;
  TxT* tx_detile = new TxT(3);
  for (uint32_t face = 0; face < faces; ++face) {
    const uint32_t last_level = asm_l1 ? first_level + 1 : base_only ? first_level : uint32_t(key.mip_max_level);
    for (uint32_t level = first_level; level <= last_level; ++level) {
      const sh::LevelGeom g = sh::ComputeLevelGeom(key, layout, plan, level);
      const uint64_t a = uint64_t(g.address) + uint64_t(face) * g.lay->array_slice_stride_bytes;
      if (a >= kPhysLimit) return Refuse(s, "address out of physical range", sl + "level address outside physical memory", why);
      nb::LevelData data;
      if (!sh::DetileLevelCpu(key, plan, g, level, face, s.memory->TranslatePhysical<const uint8_t*>(uint32_t(a)), &data)) {
        return Refuse(s, "8bpp 16in32", sl + "8bpp texture with 16in32 swap", why);
      }
      bytes += data.bytes.size();
      levels.push_back(std::move(data));
    }
  }
  delete tx_detile;
  if (levels.empty()) return Refuse(s, "no levels", sl + "no addressable level", why);
  // GitHub #62 diagnostic (11-15, Qwen N-28): black lines at the TOP of videos. For a texture the game rewrites (movie
  // planes), count leading all-zero rows of level 0; a band of zero rows above non-zero rows = the plane was read while the
  // game was still writing it. One line per event, first 40.
  if (IsDynamicBase(base_start) && levels[0].height > 8 && !levels[0].bytes.empty()) {
    const nb::LevelData& l0 = levels[0];
    const size_t row = l0.bytes.size() / l0.height;
    uint32_t lead = 0;
    while (lead < l0.height) {
      const uint8_t* r = l0.bytes.data() + size_t(lead) * row;
      bool zero = true;
      for (size_t i = 0; i < row && zero; ++i) zero = r[i] == 0;
      if (!zero) break;
      ++lead;
    }
    // Only a JUMP is interesting: animations grow or shrink a black band smoothly (boot logo: 7 -> 34 rows over 50 frames,
    // run gh62-1), a racing read shows a band that is absent in the uploads before it. Log jumps of >= 8 rows.
    static std::mutex lead_mu;
    static std::unordered_map<uint32_t, uint32_t> last_lead;
    static std::atomic<uint32_t> jump_lines{0}, jumps{0};
    uint32_t prev = 0;
    {
      std::lock_guard<std::mutex> lk(lead_mu);
      auto it = last_lead.find(base_start);
      prev = it == last_lead.end() ? lead : it->second;
      last_lead[base_start] = lead;
    }
    if (lead < l0.height && lead >= prev + 8) {
      const uint32_t n = jumps.fetch_add(1, std::memory_order_relaxed) + 1;
      if (jump_lines.fetch_add(1, std::memory_order_relaxed) < 60 || (n & 255) == 0) {
        REXLOG_INFO("[gh62-plane] jump #{} frame={} base={:08X} {}x{} fmt={} zero_top_rows {} -> {} (of {})", n, s.frame,
                    base_start, l0.width, l0.height, uint32_t(key.format), prev, lead, l0.height);
      }
    }
  }
  if (!MakeByteRoom(s, bytes)) {
    return Refuse(s, "texture byte cap full", sl + "shadow texture byte cap (WWE2K14_D3D_SHADOW_TEX_MB)", why);
  }
  uint32_t handle = 0;
  if (!TranslateOnly()) {
    nb::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = plan.format;
    desc.cube = cube;
    sh::TextureSwizzle(fetch, plan, desc.swizzle);
    std::string err;
    {
      TxT tx(4);
      handle = nb::CreateTexture(desc, levels, &err);
    }
    if (handle == nb::kNone) return Refuse(s, "upload: " + err, sl + err, why);
    if (asm_l1) {  // card S3-33h: level 1 from the resolve snapshot (the guest level-1 bytes were only a placeholder)
      if (first_level != 0 ||
          !nb::CopySnapshotToTextureLevel(handle, asm_l1->snapshot, 1, std::max(1u, width >> 1), std::max(1u, height >> 1), &err)) {
        nb::ReleaseTexture(handle);
        return Refuse(s, "assemble level 1: " + err, sl + "assemble level 1: " + err, why);
      }
      asm_l1->derived.push_back(ckey);  // dropping the entry drops this texture
      ++s.c.resolve_assembled;
    }
  }
  // FL-4 Design A (WWE2K14_D3D_LATE_TEX=4): the game rewrites this base after its draw call, and mode 4 does not wait
  // for it (draw.cpp). Hand the upload to the registry: it holds the early submits and re-detiles/refills the same
  // staging bytes at the release boundary (late_upload.cpp). Registration never affects the draw.
  if (late_upload::On() && late_upload::IsLateBase(base_start)) {
    late_upload::Register(handle, ckey, key, layout, plan, faces, first_level,
                          asm_l1 ? 2u : base_only ? 1u : uint32_t(key.mip_max_level) - first_level + 1, levels.size(),
                          ne.hash.low64, ne.hash.high64, forced_hit);
  }
  if (s.quality && !TranslateOnly() && bpath == 'G') {
    QualityUpload u;
    u.range = range;
    auto& i = u.info;
    i.known = true;
    i.ordinary_guest = true;
    // CreateTexture's Texture2D path creates a sampled texture, not a Surface
    // target. Surface handles are a separate namespace (never compare ids).
    i.sampled_only = true;
    i.two_d = !cube;
    i.width = width;
    i.height = height;
    i.base_level = first_level;
    uint32_t level = first_level;
    bool complete = !cube;
    for (const auto& data : levels) {
      complete &= data.face == 0 && data.mip == level && !data.bytes.empty() &&
                  data.width == std::max(1u, width >> level) && data.height == std::max(1u, height >> level);
      ++level;
    }
    i.resident_levels = complete ? uint32_t(levels.size()) : 0;
    i.late_upload = late_upload::IsLateBase(base_start);
    if (!s.quality->uploads.Put(handle, u)) {
      s.quality->valid = false;
      s.quality->invalid_causes |= 1;
    }
  }
  ne.handle = handle;
  ne.bytes = bytes;
  ne.frame = ne.last_use = s.frame;
  s.c.upload_bytes += bytes;
  ++s.c.misses;
  CachePut(s, ckey, ne);
  s.c.live_bytes += bytes;
  NoteCreated(s);
  ++s.epoch;  // new handle: older memos are unaffected, but keep the rule "any table change bumps"
  return finish(handle, ne.content);
}

void OnWorkerFrameEnd() {
  State& s = S();
  ++s.frame;
  if (SeedLazyDiagnosticsOn() && !nb::ShaderFormatIsDxil() && s.frame - s.seed_lazy_log_frame >= 600) {
    s.seed_lazy_log_frame = s.frame;
    const auto& c = s.seed_lazy;
    REXLOG_INFO("[d3d-seed-lazy] frame={} created={} skipped={} materialized_by_reason=bind:{} writeback:{} propagation:{} "
                "would-skip={} would-materialize={} dirty-refused={} dirty-propagation={} coverage-fallback={} mask_MB={:.2f}",
                s.frame, c.created, c.skipped, c.materialized_bind, c.materialized_writeback, c.materialized_propagation,
                c.would_skip, c.would_materialize, c.dirty_refused, c.dirty_propagation, c.backup_fallback,
                double(s.seed_mask_bytes) / 1048576.0);
    if (SeedLazyCheckMode()) {
      REXLOG_INFO("[d3d-seed-check] CHECK frame={} checks={} mismatch={} rate_skipped={} failures={}",
                  s.frame, c.checks, c.mismatch, c.check_rate_skipped, c.check_failed);
    }
    // The CHECK verdict must see every mismatch of the run: only the rate counters restart per window.
    const auto kept = s.seed_lazy;
    s.seed_lazy = {};
    s.seed_lazy.checks = kept.checks;
    s.seed_lazy.mismatch = kept.mismatch;
    s.seed_lazy.check_failed = kept.check_failed;
    s.seed_lazy.check_rate_skipped = kept.check_rate_skipped;
  }
  if (nb::MemoryOwnersDue()) {
    nb::MemoryOwnerFrontendAges ages;
    for (const auto& kv : s.cache) {
      ++ages.texture_entries;
      if (s.frame > kv.second.last_use && s.frame - kv.second.last_use > 600) ++ages.texture_old600;
    }
    for (const auto& kv : s.resolves) {
      const Res& r = kv.second;
      if (r.snapshot == nb::kNone) continue;
      ++ages.resolve_entries;
      ages.resolve_snapshot_ids.push_back(r.snapshot);
      if (s.frame > r.mem_last_use_frame && s.frame - r.mem_last_use_frame > 600) ++ages.resolve_old600;
    }
    std::sort(ages.resolve_snapshot_ids.begin(), ages.resolve_snapshot_ids.end());
    ages.resolve_snapshot_ids.erase(
        std::unique(ages.resolve_snapshot_ids.begin(), ages.resolve_snapshot_ids.end()), ages.resolve_snapshot_ids.end());
    REXLOG_INFO("{}", nb::MemoryOwnersLine(ages));
  }
  if (ResolveIntoOn()) {
    static auto last = std::chrono::steady_clock::now();
    static uint64_t last_frame = s.frame;
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - last).count();
    if (elapsed >= 1.0) {
      const double frames = double(std::max<uint64_t>(1, s.frame - last_frame));
      REXLOG_INFO("[d3d-resolve-into] mode={} window_s={:.2f} checks/s={:.2f} mismatches/s={:.2f} "
                  "covered_mismatches/s={:.2f} uncovered_mismatches/s={:.2f} covered_texels/s={:.0f} "
                  "uncovered_texels/s={:.0f} bytes_saved/s={:.0f} frames={:.0f}",
                  ResolveIntoMode(), elapsed, s.resolve_into_checks / elapsed, s.resolve_into_mismatches / elapsed,
                  s.resolve_into_covered_mismatches / elapsed, s.resolve_into_uncovered_mismatches / elapsed,
                  s.resolve_into_covered_texels / elapsed, s.resolve_into_uncovered_texels / elapsed,
                  s.resolve_into_bytes_saved / elapsed, frames);
      last = now;
      last_frame = s.frame;
      s.resolve_into_checks = s.resolve_into_mismatches = 0;
      s.resolve_into_covered_mismatches = s.resolve_into_uncovered_mismatches = 0;
      s.resolve_into_covered_texels = s.resolve_into_uncovered_texels = 0;
      s.resolve_into_bytes_saved = 0;
    }
  }
  if (s.frame % 300 == 0) {
    std::string l;
    for (int k = 0; k < 6; ++k)
      l += " " + std::string(kTxName[k]) + "=" + std::to_string(double(g_txp.ns[k]) / 1e6 / 300).substr(0, 6) + "(" +
           std::to_string(g_txp.n[k] / 300) + "/f)";
    REXLOG_INFO("[d3d-tex] time split ms/f:{} | revalidate_MB/f={:.2f} | fmemo hits/f={} forgets/f={}", l,
                double(g_txp.bytes_hashed) / 1048576.0 / 300, s.fmemo_hits / 300, s.fmemo_forgets / 300);
    s.fmemo_hits = s.fmemo_forgets = 0;
    uint64_t ls[5];
    nb::LazySnapshotStats(ls);
    static uint64_t prev[5] = {};
    if (ls[0] != prev[0]) {
      REXLOG_INFO("[d3d-tex] lazy snapshots per frame: created={:.1f} copied={:.1f} (by reader {:.1f}, before overwrite {:.1f}) "
                  "never_copied={:.1f}", (ls[0] - prev[0]) / 300.0, (ls[1] - prev[1]) / 300.0, (ls[2] - prev[2]) / 300.0,
                  (ls[3] - prev[3]) / 300.0, (ls[4] - prev[4]) / 300.0);
    }
    std::memcpy(prev, ls, sizeof(prev));
    g_txp = TxProf{};
  }
  if (s.frame - s.logged_frame < 300) return;
  s.logged_frame = s.frame;
  const Counters& c = s.c;
  const Counters& p = s.last_logged;
  if (c.hits + c.misses + c.refusals + c.resolves_registered == p.hits + p.misses + p.refusals + p.resolves_registered) return;
  REXLOG_INFO("[d3d-tex] frame={} hits={} (memo {}) misses={} (rebuilds {}) upload={:.1f}MB resolve_binds={} refusals={} "
              "(cap {} bytes {} resolve {}) resolves={} (unusable {}, written {}, composed {}, crops {}, cubes {}, mipped {}, mip_clamped {}, stale_dropped {}, seeded {}, written_dropped {}, watch_kept {}, bypassed {}, materialized {}, plain_seeded {}, assembled {}) live={} high={} cap={} evict={} live_mb={:.1f} byte_cap_mb={} res_entries={}",
              s.frame, c.hits, c.memo_hits, c.misses, c.rebuilds, double(c.upload_bytes) / (1024.0 * 1024.0), c.resolve_binds,
              c.refusals, c.refused_cap, c.refused_bytes, c.refused_resolve, c.resolves_registered, c.resolves_unusable, c.resolve_written, c.resolves_composed, c.resolve_crops, c.resolve_cubes, c.resolve_mipped, c.mip_levels_clamped, c.resolve_stale_dropped, c.resolve_seeded, c.resolve_stale_written_dropped, c.resolve_watch_kept, c.resolve_overlap_bypassed, c.resolve_materialized, c.resolve_plain_seeded, c.resolve_assembled,
              c.live, c.live_high, c.cap, c.evictions, double(c.live_bytes) / 1048576.0, c.byte_cap >> 20, s.resolves.size());
  // B1-a: per-frame cost of finding a dropped resolve's persistent entries (index lookup; walk = old full-cache scan).
  REXLOG_INFO("[d3d-tex] persist-index mode={} drops/f={:.1f} index_us/f={:.2f} walk_us/f={:.2f} keys={} check={} bad={}",
              PersistIndexMode(s), s.persist_drops / 300.0, s.persist_ns / 300000.0, s.persist_scan_ns / 300000.0,
              s.persist_idx.size(), s.persist_check_n, s.persist_check_bad);
  s.persist_drops = s.persist_ns = s.persist_scan_ns = 0;
  if (dest::CountOn()) {  // card S3-69: registry image bytes (sampled here) + dest counters
    dest::Counters& dc = dest::C();
    dc.dest_bytes = 0;
    dc.dest_entries = uint32_t(s.resolves.size());
    dc.dest_canvases = 0;
    for (const auto& kv : s.resolves) {
      const Res& r = kv.second;
      if (r.snapshot == nb::kNone) continue;
      dc.dest_bytes += uint64_t(r.w) * r.h * (r.kind == 3 ? 8u : 4u);
      dc.dest_canvases += r.canvas;
    }
    dc.dest_bytes_high = std::max(dc.dest_bytes_high, dc.dest_bytes);
    REXLOG_INFO("[d3d-dest] periodic frame={}{}", s.frame, dest::Section());
  }
  std::vector<std::pair<uint64_t, std::string>> top;
  for (const auto& kv : s.hist) top.emplace_back(kv.second, kv.first);
  std::sort(top.rbegin(), top.rend());
  for (size_t i = 0; i < top.size() && i < 6; ++i) REXLOG_INFO("[d3d-tex]   refusal x{}: {}", top[i].first, top[i].second);
  s.last_logged = c;
}


// Card S3-17: CPU reads of resolve destinations in draw-off (see textures.h). The bytes come from the live registry
// entry's snapshot only, converted with the nr CPU copy-out arithmetic (nr_frontend.cpp CopyColor, CPU branch):
// optional R/B swap, dword endian swap (8888), k_8 byte xor / 4444 packing + 16-bit endian handling, tiled 2D offsets.
// Texels outside the entry's written area (canvas cells no resolve covered) and bytes outside [base, base+length) keep
// guest memory's contents.
CpuVisResult WriteBackResolves(uint32_t base, uint32_t length,
                               const std::function<void(uint32_t, const std::vector<uint8_t>&)>& write) {
  CpuVisResult out;
  State& s = S();
  if (!length || TranslateOnly() || !EnsureInit() || !s.memory) {
    out.refused.push_back("no registry (translate-only or no memory)");
    return out;
  }
  const uint64_t req_lo = base, req_hi = uint64_t(base) + length;
  for (Res* r : Overlapping(s, base, uint32_t(std::min<uint64_t>(req_hi, kPhysLimit)))) {
    ++out.entries;
    char tag[64];
    std::snprintf(tag, sizeof(tag), "%08X+%X", r->start, r->length);
    if (r->kind != 0 && r->kind != 1 && r->kind != 5) {
      out.refused.push_back(std::string(tag) + " kind " + std::to_string(r->kind) +
                            (r->kind < 0 ? " (" + r->why_unusable + ")" : ""));
      continue;
    }
    if (r->start != r->dest_base) {
      out.refused.push_back(std::string(tag) + " image origin is not the destination origin");
      continue;
    }
    if (r->snapshot == nb::kNone || !r->w || !r->h) {
      out.refused.push_back(std::string(tag) + " no snapshot");
      continue;
    }
    // A pending seed needs nothing here: the loop below emits only resolve-covered cells.
    std::vector<uint8_t> px;
    std::string err;
    if (!nb::ReadSnapshot(r->snapshot, r->w, r->h, px, &err) || px.size() < size_t(r->w) * r->h * 4) {
      out.refused.push_back(std::string(tag) + " readback: " + err);
      continue;
    }
    const uint32_t bpp_log2 = r->kind == 0 ? 2 : r->kind == 1 ? 0 : 1, bpp = 1u << bpp_log2;
    const uint64_t lo = std::max<uint64_t>(req_lo, r->start);
    const uint64_t hi = std::min<uint64_t>(req_hi, uint64_t(r->start) + r->length);
    if (hi <= lo) continue;
    std::vector<uint8_t> buf(size_t(hi - lo));
    rr::NotePhys(uint32_t(lo), buf.size());
    std::memcpy(buf.data(), s.memory->TranslatePhysical<const uint8_t*>(uint32_t(lo)), buf.size());  // pre-copy bytes
    const uint32_t endian = r->endian;
    uint64_t texels = 0;
    for (uint32_t y = 0; y < r->h; ++y) {
      for (uint32_t x = 0; x < r->w; ++x) {
        if (r->canvas && !r->cov[size_t(y / 8) * r->cov_cols + x / 8]) continue;
        uint8_t p[4];
        std::memcpy(p, px.data() + (size_t(y) * r->w + x) * 4, 4);
        if (r->swap_rb) std::swap(p[0], p[2]);
        int64_t o = sh::TiledOffset2D(int32_t(x), int32_t(y), r->pitch_aligned, bpp_log2);
        if (bpp == 4) {
          uint32_t v;
          std::memcpy(&v, p, 4);
          v = sh::SwapDword(v, endian);
          std::memcpy(p, &v, 4);
        } else if (bpp == 2) {
          uint32_t packed = 0;
          for (uint32_t c = 0; c < 4; ++c) {
            const float f = std::min(std::max(float(p[c]) * 0.00392156886f, 0.0f), 1.0f);
            packed |= uint32_t(f * 15.0f + 0.5f) << (4 * c);
          }
          p[0] = uint8_t(packed);
          p[1] = uint8_t(packed >> 8);
          if (endian == 1 || endian == 2) std::swap(p[0], p[1]);
          if (endian == 2 || endian == 3) o ^= 2;
        } else {
          o = endian == 1 ? (o ^ 1) : endian == 2 ? (o ^ 3) : o;
        }
        const int64_t a = int64_t(r->dest_base) + o;
        if (a < int64_t(lo) || a + bpp > int64_t(hi)) continue;
        std::memcpy(buf.data() + size_t(a - int64_t(lo)), p, bpp);
        ++texels;
      }
    }
    write(uint32_t(lo), buf);
    ++out.served;
    out.texels += texels;
    out.bytes += buf.size();
  }
  return out;
}

}  // namespace wwe2k14::d3d::textures

#else  // !WWE2K14_HAS_NR: the nr backend is not built; nothing to serve.

namespace wwe2k14::d3d::textures {

void EnableSamplingQualityMetadata() {}
quality::SamplingQualityInfo GetSamplingQualityInfo(uint32_t) { return {}; }
SamplingQualityMetadataStats GetSamplingQualityMetadataStats() { return {}; }
SamplingQualityFetchInfo GetSamplingQualityFetchInfo(const DeviceState&, uint32_t) { return {}; }
uint32_t ForSlot(const DeviceState&, uint32_t, uint32_t, nr::backend::SamplerDesc*, uint64_t*, std::string* why) {
  if (why) *why = "nr backend not built";
  return UINT32_MAX;
}
uint32_t LitShadowTexture(std::string* why) {
  if (why) *why = "nr backend not built";
  return UINT32_MAX;
}
void RegisterResolveImage(uint32_t, uint32_t, const ResolveDesc&, uint32_t, const nr::backend::ResolveCensusKey&) {}
ResolveRegistrationFineStats TakeResolveRegistrationFineStats() { return {}; }
void OnWorkerFrameEnd() {}
void SetMemory(rex::memory::Memory*) {}
void ReplayMarkWritten(uint32_t, uint32_t) {}
void ForEachResolveEntry(const std::function<void(uint32_t, uint32_t, const ResolveDesc&, uint32_t,
                                                  const nr::backend::ResolveCensusKey&)>&) {}
void SetDrawShaders(const rex::graphics::Shader*, const rex::graphics::Shader*) {}
static Counters g_counters;
std::string TakeFrameBinds() { return {}; }
bool IsDynamicBase(uint32_t) { return false; }
bool FetchBaseRange(const DeviceState&, uint32_t, uint32_t*, uint32_t*, uint32_t*, uint32_t*) { return false; }
const Counters& GetCounters() { return g_counters; }
CpuVisResult WriteBackResolves(uint32_t, uint32_t, const std::function<void(uint32_t, const std::vector<uint8_t>&)>&) {
  return {};
}
}  // namespace wwe2k14::d3d::textures

#endif
