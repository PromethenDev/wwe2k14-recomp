// D3D front-buffer snapshot queue for the external-present path. See present.h.
#include "present.h"
#include "gate.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

#include "capture.h"
#include "hooks.h"
#include "draw.h"
#include "late_upload.h"
#include "native/nr/nr_backend.h"
#include "resolves.h"
#include "textures.h"
#include "worker.h"

namespace wwe2k14::d3d {
namespace nb = nr::backend;

namespace {

// ---- state ----------------------------------------------------------------------------------------------------
struct Frame {
  uint32_t snap = nb::kNone, w = 0, h = 0;
  bool swap_rb = false;  // S3-2: RB_COPY_DEST_INFO copy_dest_swap of the front-buffer resolve
  uint32_t swizzle = 0;  // S3-19: front-buffer fetch swizzle from the D3D device word (12 bits, 3 per channel)
  bool swizzle_valid = false;
  std::string info;  // card S3-44 diagnostic: what this swap presents (FLASH_TRACE only)
  uint32_t game_frame = 0;  // RR2: EvHeader::frame of the swap (picture file names shared by live + replay)
};

std::mutex g_mu;  // protects the worker-produced / CP-consumed present queue
std::map<uint64_t, Frame> g_present;  // presentable frames by swap ordinal
std::map<uint64_t, wide_core::FrameDrawStats> g_wide_stats;  // W2 counts by swap ordinal
uint64_t g_swaps = 0;
constexpr size_t kMaxPending = 8;

bool Active() {
  // Mode 1 has no real snapshots (resolves hands out fake ids): nothing to retain, read or pair.
  static const bool on = ObserveEnabled() && ShadowEnabled() && !TranslateOnly();
  return on;
}
void Release(Frame& f) {
  if (f.snap != nb::kNone) nb::ReleaseSnapshot(f.snap);
  f.snap = nb::kNone;
}

}  // namespace

void OnSwap(const EvHeader& e) {
  if (!Active() || e.kind != EvKind::kSwap) return;
  const uint64_t ord = g_swaps++;
  const wide_core::FrameDrawStats wide_stats = TakeWideDrawStats(e.frame);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_wide_stats[ord] = wide_stats;
    while (g_wide_stats.size() > kMaxPending) g_wide_stats.erase(g_wide_stats.begin());
  }
  static auto last_t = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const double dt_ms = std::chrono::duration<double, std::milli>(now - last_t).count();
  last_t = now;

