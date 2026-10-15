// Linux wall-clock thread sampler ported from WWE 13's src/thread_profiler.cpp.
// The WWE13_* environment/API names and WWE13TP1 binary format intentionally
// remain stable for external profiling scripts.

#include "thread_profiler.h"

#if defined(__linux__)
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/diagnostics/rate_census.h>
#include <rex/logging.h>

namespace wwe2k14 {
namespace {

constexpr int kMaxThreads = 64;
constexpr int kMaxDepth = 48;
constexpr uint32_t kRingSlots = 8192;
constexpr char kGuestThreadPrefix[] = "XThread";

struct Sample {
  uint64_t t_ns;
  uint32_t tid;
  uint32_t depth;
  void* pcs[kMaxDepth];
};

struct Ring {
  std::atomic<pid_t> tid{0};
  std::atomic<uint32_t> head{0};
  std::atomic<uint32_t> tail{0};
  std::atomic<uint64_t> dropped{0};
  timer_t timer{};
  // Written once by AttachThread before the ring index is published, read by the
  // worker (including the hang watchdog dump). Never mutated afterwards.
  std::string name;
  Sample slots[kRingSlots];
};

Ring* g_rings[kMaxThreads] = {};
std::atomic<int> g_ring_count{0};
std::atomic<bool> g_running{false};
std::thread g_worker;
std::mutex g_lifecycle;

// Opt-in hang watchdog (src/thread_profiler.cpp): when WWE13_HANG_DUMP names a
// path and the game's vdswap counter has been 0 for >= 5 s after the game had
// been presenting, the worker dumps the most recent SIGPROF stack of every
// attached guest thread (host PCs) plus /proc/self/maps so the freeze can be
// symbolized offline. Off unless the env var is set. This is the capture hook
// kept for hang investigations on user machines.
std::atomic<bool> g_hang_dump_enabled{false};
std::string g_hang_dump_path;

uint64_t NowNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

void OnSigprof(int, siginfo_t*, void*) {
  const int saved_errno = errno;
  const pid_t self = pid_t(syscall(SYS_gettid));
  const int count = g_ring_count.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i) {
    Ring* ring = g_rings[i];
    if (ring->tid.load(std::memory_order_relaxed) != self) {
      continue;
    }
    const uint32_t head = ring->head.load(std::memory_order_relaxed);
    if (head - ring->tail.load(std::memory_order_acquire) >= kRingSlots) {
      ring->dropped.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    Sample& sample = ring->slots[head % kRingSlots];
    sample.t_ns = NowNs();
    sample.tid = uint32_t(self);
    sample.depth = uint32_t(backtrace(sample.pcs, kMaxDepth));
    ring->head.store(head + 1, std::memory_order_release);
    errno = saved_errno;
    return;
  }
  errno = saved_errno;
}

std::vector<std::string> SplitTokens(const char* value) {
  std::vector<std::string> result;
  std::string current;
  for (const char* p = value;; ++p) {
    if (*p == ',' || *p == '\0') {
      if (!current.empty()) result.push_back(current);
      current.clear();
      if (*p == '\0') break;
    } else {
      current += *p;
    }
  }
  return result;
}

struct ProfileTargets {
  std::vector<std::string> prefixes;
  bool vdswap = false;
  size_t topcpu_count = 0;
};

ProfileTargets ParseTargets(const char* value) {
  ProfileTargets targets;
  for (const std::string& token : SplitTokens(value)) {
    if (token == "@vdswap") {
      targets.vdswap = true;
      continue;
    }
    constexpr char kTopCpuPrefix[] = "@topcpu:";
    if (token.compare(0, sizeof(kTopCpuPrefix) - 1, kTopCpuPrefix) == 0) {
      char* end = nullptr;
      errno = 0;
      const long count = std::strtol(token.c_str() + sizeof(kTopCpuPrefix) - 1, &end, 10);
      if (errno == 0 && end != token.c_str() + sizeof(kTopCpuPrefix) - 1 && *end == '\0' &&
          count > 0) {
        targets.topcpu_count = std::min<size_t>(kMaxThreads,
                                                targets.topcpu_count + size_t(count));
      } else {
        REXLOG_WARN("[wwe13-thread-profile] ignoring invalid target {}", token);
      }
      continue;
    }
    targets.prefixes.push_back(token);
  }
  return targets;
}

bool AlreadyProfiled(pid_t tid) {
  const int count = g_ring_count.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i) {
    if (g_rings[i]->tid.load(std::memory_order_relaxed) == tid) return true;
  }
  return false;
}

std::string ReadThreadComm(pid_t tid) {
  std::ifstream input("/proc/self/task/" + std::to_string(tid) + "/comm");
  std::string name;
  if (input) std::getline(input, name);
  return name;
}

struct ThreadCpuTicks {
  std::string name;
  uint64_t ticks = 0;
};
using ThreadCpuSnapshot = std::unordered_map<pid_t, ThreadCpuTicks>;

bool ReadThreadStat(const std::filesystem::path& path, ThreadCpuTicks& result) {
  std::ifstream input(path);
  std::string line;
  if (!input || !std::getline(input, line)) return false;
  const size_t name_start = line.find('(');
  const size_t name_end = line.rfind(") ");
  if (name_start == std::string::npos || name_end <= name_start) return false;
  std::istringstream fields(line.substr(name_end + 2));
  std::string state;
  if (!(fields >> state)) return false;
  uint64_t user = 0, system = 0;
  for (int field = 4; field <= 15; ++field) {
    std::string value;
    if (!(fields >> value)) return false;
    if (field == 14) {
      try { user = std::stoull(value); } catch (...) { return false; }
    } else if (field == 15) {
      try { system = std::stoull(value); } catch (...) { return false; }
    }
  }
  result.name = line.substr(name_start + 1, name_end - name_start - 1);
  result.ticks = user + system;
  return true;
}

ThreadCpuSnapshot ReadGuestThreadCpu() {
  ThreadCpuSnapshot result;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", error)) {
    if (error) break;
    const pid_t tid = pid_t(std::strtol(entry.path().filename().c_str(), nullptr, 10));
    ThreadCpuTicks ticks;
    if (ReadThreadStat(entry.path() / "stat", ticks) &&
        ticks.name.compare(0, sizeof(kGuestThreadPrefix) - 1, kGuestThreadPrefix) == 0) {
      result.emplace(tid, std::move(ticks));
    }
  }
  return result;
}

