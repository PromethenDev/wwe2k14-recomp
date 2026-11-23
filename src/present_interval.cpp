// WWE 2K14 flip/frame-time diagnostics (opt-in) and the late-flip fix: the game's present-immediate
// threshold is forced to 100% (WWE2K14_LATE_FLIP=0 keeps the game's value). The present interval is untouched.
// WWE2K14_LOCK_30=1: 30/s lock through the game's own rate conversion; =2 phase-aware 30/60 (card S3-60); =3 skip
// the game's 60->30 halve request and retain its 60 state (card S4-7; end of file).

#include "thread_profiler.h"
#include "timeline.h"
#include "hitch_catcher.h"
#include "game_profiler.h"
#ifdef WWE2K14_HAS_NR
#include "native/nr/nr_pc_sampler.h"
#include "native/nr/nr_backend.h"
#include "native/nr/fix_alt_knobs.h"
#include "native/d3d/runtime.h"
#endif
#include "native/ring_wait_pacing.h"
#include "native/load_lock_core.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>
#include <rex/system/xthread.h>

std::atomic<uint32_t> g_wwe2k14_present_interval{0};
std::atomic<uint64_t> g_wwe2k14_present_interval_us{0};

namespace {

bool FrameTimeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("WWE2K14_FRAME_TIME");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

// G1: the in-game menu can switch Lower CPU use; -1 = no menu choice (the launcher's value below applies).
std::atomic<int> g_low_power_menu{-1};

bool LowPowerWaitEnabled() {
  const int menu = g_low_power_menu.load(std::memory_order_relaxed);
  if (menu >= 0) return menu == 1;
#ifdef WWE2K14_HAS_NR
  static const nr::backend::FixAltKnob knob = nr::backend::FixAltKnobFor("WWE2K14_LOW_POWER");
  return nr::backend::FixAltInt(knob, 0) == 1;
#else
  static const bool enabled = [] {
    const char* value = std::getenv("WWE2K14_LOW_POWER");
    return wwe2k14::ring_wait_pacing::LowPowerWaitEnabled(value);
  }();
  return enabled;
#endif
}

}  // namespace

namespace wwe2k14::live_game {
bool LowPowerOn() { return LowPowerWaitEnabled(); }
void SetLowPower(bool on) { g_low_power_menu.store(on ? 1 : 0, std::memory_order_relaxed); }
}  // namespace wwe2k14::live_game

namespace {

bool V18RingWaitPacingEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("WWE2K14_V18_RING_WAIT_PACE");
    return wwe2k14::ring_wait_pacing::V18RingWaitPacingEnabled(value);
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
  wwe2k14::hitch::Start();  // stall catcher (test builds) + build id line
  wwe2k14::prof::Start();   // TEST-PROFILE.txt continuous per-thread profiler (lane all-1104)
  wwe2k14::hitch::NotePresent();
  wwe2k14::timeline::OnPresent();  // card LOG1: perf / HITCH / PAUSE notes

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

