#include "native/ring_wait_pacing.h"

#include <rex/ppc/context.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <time.h>
#endif

extern "C" PPC_FUNC(sub_82CB9310);
extern "C" PPC_FUNC(sub_82CB5840);

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kWaits = 1000;
constexpr auto kLongReadyDelay = std::chrono::microseconds(250);
constexpr auto kShortReadyDelay = std::chrono::microseconds(50);
constexpr auto kFiveMillisecondReadyDelay = std::chrono::milliseconds(5);
constexpr auto kFifteenMillisecondReadyDelay = std::chrono::milliseconds(15);
constexpr auto kMeasuredSleep = std::chrono::microseconds(50);
int g_failures = 0;
std::atomic<uint32_t> g_wrapper_helper_calls{0};
std::atomic<int64_t> g_wrapper_third_call_ns{0};
std::atomic<bool> g_two_fiber_wrapper_test{false};
std::atomic<int64_t> g_fiber_b_call_ns{0};
std::atomic<int64_t> g_fiber_a_retry_call_ns{0};

void StoreGuestU32(uint8_t* base, uint32_t address, uint32_t value) {
  base[address] = static_cast<uint8_t>(value >> 24);
  base[address + 1] = static_cast<uint8_t>(value >> 16);
  base[address + 2] = static_cast<uint8_t>(value >> 8);
  base[address + 3] = static_cast<uint8_t>(value);
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

uint64_t ThreadCpuNs() {
#if defined(_WIN32)
  // QueryThreadCycleTime has no 15.625 ms accounting quantum (GetThreadTimes
  // does). Cycles are a CPU-time proxy; callers only compare ratios, and the
  // printed "ms" uses a one-time cycles-per-ms calibration against wall time.
  ULONG64 cycles = 0;
  if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) return cycles;
  FILETIME creation{}, exit{}, kernel_time{}, user_time{};
  if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel_time, &user_time)) return 0;
  ULARGE_INTEGER kernel{};
  ULARGE_INTEGER user{};
  kernel.LowPart = kernel_time.dwLowDateTime;
  kernel.HighPart = kernel_time.dwHighDateTime;
  user.LowPart = user_time.dwLowDateTime;
  user.HighPart = user_time.dwHighDateTime;
  return (kernel.QuadPart + user.QuadPart) * 100;
#else
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

void SpinFor(std::chrono::microseconds duration) {
  const auto deadline = Clock::now() + duration;
  do {
    wwe2k14::ring_wait_pacing::SpinHint();
  } while (Clock::now() < deadline);
}

void Check(bool condition, const char* name) {
  std::printf("check=%s result=%s\n", name, condition ? "PASS" : "FAIL");
  if (!condition) ++g_failures;
}

int64_t Percentile(const std::vector<int64_t>& sorted, size_t percentile) {
  return sorted[(sorted.size() - 1) * percentile / 100];
}

#if defined(_WIN32)
constexpr const char* kCpuUnitLabel = "mcycles";  // thread cycle count / 1e6
#else
constexpr const char* kCpuUnitLabel = "ms";
#endif

struct Result {
  const char* case_label;
  const char* mode;
  int64_t requested_delay_ns;
  uint64_t cpu_ns;
  int64_t producer_p50_ns;
  int64_t producer_p95_ns;
  int64_t producer_max_ns;
  int64_t p50_ns;
  int64_t p95_ns;
  int64_t max_ns;
};

