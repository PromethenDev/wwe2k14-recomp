// Card S3-8 tiling-decision probe - diagnostic, default OFF (WWE2K14_DIAG_TILING=1).
//
// sub_82C25CE0 (generated/wwe2k14_recomp.79.cpp:71096) starts the game's scene: it calls BeginTiling (0x82CBB1D0)
// with r5 = load_u32(desc+24) (the tile count) when that count is > 1, else a plain clear (sub_82CB8BA8). Draw-off mode
// makes the game tile in demo matches where the shipped renderer never does (UHD 630). This logs, per descriptor, every
// change of the fields that decide it (+24 tile count, +272, +276 depth surface, +280 enable) with the caller and a
// call count, so a run shows WHEN the decision flips. Observes only: __imp__ is called exactly once, unchanged.
// Remove when S3-8 is settled.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/ppc/context.h>

namespace {

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_DIAG_TILING");
    return v && std::strcmp(v, "1") == 0;
  }();
  return on;
}

struct Seen {
  uint32_t tiles = 0xFFFFFFFFu, f272 = 0, f276 = 0, f280 = 0;
};
std::mutex g_mu;
std::unordered_map<uint32_t, Seen> g_seen;  // by descriptor address
std::atomic<uint64_t> g_calls{0}, g_tiled{0};
uint32_t g_logged = 0;

}  // namespace

extern "C" PPC_FUNC(__imp__sub_82C25CE0);
extern "C" PPC_FUNC(sub_82C25CE0) {
  if (Enabled()) {
    const uint32_t desc = ctx.r3.u32;
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (desc) {
      const uint32_t f280 = PPC_LOAD_U32(desc + 280), tiles = PPC_LOAD_U32(desc + 24);
      const uint32_t f272 = PPC_LOAD_U32(desc + 272), f276 = PPC_LOAD_U32(desc + 276);
      if (f280 && tiles > 1) g_tiled.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lk(g_mu);
      Seen& s = g_seen[desc];
      if ((s.tiles != tiles || s.f280 != f280 || s.f272 != f272) && g_logged < 200) {
        ++g_logged;
        REXLOG_INFO("[tiling-probe] call={} desc={:08X} tiles(+24) {} -> {} +272 {:08X} +276 {:08X} +280 {:08X} lr={:08X} "
                    "tiled_calls={}",
                    n, desc, s.tiles == 0xFFFFFFFFu ? -1 : int64_t(s.tiles), tiles, f272, f276, f280,
                    uint32_t(ctx.lr), g_tiled.load(std::memory_order_relaxed));
      }
      s.tiles = tiles, s.f272 = f272, s.f276 = f276, s.f280 = f280;
    }
    if (n % 3000 == 0) {
      REXLOG_INFO("[tiling-probe] calls={} tiled={}", n, g_tiled.load(std::memory_order_relaxed));
    }
  }
  __imp__sub_82C25CE0(ctx, base);
}

// ---- governor probe (same gate): the stopwatch begin/end pair the game's quality governor reads ----------------------
// sub_82C1A2A8 = begin (timebase into this+0), sub_82C1A2B0 = end (delta -> this+8, held peak -> this+16, 30-update
// hold at +24) (generated/wwe2k14_recomp.79.cpp:42464-42525). sub_827F26C8 compares the peak against tier thresholds
// and steps the level at controller+16 (Luna s38-adaptive). Logged: who brackets the interval (lr), the measured
// deltas (ms at the 50 MHz guest timebase) and every level change.
namespace {
struct Watch {
  uint32_t begin_lr = 0, end_lr = 0;
  uint64_t n = 0;
  double sum_ms = 0, max_ms = 0;
};
std::unordered_map<uint32_t, Watch> g_watch;
uint32_t g_watch_logged = 0;
uint64_t g_end_calls = 0;
std::unordered_map<uint32_t, uint32_t> g_level;  // controller -> last +16
constexpr double kTicksPerMs = 50000.0;  // guest timebase 50 MHz (rate census guest_ticks_per_s)
}  // namespace

extern "C" PPC_FUNC(__imp__sub_82C1A2A8);
extern "C" PPC_FUNC(sub_82C1A2A8) {
  if (Enabled()) {
    std::lock_guard<std::mutex> lk(g_mu);
    Watch& w = g_watch[ctx.r3.u32];
    if (!w.begin_lr) w.begin_lr = uint32_t(ctx.lr);
  }
  __imp__sub_82C1A2A8(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82C1A2B0);
extern "C" PPC_FUNC(sub_82C1A2B0) {
  const uint32_t self = ctx.r3.u32, lr = uint32_t(ctx.lr);
  __imp__sub_82C1A2B0(ctx, base);
  if (Enabled() && self) {
    const double ms = double(int64_t(PPC_LOAD_U64(self + 8))) / kTicksPerMs;
    const double peak = double(int64_t(PPC_LOAD_U64(self + 16))) / kTicksPerMs;
    std::lock_guard<std::mutex> lk(g_mu);
    Watch& w = g_watch[self];
    if (!w.end_lr) {
      w.end_lr = lr;
      if (g_watch_logged++ < 20) {
        REXLOG_INFO("[gov-probe] stopwatch {:08X} begin lr={:08X} end lr={:08X} first delta {:.3f} ms", self, w.begin_lr,
                    lr, ms);
      }
    }
    ++w.n;
    w.sum_ms += ms;
    w.max_ms = std::max(w.max_ms, ms);
    if (++g_end_calls % 600 == 0) {
      for (auto& [k, v] : g_watch) {
        if (!v.n) continue;
        REXLOG_INFO("[gov-probe] stopwatch {:08X} n={} mean {:.3f} ms max {:.3f} ms peak(+16) {:.3f} ms (lr {:08X}/{:08X})",
                    k, v.n, v.sum_ms / double(v.n), v.max_ms, k == self ? peak : -1.0, v.begin_lr, v.end_lr);
        v.n = 0, v.sum_ms = 0, v.max_ms = 0;
      }
    }
  }
}

extern "C" PPC_FUNC(__imp__sub_827F26C8);
extern "C" PPC_FUNC(sub_827F26C8) {
  const uint32_t ctl = ctx.r3.u32;
  __imp__sub_827F26C8(ctx, base);
  if (Enabled() && ctl) {
    const uint32_t level = PPC_LOAD_U32(ctl + 16), dwell = PPC_LOAD_U32(ctl + 44);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_level.find(ctl);
    if (it == g_level.end() || it->second != level) {
      REXLOG_INFO("[gov-probe] governor {:08X} level {} -> {} dwell {} end_calls {}", ctl,
                  it == g_level.end() ? -1 : int64_t(it->second), level, dwell, g_end_calls);
      g_level[ctl] = level;
    }
  }
}
