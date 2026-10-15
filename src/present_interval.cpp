// WWE 2K14 flip/frame-time diagnostics (opt-in) and the late-flip fix: the game's present-immediate
// threshold is forced to 100% (WWE2K14_LATE_FLIP=0 keeps the game's value). The present interval is untouched.

#include "thread_profiler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {

bool FrameTimeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("WWE2K14_FRAME_TIME");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

uint64_t SteadyUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

std::atomic<uint32_t> g_d3d_device{0};
std::atomic<uint64_t> g_frame_sequence{0};
std::atomic<uint64_t> g_previous_flip_us{0};
std::once_flag g_profiler_start_once;

bool LateFlipImmediate() {
  static const bool enabled = [] {
    const char* value = std::getenv("WWE2K14_LATE_FLIP");
    return !(value != nullptr && std::strcmp(value, "0") == 0);
  }();
  return enabled;
}

void StartProfilerIfRequested() {
  std::call_once(g_profiler_start_once, [] { wwe2k14::StartThreadProfilerFromEnv(); });
}

}  // namespace

// High-confidence WWE 13 -> 2K14 instruction-alignment counterpart of
// sub_82B9A548. 2K14 generated disassembly confirms the packed r3 fields and
// device fields at +16952/+16956/+16960 (generated/wwe2k14_recomp.85.cpp:
// 18031-18063,18088-18095,18148-18151).
extern "C" PPC_FUNC(__imp__sub_82CB6318);
//
// Late-flip port of WWE 13 commit 9c40fa2. 2K14 evidence (generated/wwe2k14_recomp.85.cpp, 0x82CB6318):
//  :18009 `mr r30,r3` (packed arg), :18043 `rlwinm r29,r30,24,28,31` = interval, :18047 `clrlwi r27,r30,24` =
//  threshold (low byte of r3). :18070-18089 r7 = percent of the vblank period elapsed since the last flip
//  (timebase delta * 100 / (freq/rate), +1, capped at 100). :18099-18116: r11 = prev flip vblank [dev+16960] +
//  interval; if r11 <= current vblank [dev+16952] the frame is late -> flip now (r11 = vblank), BUT
//  `cmplw r7,r27; ble` skips the bump only when r7 <= threshold, otherwise `addi r11,r11,1` waits one more vblank.
//  r7 is capped at 100, so threshold 100 never takes the bump: every late frame flips immediately.
extern "C" PPC_FUNC(sub_82CB6318) {
  StartProfilerIfRequested();

  {
    static uint32_t last_threshold = UINT32_MAX;
    const uint32_t game_threshold = ctx.r3.u32 & 0xFF;
    if (game_threshold != last_threshold) {
      last_threshold = game_threshold;
      REXLOG_INFO("[wwe2k14-flip] game present-immediate threshold {}% interval {} ({})", game_threshold,
                  (ctx.r3.u32 >> 8) & 0xF, LateFlipImmediate() ? "late flips immediate, threshold forced to 100%" : "kept");
    }
    if (LateFlipImmediate()) {
      ctx.r3.u32 = (ctx.r3.u32 & ~0xFFu) | 100;
    }
  }

  if (FrameTimeEnabled()) {
    const uint32_t packed = ctx.r3.u32;
    const uint32_t threshold = packed & 0xFF;
    const uint32_t interval = (packed >> 8) & 0xF;
    const uint32_t device = g_d3d_device.load(std::memory_order_relaxed);
    const uint64_t now_us = SteadyUs();
    const uint64_t previous_us = g_previous_flip_us.exchange(now_us, std::memory_order_relaxed);
    const uint64_t sequence = g_frame_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t delta_us = previous_us && now_us >= previous_us ? now_us - previous_us : 0;

    uint32_t vblank_count = 0;
    uint32_t previous_flip_vblank = 0;
    uint32_t due_vblank = 0;
    bool valid = false;
    bool late = false;
    bool missed = false;
    if (device != 0 && interval != 0) {
      vblank_count = PPC_LOAD_U32(device + 16952);
      previous_flip_vblank = PPC_LOAD_U32(device + 16960);
      due_vblank = previous_flip_vblank + interval;
      // The first callback may observe an uninitialized previous-flip field;
      // do not turn that startup sentinel into a false missed-frame count.
      valid = previous_flip_vblank != 0;
      if (valid) {
        const int32_t vblank_delta = static_cast<int32_t>(vblank_count - due_vblank);
        late = vblank_delta >= 0;
        missed = vblank_delta > 0;
      }
    }

    static uint32_t last_threshold = UINT32_MAX;
    if (threshold != last_threshold) {
      last_threshold = threshold;
      REXLOG_INFO("[wwe13-flip] 2K14 game present-immediate threshold {}% interval {} (observed after override: {})",
                  threshold, interval, LateFlipImmediate() ? "late flip 100" : "off");
    }
    REXLOG_INFO("[wwe13-frame-time] seq={} t_us={} dt_us={} interval={} threshold={} present",
                sequence, now_us, delta_us, interval, threshold);
    REXLOG_INFO("[wwe13-flip] seq={} valid={} vblank={} due={} late={} missed={}", sequence,
                valid ? 1 : 0, vblank_count, due_vblank, valid ? int(late) : -1,
                valid ? int(missed) : -1);
  }

  // Instrument only: preserve every guest argument and call the original body.
  __imp__sub_82CB6318(ctx, base);
}

