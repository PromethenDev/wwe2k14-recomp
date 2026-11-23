#ifdef NDEBUG
#undef NDEBUG
#endif

#include "launcher_core.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
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
#ifndef _WIN32
  {
    // GitHub #107: leftover guest memory files from forced closes are removed; mapped and fresh ones stay.
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "k14-stale-shm-test";
    fs::remove_all(root);
    fs::create_directories(root / "shm");
    fs::create_directories(root / "proc" / "4242");
    fs::create_directories(root / "proc" / "self-ish");
    auto make = [&](const char* name, bool old) {
      std::ofstream(root / "shm" / name) << std::string(8192, 'x');
      if (old) fs::last_write_time(root / "shm" / name, fs::file_time_type::clock::now() - std::chrono::hours(2));
    };
    make("xenia_memory_111", true);   // stale: must go
    make("xenia_memory_222", true);   // mapped by pid 4242: must stay
    make("xenia_memory_333", false);  // just created: must stay
    make("other_file", true);         // not ours: must stay
    std::ofstream(root / "proc" / "4242" / "maps")
        << "7f0000000000-7f0100000000 rw-s 00000000 00:1a 123 /dev/shm/xenia_memory_222\n";
    const wwe2k14::launcher::StaleMemoryCleanup cleaned =
        wwe2k14::launcher::RemoveStaleGuestMemoryFiles(root / "shm", 30.0, root / "proc");
    assert(cleaned.files == 1);
    assert(!fs::exists(root / "shm" / "xenia_memory_111"));
    assert(fs::exists(root / "shm" / "xenia_memory_222"));
    assert(fs::exists(root / "shm" / "xenia_memory_333"));
    assert(fs::exists(root / "shm" / "other_file"));
    // V152 review: a process of this user whose maps cannot be read blocks the whole cleanup (fail closed).
    make("xenia_memory_444", true);
    fs::create_directories(root / "proc" / "5151");
    std::ofstream(root / "proc" / "5151" / "maps") << "unreadable\n";
    fs::permissions(root / "proc" / "5151" / "maps", fs::perms::none);
    const bool can_read_anyway = static_cast<bool>(std::ifstream(root / "proc" / "5151" / "maps"));  // root ignores modes
    const wwe2k14::launcher::StaleMemoryCleanup blocked =
        wwe2k14::launcher::RemoveStaleGuestMemoryFiles(root / "shm", 30.0, root / "proc");
    if (!can_read_anyway) {
      assert(blocked.files == 0);
      assert(fs::exists(root / "shm" / "xenia_memory_444"));
    }
    fs::permissions(root / "proc" / "5151" / "maps", fs::perms::owner_all);
    fs::remove_all(root);
    std::cout << "PASS stale guest memory cleanup (fail closed on an unreadable process)\n";
  }
#endif
  std::cout << "PASS startup watch reports an unsuccessful early exit, accepts exit code 0, and observes cancellation promptly\n";
  return 0;
}
