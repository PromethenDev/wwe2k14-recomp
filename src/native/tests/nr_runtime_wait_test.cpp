// Host-only tests for the runtime shader pending-job wait (no SDK, GPU, or game).
#include "../nr/rts_wait.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace {

using namespace std::chrono_literals;
using nr::shared::rts_wait::Result;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
  }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void TestOnlyTheExactKeyWakes() {
  std::mutex mu;
  std::condition_variable cv;
  std::unordered_set<uint64_t> pending{0x1235, 0xABC1};
  const uint64_t wanted = 0x1235;
  const uint64_t other_stage_or_key = 0xABC1;
  std::atomic<bool> translator_ready{false};
  std::atomic<bool> about_to_wait{false};
  std::thread::id translator_id;
  std::thread translator([&] {
    translator_id = std::this_thread::get_id();
    translator_ready.store(true, std::memory_order_release);
    while (!about_to_wait.load(std::memory_order_acquire)) std::this_thread::yield();
    // The waiter's mutex is still held here; acquiring it proves WaitUntil released it inside wait_for.
    {
      std::lock_guard<std::mutex> lock(mu);
      pending.erase(other_stage_or_key);  // Must not wake the wanted key.
    }
    cv.notify_all();
    std::this_thread::sleep_for(30ms);
    {
      std::lock_guard<std::mutex> lock(mu);
      pending.erase(wanted);
    }
    cv.notify_all();
  });
  while (!translator_ready.load(std::memory_order_acquire)) std::this_thread::yield();

  const auto start = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lock(mu);
  about_to_wait.store(true, std::memory_order_release);
  const Result result = nr::shared::rts_wait::WaitUntil(
      cv, lock, [&] { return !pending.count(wanted); }, 200ms, std::this_thread::get_id(), translator_id);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(result == Result::kReady);
  CHECK(elapsed >= 25ms);  // The unrelated completion did not satisfy this key.
  CHECK(elapsed < 500ms);
  lock.unlock();
  translator.join();
}

void TestTimeout() {
  std::mutex mu;
  std::condition_variable cv;
  bool pending = true;
  std::unique_lock<std::mutex> lock(mu);
  const Result result = nr::shared::rts_wait::WaitUntil(cv, lock, [&] { return !pending; }, 10ms,
                                                         std::this_thread::get_id(), std::thread::id{});
  CHECK(result == Result::kTimedOut);
  CHECK(pending);
}

void TestWaitLimitParsing() {
  CHECK(nr::shared::rts_wait::ParseLimitMs(nullptr) == 250);
  CHECK(nr::shared::rts_wait::ParseLimitMs("") == 250);
  CHECK(nr::shared::rts_wait::ParseLimitMs("250") == 250);
  CHECK(nr::shared::rts_wait::ParseLimitMs("0") == 0);
  CHECK(nr::shared::rts_wait::ParseLimitMs("invalid") == 250);
  CHECK(nr::shared::rts_wait::ParseLimitMs("-1") == 250);
}

void TestTranslatorNeverWaitsForItself() {
  std::mutex mu;
  std::condition_variable cv;
  bool pending = true;
  const std::thread::id current = std::this_thread::get_id();
  std::unique_lock<std::mutex> lock(mu);
  const auto start = std::chrono::steady_clock::now();
  const Result result = nr::shared::rts_wait::WaitUntil(cv, lock, [&] { return !pending; }, 250ms, current,
                                                         current);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(result == Result::kWouldDeadlock);
  CHECK(elapsed < 10ms);
}

}  // namespace

int main() {
  TestOnlyTheExactKeyWakes();
  TestTimeout();
  TestWaitLimitParsing();
  TestTranslatorNeverWaitsForItself();
  std::printf("runtime shader wait: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