// High-confidence counterpart of WWE 13 sub_82B9D278(source, device).
// The 2K14 disassembly at 0x82CB9048 uses r4 as the device pointer.
extern "C" PPC_FUNC(__imp__sub_82CB9048);
extern "C" PPC_FUNC(sub_82CB9048) {
  g_d3d_device.store(ctx.r4.u32, std::memory_order_relaxed);
  __imp__sub_82CB9048(ctx, base);
}

namespace {

bool RingRetryStatsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("WWE2K14_RING_RETRY_STATS");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

struct RingRetryContext {
  uint32_t device = 0;
  uint32_t start_cursor = 0;
  bool active = false;
};

struct RingRetryStats {
  std::chrono::steady_clock::time_point start{};
  uint64_t ring_retries = 0;
  uint64_t range_retries = 0;
  uint64_t other_retries = 0;
  uint64_t ring_need_sum_dw = 0;
  uint64_t ring_ptr_distance_sum_dw = 0;
  uint32_t ring_need_min_dw = 0xFFFFFFFFu;
  uint32_t ring_need_max_dw = 0;
  uint32_t ring_ptr_distance_min_dw = 0xFFFFFFFFu;
  uint32_t ring_ptr_distance_max_dw = 0;
  uint64_t range_request_sum = 0;
  uint32_t range_request_min = 0xFFFFFFFFu;
  uint32_t range_request_max = 0;
};

thread_local RingRetryContext g_ring_retry_context;
thread_local RingRetryStats g_ring_retry_stats;

void RecordRingRetry(uint32_t state, uint8_t* base, uint32_t caller) {
  const uint32_t device = PPC_LOAD_U32(state);
  if (!device) {
    return;
  }
  const uint32_t ring_pointer = PPC_LOAD_U32(device + 11024);
  if (!ring_pointer) {
    return;
  }

  const uint32_t write_cursor = PPC_LOAD_U32(device + 11036);
  const uint32_t guest_cursor = PPC_LOAD_U32(ring_pointer);
  const uint32_t pointer_distance_dw = write_cursor - guest_cursor;
  const uint32_t range_request_arg = PPC_LOAD_U32(state + 4);
  RingRetryStats& stats = g_ring_retry_stats;
  if (stats.start == std::chrono::steady_clock::time_point{}) {
    stats.start = std::chrono::steady_clock::now();
  }

  if (caller == 0x82CB93A8 && g_ring_retry_context.active &&
      g_ring_retry_context.device == device) {
    // These are the exact unsigned-subtraction operands compared in the
    // sub_82CB9310 backedge at 0x82CB93B0 (generated source lines 25371-25384).
    const uint32_t need_dw = write_cursor - g_ring_retry_context.start_cursor;
    ++stats.ring_retries;
    stats.ring_need_sum_dw += need_dw;
    stats.ring_ptr_distance_sum_dw += pointer_distance_dw;
    if (need_dw < stats.ring_need_min_dw) {
      stats.ring_need_min_dw = need_dw;
    }
    if (need_dw > stats.ring_need_max_dw) {
      stats.ring_need_max_dw = need_dw;
    }
    if (pointer_distance_dw < stats.ring_ptr_distance_min_dw) {
      stats.ring_ptr_distance_min_dw = pointer_distance_dw;
    }
    if (pointer_distance_dw > stats.ring_ptr_distance_max_dw) {
      stats.ring_ptr_distance_max_dw = pointer_distance_dw;
    }
  } else if (caller == 0x82CB8D60) {
    // The range-wait loop enters this helper at 0x82CB8D60. Retain its raw
    // helper argument rather than assuming it is a ring-space dword count.
    ++stats.range_retries;
    stats.range_request_sum += range_request_arg;
    if (range_request_arg < stats.range_request_min) {
      stats.range_request_min = range_request_arg;
    }
    if (range_request_arg > stats.range_request_max) {
      stats.range_request_max = range_request_arg;
    }
  } else {
    ++stats.other_retries;
  }

  const auto now = std::chrono::steady_clock::now();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - stats.start)
                              .count();
  if (elapsed_ms < 1000) {
    return;
  }
  const double scale = 1000.0 / static_cast<double>(elapsed_ms);
  const uint64_t counted = stats.ring_retries + stats.range_retries + stats.other_retries;
  REXLOG_INFO(
      "[ring-retry-census] elapsed_ms={} retries_per_s={:.2f} ring_retries_per_s={:.2f} "
      "range_retries_per_s={:.2f} other_retries_per_s={:.2f} ring_need_dw_avg={:.2f} "
      "ring_need_dw_min={} ring_need_dw_max={} ring_ptr_distance_dw_avg={:.2f} "
      "ring_ptr_distance_dw_min={} ring_ptr_distance_dw_max={} range_request_arg_avg={:.2f} "
      "range_request_arg_min={} range_request_arg_max={}",
      elapsed_ms, counted * scale, stats.ring_retries * scale, stats.range_retries * scale,
      stats.other_retries * scale,
      stats.ring_retries ? static_cast<double>(stats.ring_need_sum_dw) / stats.ring_retries : 0.0,
      stats.ring_retries ? stats.ring_need_min_dw : 0,
      stats.ring_need_max_dw,
      stats.ring_retries ? static_cast<double>(stats.ring_ptr_distance_sum_dw) / stats.ring_retries
                         : 0.0,
      stats.ring_retries ? stats.ring_ptr_distance_min_dw : 0,
      stats.ring_ptr_distance_max_dw,
      stats.range_retries ? static_cast<double>(stats.range_request_sum) / stats.range_retries : 0.0,
      stats.range_retries ? stats.range_request_min : 0,
      stats.range_request_max);
  stats = {};
  stats.start = now;
}

}  // namespace

// These strong PPC_FUNC overrides use the same function-hook pattern as the
// frame-time wrappers above. With WWE2K14_RING_RETRY_STATS unset, they only
// take the cached disabled branch and forward unchanged guest state.
extern "C" PPC_FUNC(__imp__sub_82CB5840);
extern "C" PPC_FUNC(sub_82CB5840) {
  if (RingRetryStatsEnabled()) {
    RecordRingRetry(ctx.r3.u32, base, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82CB5840(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82CB9310);
extern "C" PPC_FUNC(sub_82CB9310) {
  if (!RingRetryStatsEnabled()) {
    __imp__sub_82CB9310(ctx, base);
    return;
  }
  const RingRetryContext previous = g_ring_retry_context;
  g_ring_retry_context.device = ctx.r3.u32;
  g_ring_retry_context.start_cursor = ctx.r4.u32;
  g_ring_retry_context.active = true;
  __imp__sub_82CB9310(ctx, base);
  g_ring_retry_context = previous;
}