Result Run(const char* case_label, const char* mode,
           std::chrono::microseconds ready_delay, bool low_power) {
  std::mutex request_mutex;
  std::condition_variable request_cv;
  uint32_t requested = 0;
  std::atomic<uint32_t> ready_generation{0};
  std::atomic<int64_t> ready_at_ns{0};
  std::vector<int64_t> producer_delay_ns(kWaits);
  std::thread producer([&] {
    for (uint32_t generation = 1; generation <= kWaits; ++generation) {
      {
        std::unique_lock<std::mutex> lock(request_mutex);
        request_cv.wait(lock, [&] { return requested >= generation; });
      }
      const int64_t producer_start_ns = NowNs();
      SpinFor(ready_delay);
      const int64_t ready_ns = NowNs();
      producer_delay_ns[generation - 1] = ready_ns - producer_start_ns;
      ready_at_ns.store(ready_ns, std::memory_order_relaxed);
      ready_generation.store(generation, std::memory_order_release);
    }
  });

  wwe2k14::ring_wait_pacing::PollBackoff backoff;
  std::vector<int64_t> latency_ns;
  latency_ns.reserve(kWaits);
  const uint64_t cpu_before = ThreadCpuNs();
  for (uint32_t generation = 1; generation <= kWaits; ++generation) {
    if (low_power) backoff.Reset();
    {
      std::lock_guard<std::mutex> lock(request_mutex);
      requested = generation;
    }
    request_cv.notify_one();
    // This models the unchanged guest helper poll. Its OFF path performs no
    // native SpinHint; enabled mode only adds the tested post-miss pacing.
    while (ready_generation.load(std::memory_order_acquire) < generation) {
      if (low_power) backoff.PauseAfterMiss();
    }
    latency_ns.push_back(NowNs() - ready_at_ns.load(std::memory_order_relaxed));
  }
  const uint64_t cpu_after = ThreadCpuNs();
  producer.join();

  std::sort(latency_ns.begin(), latency_ns.end());
  std::sort(producer_delay_ns.begin(), producer_delay_ns.end());
  return {case_label, mode,
          std::chrono::duration_cast<std::chrono::nanoseconds>(ready_delay).count(),
          cpu_after >= cpu_before ? cpu_after - cpu_before : 0,
          Percentile(producer_delay_ns, 50), Percentile(producer_delay_ns, 95),
          producer_delay_ns.back(),
          Percentile(latency_ns, 50), Percentile(latency_ns, 95), latency_ns.back()};
}

struct SleepMeasurement {
  int64_t p50_ns;
  int64_t p95_ns;
  int64_t max_ns;
  uint32_t high_resolution_timer_uses;
  uint32_t timer_fallback_uses;
  uint32_t blocking_sleep_fallback_uses;
  uint32_t posix_sleep_uses;
};

SleepMeasurement MeasureSleepStep(std::chrono::microseconds duration) {
  std::vector<int64_t> durations;
  durations.reserve(200);
  uint32_t high_resolution_timer_uses = 0;
  uint32_t timer_fallback_uses = 0;
  uint32_t blocking_sleep_fallback_uses = 0;
  uint32_t posix_sleep_uses = 0;
  for (int i = 0; i < 200; ++i) {
    const int64_t before = NowNs();
    switch (wwe2k14::ring_wait_pacing::WaitForSleepStep(duration)) {
      case wwe2k14::ring_wait_pacing::WaitBackend::kHighResolutionTimer:
        ++high_resolution_timer_uses;
        break;
      case wwe2k14::ring_wait_pacing::WaitBackend::kTimerFallback:
        ++timer_fallback_uses;
        break;
      case wwe2k14::ring_wait_pacing::WaitBackend::kBlockingSleepFallback:
        ++blocking_sleep_fallback_uses;
        break;
      case wwe2k14::ring_wait_pacing::WaitBackend::kPosixSleep:
        ++posix_sleep_uses;
        break;
    }
    durations.push_back(NowNs() - before);
  }
  std::sort(durations.begin(), durations.end());
  return {Percentile(durations, 50), Percentile(durations, 95), durations.back(),
          high_resolution_timer_uses, timer_fallback_uses, blocking_sleep_fallback_uses,
          posix_sleep_uses};
}