  const resolves::ResolveRecord* r = resolves::Last();
  const char* fb = "missing";
  bool exact_front_buffer = false;
  Frame f;
  f.game_frame = e.frame;
  if (e.args[9]) {  // card S3-19: dev+14012 at Swap (capture.cpp), bits 18..29 = fetch swizzle
    f.swizzle = (e.args[9] >> 18) & 0xFFF;
    f.swizzle_valid = true;
  }
  if (r && r->snapshot != nb::kNone) {
    // desc.src_rect is the hook argument (r5 RECT*): legitimately all-zero (NULL = whole surface) for the front-buffer
    // resolve (m2b-2: size=0x0 on every swap). The image the resolve produced is the union rect in ResolveExtra.
    const resolves::ResolveExtra* ex = resolves::LastExtra();
    int32_t rw = 0, rh = 0;
    if (ex && ex->seq == r->seq) {
      rw = ex->union_rect[2] - ex->union_rect[0];
      rh = ex->union_rect[3] - ex->union_rect[1];
    }
    if (ex && ex->seq == r->seq) f.swap_rb = ((ex->dest_info >> 24) & 1) != 0;
    if (rw > 0 && rh > 0) {
      f.w = uint32_t(rw);
      f.h = uint32_t(rh);
    }
    exact_front_buffer = r->desc.dest_texture == e.args[0];
    if (exact_front_buffer) {
      fb = "exact";
    } else {
      fb = "last_resolve_only";  // r4 of the Swap is not the last resolve's destination: fallback, flagged
    }
  }
  // Only the exact front-buffer resolve becomes a queued D3D-present snapshot; no comparator-owned copies remain.
  if (PresentFromD3d() && exact_front_buffer && f.w && f.h) f.snap = nb::RetainSnapshot(r->snapshot);
  static const bool flash_trace = [] {
    const char* v = std::getenv("WWE2K14_NR_FLASH_TRACE");
    return v && *v == '1';
  }();
  if (flash_trace) {  // card S3-44 diagnostic (remove when settled)
    static uint32_t prev_seq = 0;
    const resolves::ResolveExtra* ex = resolves::LastExtra();
    char b[320];
    std::snprintf(b, sizeof(b),
                  "ord=%llu gframe=%u events=%u fb=%s r4=%08X dt=%.1fms res_seq=%u snap=%u cls=%d dest=%08X rect=%d,%d,%d,%d "
                  "union=%d,%d,%d,%d ms=%u merged=%d epoch=%u clear=%d src=%08X pclear=%d lag=%.1fms",
                  static_cast<unsigned long long>(ord), e.frame, e.seq - prev_seq, fb, e.args[0], dt_ms, r ? r->seq : 0u,
                  r ? r->snapshot : 0u, ex ? int(ex->cls) : -1, ex ? ex->dest_base : 0u, ex ? ex->rect[0] : 0,
                  ex ? ex->rect[1] : 0, ex ? ex->rect[2] : 0, ex ? ex->rect[3] : 0, ex ? ex->union_rect[0] : 0,
                  ex ? ex->union_rect[1] : 0, ex ? ex->union_rect[2] : 0, ex ? ex->union_rect[3] : 0,
                  ex ? ex->samples : 0u, ex ? int(ex->merged) : 0, ex ? ex->tiling_epoch : 0u,
                  r ? int(r->desc.clear_color) : 0, r ? r->desc.src_object : 0u, ex ? int(ex->flushed_clear) : -1,
                  double(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() -
                         GateSwapPublishedNs(e.frame)) / 1e6);
    f.info = b;
    // per-frame texture counter deltas + the frame's resolves
    static textures::Counters last{};
    const textures::Counters& tc = textures::GetCounters();
    char t[200];
    std::snprintf(t, sizeof(t), " | tex: miss=%llu rebuild=%llu refuse=%llu rbind=%llu bypass=%llu stale=%llu seeded=%llu | res:",
                  (unsigned long long)(tc.misses - last.misses), (unsigned long long)(tc.rebuilds - last.rebuilds),
                  (unsigned long long)(tc.refusals - last.refusals), (unsigned long long)(tc.resolve_binds - last.resolve_binds),
                  (unsigned long long)(tc.resolve_overlap_bypassed - last.resolve_overlap_bypassed),
                  (unsigned long long)(tc.resolve_stale_dropped - last.resolve_stale_dropped),
                  (unsigned long long)(tc.resolve_seeded - last.resolve_seeded));
    last = tc;
    f.info += t;
    f.info += resolves::TakeFrameResolves();
    {
      static uint64_t last_tf = 0, last_sf = 0;
      const WorkerStats& ws = WorkerStatsRef();
      const uint64_t tf = ws.tex_fail_draws.load(), sf = ws.submit_fail.load();
      char k[96];
      std::snprintf(k, sizeof(k), " | dropped: tex_fail=%llu submit_fail=%llu skips:", (unsigned long long)(tf - last_tf),
                    (unsigned long long)(sf - last_sf));
      last_tf = tf, last_sf = sf;
      f.info += k;
      f.info += TakeFrameSkips();
    }
    f.info += " | binds:";
    f.info += textures::TakeFrameBinds();
    prev_seq = e.seq;
  }
  if (ord < 8 || ord % 600 == 0)
  REXLOG_INFO(
      "[d3d-present] swap={} frame={} front_buffer={} r4=0x{:08X} r5=0x{:08X} size={}x{} spacing_ms={:.2f} "
      "(30/60 check: swap params r5 not decoded, spacing only)",
      ord, e.frame, fb, e.args[0], e.args[1], f.w, f.h, dt_ms);
  if (f.snap == nb::kNone) return;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto old = g_present.find(ord);
    if (old != g_present.end()) Release(old->second);
    g_present[ord] = f;
    while (g_present.size() > kMaxPending) {  // frames the CP never took: keep the newest few
      Release(g_present.begin()->second);
      g_present.erase(g_present.begin());
    }
  }
}

