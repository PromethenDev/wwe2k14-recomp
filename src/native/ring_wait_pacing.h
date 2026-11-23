// Native short-step pacing for the WWE 2K14 ring retry call site.
// The caller owns the poll and its authoritative state source; this helper only
// changes how often the caller runs the next poll.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace wwe2k14::ring_wait_pacing {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kRingHelperReturnAddress = 0x82CB93A8;
constexpr auto kWaitSleepEligibilityAfter = std::chrono::milliseconds(1);
#if defined(_WIN32)
constexpr auto kWindowsTimerAfter = kWaitSleepEligibilityAfter;
// Windows timer steps are requested at 1 ms, and are used only after one wait
// has already crossed the shared eligibility threshold.
constexpr std::chrono::microseconds kWindowsTimerSleepStep = std::chrono::milliseconds(1);
#endif
constexpr std::chrono::microseconds kDefaultSleepStep = std::chrono::microseconds(50);

enum class WaitBackend {
  kHighResolutionTimer,
  kTimerFallback,
  kBlockingSleepFallback,
  kPosixSleep,
};

inline bool LowPowerWaitEnabled(const char* value) {
  return value != nullptr && value[0] == '1' && value[1] == '\0';
}

// M1 ring-retry pacing default. An absent switch enables pacing; exact 1 is opt-in when set.
inline bool V18RingWaitPacingEnabled(const char* value) {
  if (!value) return true;
  return value[0] == '1' && value[1] == '\0';
}

inline bool ShouldWrapRingRetryHook(bool stats_enabled, bool low_power_enabled) {
  return stats_enabled || low_power_enabled;
}

inline void SpinHint() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#else
  std::this_thread::yield();
#endif
}

#if defined(_WIN32)
inline void LogTimerFallback(DWORD high_resolution_error, DWORD normal_timer_error,
                             bool normal_timer_available) {
  static std::once_flag log_once;
  std::call_once(log_once, [=] {
    if (normal_timer_available) {
      std::fprintf(stderr,
                   "[ring-wait] high-resolution waitable timer unavailable (error=%lu); "
                   "using normal waitable timer\n",
                   static_cast<unsigned long>(high_resolution_error));
    } else {
      std::fprintf(stderr,
                   "[ring-wait] high-resolution waitable timer unavailable (error=%lu), "
                   "normal waitable timer unavailable (error=%lu); using blocking Sleep fallback\n",
                   static_cast<unsigned long>(high_resolution_error),
                   static_cast<unsigned long>(normal_timer_error));
    }
  });
}

class HighResolutionWaitableTimer {
 public:
  HighResolutionWaitableTimer()
      : high_resolution_handle_(CreateWaitableTimerExW(nullptr, nullptr,
                                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                        TIMER_MODIFY_STATE | SYNCHRONIZE)) {
    if (!high_resolution_handle_) {
      high_resolution_error_ = GetLastError();
      EnsureNormalTimer();
      LogTimerFallback(high_resolution_error_, normal_timer_error_, normal_handle_ != nullptr);
    }
  }
  ~HighResolutionWaitableTimer() {
    if (high_resolution_handle_) CloseHandle(high_resolution_handle_);
    if (normal_handle_) CloseHandle(normal_handle_);
  }

  HighResolutionWaitableTimer(const HighResolutionWaitableTimer&) = delete;
  HighResolutionWaitableTimer& operator=(const HighResolutionWaitableTimer&) = delete;

  WaitBackend Wait(std::chrono::microseconds duration) {
    if (duration <= std::chrono::microseconds::zero()) return WaitBackend::kBlockingSleepFallback;
    DWORD wait_error = ERROR_SUCCESS;
    if (high_resolution_handle_ && WaitOnHandle(high_resolution_handle_, duration, wait_error)) {
      return WaitBackend::kHighResolutionTimer;
    }
    if (high_resolution_handle_ && high_resolution_error_ == ERROR_SUCCESS) {
      high_resolution_error_ = wait_error;
    }

    EnsureNormalTimer();
    LogTimerFallback(high_resolution_error_, normal_timer_error_, normal_handle_ != nullptr);
    wait_error = ERROR_SUCCESS;
    if (normal_handle_ && WaitOnHandle(normal_handle_, duration, wait_error)) {
      return WaitBackend::kTimerFallback;
    }
    if (normal_handle_ && normal_timer_error_ == ERROR_SUCCESS) {
      normal_timer_error_ = wait_error;
    }

    // Timer creation or use failed. Keep the thread blocked; never turn this
    // error path into the production short-step busy spin.
    const auto milliseconds = (duration.count() + 999) / 1000;
    Sleep(static_cast<DWORD>(milliseconds > 0 ? milliseconds : 1));
    return WaitBackend::kBlockingSleepFallback;
  }

 private:
  static bool WaitOnHandle(HANDLE handle, std::chrono::microseconds duration, DWORD& error) {
    if (!handle) return false;
    LARGE_INTEGER due{};
    const int64_t ticks_100ns = duration.count() * 10;
    due.QuadPart = -static_cast<LONGLONG>(ticks_100ns > 0 ? ticks_100ns : 1);
    if (!SetWaitableTimer(handle, &due, 0, nullptr, nullptr, FALSE)) {
      error = GetLastError();
      return false;
    }
    const DWORD wait_result = WaitForSingleObject(handle, INFINITE);
    if (wait_result == WAIT_OBJECT_0) return true;
    // Capture the wait failure before cancellation can overwrite last-error.
    error = GetLastError();
    CancelWaitableTimer(handle);
    if (wait_result != WAIT_FAILED) error = ERROR_GEN_FAILURE;
    return false;
  }