void TestGateAndHookState() {
  using namespace wwe2k14::ring_wait_pacing;
  Check(!LowPowerWaitEnabled(nullptr), "env_missing_defaults_off");
  Check(!LowPowerWaitEnabled("0"), "env_zero_is_off");
  Check(!LowPowerWaitEnabled("true"), "env_other_value_is_off");
  Check(LowPowerWaitEnabled("1"), "env_one_enables");

  Check(!ShouldWrapRingRetryHook(false, false), "hook_off_path_direct_forward");
  Check(ShouldWrapRingRetryHook(true, false), "stats_path_still_wraps");
  Check(ShouldWrapRingRetryHook(false, true), "low_power_path_wraps");

  RingRetryHookState state;
  Check(!state.Active(), "hook_initially_inactive");
  const auto off_snapshot = state.BeginInvocation(false);
  Check(!state.Active(), "hook_disabled_invocation_inactive");
  Check(!state.BeforeHelperCall(kRingHelperReturnAddress), "off_path_does_not_pause");
  state.AfterHelperReturn(kRingHelperReturnAddress, 1);
  Check(!state.RetryPending(), "off_path_does_not_record_retry");
  state.Restore(off_snapshot);
  Check(!state.Active(), "off_path_restores_inactive_state");

  const auto outer_snapshot = state.BeginInvocation(true);
  Check(state.Active(), "enabled_invocation_active");
  Check(!state.BeforeHelperCall(kRingHelperReturnAddress), "first_helper_call_does_not_pause");
  state.AfterHelperReturn(kRingHelperReturnAddress + 4, 1);
  Check(!state.RetryPending(), "other_call_site_does_not_record_retry");
  state.AfterHelperReturn(kRingHelperReturnAddress, 0);
  Check(!state.RetryPending(), "completed_helper_does_not_record_retry");
  state.AfterHelperReturn(kRingHelperReturnAddress, 1);
  Check(state.RetryPending(), "final_iteration_miss_waits_for_backedge");
  // Model a generated ring-space failure that branches back to the helper.
  Check(state.BeforeHelperCall(kRingHelperReturnAddress), "retry_backedge_paces_next_helper");
  state.AfterHelperReturn(kRingHelperReturnAddress, 1);
  Check(state.RetryPending(), "next_retry_result_is_deferred");

  const auto nested_snapshot = state.BeginInvocation(false);
  Check(!state.BeforeHelperCall(kRingHelperReturnAddress), "nested_off_scope_suppresses_pause");
  state.Restore(nested_snapshot);
  Check(state.BeforeHelperCall(kRingHelperReturnAddress), "nested_restore_reinstates_outer_scope");
  state.Restore(outer_snapshot);
  Check(!state.Active(), "outer_scope_restores_inactive_state");
  Check(!state.BeforeHelperCall(kRingHelperReturnAddress), "post_scope_call_does_not_pause");

  // A final nonzero helper result followed by function exit has no subsequent
  // BeforeHelperCall, so pending retry state is discarded without any wait.
  RingRetryHookState final_iteration;
  const auto final_scope = final_iteration.BeginInvocation(true);
  Check(!final_iteration.BeforeHelperCall(kRingHelperReturnAddress),
        "final_iteration_first_call_does_not_pause");
  final_iteration.AfterHelperReturn(kRingHelperReturnAddress, 1);
  Check(final_iteration.RetryPending(), "final_iteration_miss_deferred_until_backedge");
  final_iteration.Restore(final_scope);
  Check(!final_iteration.Active() && !final_iteration.RetryPending(),
        "final_iteration_exit_discards_pending_retry");
}

void TestPresentIntervalWrapperPath() {
#if defined(_WIN32)
  Check(_putenv_s("WWE2K14_LOW_POWER", "1") == 0, "wrapper_test_sets_low_power_env");
  Check(_putenv_s("WWE2K14_RING_RETRY_STATS", "0") == 0,
        "wrapper_test_disables_ring_stats");
#else
  Check(setenv("WWE2K14_LOW_POWER", "1", 1) == 0, "wrapper_test_sets_low_power_env");
  Check(setenv("WWE2K14_RING_RETRY_STATS", "0", 1) == 0,
        "wrapper_test_disables_ring_stats");
#endif
  g_wrapper_helper_calls.store(0, std::memory_order_relaxed);
  g_wrapper_third_call_ns.store(0, std::memory_order_relaxed);

  PPCContext ctx{};
  sub_82CB9310(ctx, nullptr);

  Check(g_wrapper_helper_calls.load(std::memory_order_relaxed) == 3,
        "present_interval_wrapper_calls_helper_three_times");
  const int64_t paced_call_ns = g_wrapper_third_call_ns.load(std::memory_order_relaxed);
#if defined(_WIN32)
  Check(paced_call_ns >= 500000, "present_interval_wrapper_lr_paces_retry");
#else
  Check(paced_call_ns >= 20000, "present_interval_wrapper_lr_paces_retry");
#endif
}

