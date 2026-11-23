// WWE '13 PC: GPU time census (gpu_census.h). Test-only diagnostic, off by default.
#include "gpu/gpu_census.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(wwe13_gpu_census, false, "WWE13",
                    "Test only: split the GPU time of each frame into draws (by target size), resolves, "
                    "clears, uploads and present, logged as [census] every 10 s (D3D12 build; costs GPU time)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(REBLUE_D3D12) && !defined(REBLUE_D3D11)

#include <algorithm>
#include <format>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <plume_d3d12.h>
#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/resources.h"

namespace bd::gpu::census {
namespace {
constexpr u32 kCapacity = 16384;      // timestamps per frame slot
constexpr u16 kEnd = 0xFFFF;          // end of a command list: the gap after it is not charged
constexpr u16 kNone = 0xFFFE;
constexpr u32 kMaxBuckets = 48;       // draw target sizes
constexpr u16 kBucketBase = 16;       // draw keys: kBucketBase + bucket
constexpr u32 kMaxResolveSigs = 48;  // resolve signatures (source -> destination)
constexpr u16 kResolveBase = kBucketBase + kMaxBuckets + 1;
constexpr u32 kKeys = kResolveBase + kMaxResolveSigs + 1;

const char *const kKindNames[kKindCount] = {"draw", "resolve-copy", "resolve-draw", "clear",
                                            "upload", "present", "resolve-barrier", "other"};

struct Slot {
  std::unique_ptr<plume::RenderQueryPool> pool;
  std::vector<u16> keys;
  u32 count = 0;
  u32 list_first = 0;  // first timestamp of the open list (resolved at its end)
  bool overflow = false;
};

struct Census {
  std::array<Slot, kNumFrames> slots;
  u16 current = kNone;
  // Draw buckets: target width x height (0 x 0 = no colour target).
  std::array<u32, kMaxBuckets> bucket_w{}, bucket_h{};
  u32 buckets = 0;
  struct Sig { u32 sw, sh, sf, ss, dw, dh, df, sc; u8 kind; };
  std::array<Sig, kMaxResolveSigs> sigs{};
  u32 nsigs = 0;
  std::mutex mutex;  // accumulators (written on retire, read on present)
  std::array<double, kKeys> ns{};
  std::array<u64, kKeys> marks{};
  double between_lists_ns = 0;
  u64 frames = 0, overflows = 0;
  std::chrono::steady_clock::time_point window_start{};
};

Census &C() {
  static Census c;
  return c;
}

u16 DrawKey(Census &c, const GuestTexture *rt) {
  const u32 w = rt ? rt->width : 0, h = rt ? rt->height : 0;
  for (u32 i = 0; i < c.buckets; ++i)
    if (c.bucket_w[i] == w && c.bucket_h[i] == h)
      return u16(kBucketBase + i);
  if (c.buckets < kMaxBuckets) {
    c.bucket_w[c.buckets] = w;
    c.bucket_h[c.buckets] = h;
    return u16(kBucketBase + c.buckets++);
  }
  return u16(kBucketBase + kMaxBuckets);  // "other sizes"
}

u16 ResolveKey(Census &c, Kind kind, const GuestTexture *src, const GuestTexture *dst) {
  Census::Sig g{src->width, src->height, u32(src->format), u32(src->sampleCount), dst->width, dst->height,
                u32(dst->format), u32(dst->resolveScale * 100.0f + 0.5f), u8(kind)};
  for (u32 i = 0; i < c.nsigs; ++i) {
    const auto &e = c.sigs[i];
    if (e.sw == g.sw && e.sh == g.sh && e.sf == g.sf && e.ss == g.ss && e.dw == g.dw && e.dh == g.dh &&
        e.df == g.df && e.sc == g.sc && e.kind == g.kind)
      return u16(kResolveBase + i);
  }
  if (c.nsigs < kMaxResolveSigs) {
    c.sigs[c.nsigs] = g;
    return u16(kResolveBase + c.nsigs++);
  }
  return u16(kResolveBase + kMaxResolveSigs);
}

void Write(VideoState &s, Census &c, u16 key) {
  if (!s.command_list_open || !s.command_list || !s.device)
    return;
  Slot &slot = c.slots[s.frame.load(std::memory_order_relaxed) % kNumFrames];
  if (!slot.pool) {
    slot.pool = s.device->createQueryPool(kCapacity);
    slot.keys.assign(kCapacity, kNone);
  }
  if (slot.count >= kCapacity) {
    slot.overflow = true;
    return;
  }
  auto *pool = static_cast<plume::D3D12QueryPool *>(slot.pool.get());
  if (!pool->d3d)
    return;
  static_cast<plume::D3D12CommandList *>(s.command_list)
      ->d3d->EndQuery(pool->d3d, D3D12_QUERY_TYPE_TIMESTAMP, slot.count);
  slot.keys[slot.count++] = key;
}
}  // namespace

bool Enabled() {
  static const bool on = REXCVAR_GET(wwe13_gpu_census);
  return on;
}

void Mark(VideoState &s, Kind kind, const GuestTexture *rt, const GuestTexture *dst) {
  if (!Enabled())
    return;
  Census &c = C();
  const u16 key = kind == kDraw ? DrawKey(c, rt)
                  : (kind == kResolveCopy || kind == kResolveDraw) && rt && dst ? ResolveKey(c, kind, rt, dst)
                  : u16(kind);
  if (key == c.current)
    return;
  c.current = key;
  Write(s, c, key);
}

void OnListEnd(VideoState &s) {
  if (!Enabled() || !s.command_list_open || !s.command_list)
    return;
  Census &c = C();
  Write(s, c, kEnd);
  c.current = kNone;
  Slot &slot = c.slots[s.frame.load(std::memory_order_relaxed) % kNumFrames];
  if (!slot.pool || slot.count <= slot.list_first)
    return;
  auto *pool = static_cast<plume::D3D12QueryPool *>(slot.pool.get());
  auto *readback = static_cast<plume::D3D12Buffer *>(pool->readbackBuffer.get());
  static_cast<plume::D3D12CommandList *>(s.command_list)
      ->d3d->ResolveQueryData(pool->d3d, D3D12_QUERY_TYPE_TIMESTAMP, slot.list_first,
                              slot.count - slot.list_first, readback->d3d,
                              u64(slot.list_first) * sizeof(u64));
  slot.list_first = slot.count;
}

void OnSlotRetired(VideoState &s, u32 index) {
  if (!Enabled())
    return;
  Census &c = C();
  Slot &slot = c.slots[index % kNumFrames];
  if (slot.pool && slot.count > 1 && slot.list_first == slot.count) {
    slot.pool->queryResults();  // ns
    const u64 *t = slot.pool->getResults();
    std::lock_guard lock(c.mutex);
    for (u32 i = 0; i + 1 < slot.count; ++i) {
      const u16 key = slot.keys[i];
      const double dt = t[i + 1] >= t[i] ? double(t[i + 1] - t[i]) : 0.0;
      if (key == kEnd) {
        c.between_lists_ns += dt;
        continue;
      }
      if (key < kKeys) {
        c.ns[key] += dt;
        ++c.marks[key];
      }
    }
    ++c.frames;
    c.overflows += slot.overflow;
  }
  slot.count = 0;
  slot.list_first = 0;
  slot.overflow = false;
}

namespace {
bool g_manual_windows = false;  // Flush() owns the windows (replay in-run A/B)
void LogWindowLocked(Census &c, const char *label);
}  // namespace

void Flush(const char *label) {
  if (!Enabled())
    return;
  Census &c = C();
  std::lock_guard lock(c.mutex);
  g_manual_windows = true;
  if (c.frames)
    LogWindowLocked(c, label);
}

void OnPresent() {
  if (!Enabled() || g_manual_windows)
    return;
  Census &c = C();
  const auto now = std::chrono::steady_clock::now();
  if (c.window_start.time_since_epoch().count() == 0) {
    c.window_start = now;
    return;
  }
  if (std::chrono::duration<double>(now - c.window_start).count() < 10.0)
    return;
  c.window_start = now;
  std::lock_guard lock(c.mutex);
  if (!c.frames)
    return;
  LogWindowLocked(c, "");
}

namespace {
void LogWindowLocked(Census &c, const char *label) {
  const double n = double(c.frames);
  double total = 0;
  for (double v : c.ns)
    total += v;
  BD_INFO("[census] {}{} frames: GPU {:.2f} ms/frame recorded, {:.2f} ms between lists, {} overflows",
          label, c.frames, total / n / 1e6, c.between_lists_ns / n / 1e6, c.overflows);
  std::vector<std::pair<double, u32>> order;
  for (u32 k = 0; k < kKeys; ++k)
    if (c.ns[k] > 0)
      order.push_back({c.ns[k], k});
  std::sort(order.rbegin(), order.rend());
  std::string line;
  for (const auto &[v, k] : order) {
    std::string name;
    if (k < kKindCount)
      name = kKindNames[k];
    else if (k == kBucketBase + kMaxBuckets)
      name = "draw other-size";
    else if (k >= kResolveBase && k < kResolveBase + c.nsigs) {
      const auto &g = c.sigs[k - kResolveBase];
      name = std::format("{} {}x{} f{} s{} -> {}x{} f{} x{}%", g.kind == kResolveDraw ? "resolve-draw" : "resolve-copy",
                         g.sw, g.sh, g.sf, g.ss, g.dw, g.dh, g.df, g.sc);
    } else if (k >= kResolveBase)
      name = "resolve other";
    else
      name = "draw " + std::to_string(c.bucket_w[k - kBucketBase]) + "x" +
             std::to_string(c.bucket_h[k - kBucketBase]);
    line += std::format(" | {} {:.2f} ms ({:.0f} runs)", name, v / n / 1e6, c.marks[k] / n);
    if (line.size() > 600) {
      BD_INFO("[census]{}", line);
      line.clear();
    }
  }
  if (!line.empty())
    BD_INFO("[census]{}", line);
  c.ns.fill(0);
  c.marks.fill(0);
  c.between_lists_ns = 0;
  c.frames = 0;
  c.overflows = 0;
}
}  // namespace
}  // namespace bd::gpu::census

#else  // Vulkan / D3D11 builds: no census

namespace bd::gpu::census {
bool Enabled() { return false; }
void Mark(VideoState &, Kind, const GuestTexture *, const GuestTexture *) {}
void OnListEnd(VideoState &) {}
void OnSlotRetired(VideoState &, u32) {}
void OnPresent() {}
void Flush(const char *) {}
}  // namespace bd::gpu::census

#endif