  {  // Owner pacing work 2026-10-17 (WWE2K14_PRESENT_REPEAT=1): while the game's present interval is 2 (30 fps), the
     // host presents every guest frame twice (two refreshes under FIFO, SDK Presenter), so frames keep an even 33 ms
     // cadence on screen instead of a late frame being followed by a one-refresh catch-up frame.
    static const bool repeat = [] {
      const char* v = std::getenv("WWE2K14_PRESENT_REPEAT");
      return v && std::strcmp(v, "1") == 0;
    }();
    if (repeat) {
      const uint32_t want = ((ctx.r3.u32 >> 8) & 0xF) >= 2 ? 2u : 1u;
      static uint32_t last = 0;
      if (want != last) {
        last = want;
        rex::ui::SetGuestPresentRepeat(want);
        static int n = 0;
        if (n++ < 20) REXLOG_INFO("[wwe2k14-flip] host presents per frame -> {}", want);
      }
    }
  }
  {  // card S3-16: last present interval for the autopilot (entrance cadence = 2) - the GAME's value
    g_wwe2k14_present_interval.store((ctx.r3.u32 >> 8) & 0xF, std::memory_order_relaxed);
    g_wwe2k14_present_interval_us.store(SteadyUs(), std::memory_order_relaxed);
  }
  const bool frame_time = FrameTimeEnabled();
#ifdef WWE2K14_HAS_NR
  const bool fix_alt_on = nr::backend::FixAltEnabled();
#else
  const bool fix_alt_on = false;
#endif
  uint64_t flip_now_us = 0;
  uint64_t flip_delta_us = 0;
  if (frame_time || fix_alt_on) {
    flip_now_us = SteadyUs();
    const uint64_t previous_us = g_previous_flip_us.exchange(flip_now_us, std::memory_order_relaxed);
    flip_delta_us = previous_us && flip_now_us >= previous_us ? flip_now_us - previous_us : 0;
  }
#ifdef WWE2K14_HAS_NR
  if (fix_alt_on) {
    wwe2k14::d3d::runtime::NoteFixAltGuestFlip(flip_delta_us, (ctx.r3.u32 >> 8) & 0xF);
  }
#endif
  if (frame_time) {
    const uint32_t packed = ctx.r3.u32;
    const uint32_t threshold = packed & 0xFF;
    const uint32_t interval = (packed >> 8) & 0xF;
    const uint32_t device = g_d3d_device.load(std::memory_order_relaxed);
    const uint64_t sequence = g_frame_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t delta_us = flip_delta_us;

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
                sequence, flip_now_us, delta_us, interval, threshold);
#ifdef WWE2K14_HAS_NR
    nr::sampler::NoteFrame(delta_us);  // stutter attribution in the PC sampler (no-op unless WWE2K14_NR_PC_SAMPLER=1)
#endif
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
  bool low_power_enabled = false;
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

struct LowPowerRingRetry {
  wwe2k14::ring_wait_pacing::RingRetryHookState hook_state;
};

struct GuestFiberKey {
  const PPCContext* context;
  uint32_t fiber_ptr;
  bool operator==(const GuestFiberKey&) const = default;
};

struct GuestFiberKeyHash {
  size_t operator()(const GuestFiberKey& key) const noexcept {
    const size_t context_hash = std::hash<const PPCContext*>{}(key.context);
    const size_t fiber_hash = std::hash<uint32_t>{}(key.fiber_ptr);
    return context_hash ^ (fiber_hash + 0x9E3779B9u + (context_hash << 6) + (context_hash >> 2));
  }
};

thread_local std::unordered_map<GuestFiberKey, LowPowerRingRetry, GuestFiberKeyHash>
    g_low_power_ring_retry_by_fiber;
// Bumped on every insert/erase so a cached entry pointer is trusted only while
// the map is unchanged (nested or re-entrant waits change it).
thread_local uint64_t g_low_power_map_generation = 0;

class LowPowerRingRetryScope {
 public:
  LowPowerRingRetryScope() = default;
  ~LowPowerRingRetryScope() {
    if (active_) {
      g_low_power_ring_retry_by_fiber.erase(key_);
      ++g_low_power_map_generation;
    }
  }

  LowPowerRingRetryScope(const LowPowerRingRetryScope&) = delete;
  LowPowerRingRetryScope& operator=(const LowPowerRingRetryScope&) = delete;

  void Activate(GuestFiberKey key) {
    key_ = key;
    active_ = true;
  }