void TestTwoFiberKey() {
  constexpr uint32_t pcr_address = 0x100;
  constexpr uint32_t current_thread_address = 0x300;
  constexpr uint32_t fiber_a = 0x500;
  constexpr uint32_t fiber_b = 0x600;
  std::vector<uint8_t> guest_memory(0x1000);
  const uint32_t current_thread_field =
      pcr_address + static_cast<uint32_t>(offsetof(rex::system::X_KPCR, prcb_data)) +
      static_cast<uint32_t>(offsetof(rex::system::X_KPRCB, current_thread));
  const uint32_t fiber_field =
      current_thread_address + static_cast<uint32_t>(offsetof(rex::system::X_KTHREAD, fiber_ptr));
  StoreGuestU32(guest_memory.data(), current_thread_field, current_thread_address);
  StoreGuestU32(guest_memory.data(), fiber_field, fiber_a);

  PPCContext ctx{};
  ctx.r13.u32 = pcr_address;
  g_wrapper_helper_calls.store(0, std::memory_order_relaxed);
  g_fiber_b_call_ns.store(0, std::memory_order_relaxed);
  g_fiber_a_retry_call_ns.store(0, std::memory_order_relaxed);
  g_two_fiber_wrapper_test.store(true, std::memory_order_relaxed);
  sub_82CB9310(ctx, guest_memory.data());
  g_two_fiber_wrapper_test.store(false, std::memory_order_relaxed);

  const int64_t fiber_b_call_ns = g_fiber_b_call_ns.load(std::memory_order_relaxed);
  const int64_t fiber_a_retry_call_ns = g_fiber_a_retry_call_ns.load(std::memory_order_relaxed);
  std::printf("two_fiber=helper_calls=%u fiber_b_call_ns=%lld fiber_a_retry_call_ns=%lld\n",
              g_wrapper_helper_calls.load(std::memory_order_relaxed),
              static_cast<long long>(fiber_b_call_ns),
              static_cast<long long>(fiber_a_retry_call_ns));
  Check(g_wrapper_helper_calls.load(std::memory_order_relaxed) == 4,
        "two_fiber_wrapper_calls_all_helpers");
  Check(fiber_b_call_ns >= 0 && fiber_b_call_ns < 500000,
        "two_fiber_b_does_not_inherit_a_pending_retry");
#if defined(_WIN32)
  Check(fiber_a_retry_call_ns >= 900000 && fiber_a_retry_call_ns < 5000000,
        "two_fiber_a_retains_its_eligible_retry");
#else
  Check(fiber_a_retry_call_ns >= 35000 && fiber_a_retry_call_ns < 500000,
        "two_fiber_a_retains_its_eligible_retry");
#endif
}

}  // namespace

extern "C" PPC_FUNC(__imp__sub_82CB9310) {
  if (g_two_fiber_wrapper_test.load(std::memory_order_relaxed)) {
    ctx.lr = wwe2k14::ring_wait_pacing::kRingHelperReturnAddress;
    sub_82CB5840(ctx, base);
    sub_82CB5840(ctx, base);

    constexpr uint32_t current_thread_address = 0x300;
    constexpr uint32_t fiber_a = 0x500;
    constexpr uint32_t fiber_b = 0x600;
    const uint32_t fiber_field =
        current_thread_address + static_cast<uint32_t>(offsetof(rex::system::X_KTHREAD, fiber_ptr));
    StoreGuestU32(base, fiber_field, fiber_b);
    const int64_t fiber_b_start_ns = NowNs();
    ctx.lr = wwe2k14::ring_wait_pacing::kRingHelperReturnAddress;
    sub_82CB5840(ctx, base);
    g_fiber_b_call_ns.store(NowNs() - fiber_b_start_ns, std::memory_order_relaxed);

    SpinFor(std::chrono::milliseconds(2));
    StoreGuestU32(base, fiber_field, fiber_a);
    const int64_t fiber_a_start_ns = NowNs();
    ctx.lr = wwe2k14::ring_wait_pacing::kRingHelperReturnAddress;
    sub_82CB5840(ctx, base);
    g_fiber_a_retry_call_ns.store(NowNs() - fiber_a_start_ns, std::memory_order_relaxed);
    return;
  }

  for (uint32_t call = 0; call < 3; ++call) {
    if (call == 2) {
      g_wrapper_third_call_ns.store(NowNs(), std::memory_order_relaxed);
    }
    ctx.lr = wwe2k14::ring_wait_pacing::kRingHelperReturnAddress;
    sub_82CB5840(ctx, base);
    if (call == 1) SpinFor(std::chrono::milliseconds(2));
    if (call == 2) {
      const int64_t start_ns = g_wrapper_third_call_ns.load(std::memory_order_relaxed);
      g_wrapper_third_call_ns.store(NowNs() - start_ns, std::memory_order_relaxed);
    }
  }
}