struct TimedThreadCpuSnapshot {
  std::chrono::steady_clock::time_point time;
  ThreadCpuSnapshot threads;
};

bool AttachThread(pid_t tid, const std::string& name, long interval_ns);

bool AttachTopCpuThreads(const ThreadCpuSnapshot& previous, const ThreadCpuSnapshot& current,
                        size_t count, long interval_ns) {
  struct Candidate { pid_t tid; const std::string* name; uint64_t ticks; };
  std::vector<Candidate> candidates;
  for (const auto& [tid, ticks] : current) {
    const auto prior = previous.find(tid);
    if (prior == previous.end() || ticks.ticks < prior->second.ticks || AlreadyProfiled(tid)) continue;
    candidates.push_back({tid, &ticks.name, ticks.ticks - prior->second.ticks});
  }
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    return a.ticks != b.ticks ? a.ticks > b.ticks : a.tid < b.tid;
  });
  size_t attached = 0;
  for (const Candidate& candidate : candidates) {
    if (attached >= count || g_ring_count.load(std::memory_order_relaxed) >= kMaxThreads) break;
    if (AttachThread(candidate.tid, *candidate.name, interval_ns)) ++attached;
  }
  return attached != 0;
}

bool ScanTargets(const ProfileTargets& targets, long interval_ns, bool scan_prefixes) {
  bool attached = false;
  if (targets.vdswap) {
    const auto sample = rex::diagnostics::rate_census::GetLatestSample();
    if (sample && sample->vdswap_tid != 0 && !AlreadyProfiled(pid_t(sample->vdswap_tid))) {
      std::string name = ReadThreadComm(pid_t(sample->vdswap_tid));
      if (name.empty()) name = "unknown";
      attached |= AttachThread(pid_t(sample->vdswap_tid), name, interval_ns);
    }
  }
  if (!scan_prefixes || targets.prefixes.empty()) return attached;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", error)) {
    if (error) break;
    const pid_t tid = pid_t(std::strtol(entry.path().filename().c_str(), nullptr, 10));
    const std::string name = ReadThreadComm(tid);
    for (const std::string& prefix : targets.prefixes) {
      if (name.compare(0, prefix.size(), prefix) == 0 && !AlreadyProfiled(tid)) {
        attached |= AttachThread(tid, name, interval_ns);
      }
    }
  }
  return attached;
}

