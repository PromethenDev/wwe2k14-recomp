// w13r_loader_window.cpp - SLOWLOAD fix (docs/cards/SLOWLOAD-match-loading.md, owner 2026-11-23 "yes start the fix").
//
// The game's loader threads (file-read-end, object registration) work under the render device's critical section
// (device +176) and yield it through sub_82BAB758 (generated/wwe2k14_recomp.75.cpp:54484). The render/VdSwap thread
// holds that lock for most of each frame and frees it only between the unlock in its loop
// (sub_82BAB190 called from 0x82BB9778, generated/wwe2k14_recomp.76.cpp:8719) and the re-lock after the LoopEnd wait
// (:8733-8738). On the old renderer the main thread usually finishes after that unlock, so the render thread waits
// for LoopEnd with the lock free and loaders get a slice. Under w13r the main thread has finished by then, the
// render thread re-locks at once, and each loader yield hands the lock straight back: loads take 2-3x longer
// (UHD 630: 6-8 s -> 17-21 s; old renderer + 1 ms under the lock reproduces it, 36 s, run sl7).
//
// Fix: at that one unlock, if another thread now holds or waits for the lock, the render thread waits up to
// W microseconds (default 2000) or until the lock is free before continuing, giving loaders the window the old
// renderer gave them. Nothing changes when no loader contends (menus, matches without streaming).
// WWE2K14_W13R_LOADER_WINDOW_US=<us> sets W; 0 turns it off.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

extern "C" PPC_FUNC(__imp__sub_82BAB190);

namespace {
constexpr uint32_t kRenderLoopUnlockReturn = 0x82BB9778;  // lr of the render loop's unlock call (76.cpp:8716-8719)
constexpr uint32_t kDeviceLockOffset = 176;                // device +176 = X_RTL_CRITICAL_SECTION (75.cpp:53606)
constexpr uint32_t kLockCountOffset = 0x10;                // X_RTL_CRITICAL_SECTION::lock_count, host order (-1 = free)

uint32_t WindowUs() {
  static const uint32_t us = [] {
    const char* v = std::getenv("WWE2K14_W13R_LOADER_WINDOW_US");
    const uint32_t w = v && *v ? uint32_t(std::strtoul(v, nullptr, 10)) : 2000u;
    REXLOG_INFO("[loader-window] {} us at the render loop unlock (WWE2K14_W13R_LOADER_WINDOW_US; 0 = off)", w);
    return w;
  }();
  return us;
}

struct Stats {
  uint64_t windows = 0, early = 0, wait_us = 0;
  std::chrono::steady_clock::time_point last_log{};
};

int32_t LockCount(uint8_t* base, uint32_t cs) {
  // The SDK keeps lock_count in host byte order and updates it with host atomics (xboxkrnl_rtl.cpp:341, 386-494).
  return __atomic_load_n(reinterpret_cast<int32_t*>(base + cs + kLockCountOffset), __ATOMIC_ACQUIRE);
}
}  // namespace

extern "C" PPC_FUNC(sub_82BAB190) {
  const uint32_t return_lr = ctx.lr;
  const uint32_t cs = ctx.r3.u32 + kDeviceLockOffset;
  __imp__sub_82BAB190(ctx, base);
  if (return_lr != kRenderLoopUnlockReturn) return;
  const uint32_t window_us = WindowUs();
  if (window_us == 0 || LockCount(base, cs) == -1) return;  // nobody holds or waits for it: nothing to give

  using Clock = std::chrono::steady_clock;
  static Stats stats;  // only the render thread reaches this call site
  const Clock::time_point start = Clock::now();
  const Clock::time_point end = start + std::chrono::microseconds(window_us);
  bool freed = false;
  for (;;) {
    if (LockCount(base, cs) == -1) {
      freed = true;
      break;
    }
    const Clock::time_point now = Clock::now();
    if (now >= end) break;
    if (end - now > std::chrono::microseconds(300)) {
      std::this_thread::yield();
    } else {
#if defined(_M_X64) || defined(__x86_64__)
      _mm_pause();
#endif
    }
  }
  const Clock::time_point stop = Clock::now();
  ++stats.windows;
  stats.early += freed;
  stats.wait_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(stop - start).count());
  if (stop - stats.last_log >= std::chrono::seconds(10)) {
    REXLOG_INFO("[loader-window] windows={} ended_free={} wait_ms={:.1f} (since last line)", stats.windows,
                stats.early, double(stats.wait_us) / 1000.0);
    stats.windows = stats.early = stats.wait_us = 0;
    stats.last_log = stop;
  }
}