extern "C" PPC_FUNC(__imp__sub_82CB5840) {
  const uint32_t call = g_wrapper_helper_calls.fetch_add(1, std::memory_order_relaxed);
  ctx.r3.u32 = call < 2 ? 1 : 0;
}

// Unused present-interval entry points are retained by the Windows linker in
// this standalone executable; satisfy their imports without linking the game.
extern "C" PPC_FUNC(__imp__sub_82CB6318) {}
extern "C" PPC_FUNC(__imp__sub_82CB9048) {}
extern "C" PPC_FUNC(__imp__sub_82BBA580) {}
extern "C" PPC_FUNC(__imp__sub_82BBA620) {}
extern "C" PPC_FUNC(__imp__sub_82BABD38) {}
namespace wwe2k14 {
void StartThreadProfilerFromEnv() {}
}  // namespace wwe2k14
namespace rex::ui {
void SetGuestPresentRepeat(uint32_t) {}
}  // namespace rex::ui

// The standalone wrapper test does not link the runtime logger; ring stats
// remain disabled, so this stub only satisfies the compiled diagnostic path.
namespace rex {
spdlog::logger* GetLoggerRaw(LogCategoryId) { return nullptr; }
}  // namespace rex

int main() {
  TestGateAndHookState();
  TestPresentIntervalWrapperPath();
  TestTwoFiberKey();

#if defined(_WIN32)
  const SleepMeasurement timer =
      MeasureSleepStep(wwe2k14::ring_wait_pacing::kWindowsTimerSleepStep);
  const double eligible_after_ms = std::chrono::duration<double, std::milli>(
                                       wwe2k14::ring_wait_pacing::kWindowsTimerAfter)
                                       .count();
  std::printf("timer_step=requested_us=%lld eligible_after_ms=%.3f "
              "high_resolution_timer=%u/200 normal_timer=%u/200 blocking_sleep=%u/200 "
              "actual_ns_p50=%lld p95=%lld max=%lld\n",
              static_cast<long long>(wwe2k14::ring_wait_pacing::kWindowsTimerSleepStep.count()),
              eligible_after_ms, timer.high_resolution_timer_uses, timer.timer_fallback_uses,
              timer.blocking_sleep_fallback_uses, static_cast<long long>(timer.p50_ns),
              static_cast<long long>(timer.p95_ns), static_cast<long long>(timer.max_ns));
  Check(timer.high_resolution_timer_uses + timer.timer_fallback_uses == 200,
        "windows_timer_or_normal_timer_backend_used");
  Check(timer.blocking_sleep_fallback_uses == 0,
        "windows_timer_path_does_not_use_sleep_fallback");
  Check(timer.p50_ns >= 900000, "windows_timer_p50_not_early");
  Check(timer.p95_ns < 5000000, "windows_timer_p95_below_5ms");
#else
  const SleepMeasurement sleep = MeasureSleepStep(kMeasuredSleep);
  std::printf("sleep_step=requested_us=%lld backend=POSIX-sleep_for=%u/200 actual_ns_p50=%lld p95=%lld max=%lld\n",
              static_cast<long long>(kMeasuredSleep.count()), sleep.posix_sleep_uses,
              static_cast<long long>(sleep.p50_ns),
              static_cast<long long>(sleep.p95_ns), static_cast<long long>(sleep.max_ns));
  Check(sleep.p50_ns >= 35000, "sleep_step_median_not_early");
  Check(sleep.p50_ns < 500000, "sleep_step_median_below_500us");
  Check(sleep.p95_ns < 5000000, "sleep_step_p95_below_5ms");
  Check(sleep.posix_sleep_uses == 200, "posix_sleep_backend_used");
#endif

  const Result short_control = Run("short_wait_50us", "off-direct-poll", kShortReadyDelay, false);
  const Result short_enabled = Run("short_wait_50us", "enabled-poll-backoff", kShortReadyDelay, true);
  const Result quarter_control = Run("wait_250us", "off-direct-poll", kLongReadyDelay, false);
  const Result quarter_enabled = Run("wait_250us", "enabled-poll-backoff", kLongReadyDelay, true);
  const Result five_control =
      Run("wait_5ms", "off-direct-poll", kFiveMillisecondReadyDelay, false);
  const Result five_enabled =
      Run("wait_5ms", "enabled-poll-backoff", kFiveMillisecondReadyDelay, true);
  const Result fifteen_control =
      Run("wait_15ms", "off-direct-poll", kFifteenMillisecondReadyDelay, false);
  const Result fifteen_enabled =
      Run("wait_15ms", "enabled-poll-backoff", kFifteenMillisecondReadyDelay, true);
  for (const Result result : {short_control, short_enabled, quarter_control, quarter_enabled,
                              five_control, five_enabled, fifteen_control, fifteen_enabled}) {
    std::printf("case=%s mode=%s waits=%u requested_producer_delay_us=%lld "
                "actual_producer_delay_ns_p50=%lld p95=%lld max=%lld consumer_cpu_%s=%.3f "
                "wakeup_latency_ns_p50=%lld p95=%lld max=%lld\n",
                result.case_label, result.mode, kWaits,
                static_cast<long long>(result.requested_delay_ns / 1000),
                static_cast<long long>(result.producer_p50_ns),
                static_cast<long long>(result.producer_p95_ns),
                static_cast<long long>(result.producer_max_ns), kCpuUnitLabel, double(result.cpu_ns) / 1.0e6,
                static_cast<long long>(result.p50_ns), static_cast<long long>(result.p95_ns),
                static_cast<long long>(result.max_ns));
  }
  Check(short_control.cpu_ns > 0 && short_enabled.cpu_ns > 0 && quarter_control.cpu_ns > 0 &&
            quarter_enabled.cpu_ns > 0 && five_control.cpu_ns > 0 && five_enabled.cpu_ns > 0 &&
            fifteen_control.cpu_ns > 0 && fifteen_enabled.cpu_ns > 0,
        "thread_cpu_measurements_available");
  auto ProducerDelaysMatchRequest = [](const Result& result) {
    return result.producer_p50_ns >= result.requested_delay_ns &&
           result.producer_p95_ns < result.requested_delay_ns + 5000000;
  };
  Check(ProducerDelaysMatchRequest(short_control) && ProducerDelaysMatchRequest(short_enabled) &&
            ProducerDelaysMatchRequest(quarter_control) &&
            ProducerDelaysMatchRequest(quarter_enabled) &&
            ProducerDelaysMatchRequest(five_control) && ProducerDelaysMatchRequest(five_enabled) &&
            ProducerDelaysMatchRequest(fifteen_control) &&
            ProducerDelaysMatchRequest(fifteen_enabled),
        "producer_delays_measured_near_requested_values");
  Check(short_enabled.cpu_ns * 10 <= short_control.cpu_ns * 11,
        "wait_50us_enabled_cpu_at_most_110pct_off");
  Check(quarter_enabled.cpu_ns * 10 <= quarter_control.cpu_ns * 11,
        "wait_250us_enabled_cpu_at_most_110pct_off");
  Check(five_enabled.cpu_ns * 2 < five_control.cpu_ns,
        "wait_5ms_enabled_cpu_below_50pct_off");
  Check(fifteen_enabled.cpu_ns * 2 < fifteen_control.cpu_ns,
        "wait_15ms_enabled_cpu_below_50pct_off");
  Check(short_enabled.p95_ns < 5000000 && quarter_enabled.p95_ns < 5000000 &&
            five_enabled.p95_ns < 5000000 && fifteen_enabled.p95_ns < 5000000,
        "enabled_wakeup_p95_below_5ms");

  std::printf("host_test=%s failures=%d\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
  return g_failures == 0 ? 0 : 1;
}
