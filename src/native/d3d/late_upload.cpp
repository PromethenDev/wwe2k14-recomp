// FL-4 "Design A" (WWE2K14_D3D_LATE_TEX=4) - see late_upload.h for the design and the one-source rule.
#include "late_upload.h"

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging.h>

#include "textures.h"

namespace sh = nr::shared;
namespace nb = nr::backend;

namespace wwe2k14::d3d::late_upload {
namespace {

// textures.cpp:50 guards its level reads with this bound before TranslatePhysical.
constexpr uint32_t kPhysLimit = 0x20000000u;

struct Pending {
  uint32_t handle = 0;
  std::string ckey;
  sh::TextureKeys::TextureKey key;
  rex::graphics::texture_util::TextureGuestLayout layout;
  sh::TexFormatPlan plan;
  uint32_t faces = 0, first_level = 0, per_face = 0;
  uint64_t hash_lo = 0, hash_hi = 0;
  bool forced = false;  // card S3-44a: registered by a rebuilt cache hit
  std::vector<nb::LevelData> levels;  // re-detile destinations (filled at the release)
};

// The registry is only touched with the backend lock held by the caller (mode 2: the translating worker vs the CP
// thread); the mutex covers the remaining combinations (e.g. a release during a record-thread sync).
std::mutex g_mu;
std::vector<Pending> g_pending;
uint64_t g_released_present = 0, g_released_irq = 0, g_refilled = 0, g_unchanged = 0, g_skipped_stale = 0;
uint64_t g_forced = 0, g_forced_refilled = 0, g_forced_unchanged = 0, g_forced_stale = 0;  // card S3-44a
uint64_t g_present_calls = 0;
uint64_t g_log_lines = 0;

// Re-detile exactly the levels the translate loop detiled (textures.cpp:1444-1457): same geometry helpers, same order
// (face major), same physical bound. Returns false when the texture no longer matches the registered layout.
bool ReDetile(Pending& p, rex::memory::Memory* memory, std::string* error) {
  uint32_t i = 0;
  for (uint32_t face = 0; face < p.faces; ++face) {
    for (uint32_t l = 0; l < p.per_face; ++l, ++i) {
      const uint32_t level = p.first_level + l;
      const sh::LevelGeom g = sh::ComputeLevelGeom(p.key, p.layout, p.plan, level);
      const uint64_t a = uint64_t(g.address) + uint64_t(face) * g.lay->array_slice_stride_bytes;
      if (a >= kPhysLimit) {
        *error = "level address outside physical memory";
        return false;
      }
      if (!sh::DetileLevelCpu(p.key, p.plan, g, level, face,
                              memory->TranslatePhysical<const uint8_t*>(uint32_t(a)), &p.levels[i])) {
        *error = "8bpp texture with 16in32 swap";
        return false;
      }
    }
  }
  if (i != p.levels.size()) {
    *error = "level count";
    return false;
  }
  return true;
}

}  // namespace

bool On() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_D3D_LATE_TEX");
    return v && *v == '4';
  }();
  return on;
}

bool HitsOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_D3D_LATE4_HITS");
    return On() && !(v && *v == '0');
  }();
  return on;
}

void Register(uint32_t handle, const std::string& ckey, const sh::TextureKeys::TextureKey& key,
              const rex::graphics::texture_util::TextureGuestLayout& layout, const sh::TexFormatPlan& plan,
              uint32_t faces, uint32_t first_level, uint32_t per_face, size_t levels_count, uint64_t hash_lo,
              uint64_t hash_hi, bool forced_hit) {
  if (!On()) return;
  if (handle == nb::kNone || !faces || !per_face || size_t(faces) * per_face != levels_count) {
    // The caller's level loop and the parameters disagree (or the texture failed): never registered this way, but say so
    // once, otherwise mode 4 would silently do nothing.
    if (g_log_lines++ < 8) {
      REXLOG_WARN("[d3d-late4] register refused (handle {} faces {} per_face {} levels {})", handle, faces, per_face,
                  levels_count);
    }
    return;
  }
  Pending p;
  p.handle = handle;
  p.ckey = ckey;
  p.key = key;
  p.layout = layout;
  p.plan = plan;
  p.faces = faces;
  p.first_level = first_level;
  p.per_face = per_face;
  p.hash_lo = hash_lo;
  p.hash_hi = hash_hi;
  p.forced = forced_hit;
  p.levels.resize(levels_count);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (forced_hit) ++g_forced;
    g_pending.push_back(std::move(p));
  }
  // One source for the deferral: the backend's own early-submit gate. Cleared at the next release, never by anyone else.
  nb::SetKickHold(true);
}

void Release(Why why) {
  if (!On()) return;
  std::vector<Pending> items;
  bool empty = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (why == kPresent && ++g_present_calls % 600 == 0) {
      REXLOG_INFO(
          "[d3d-late4] pending={} released_present={} released_irq={} refilled={} unchanged={} skipped_stale={} | "
          "S3-44a forced_hits={} forced_refilled={} forced_unchanged={} forced_stale={}",
          g_pending.size(), g_released_present, g_released_irq, g_refilled, g_unchanged, g_skipped_stale, g_forced,
          g_forced_refilled, g_forced_unchanged, g_forced_stale);
    }
    if (g_pending.empty()) {
      nb::SetKickHold(false);
      // card S3-67: learner resolve outside g_mu (it takes g_probe_mu); still under the caller's backend lock
      empty = true;
    } else {
      items.swap(g_pending);
      if (why == kPresent) g_released_present += items.size();
      else g_released_irq += items.size();
    }
    // The hold stays up until every refill below is done: a submission in between would let the GPU run the recorded
    // copies while the staging bytes are being rewritten.
  }
  if (empty) {
    LearnAtRelease();
    return;
  }
  rex::memory::Memory* memory = textures::SharedMemory();
  XXH3_state_t* state = XXH3_createState();
  uint64_t refilled = 0, unchanged = 0, stale = 0, f_refilled = 0, f_unchanged = 0, f_stale = 0;
  for (Pending& p : items) {
    if (!memory || !state) {
      ++stale;
      f_stale += p.forced;
      continue;
    }
    const XXH128_hash_t h = sh::TextureHash(state, p.ckey, p.key, p.layout, memory);
    if (h.low64 == p.hash_lo && h.high64 == p.hash_hi) {
      ++unchanged;  // the bytes the upload already holds are still the guest's
      f_unchanged += p.forced;
      continue;
    }
    std::string err;
    if (!ReDetile(p, memory, &err)) {
      ++stale;
      f_stale += p.forced;
      REXLOG_WARN("[d3d-late4] re-detile failed ({}) for handle {}", err, p.handle);
      continue;
    }
    if (nb::RefillTextureUpload(p.handle, p.levels, &err)) {
      ++refilled;
      f_refilled += p.forced;
    } else {
      ++stale;  // the command list holding the upload was submitted: those bytes are already fixed
      f_stale += p.forced;
      if (g_log_lines++ < 8) REXLOG_WARN("[d3d-late4] refill skipped ({}) for handle {}", err, p.handle);
    }
  }
  if (state) XXH3_freeState(state);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_refilled += refilled;
    g_unchanged += unchanged;
    g_skipped_stale += stale;
    g_forced_refilled += f_refilled;
    g_forced_unchanged += f_unchanged;
    g_forced_stale += f_stale;
  }
  // Card S3-67: after the refill re-read (a learner hash taken later than the refill read can only learn more), before
  // the hold drops.
  LearnAtRelease();
  nb::SetKickHold(false);
}

}  // namespace wwe2k14::d3d::late_upload
