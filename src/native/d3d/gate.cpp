// WP9 (decision 0006 stage 2): see gate.h. Written for this project.
// Map: 4-way set-associative table packet_addr -> seq (8 bytes per way, key in the high half), written by the game
// thread at record time, read by the CP thread. A newer record for the same address replaces the older one, a full set
// evicts its oldest seq. A lookup that finds nothing means "unpaired": the CP does not wait.
#include "gate.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "capture.h"
#include "../nr/nr_record_queue.h"  // nr::CpuRelax

namespace wwe2k14::d3d {
namespace {

constexpr uint32_t kSets = 1u << 17, kWays = 4;
constexpr uint32_t kPhysMask = 0x1FFFFFFF;
constexpr uint32_t kSwapSlots = 256;

struct Gate {
  std::atomic<uint64_t> map[kSets * kWays];  // (key << 32) | seq, key = (addr & kPhysMask) | 0x80000000 (never 0)
  std::atomic<uint64_t> swap_map[kSwapSlots];  // (frame << 32 | seq) per frame % kSwapSlots, frame+1 stored (0 = empty)
  std::atomic<uint32_t> done{0};
  std::atomic<uint32_t> waiters{0};
  std::mutex mu;
  std::condition_variable cv;
  uint32_t timeout_ms = 50;
  bool enabled = false;
  // stats (registered on the game thread, the rest on the CP thread; relaxed)
  std::atomic<uint64_t> registered{0}, passed{0}, waited{0}, timeouts{0}, unpaired{0}, wait_ns{0};
  std::atomic<uint64_t> swap_waited{0}, swap_timeouts{0}, swap_unpaired{0}, swap_wait_ns{0}, max_wait_ns{0};
  std::atomic<uint64_t> markers{0};  // card DE-2M: indexed-draw markers the CP reached
};

std::atomic<bool> g_on{false};
std::atomic<Gate*> g_gate{nullptr};

Gate& G() { return *g_gate.load(std::memory_order_acquire); }

}  // namespace

// Called once by capture Init (game thread, inside its call_once) BEFORE the first event. Must not call ShadowEnabled():
// that is the very call_once this runs in (re-entry deadlocked the first live run).
void GateConfigure(bool shadow_on) {
  if (!shadow_on || g_gate.load()) return;
  {
    Gate* p = new Gate;
    for (auto& e : p->map) e.store(0, std::memory_order_relaxed);
    for (auto& e : p->swap_map) e.store(0, std::memory_order_relaxed);
    if (const char* t = std::getenv("WWE2K14_D3D_SHADOW_GATE_TIMEOUT_MS")) {
      const int v = std::atoi(t);
      if (v > 0) p->timeout_ms = uint32_t(v);
    }
    const char* ge = std::getenv("WWE2K14_D3D_SHADOW_GATE");
    p->enabled = !(ge && std::strcmp(ge, "0") == 0);
    g_gate.store(p, std::memory_order_release);
    g_on.store(p->enabled, std::memory_order_release);
  }
}

namespace {

inline uint32_t SetOf(uint32_t key) { return ((key * 2654435761u) >> 12) & (kSets - 1); }
inline bool SeqReached(uint32_t done, uint32_t seq) { return int32_t(done - seq) >= 0; }
inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// Waits until done >= seq. Returns false on timeout. Spin ~50 us, yield, then a condition variable.
bool WaitSeq(Gate& g, uint32_t seq, uint64_t* waited_ns, uint32_t timeout_ms = 0) {
  const uint64_t t0 = NowNs();
  bool ok = false;
  for (int i = 0; i < 2000 && !ok; ++i) {
    if (SeqReached(g.done.load(std::memory_order_acquire), seq)) ok = true;
    else nr::CpuRelax();
  }
  for (int i = 0; i < 100 && !ok; ++i) {
    if (SeqReached(g.done.load(std::memory_order_acquire), seq)) ok = true;
    else std::this_thread::yield();
  }
  if (!ok) {
    g.waiters.fetch_add(1, std::memory_order_seq_cst);
    std::unique_lock<std::mutex> lk(g.mu);
    ok = g.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms ? timeout_ms : g.timeout_ms),
                       [&] { return SeqReached(g.done.load(std::memory_order_acquire), seq); });
    g.waiters.fetch_sub(1, std::memory_order_seq_cst);
  }
  *waited_ns = NowNs() - t0;
  return ok;
}

void NoteWait(Gate& g, uint64_t ns, bool swap) {
  (swap ? g.swap_wait_ns : g.wait_ns).fetch_add(ns, std::memory_order_relaxed);
  uint64_t m = g.max_wait_ns.load(std::memory_order_relaxed);
  while (ns > m && !g.max_wait_ns.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
}

}  // namespace

bool GateEnabled() { return g_on.load(std::memory_order_acquire); }

