// Host test of src/timer_queue_sleep.cpp (GAP-combo1): recurring tick rate and CPU in the three modes, one-shot, disarm, self-disarm.
// Run once per mode: WWE2K14_TIMER_SLEEP unset, 1 or 2. Prints PASS or FAIL lines; exit 0 on PASS.
#include <rex/thread/timer_queue.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <time.h>

using namespace rex::thread;
using Clock = TimerQueueWaitItem::clock;
static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
static double ProcessCpuS() {
  timespec t{};
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
  return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}
int main() {
  const char* mode = std::getenv("WWE2K14_TIMER_SLEEP");
  std::atomic<uint32_t> ticks{0};
  auto rec = QueueTimerRecurring([&](void*) { ticks.fetch_add(1, std::memory_order_relaxed); }, nullptr, Clock::now(), std::chrono::milliseconds(1));
  const double c0 = ProcessCpuS();
  std::this_thread::sleep_for(std::chrono::seconds(3));
  const double cpu_pct = 100.0 * (ProcessCpuS() - c0) / 3.0;
  const uint32_t n = ticks.load();
  std::printf("mode=%s ticks/s=%.0f process cpu=%.2f %% of a core\n", mode ? mode : "unset", n / 3.0, cpu_pct);
  Check(n > 2700 && n < 3100, "recurring 1 ms tick rate");
  if (mode && !std::strcmp(mode, "1")) Check(cpu_pct < 5.0, "sleeping thread under 5 % of a core");
  else Check(cpu_pct > 5.0, "spinning thread as the SDK (over 5 % of a core)");
  if (auto p = rec.lock()) p->Disarm();
  const uint32_t after = ticks.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Check(ticks.load() == after, "disarm stops the recurring timer, no callback after it returned");
  std::atomic<int> once{0};
  const auto due = Clock::now() + std::chrono::milliseconds(20);
  std::atomic<int64_t> late_us{-1};
  QueueTimerOnce([&](void*) {
    late_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - due).count();
    once.fetch_add(1);
  }, nullptr, due);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  Check(once.load() == 1, "one-shot runs once");
  Check(late_us.load() >= 0 && late_us.load() < 15000, "one-shot not early, under 15 ms late");
  std::printf("one-shot late %lld us\n", (long long)late_us.load());
  std::atomic<int> self{0};
  std::shared_ptr<std::weak_ptr<TimerQueueWaitItem>> slot = std::make_shared<std::weak_ptr<TimerQueueWaitItem>>();
  std::atomic<bool> ready{false};
  *slot = QueueTimerRecurring([&, slot](void*) {
    while (!ready.load()) std::this_thread::yield();
    self.fetch_add(1);
    if (auto p = slot->lock()) p->Disarm();
  }, nullptr, Clock::now(), std::chrono::milliseconds(1));
  ready = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  Check(self.load() == 1, "self-disarm from the callback runs once");
  std::printf(g_fail ? "timer-queue-sleep-test FAILED %d\n" : "timer-queue-sleep-test PASS\n", g_fail);
  std::fflush(stdout);
  std::_Exit(g_fail ? 1 : 0);
}