bool AttachThread(pid_t tid, const std::string& name, long interval_ns) {
  const int index = g_ring_count.load(std::memory_order_relaxed);
  if (index >= kMaxThreads) return false;
  Ring* ring = new Ring();
  ring->tid.store(tid, std::memory_order_relaxed);
  ring->name = name;
  sigevent sev{};
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = SIGPROF;
  sev._sigev_un._tid = tid;
  if (timer_create(CLOCK_MONOTONIC, &sev, &ring->timer) != 0) {
    REXLOG_WARN("[wwe13-thread-profile] timer_create failed for tid {} ({})", tid, name);
    delete ring;
    return false;
  }
  g_rings[index] = ring;
  g_ring_count.store(index + 1, std::memory_order_release);
  itimerspec spec{};
  spec.it_interval.tv_nsec = interval_ns;
  spec.it_value.tv_nsec = interval_ns;
  timer_settime(ring->timer, 0, &spec, nullptr);
  REXLOG_INFO("[wwe13-thread-profile] sampling tid={} name={} interval_ns={}", tid, name,
              interval_ns);
  return true;
}

void CopyMaps(const std::string& path) {
  std::ifstream input("/proc/self/maps", std::ios::binary);
  std::ofstream output(path + ".maps", std::ios::binary | std::ios::trunc);
  output << input.rdbuf();
}

void WriteSample(FILE* file, const Sample& sample) {
  std::fwrite(&sample.t_ns, sizeof(sample.t_ns), 1, file);
  std::fwrite(&sample.tid, sizeof(sample.tid), 1, file);
  std::fwrite(&sample.depth, sizeof(sample.depth), 1, file);
  for (uint32_t d = 0; d < sample.depth; ++d) {
    const uint64_t pc = uint64_t(uintptr_t(sample.pcs[d]));
    std::fwrite(&pc, sizeof(pc), 1, file);
  }
}

void Drain(FILE* file) {
  const int count = g_ring_count.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i) {
    Ring* ring = g_rings[i];
    uint32_t tail = ring->tail.load(std::memory_order_relaxed);
    const uint32_t head = ring->head.load(std::memory_order_acquire);
    for (; tail != head; ++tail) WriteSample(file, ring->slots[tail % kRingSlots]);
    ring->tail.store(tail, std::memory_order_release);
  }
  std::fflush(file);
}