void NoteDrawKick(uint32_t seq);  // card S3-44 (below)
void GateRegister(uint32_t packet_addr, uint32_t seq) {
  NoteDrawKick(seq);  // card S3-44
  static const bool walk_off = ShadowRingWalkMode() == 0;
  if (walk_off || !g_gate.load(std::memory_order_acquire)) return;
  Gate& g = G();
  const uint32_t key = (packet_addr & kPhysMask) | 0x80000000u;
  std::atomic<uint64_t>* set = &g.map[size_t(SetOf(key)) * kWays];
  int victim = 0;
  uint32_t victim_seq = 0;
  for (uint32_t w = 0; w < kWays; ++w) {
    const uint64_t e = set[w].load(std::memory_order_relaxed);
    if (uint32_t(e >> 32) == key) {
      set[w].store((uint64_t(key) << 32) | seq, std::memory_order_release);
      g.registered.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (e == 0) { victim = int(w); victim_seq = 0; break; }
    if (w == 0 || int32_t(uint32_t(e) - victim_seq) < 0) { victim = int(w); victim_seq = uint32_t(e); }
  }
  set[victim].store((uint64_t(key) << 32) | seq, std::memory_order_release);
  g.registered.fetch_add(1, std::memory_order_relaxed);
}

// Card S3-44 diagnostic (FLASH_TRACE): steady-clock time each game Swap was published, by frame % 256.
std::atomic<int64_t> g_swap_pub_ns[256];
int64_t GateSwapPublishedNs(uint32_t frame) { return g_swap_pub_ns[frame % 256].load(std::memory_order_relaxed); }

void GateRegisterSwap(uint32_t frame, uint32_t seq) {
  g_swap_pub_ns[frame % 256].store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch()).count(),
                                   std::memory_order_relaxed);
  if (!g_gate.load(std::memory_order_acquire)) return;
  Gate& g = G();
  g.swap_map[frame % kSwapSlots].store((uint64_t(frame + 1) << 32) | seq, std::memory_order_release);
}

void GateAdvance(uint32_t done_seq) {
  if (!g_gate.load(std::memory_order_acquire)) return;
  Gate& g = G();
  g.done.store(done_seq, std::memory_order_release);
  if (g.waiters.load(std::memory_order_seq_cst)) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.cv.notify_all();
  }
}

uint32_t GateDoneSeq() { return g_gate.load() ? G().done.load(std::memory_order_acquire) : 0; }