 private:
  GuestFiberKey key_{};
  bool active_ = false;
};

GuestFiberKey CurrentGuestFiberKey(const PPCContext& ctx, uint8_t* base) {
  uint32_t fiber_ptr = 0;
  const uint32_t pcr = ctx.r13.u32;
  if (pcr != 0) {
    const uint32_t current_thread = PPC_LOAD_U32(
        pcr + offsetof(rex::system::X_KPCR, prcb_data) +
        offsetof(rex::system::X_KPRCB, current_thread));
    if (current_thread != 0) {
      fiber_ptr = PPC_LOAD_U32(current_thread + offsetof(rex::system::X_KTHREAD, fiber_ptr));
    }
  }
  return {&ctx, fiber_ptr};
}

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
    // These are the exact unsigned-subtraction operands used by the ring-loop
    // predicate before its branch back to the helper call (generated source
    // lines 25371-25384; target 0x82CB93A0).
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

// The default-off ring helper observes the mode sampled once at wait entry and
// forwards without a guest-fiber lookup. Ring-retry stats remain independent.
extern "C" PPC_FUNC(__imp__sub_82CB5840);
extern "C" PPC_FUNC(sub_82CB5840) {
  const bool low_power_enabled = g_ring_retry_context.low_power_enabled;
  const bool stats_enabled = RingRetryStatsEnabled();
  if (!low_power_enabled && !stats_enabled) {
    __imp__sub_82CB5840(ctx, base);
    return;
  }
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  if (!low_power_enabled) {
    RecordRingRetry(ctx.r3.u32, base, caller);
    __imp__sub_82CB5840(ctx, base);
    return;
  }

  const GuestFiberKey fiber_key = CurrentGuestFiberKey(ctx, base);
  // One map lookup per helper call: the entry pointer stays valid across the
  // helper unless the map changed (generation check), in which case re-find.
  const auto state_before = g_low_power_ring_retry_by_fiber.find(fiber_key);
  LowPowerRingRetry* entry =
      state_before != g_low_power_ring_retry_by_fiber.end() ? &state_before->second : nullptr;
  const uint64_t generation_before = g_low_power_map_generation;
  // A pending retry is paced here, before entering the helper again. Reaching
  // this exact call site proves the generated ring-space check took its
  // backedge; a final helper miss that exits the loop does not wait.
  if (entry) entry->hook_state.BeforeHelperCall(caller);
  if (stats_enabled) RecordRingRetry(ctx.r3.u32, base, caller);
  __imp__sub_82CB5840(ctx, base);
  // Record a retry result, but do not pause yet: the caller's ring-space test
  // decides whether there will actually be another helper call.
  if (generation_before != g_low_power_map_generation) {
    const auto state_after = g_low_power_ring_retry_by_fiber.find(fiber_key);
    entry = state_after != g_low_power_ring_retry_by_fiber.end() ? &state_after->second : nullptr;
  }
  if (entry) entry->hook_state.AfterHelperReturn(caller, ctx.r3.u32);
}

extern "C" PPC_FUNC(__imp__sub_82CB9310);
extern "C" PPC_FUNC(sub_82CB9310) {
  const bool stats_enabled = RingRetryStatsEnabled();
  const bool low_power_enabled = LowPowerWaitEnabled() || V18RingWaitPacingEnabled();
  const RingRetryContext previous = g_ring_retry_context;
  g_ring_retry_context.low_power_enabled = low_power_enabled;
  if (!wwe2k14::ring_wait_pacing::ShouldWrapRingRetryHook(stats_enabled, low_power_enabled)) {
    __imp__sub_82CB9310(ctx, base);
    g_ring_retry_context = previous;
    return;
  }
  GuestFiberKey fiber_key{};
  LowPowerRingRetryScope low_power_scope;
  if (low_power_enabled) {
    fiber_key = CurrentGuestFiberKey(ctx, base);
    const size_t entries_before = g_low_power_ring_retry_by_fiber.size();
    auto& hook_state = g_low_power_ring_retry_by_fiber[fiber_key].hook_state;
    if (g_low_power_ring_retry_by_fiber.size() != entries_before) ++g_low_power_map_generation;
    // A newly entered wait starts with fresh state even if an abandoned fiber
    // left an entry under a guest address that has since been reused.
    hook_state.BeginInvocation(true);
    low_power_scope.Activate(fiber_key);
  }
  if (stats_enabled) {
    g_ring_retry_context.device = ctx.r3.u32;
    g_ring_retry_context.start_cursor = ctx.r4.u32;
    g_ring_retry_context.active = true;
  }
  __imp__sub_82CB9310(ctx, base);
  g_ring_retry_context = previous;
}

// Owner request 2026-10-17: 30 fps lock as in WWE '13 (wwe13-recomp/src/present_interval.cpp, WWE13_LOCK_30). Forcing
// only the present interval gave slow motion (owner test, launcher 24 v1): the game's time step follows its frame-rate
// global. 2K14 counterparts (DeepSeek + Luna packets R-30, /tmp/opencode/r30-*.md; generated bodies identical in size
// and constants to WWE '13 sub_82AA65B0 / sub_82AA6650): rate global 0x8392DC90 (25/30/50/60), halve 60->30 =
// sub_82BBA580 (wwe2k14_recomp.76.cpp:10922), restore 30->60 = sub_82BBA620 (:11028); the interval chooser
// sub_82BABD38 then selects interval 2 itself. WWE2K14_LOCK_30=1 lets the game's first halve run (time step and
// constants for 30) and suppresses every later restore, so the session stays in the game's own 30 fps state.
// WWE2K14_LOCK_30=2 (card S3-60, opt-in): phase-aware 30/60. Every halve runs (entrances/cutscenes stay on the game's
// own 30 state); a restore request at rate 30/25 is deferred and applied by calling the game's restore at the entry of
// the frame-end sub_82BABD38 (main thread; it then picks interval 1 itself, generated/wwe2k14_recomp.75.cpp:55347)
// once WWE2K14_LOCK_30_HOLD_MS (default 8000) passed without a new halve. A halve >= 1 s after the request cancels it
// (a short 60 blip between entrance segments, 2.2-7 s in lane-A logs, is absorbed); a halve inside 1 s is the game's
// 12-frame follow-up call (a no-op restore in unlocked runs) and is ignored. WWE2K14_LOCK_30=3 (card S4-7) skips the
// original halve only if the game's rate global is 60, leaving the game's rate/time-step fields untouched; restore
// calls still go through the original function.
namespace {
int Lock30Mode() {
  static const int mode = [] {
    const char* value = std::getenv("WWE2K14_LOCK_30");
    if (value == nullptr) return 0;
    if (std::strcmp(value, "1") == 0) return 1;
    if (std::strcmp(value, "2") == 0) return 2;
    if (std::strcmp(value, "3") == 0) return 3;
    if (std::strcmp(value, "4") == 0) return 4;
    return 0;
  }();
  return mode;
}
// Card N4e: LOCK_30=4 = LOCK_30=1 (the game's first halve runs, later restores suppressed) plus: while the renderer
// reports a loading phase (wwe2k14::loadlock, N consecutive frames without a 3D scene) the rate is restored to 60 at
// the frame-end hook, and the game's own halve is re-applied the first frame the scene is back.
bool Lock30Requested() { return Lock30Mode() == 1 || Lock30Mode() == 4; }
std::atomic<bool> g_lock4_forced{false};  // we restored the rate to 60 for a loading phase
std::atomic<bool> g_lock30_active{false};
constexpr uint32_t kRateGlobal = 0x8392DC90;

uint64_t Lock2HoldUs() {
  static const uint64_t us = [] {
    const char* value = std::getenv("WWE2K14_LOCK_30_HOLD_MS");
    const long ms = value ? std::strtol(value, nullptr, 10) : 8000;
    return uint64_t(ms > 0 ? ms : 0) * 1000;
  }();
  return us;
}
constexpr uint64_t kLock2EchoUs = 1000000;
std::atomic<uint64_t> g_lock2_pending_us{0};  // restore request time (0 = none pending)
std::atomic<uint32_t> g_lock2_pending_lr{0};
std::atomic<uint32_t> g_lock2_switches{0};
std::atomic<int> g_lock2_lines{0};
bool Lock2Log() { return g_lock2_lines.fetch_add(1, std::memory_order_relaxed) < 200; }
}  // namespace

extern "C" PPC_FUNC(__imp__sub_82BBA580);
extern "C" PPC_FUNC(sub_82BBA580) {
  const uint32_t before = PPC_LOAD_U32(kRateGlobal);
  const uint32_t caller = uint32_t(ctx.lr);
  if (Lock30Mode() == 3 && before == 60) {
    static std::atomic<int> skipped_halves{0};
    if (skipped_halves.fetch_add(1, std::memory_order_relaxed) < 40) {
      REXLOG_INFO("[wwe2k14-rate] halve 60 -> 30 requested; LOCK_30=3 skipped, rate remains 60 lr={:08X}", caller);
    }
    return;
  }
  __imp__sub_82BBA580(ctx, base);
  const uint32_t after = PPC_LOAD_U32(kRateGlobal);
  if (Lock30Mode() == 2) {
    const uint64_t now = SteadyUs();
    const uint64_t pending = g_lock2_pending_us.load(std::memory_order_relaxed);
    if (before != after) {
      const uint32_t n = g_lock2_switches.fetch_add(1, std::memory_order_relaxed) + 1;
      if (Lock2Log()) REXLOG_INFO("[wwe2k14-rate] lock2 halve {} -> {} lr={:08X} switch={}", before, after, caller, n);
    } else if (pending != 0 && now - pending >= kLock2EchoUs) {
      g_lock2_pending_us.store(0, std::memory_order_relaxed);
      if (Lock2Log())
        REXLOG_INFO("[wwe2k14-rate] lock2 restore cancelled by halve {} ms after the request lr={:08X} (stay {})",
                    (now - pending) / 1000, caller, after);
    } else if (Lock2Log()) {
      REXLOG_INFO("[wwe2k14-rate] lock2 halve at {} lr={:08X} ({})", after, caller,
                  pending ? "follow-up inside 1 s, restore still pending" : "no-op");
    }
    return;
  }
  if (Lock30Requested() && !g_lock30_active.load(std::memory_order_relaxed) && after == 30) {
    g_lock30_active.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[wwe2k14-rate] lock30 active after the game's first halve ({} -> {})", before, after);
  } else if (FrameTimeEnabled() || Lock30Requested()) {
    static int n = 0;
    if (n++ < 40) REXLOG_INFO("[wwe2k14-rate] halve {} -> {}", before, after);
  }
}

extern "C" PPC_FUNC(__imp__sub_82BBA620);
extern "C" PPC_FUNC(sub_82BBA620) {
  const uint32_t before = PPC_LOAD_U32(kRateGlobal);
  const int mode = Lock30Mode();
  if (mode == 2 && (before == 30 || before == 25)) {
    uint64_t expected = 0;
    const uint64_t now = SteadyUs();
    if (g_lock2_pending_us.compare_exchange_strong(expected, now, std::memory_order_relaxed)) {
      g_lock2_pending_lr.store(uint32_t(ctx.lr), std::memory_order_relaxed);
      if (Lock2Log())
        REXLOG_INFO("[wwe2k14-rate] lock2 restore request at rate {} lr={:08X} deferred {} ms", before,
                    uint32_t(ctx.lr), Lock2HoldUs() / 1000);
    } else if (Lock2Log()) {
      REXLOG_INFO("[wwe2k14-rate] lock2 restore request at rate {} lr={:08X} (already pending {} ms)", before,
                  uint32_t(ctx.lr), (now - expected) / 1000);
    }
    return;
  }
  if (Lock30Requested() && g_lock30_active.load(std::memory_order_relaxed)) {
    static int n = 0;
    if (n++ < 40) REXLOG_INFO("[wwe2k14-rate] restore at rate {} suppressed (locked at 30)", before);
    return;
  }
  __imp__sub_82BBA620(ctx, base);
  if (mode == 3) {
    static std::atomic<int> passed_restores{0};
    if (passed_restores.fetch_add(1, std::memory_order_relaxed) < 40) {
      const uint32_t after = PPC_LOAD_U32(kRateGlobal);
      if (before == 60) {
        REXLOG_INFO("[wwe2k14-rate] restore 30 -> 60 requested; LOCK_30=3 passed through, actual rate {} -> {} lr={:08X}",
                    before, after, uint32_t(ctx.lr));
      } else {
        REXLOG_INFO("[wwe2k14-rate] LOCK_30=3 restore passed through, rate {} -> {} lr={:08X}",
                    before, after, uint32_t(ctx.lr));
      }
    }
    return;
  }
  if (FrameTimeEnabled()) {
    static int n = 0;
    if (n++ < 40) REXLOG_INFO("[wwe2k14-rate] restore {} -> {}", before, PPC_LOAD_U32(kRateGlobal));
  }
}

// LOCK_30=2: apply a deferred restore at the frame-end entry. The restore body is a leaf without arguments that only
// writes the rate struct; the volatile registers it clobbers are saved around it so the frame-end sees its own entry
// state (r3 = its argument).
extern "C" PPC_FUNC(__imp__sub_82BABD38);
extern "C" PPC_FUNC(sub_82BABD38) {
  if (Lock30Mode() == 4 && g_lock30_active.load(std::memory_order_relaxed)) {
    const bool loading = wwe2k14::loadlock::g_loading.load(std::memory_order_relaxed);
    const uint32_t before = PPC_LOAD_U32(kRateGlobal);
    const bool do_restore = loading && (before == 30 || before == 25);
    const bool do_halve = !loading && g_lock4_forced.load(std::memory_order_relaxed);
    if (do_restore || do_halve) {
      const PPCRegister r5 = ctx.r5, r6 = ctx.r6, r7 = ctx.r7, r8 = ctx.r8, r9 = ctx.r9, r10 = ctx.r10, r11 = ctx.r11;
      const PPCRegister f0 = ctx.f0, f10 = ctx.f10, f11 = ctx.f11, f12 = ctx.f12, f13 = ctx.f13;
      const auto cr6 = ctx.cr6;
      const auto fpscr = ctx.fpscr;
      if (do_restore) {
        __imp__sub_82BBA620(ctx, base);
        g_lock4_forced.store(true, std::memory_order_relaxed);
      } else {
        __imp__sub_82BBA580(ctx, base);  // no-op unless the rate is 60/50
        g_lock4_forced.store(false, std::memory_order_relaxed);
      }
      ctx.r5 = r5; ctx.r6 = r6; ctx.r7 = r7; ctx.r8 = r8; ctx.r9 = r9; ctx.r10 = r10; ctx.r11 = r11;
      ctx.f0 = f0; ctx.f10 = f10; ctx.f11 = f11; ctx.f12 = f12; ctx.f13 = f13;
      ctx.cr6 = cr6;
      ctx.fpscr = fpscr;
      static std::atomic<int> lines{0};
      if (lines.fetch_add(1, std::memory_order_relaxed) < 200)
        REXLOG_INFO("[wwe2k14-rate] lock4 {} rate {} -> {}", do_restore ? "loading: restore" : "scene back: halve", before,
                    PPC_LOAD_U32(kRateGlobal));
    }
  }
  if (Lock30Mode() == 2) {
    uint64_t pending = g_lock2_pending_us.load(std::memory_order_relaxed);
    if (pending != 0) {
      const uint64_t now = SteadyUs();
      if (now - pending >= Lock2HoldUs() &&
          g_lock2_pending_us.compare_exchange_strong(pending, 0, std::memory_order_relaxed)) {
        const uint32_t before = PPC_LOAD_U32(kRateGlobal);
        const PPCRegister r5 = ctx.r5, r6 = ctx.r6, r7 = ctx.r7, r8 = ctx.r8, r9 = ctx.r9, r10 = ctx.r10, r11 = ctx.r11;
        const PPCRegister f0 = ctx.f0, f10 = ctx.f10, f11 = ctx.f11, f12 = ctx.f12, f13 = ctx.f13;
        const auto cr6 = ctx.cr6;
        const auto fpscr = ctx.fpscr;
        __imp__sub_82BBA620(ctx, base);
        ctx.r5 = r5; ctx.r6 = r6; ctx.r7 = r7; ctx.r8 = r8; ctx.r9 = r9; ctx.r10 = r10; ctx.r11 = r11;
        ctx.f0 = f0; ctx.f10 = f10; ctx.f11 = f11; ctx.f12 = f12; ctx.f13 = f13;
        ctx.cr6 = cr6;
        ctx.fpscr = fpscr;
        const uint32_t after = PPC_LOAD_U32(kRateGlobal);
        const uint32_t n = before != after ? g_lock2_switches.fetch_add(1, std::memory_order_relaxed) + 1
                                           : g_lock2_switches.load(std::memory_order_relaxed);
        if (Lock2Log())
          REXLOG_INFO("[wwe2k14-rate] lock2 restore applied {} -> {} after {} ms (request lr={:08X}) switch={}", before,
                      after, (now - pending) / 1000, g_lock2_pending_lr.load(std::memory_order_relaxed), n);
      }
    }
  }
  __imp__sub_82BABD38(ctx, base);
}
