// D5 reset-race probe — diagnostic, default OFF (docs/cards/D5-pump-delay-test.md; H-D5a in
// /tmp/opencode/d5-sublist-luna.md Finding 4).
//
// Hypothesis: a manager reset path (sub_824E7310 or sub_825A3510, both calling the holder reset
// helper sub_82BDAC70 and zeroing descriptor+8) runs on a producer thread WHILE the vdswap
// thread is inside the pump drain sub_82BB9418, so the drain follows an old CALL into a sub-list
// whose opcode-82 terminator is being overwritten.
//
// This file only observes: every gated call to the three reset functions is counted, and when the
// drain flag is active on another thread it is counted and logged as an overlap (first 40). A 30 s
// wall-clock summary line reports the counters. The guest is never modified: every override calls
// __imp__ exactly once with the same ctx/base.
//
// Gate: WWE2K14_DIAG_D5_RESET=1, read once. Unset: each override below is one branch plus the
// original call. No sleeps; relaxed atomics plus one acquire/release pair for the drain flag.
// Remove this file (and its call sites in src/cage_hang_probe.cpp) when the card is settled.

#include "d5_reset_probe.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <functional>
#include <thread>

std::atomic<int> g_d5_drain_active{0};
std::atomic<uint32_t> g_d5_drain_tid{0};
std::atomic<uint32_t> g_d5_drain_count{0};

namespace {

constexpr int64_t kSummaryIntervalNs = 30'000'000'000LL;  // 30 s
constexpr uint32_t kMaxOverlapLogs = 40;

struct FnCounters {
  std::atomic<uint32_t> calls{0};
  std::atomic<uint32_t> overlaps{0};
};

FnCounters g_fn_824E7310;
FnCounters g_fn_825A3510;
FnCounters g_fn_82BDAC70;
std::atomic<uint32_t> g_overlap_logged{0};
std::atomic<uint64_t> g_ser_waits{0}, g_ser_wait_us{0}, g_ser_timeouts{0};  // D5 serialisation (WWE2K14_D5_SERIALIZE)
std::atomic<int64_t> g_last_summary_ns{0};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

namespace d5probe {

bool Serialize() {
  static const bool on = [] {
    const char* value = std::getenv("WWE2K14_D5_SERIALIZE");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  return on;
}

bool Enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("WWE2K14_DIAG_D5_RESET");
    return (value != nullptr && std::strcmp(value, "1") == 0) || Serialize();
  }();
  return enabled;
}

uint32_t HostThreadId() {
#if defined(__linux__)
  return static_cast<uint32_t>(syscall(SYS_gettid));
#else
  return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

void MaybeLogSummary() {
  const int64_t now = NowNs();
  int64_t last = g_last_summary_ns.load(std::memory_order_relaxed);
  if (now - last < kSummaryIntervalNs) return;
  if (!g_last_summary_ns.compare_exchange_strong(last, now, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
    return;
  }
  REXLOG_INFO(
      "[d5-reset] calls 824E7310={} 825A3510={} 82BDAC70={} overlaps={} "
      "(per fn 824E7310={} 825A3510={} 82BDAC70={}) drains={} serialize={} waits={} wait_ms={} timeouts={}",
      g_fn_824E7310.calls.load(std::memory_order_relaxed),
      g_fn_825A3510.calls.load(std::memory_order_relaxed),
      g_fn_82BDAC70.calls.load(std::memory_order_relaxed),
      g_fn_824E7310.overlaps.load(std::memory_order_relaxed) +
          g_fn_825A3510.overlaps.load(std::memory_order_relaxed) +
          g_fn_82BDAC70.overlaps.load(std::memory_order_relaxed),
      g_fn_824E7310.overlaps.load(std::memory_order_relaxed),
      g_fn_825A3510.overlaps.load(std::memory_order_relaxed),
      g_fn_82BDAC70.overlaps.load(std::memory_order_relaxed),
      g_d5_drain_count.load(std::memory_order_relaxed), int(Serialize()), g_ser_waits.load(std::memory_order_relaxed),
      g_ser_wait_us.load(std::memory_order_relaxed) / 1000, g_ser_timeouts.load(std::memory_order_relaxed));
}

}  // namespace d5probe

namespace {

// Counts one call from the gated overrides and, when the drain is active on another thread,
// logs one overlap (first 40). `ctx` is read only.
void CountCall(const char* fn, FnCounters& counters, const PPCContext& ctx) {
  counters.calls.fetch_add(1, std::memory_order_relaxed);
  if (g_d5_drain_active.load(std::memory_order_acquire) != 0) {
    const uint32_t tid = d5probe::HostThreadId();
    const uint32_t drain_tid = g_d5_drain_tid.load(std::memory_order_relaxed);
    if (tid != drain_tid) {
      counters.overlaps.fetch_add(1, std::memory_order_relaxed);
      const uint32_t n = g_overlap_logged.fetch_add(1, std::memory_order_relaxed);
      if (n < kMaxOverlapLogs) {
        REXLOG_WARN("[d5-reset] overlap {} r3={:08X} r4={:08X} lr={:08X} tid={:08X} drain_tid={:08X}",
                    fn, ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr), tid, drain_tid);
      }
      if (d5probe::Serialize()) {
        // Card D5 fix candidate: let the drain that may be executing a CALL into this object's sub-list finish before
        // the list is rewound (the order a fast drain gives). Bounded: never wait more than 100 ms.
        const int64_t t0 = NowNs();
        bool timed_out = false;
        while (g_d5_drain_active.load(std::memory_order_acquire) != 0) {
          if (NowNs() - t0 > 100'000'000LL) {
            timed_out = true;
            break;
          }
          std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        g_ser_waits.fetch_add(1, std::memory_order_relaxed);
        g_ser_wait_us.fetch_add(uint64_t(NowNs() - t0) / 1000, std::memory_order_relaxed);
        if (timed_out) g_ser_timeouts.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  d5probe::MaybeLogSummary();
}

}  // namespace

// Reset path 1: walks the object's slot array, resets holders and rewinds descriptor+8
// (generated/wwe2k14_recomp.21.cpp:21879-21935).
extern "C" PPC_FUNC(__imp__sub_824E7310);
extern "C" PPC_FUNC(sub_824E7310) {
  if (d5probe::Enabled()) {
    CountCall("sub_824E7310", g_fn_824E7310, ctx);
  }
  __imp__sub_824E7310(ctx, base);
}

// Reset path 2: per-record loop with the same reset + descriptor rewind
// (generated/wwe2k14_recomp.27.cpp:35287-35427).
extern "C" PPC_FUNC(__imp__sub_825A3510);
extern "C" PPC_FUNC(sub_825A3510) {
  if (d5probe::Enabled()) {
    CountCall("sub_825A3510", g_fn_825A3510, ctx);
  }
  __imp__sub_825A3510(ctx, base);
}

// Holder reset helper (generated/wwe2k14_recomp.77.cpp:20104). Called only by the two paths above
// today, so its overlap count also covers them.
extern "C" PPC_FUNC(__imp__sub_82BDAC70);
extern "C" PPC_FUNC(sub_82BDAC70) {
  if (d5probe::Enabled()) {
    CountCall("sub_82BDAC70", g_fn_82BDAC70, ctx);
  }
  __imp__sub_82BDAC70(ctx, base);
}