// Card S3-44 (WWE2K14_D3D_LATE_TEX): the seq of the last draw the CP thread has reached (set before it waits).
std::atomic<uint32_t> g_cp_arrived{0};
bool GateWaitCpArrival(uint32_t seq, uint32_t timeout_ms) {
  const uint64_t t0 = NowNs();
  for (uint32_t i = 0;; ++i) {
    if (SeqReached(g_cp_arrived.load(std::memory_order_acquire), seq)) return true;
    if (NowNs() - t0 > uint64_t(timeout_ms) * 1000000ull) return false;
    if (i < 2000) nr::CpuRelax();
    else std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}
// Card S3-44: ring kicks by the guest (CP_RB_WPTR writes, SDK hook) and the kick count at each published draw event.
std::atomic<uint32_t> g_kicks{0};
std::atomic<uint32_t> g_seq_kick[65536];
void OnGuestKick(uint32_t) {
  g_kicks.fetch_add(1, std::memory_order_release);
}
void NoteDrawKick(uint32_t seq) { g_seq_kick[seq & 0xFFFF].store(g_kicks.load(std::memory_order_acquire), std::memory_order_relaxed); }
bool GateWaitKickAfter(uint32_t seq, uint32_t timeout_ms) {
  const uint32_t k0 = g_seq_kick[seq & 0xFFFF].load(std::memory_order_relaxed);
  const uint64_t t0 = NowNs();
  for (uint32_t i = 0;; ++i) {
    if (int32_t(g_kicks.load(std::memory_order_acquire) - k0) > 0) return true;
    if (NowNs() - t0 > uint64_t(timeout_ms) * 1000000ull) return false;
    if (i < 2000) nr::CpuRelax();
    else std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}
void (*g_cp_draw_probe)(uint32_t) = nullptr;  // card S3-44 diagnostic (FLASH_TRACE), set by draw.cpp
void SetCpDrawProbe(void (*fn)(uint32_t)) { g_cp_draw_probe = fn; }
static void GateCpDrawImpl(uint32_t packet_addr);
static void GateCpTarget(Gate& g, uint32_t seq);
void GateCpDraw(uint32_t packet_addr) {
  GateCpDrawImpl(packet_addr);
  if (g_cp_draw_probe) g_cp_draw_probe(packet_addr);  // after the wait: the worker has translated this draw
}
static void GateCpDrawImpl(uint32_t packet_addr) {
  if (!g_on.load(std::memory_order_acquire)) return;
  { thread_local bool noted = false; if (!noted) { noted = true; NoteThreadRole(3); } }
  Gate& g = G();
  static const bool walk_off = ShadowRingWalkMode() == 0;
  if (walk_off) {
    // With the ring-key source retired, preserve the established missing-key behavior: count as unpaired and do not wait.
    g.unpaired.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t key = (packet_addr & kPhysMask) | 0x80000000u;
  const std::atomic<uint64_t>* set = &g.map[size_t(SetOf(key)) * kWays];
  bool found = false;
  uint32_t seq = 0;
  for (uint32_t w = 0; w < kWays; ++w) {
    const uint64_t e = set[w].load(std::memory_order_acquire);
    if (uint32_t(e >> 32) == key) {
      seq = uint32_t(e);
      found = true;
      break;
    }
  }
  if (!found) {
    g.unpaired.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  GateCpTarget(g, seq);
}
// Card DE-2M: the marker of an indexed draw carries its event seq (hle_marker.h); same arrival + wait as a looked-up draw.
void GateCpSeq(uint32_t seq) {
  if (!g_on.load(std::memory_order_acquire)) return;
  { thread_local bool noted = false; if (!noted) { noted = true; NoteThreadRole(3); } }
  Gate& g = G();
  g.markers.fetch_add(1, std::memory_order_relaxed);
  GateCpTarget(g, seq);
}
static void GateCpTarget(Gate& g, uint32_t seq) {
  {  // card S3-44: publish the CP's arrival at this draw before waiting for the worker
    uint32_t prev = g_cp_arrived.load(std::memory_order_relaxed);
    while (!SeqReached(prev, seq) && !g_cp_arrived.compare_exchange_weak(prev, seq, std::memory_order_release)) {}
  }
  if (SeqReached(g.done.load(std::memory_order_acquire), seq)) {
    g.passed.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint64_t ns = 0;
  const bool ok = WaitSeq(g, seq, &ns);
  g.waited.fetch_add(1, std::memory_order_relaxed);
  if (!ok) g.timeouts.fetch_add(1, std::memory_order_relaxed);
  NoteWait(g, ns, false);
}

int GateCpSwapStatus(uint32_t frame) {
  if (!g_on.load(std::memory_order_acquire)) return 3;
  Gate& g = G();
  const uint64_t e = g.swap_map[frame % kSwapSlots].load(std::memory_order_acquire);
  if (uint32_t(e >> 32) != frame + 1) {
    g.swap_unpaired.fetch_add(1, std::memory_order_relaxed);
    return 2;
  }
  const uint32_t seq = uint32_t(e);
  if (SeqReached(g.done.load(std::memory_order_acquire), seq)) return 0;
  uint64_t ns = 0;
  const bool ok = WaitSeq(g, seq, &ns);
  g.swap_waited.fetch_add(1, std::memory_order_relaxed);
  if (!ok) g.swap_timeouts.fetch_add(1, std::memory_order_relaxed);
  NoteWait(g, ns, true);
  return ok ? 0 : 1;
}

void GateCpSwap(uint32_t frame) { (void)GateCpSwapStatus(frame); }

int GateWaitPublished(uint32_t timeout_ms, uint64_t* waited_ns) {
  *waited_ns = 0;
  if (!g_on.load(std::memory_order_acquire)) return 3;
  Gate& g = G();
  const uint32_t seq = LastPublishedSeq();
  if (!seq) return 0;
  if (SeqReached(g.done.load(std::memory_order_acquire), seq)) return 0;
  return WaitSeq(g, seq, waited_ns, timeout_ms) ? 0 : 1;
}

GateStats GateSnapshot() {
  GateStats s;
  if (!g_gate.load(std::memory_order_acquire)) return s;
  Gate& g = G();
  s.registered = g.registered.load(std::memory_order_relaxed);
  s.passed = g.passed.load(std::memory_order_relaxed);
  s.waited = g.waited.load(std::memory_order_relaxed);
  s.timeouts = g.timeouts.load(std::memory_order_relaxed);
  s.unpaired = g.unpaired.load(std::memory_order_relaxed);
  s.markers = g.markers.load(std::memory_order_relaxed);
  s.wait_ns = g.wait_ns.load(std::memory_order_relaxed);
  s.swap_waited = g.swap_waited.load(std::memory_order_relaxed);
  s.swap_timeouts = g.swap_timeouts.load(std::memory_order_relaxed);
  s.swap_unpaired = g.swap_unpaired.load(std::memory_order_relaxed);
  s.swap_wait_ns = g.swap_wait_ns.load(std::memory_order_relaxed);
  s.max_wait_ns = g.max_wait_ns.load(std::memory_order_relaxed);
  return s;
}

std::atomic<uint32_t> g_cp_where{0};  // card S3-29b
void SetCpWhere(uint32_t where) { g_cp_where.store(where, std::memory_order_relaxed); }
uint32_t CpWhere() { return g_cp_where.load(std::memory_order_relaxed); }

}  // namespace wwe2k14::d3d