bool PresentFromD3d() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_NR_PRESENT_D3D");
    return e && *e == '1' && ObserveEnabled() && ShadowEnabled() && !TranslateOnly();
  }();
  return on;
}

wide_core::FrameDrawStats TakeWideSwapStats(uint64_t ord) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_wide_stats.find(ord);
  if (it == g_wide_stats.end()) return {};
  const wide_core::FrameDrawStats stats = it->second;
  g_wide_stats.erase(it);
  return stats;
}

bool NrDrawOff() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_NR_DRAW_OFF");
    if (!PresentFromD3d()) return false;
    if (e && *e == '0') {
      REXLOG_WARN("[d3d-present] WWE2K14_NR_DRAW_OFF=0 is ignored; packet drawing was removed (draw-off is required)");
    }
    return true;
  }();
  return on;
}

std::atomic<bool> g_presented_once{false};
std::mutex g_irq_mu;
std::vector<uint64_t> g_irq_prev, g_irq_cur;  // kick serials of this / the previous guest frame's interrupts (card S3-8)
std::vector<uint64_t> g_irq_prev2;  // card S3-36: the frame before the previous one (WWE2K14_D3D_IRQ_LAG=2)
void OnGpuInterruptFrameEnd() {
  std::lock_guard<std::mutex> lk(g_irq_mu);
  g_irq_prev2.swap(g_irq_prev);  // card S3-36 (prev2 <- prev, prev <- cur)
  g_irq_prev.swap(g_irq_cur);
  g_irq_cur.clear();
}
// Card S3-8 (draw-off, WWE2K14_D3D_IRQ_SYNC=1): the game times its GPU work between two command-stream interrupts
// (stopwatch begin/end in its interrupt handler sub_82CB9048) and lowers its scene quality when that time is long. In
// draw-off the CP reaches those interrupts as soon as the worker has RECORDED the work, so the game saw ~5 ms instead of
// the real GPU time and kept 4x MSAA tiling on the UHD 630. The SDK calls this on the CP thread before it dispatches a
// PM4_INTERRUPT (DrawObserver::OnBeforeInterrupt, no SDK lock held): wait until the GPU has finished everything the D3D
// worker has recorded so far (one source: the backend's submission fences).
void OnBeforeGpuInterrupt() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_IRQ_SYNC");
    return e && *e == '1' && NrDrawOff();
  }();
  // Only once the D3D path has presented a frame (the backend's lists exist and run); boot interrupts pass through.
  if (!on || !g_presented_once.load(std::memory_order_acquire)) return;
  static std::atomic<uint64_t> n{0}, ns{0};
  const auto t0 = std::chrono::steady_clock::now();
  // Submit under the lock, then poll the fences with the lock taken only briefly, so the worker keeps recording the next
  // work while the GPU catches up (the CP thread is the only one held, as on hardware where the CP waits for the GPU).
  // Interrupt i of guest frame N waits for the GPU to pass the point where interrupt i of frame N-1 was raised (its kick
  // serial), not for the GPU to go idle: the game's stopwatch then reads the real GPU time between its markers, one
  // frame late, and the GPU keeps a frame queued. Pairing by index within the frame (frames counted by
  // OnGpuInterruptFrameEnd at each swap) - pairing by "two interrupts back" (launcher 12, s312-20261015-141113) mixed in
  // the game's second stopwatch and made the governor flip its scene mode 67 times in 4 minutes.
  // WWE2K14_D3D_IRQ_LAG=0: wait for everything submitted now (drains the GPU at every marker, S49).
  static const bool drain = [] {
    const char* e = std::getenv("WWE2K14_D3D_IRQ_LAG");
    return e && *e == '0';
  }();
  // Card S3-36 (WWE2K14_D3D_IRQ_LAG=2): pair interrupt i with frame N-2's marker - the GPU may keep two frames queued
  // (fewer GPU idle gaps on the UHD 630); the game's stopwatch still reads one frame's GPU time per marker pair.
  static const bool lag2 = [] {
    const char* e = std::getenv("WWE2K14_D3D_IRQ_LAG");
    return e && *e == '2';
  }();
  uint64_t target = 0;
  SetCpWhere(51);  // card S3-29b
  {
    std::lock_guard<BackendMutex> lk(BackendLock());
    SetCpWhere(52);
    // FL-4 Design A: the interrupt marker is the second release boundary for held late uploads - the bytes are
    // re-read and refilled into the still-unsubmitted list just before KickForSync hands it over.
    late_upload::Release(late_upload::kIrq);
    const uint64_t now = nr::backend::KickForSync();
    std::lock_guard<std::mutex> lk2(g_irq_mu);
    const size_t i = g_irq_cur.size();
    const std::vector<uint64_t>& ref = lag2 && !g_irq_prev2.empty() ? g_irq_prev2 : g_irq_prev;
    if (drain) target = now;
    else if (i < ref.size()) target = ref[i];
    else if (!ref.empty()) target = ref.back();
    g_irq_cur.push_back(now);
  }
  // Card S3-29: bounded (WWE2K14_D3D_IRQ_WAIT_MS, default 0 = unbounded as before). The wait only gives the game's
  // stopwatch the real GPU time; on expiry the interrupt proceeds (counted), no fence or resource state is touched.
  static const uint32_t max_ms = [] {
    const char* e = std::getenv("WWE2K14_D3D_IRQ_WAIT_MS");
    return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0u;
  }();
  static std::atomic<uint64_t> expired{0};
  uint32_t spins = 0;
  SetCpWhere(53);
  while (target) {
    {
      std::lock_guard<BackendMutex> lk(BackendLock());
      if (nr::backend::GpuReachedSerial(target)) break;
    }
    if (max_ms && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(max_ms)) {
      if (expired.fetch_add(1, std::memory_order_relaxed) % 100 == 0) {
        REXLOG_WARN("[d3d-irq] interrupt GPU sync expired after {} ms (target serial {}, expired total {})", max_ms,
                    target, expired.load());
      }
      break;
    }
    // Back off after a short spin so the D3D worker can take BackendLock (std::mutex is not fair); only with the bound on.
    if (!max_ms || ++spins < 64) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  const uint64_t d = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  ns.fetch_add(d, std::memory_order_relaxed);
  if (n.fetch_add(1, std::memory_order_relaxed) % 600 == 599) {
    REXLOG_INFO("[d3d-irq] interrupt GPU sync: n={} mean wait {:.2f} ms", n.load(), double(ns.exchange(0)) / 600.0 / 1e6);
  }
}

uint32_t TakePresentFrame(uint64_t ord, uint32_t* w, uint32_t* h, bool* swap_rb, int32_t* swizzle, std::string* info,
                          uint32_t* game_frame) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_present.find(ord);
  if (it == g_present.end()) return nb::kNone;
  const Frame f = it->second;
  g_present.erase(it);
  if (w) *w = f.w;
  if (h) *h = f.h;
  if (swap_rb) *swap_rb = f.swap_rb;
  if (swizzle) *swizzle = f.swizzle_valid ? int32_t(f.swizzle) : -1;
  if (info) *info = f.info;
  if (game_frame) *game_frame = f.game_frame;
  if (f.snap != nb::kNone) g_presented_once.store(true, std::memory_order_release);
  return f.snap;  // ownership moves to the caller
}

}  // namespace wwe2k14::d3d