// Called from the profiler worker when vdswap has been 0 for >= 5 s. Writes the
// most recent sampled stack of every attached guest thread as host PCs plus a
// copy of /proc/self/maps next to the dump, so the freeze can be symbolized
// with the (unstripped, PIE) wwe2k14 binary. Diagnostic only; a sample can be
// read while the signal handler is writing it.
extern "C" void wwe2k14_hang_extra_dump(FILE*) __attribute__((weak));
void DumpHang(const rex::diagnostics::rate_census::Sample& latest) {
  const std::string path = g_hang_dump_path;
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    REXLOG_WARN("[hang-watchdog] cannot open {}", path);
    return;
  }
  std::fprintf(file, "# hang-watchdog: vdswap=0 for >= 5 s after presenting\n");
  std::fprintf(file,
               "sample t_ms=%llu vdswap=%.3f present=%.3f vblank=%.3f guest_ms_per_s=%.3f "
               "vdswap_tid=%u vdswap_thread_cpu_pct=%.3f\n",
               (unsigned long long)latest.t_ms, latest.vdswap, latest.present, latest.vblank,
               latest.guest_ms_per_s, latest.vdswap_tid, latest.vdswap_thread_cpu_pct);
  const int count = g_ring_count.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i) {
    Ring* ring = g_rings[i];
    const uint32_t head = ring->head.load(std::memory_order_acquire);
    std::fprintf(file, "thread tid=%d name=%s samples=%u dropped=%llu\n", int(ring->tid.load()),
                 ring->name.c_str(), head, (unsigned long long)ring->dropped.load());
    if (head == 0) {
      continue;
    }
    const Sample& sample = ring->slots[(head - 1) % kRingSlots];
    std::fprintf(file, "  depth=%u\n", sample.depth);
    const uint32_t depth = sample.depth > kMaxDepth ? kMaxDepth : sample.depth;
    for (uint32_t d = 0; d < depth; ++d) {
      std::fprintf(file, "  pc[%u]=0x%llx\n", d, (unsigned long long)uintptr_t(sample.pcs[d]));
    }
  }
  std::fclose(file);
  CopyMaps(path);
  // Optional companion probe (src/cage_hang_probe.cpp) that appends guest
  // memory at the stuck queue/command; resolved weakly so the profiler does
  // not depend on it.
  if (wwe2k14_hang_extra_dump != nullptr) {
    FILE* extra = std::fopen(path.c_str(), "ab");
    if (extra) {
      wwe2k14_hang_extra_dump(extra);
      std::fclose(extra);
    }
  }
  REXLOG_WARN("[hang-watchdog] dumped frozen guest stacks to {} (+ .maps)", path);
}

void Worker(ProfileTargets targets, long interval_ns, std::string out_path) {
  FILE* file = std::fopen(out_path.c_str(), "wb");
  if (!file) {
    REXLOG_WARN("[wwe13-thread-profile] cannot open {}", out_path);
    return;
  }
  std::fwrite("WWE13TP1", 1, 8, file);
  const auto start = std::chrono::steady_clock::now();
  auto next_scan = start;
  auto next_cpu_sample = start;
  std::deque<TimedThreadCpuSnapshot> cpu_snapshots;
  if (targets.topcpu_count) {
    cpu_snapshots.push_back({start, ReadGuestThreadCpu()});
    next_cpu_sample += std::chrono::seconds(1);
  }
  bool evaluated120 = false, evaluated240 = false;
  const auto eval_topcpu = [&](bool& evaluated) {
    if (evaluated || cpu_snapshots.size() < 2) return false;
    evaluated = true;
    const auto& current = cpu_snapshots.back();
    const auto cutoff = current.time - std::chrono::seconds(5);
    const TimedThreadCpuSnapshot* previous = nullptr;
    for (auto it = cpu_snapshots.rbegin() + 1; it != cpu_snapshots.rend(); ++it) {
      if (it->time <= cutoff) { previous = &*it; break; }
    }
    return previous && AttachTopCpuThreads(previous->threads, current.threads,
                                            targets.topcpu_count, interval_ns);
  };
  // Hang watchdog state (see DumpHang): only active with WWE13_HANG_DUMP.
  const bool hang_watch = g_hang_dump_enabled.load(std::memory_order_relaxed);
  bool saw_swap = false;
  bool in_zero = false;
  std::chrono::steady_clock::time_point zero_since = start;
  std::chrono::steady_clock::time_point next_hang_dump = start;
  while (g_running.load(std::memory_order_acquire)) {
    const auto now = std::chrono::steady_clock::now();
    bool attached = false;
    if (hang_watch) {
      const auto sample = rex::diagnostics::rate_census::GetLatestSample();
      if (sample) {
        if (sample->vdswap > 0.0) {
          saw_swap = true;
          in_zero = false;
        } else if (saw_swap && !in_zero) {
          in_zero = true;
          zero_since = now;
        }
        if (in_zero && now - zero_since >= std::chrono::seconds(5) && now >= next_hang_dump) {
          DumpHang(*sample);
          next_hang_dump = now + std::chrono::seconds(5);
        }
      }
    }
    if (targets.topcpu_count && now >= next_cpu_sample) {
      next_cpu_sample = now + std::chrono::seconds(1);
      cpu_snapshots.push_back({now, ReadGuestThreadCpu()});
      while (cpu_snapshots.size() > 8) cpu_snapshots.pop_front();
    }
    if (targets.topcpu_count && now - start >= std::chrono::seconds(120)) {
      attached |= eval_topcpu(evaluated120);
    }
    if (targets.topcpu_count && now - start >= std::chrono::seconds(240)) {
      attached |= eval_topcpu(evaluated240);
    }
    const bool scan_prefixes = now - start < std::chrono::minutes(10);
    if (now >= next_scan && (scan_prefixes ? (!targets.prefixes.empty() || targets.vdswap)
                                           : targets.vdswap)) {
      next_scan = now + std::chrono::seconds(2);
      attached |= ScanTargets(targets, interval_ns, scan_prefixes);
    }
    if (attached) CopyMaps(out_path);
    Drain(file);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const int count = g_ring_count.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i) {
    timer_delete(g_rings[i]->timer);
    REXLOG_INFO("[wwe13-thread-profile] tid={} dropped={}", g_rings[i]->tid.load(),
                g_rings[i]->dropped.load());
  }
  Drain(file);
  CopyMaps(out_path);
  std::fclose(file);
}

}  // namespace

