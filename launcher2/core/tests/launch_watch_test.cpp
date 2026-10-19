#ifdef NDEBUG
#undef NDEBUG
#endif

#include "launcher_core.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

int main() {
  LaunchPlan failed_early;
  LaunchPlan normal_exit;
#ifdef _WIN32
  failed_early.executable = fs::path("C:\\Windows\\System32\\cmd.exe");
  failed_early.arguments = {"/c", "exit 1"};
  normal_exit.executable = fs::path("C:\\Windows\\System32\\cmd.exe");
  normal_exit.arguments = {"/c", "exit 0"};
#else
  failed_early.executable = "/bin/false";
  normal_exit.executable = "/bin/true";
#endif

  std::atomic<bool> process_started{false};
  const Result failed = StartGame(failed_early, 3.0, &process_started);
  assert(!failed.ok && failed.error == kGameClosedEarlyMessage);
  assert(process_started.load(std::memory_order_acquire));

  process_started.store(false, std::memory_order_relaxed);
  const Result normal = StartGame(normal_exit, 3.0, &process_started);
  assert(normal.ok && process_started.load(std::memory_order_acquire));

  LaunchPlan long_running;
#ifdef _WIN32
  long_running.executable = fs::path("C:\\Windows\\System32\\cmd.exe");
  long_running.arguments = {"/c", "ping -n 4 127.0.0.1 > nul"};
#else
  long_running.executable = "/bin/sleep";
  long_running.arguments = {"2"};
#endif
  CancelFlag cancel{false};
  std::thread request_cancel([&cancel] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cancel.store(true, std::memory_order_release);
  });
  const auto start = std::chrono::steady_clock::now();
  const Result cancelled = StartGame(long_running, 5.0, nullptr, &cancel);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  request_cancel.join();
  assert(cancelled.ok);
  assert(elapsed < std::chrono::seconds(2));
  std::cout << "PASS startup watch reports an unsuccessful early exit, accepts exit code 0, and observes cancellation promptly\n";
  return 0;
}