  // r5 #4 waiver: on Windows older than 10 1803 the high-resolution timer does not
  // exist and the normal timer rounds a 1 ms request to the ~15.6 ms system tick.
  // Pacing then coarsens to that step (still opt-in, still only after 1 ms of waiting).
  void EnsureNormalTimer() {
    // A failed creation is retried at most kNormalTimerRetries times in total
    // (the blocking Sleep fallback covers the failed attempts).
    if (normal_handle_ || normal_create_attempts_ >= kNormalTimerRetries) return;
    ++normal_create_attempts_;
    normal_handle_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!normal_handle_) normal_timer_error_ = GetLastError();
  }

  static constexpr uint32_t kNormalTimerRetries = 3;
  uint32_t normal_create_attempts_ = 0;
  HANDLE high_resolution_handle_ = nullptr;
  HANDLE normal_handle_ = nullptr;
  DWORD high_resolution_error_ = ERROR_SUCCESS;
  DWORD normal_timer_error_ = ERROR_SUCCESS;
};

inline WaitBackend WaitForSleepStep(std::chrono::microseconds duration) {
  if (duration <= std::chrono::microseconds::zero()) return WaitBackend::kBlockingSleepFallback;
  // The timer is created lazily once per host thread. PollBackoff calls this
  // only after the current wait has already spent at least 1 ms polling.
  static thread_local HighResolutionWaitableTimer timer;
  return timer.Wait(duration);
}
#else
inline WaitBackend WaitForSleepStep(std::chrono::microseconds duration) {
  std::this_thread::sleep_for(duration);
  return WaitBackend::kPosixSleep;
}
#endif

class PollBackoff {
 public:
  explicit PollBackoff(std::chrono::microseconds sleep_step = kDefaultSleepStep)
      : sleep_step_(sleep_step) {}

  void Reset() {
    started_ = Clock::now();
    misses_ = 0;
    eligible_ = false;
  }

  // Call only after the unchanged ready/predicate check reports false.
  // Before the wait is 1 ms old this is a counter increment; the clock is read
  // on each of the first kAlwaysCheckFirstMisses misses and then every kClockCheckEveryMisses-th miss (the first sleep may therefore
  // start up to that many misses after the 1 ms mark, never before it).
  // Once eligible, every miss sleeps without reading the clock again.
  void PauseAfterMiss() {
    if (!eligible_) {
      const uint32_t miss = misses_++;
      if (miss >= kAlwaysCheckFirstMisses && (miss & (kClockCheckEveryMisses - 1)) != 0) return;
      if (Clock::now() - started_ < kWaitSleepEligibilityAfter) return;
      eligible_ = true;
    }
#if defined(_WIN32)
    WaitForSleepStep(kWindowsTimerSleepStep);
#else
    WaitForSleepStep(sleep_step_);
#endif
  }

  bool Eligible() const { return eligible_; }

 private:
  static constexpr uint32_t kAlwaysCheckFirstMisses = 4;
  static constexpr uint32_t kClockCheckEveryMisses = 8;  // power of two
  std::chrono::microseconds sleep_step_;
  Clock::time_point started_{};
  uint32_t misses_ = 0;
  bool eligible_ = false;
};

class RingRetryHookState {
 public:
  struct Snapshot {
    bool active;
    bool retry_pending;
    bool backoff_started;
    PollBackoff backoff;
  };

  Snapshot BeginInvocation(bool enabled) {
    // The caller samples its mode once at invocation entry; Active() remains
    // unchanged for all retries until Restore() or the next invocation.
    Snapshot previous{active_, retry_pending_, backoff_started_, backoff_};
    active_ = enabled;
    if (enabled) {
      retry_pending_ = false;
      backoff_started_ = false;
    }
    return previous;
  }

  void Restore(Snapshot previous) {
    active_ = previous.active;
    retry_pending_ = previous.retry_pending;
    backoff_started_ = previous.backoff_started;
    backoff_ = previous.backoff;
  }

  bool Active() const { return active_; }
  bool RetryPending() const { return retry_pending_; }

  // The exact helper is entered again only after the generated ring-space test
  // takes its retry backedge. Defer pacing until that entry so a final miss
  // followed by loop exit never sleeps.
  bool BeforeHelperCall(uint32_t guest_return_address) {
    if (!active_ || guest_return_address != kRingHelperReturnAddress || !retry_pending_) return false;
    if (!backoff_started_) {
      backoff_.Reset();
      backoff_started_ = true;
    }
    retry_pending_ = false;
    backoff_.PauseAfterMiss();
    return true;
  }

  void AfterHelperReturn(uint32_t guest_return_address, uint32_t helper_result) {
    if (!active_ || guest_return_address != kRingHelperReturnAddress || helper_result == 0) return;
    retry_pending_ = true;
  }

 private:
  bool active_ = false;
  bool retry_pending_ = false;
  bool backoff_started_ = false;
  PollBackoff backoff_;
};

}  // namespace wwe2k14::ring_wait_pacing