void StartThreadProfilerFromEnv() {
  const char* names = std::getenv("WWE13_THREAD_PROFILE");
  const char* hang_env = std::getenv("WWE13_HANG_DUMP");
  const bool hang_watch = hang_env && *hang_env;
  const char* effective_names = (names && *names) ? names : (hang_watch ? "XThread*,@vdswap,@topcpu:8" : nullptr);
  if (!effective_names) return;
  std::lock_guard<std::mutex> lock(g_lifecycle);
  if (g_running.load()) return;
  if (hang_watch) {
    g_hang_dump_path = hang_env;
    g_hang_dump_enabled.store(true, std::memory_order_release);
  }
  long hz = 500;
  if (const char* value = std::getenv("WWE13_THREAD_PROFILE_HZ")) {
    hz = std::max(2L, std::min(5000L, std::strtol(value, nullptr, 10)));
  }
  const char* out_env = std::getenv("WWE13_THREAD_PROFILE_OUT");
  const std::string out_path = out_env && *out_env ? out_env : "thread-profile.bin";
  void* warm[4];
  backtrace(warm, 4);
  struct sigaction action {};
  action.sa_sigaction = OnSigprof;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaction(SIGPROF, &action, nullptr);
  g_running.store(true, std::memory_order_release);
  g_worker = std::thread(Worker, ParseTargets(effective_names), 1000000000L / hz, out_path);
  std::atexit(StopThreadProfiler);
  REXLOG_INFO("[wwe13-thread-profile] enabled names={} hz={} out={} hang_dump={}", effective_names,
              hz, out_path, hang_watch ? g_hang_dump_path : "(off)");
}

void StopThreadProfiler() {
  std::lock_guard<std::mutex> lock(g_lifecycle);
  if (!g_running.exchange(false)) return;
  if (g_worker.joinable()) g_worker.join();
}

}  // namespace wwe2k14
#else
namespace wwe2k14 {
void StartThreadProfilerFromEnv() {}
void StopThreadProfiler() {}
}  // namespace wwe2k14
#endif
