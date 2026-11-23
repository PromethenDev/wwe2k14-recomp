// Imported into WWE 2K14 native workspace (Track E, 2026-10-13) from
//   <wwe13-recomp-nr>/src/nr/nr_pc_sampler.h
//   repo HEAD 19c8bb361a9b7d6cba65afffc628bda023da2e0c (file last changed in 9f9b8a1),
//   original sha256 996c3c7a663eb0679fd6fed7e2b1426868ea86f50735d32852eb2b4192888dfa.
// Local changes: WWE13_NR* renamed to WWE2K14_NR* (comments/env names only); 2026-10-15 S3-5: AddCurrentThread (one extra
// sampled thread, kind 2 = D3D worker) in both variants.
// Opt-in PC sampler (WWE2K14_NR_PC_SAMPLER=1; Linux x86-64 only): perf is not available to unprivileged users on the
// dev box (perf_event_paranoid=4), so this samples the instruction pointer of chosen threads with SIGPROF.
// Each sample = (thread kind, pc - executable base). Dump() writes "kind offset count" lines; symbolise with
// tools/nr/pc-sampler-report.sh. Leaf PC only (no unwinding), ~2000 samples/s per thread while a replay runs.
// Windows (P31 diagnostic): a dedicated sampler thread polls at ~1 kHz with a plain sleep loop (SuspendThread ->
// GetThreadContext -> read Rip, *Rsp, *(Rbp+8) with ReadProcessMemory -> ResumeThread). The suspended window does no heap
// allocation and takes no loader lock. Module/offset resolution and the file write happen on the sampler thread (and once
// at exit) - never on the sampled threads: Dump() is only a cheap "set the output path" call.
#pragma once

#if defined(__linux__) && !defined(__ANDROID__) && defined(__x86_64__)
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>

namespace nr::sampler {
constexpr uint32_t kCap = 1u << 20;
struct Sample {
  uint32_t kind;
  uint64_t pc;
  uint64_t ra_sp;  // *(rsp): the return address when the sample is inside a leaf libc routine such as memcpy
  uint64_t ra_bp;  // *(rbp + 8): the caller of the nearest frame-pointer frame
  uint32_t epoch;  // guest frame (NoteFrame)
};
inline Sample* g_samples = nullptr;
inline std::atomic<uint32_t> g_count{0};
inline pthread_t g_targets[4];
inline std::atomic<bool> g_extra_set{false};  // slot 3: a thread added by AddCurrentThread
inline std::atomic<int> g_tids[4];  // kernel tids learned by the first signal; later signals use tgkill (safe on dead threads)
inline uint32_t g_kinds[4];
inline int g_ntargets = 0;
inline std::atomic<bool> g_running{false};
inline uintptr_t g_base = 0;
inline uintptr_t g_end = 0;  // end of the main executable's PT_LOAD segments
// ALL mode (WWE2K14_NR_PC_SAMPLER_ALL=1, PERFSTAB-1b 2026-11-17): every thread of the process gets a per-thread
// CPU-time timer (timer_create on the thread's CPU clock, SIGEV_THREAD_ID -> SIGPROF every 1 ms of THAT thread's CPU
// time), so samples are proportional to CPU used and idle/sleeping threads are never woken. New threads are picked up
// once a second from /proc/self/task. kind = 100 + slot, slot names in pcsamples-threads.txt. Samples are drained from
// the ring into g_hist by the sampler thread (1 s lag), so the run length is not limited by kCap.
inline bool g_all = false;
constexpr int kMaxSlots = 256;
inline std::atomic<int> g_slot_tid[kMaxSlots];
inline char g_slot_name[kMaxSlots][20];
inline std::atomic<uint64_t> g_slot_samples[kMaxSlots];
inline int g_nslots = 0;
inline uint64_t g_drained = 0, g_lost = 0;
inline std::map<std::array<uint64_t, 4>, uint64_t> g_hist;  // guarded by g_mutex
inline std::mutex g_mutex;
inline std::string g_path;
inline std::atomic<bool> g_have_path{false};
constexpr int kDumpSeconds = 10;
// Stutter attribution (2026-10-17 night): every sample carries the guest frame it was taken in; NoteFrame (called by the
// game's flip hook, src/present_interval.cpp) marks frames longer than 45 ms. DumpNow writes such samples with kind + 10.
inline std::atomic<uint32_t> g_epoch{0};
inline std::atomic<uint64_t> g_spike_bits[1024];  // epoch % 65536
// In-run A/B arm (N4 step 2b, 2026-11-18): set by a native-body switch running with _AB=N (1 = native block). ALL-mode
// samples get kind + 2000 * arm, so tools/nr/pc-profile-all.py --by-arm splits every thread's CPU by arm.
inline std::atomic<uint32_t> g_arm{0};
inline void NoteArm(uint32_t arm) { g_arm.store(arm, std::memory_order_relaxed); }
inline void NoteFrame(uint64_t dt_us) {
  const uint32_t e = g_epoch.fetch_add(1, std::memory_order_relaxed);
  const uint32_t n = (e + 1) & 0xFFFF;
  g_spike_bits[n >> 6].fetch_and(~(uint64_t(1) << (n & 63)), std::memory_order_relaxed);  // the next frame starts clean
  if (dt_us > 45000) g_spike_bits[(e & 0xFFFF) >> 6].fetch_or(uint64_t(1) << (e & 63), std::memory_order_relaxed);
}
inline bool SpikeEpoch(uint32_t e) {
  e &= 0xFFFF;
  return (g_spike_bits[e >> 6].load(std::memory_order_relaxed) >> (e & 63)) & 1;
}

inline void Handler(int, siginfo_t*, void* ctx) {
  const uint32_t i = g_count.fetch_add(1, std::memory_order_relaxed);
  if (!g_all && i >= kCap) return;
  const pthread_t self = pthread_self();
  uint32_t kind = 99;
  if (g_all) {
    const int tid = int(syscall(SYS_gettid));
    for (int t = 0; t < kMaxSlots; ++t) {
      if (g_slot_tid[t].load(std::memory_order_relaxed) == tid) {
        kind = 100 + uint32_t(t) + 2000u * g_arm.load(std::memory_order_relaxed);
        g_slot_samples[t].fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  } else
  for (int t = 0; t < g_ntargets; ++t) {
    if (pthread_equal(self, g_targets[t])) {
      kind = g_kinds[t];
      g_tids[t].store(int(syscall(SYS_gettid)), std::memory_order_relaxed);
    }
  }
  if (!g_all && g_extra_set.load(std::memory_order_acquire) && pthread_equal(self, g_targets[3])) kind = g_kinds[3];
  const uintptr_t pc = uintptr_t(static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
  const auto* uc = static_cast<ucontext_t*>(ctx);
  const uintptr_t sp = uintptr_t(uc->uc_mcontext.gregs[REG_RSP]), bp = uintptr_t(uc->uc_mcontext.gregs[REG_RBP]);
  g_samples[g_all ? i % kCap : i] = {kind, uint64_t(pc), sp ? *reinterpret_cast<const uint64_t*>(sp) : 0,
                  (bp && (bp & 7) == 0 && bp > sp && bp - sp < (1u << 20)) ? *reinterpret_cast<const uint64_t*>(bp + 8) : 0,
                  g_epoch.load(std::memory_order_relaxed)};
}

inline int PhdrCb(struct dl_phdr_info* info, size_t, void*) {
  g_base = uintptr_t(info->dlpi_addr);  // first object = the main executable
  for (int k = 0; k < info->dlpi_phnum; ++k) {
    const auto& ph = info->dlpi_phdr[k];
    if (ph.p_type == PT_LOAD) g_end = std::max<uintptr_t>(g_end, g_base + ph.p_vaddr + ph.p_memsz);
  }
  return 1;
}

// ALL mode helpers (sampler thread only).
inline void DrainTo(uint64_t upto) {  // caller holds g_mutex; ring entries [g_drained, upto)
  if (upto <= g_drained) return;
  if (upto - g_drained > kCap) {
    g_lost += (upto - g_drained) - kCap;
    g_drained = upto - kCap;
  }
  for (uint64_t i = g_drained; i < upto; ++i) {
    const Sample& x = g_samples[i % kCap];
    const bool in_exe = x.pc >= g_base && x.pc < g_end;
    // Samples inside the executable need no return-address hints (keeps the histogram small on long runs).
    ++g_hist[{x.kind + (SpikeEpoch(x.epoch) ? 1000u : 0u), x.pc, in_exe ? 0 : x.ra_sp, in_exe ? 0 : x.ra_bp}];
  }
  g_drained = upto;
}
inline bool ReadTask(int tid, uint64_t* ticks, char name[20]) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
  FILE* f = std::fopen(path, "r");
  if (!f) return false;
  char buf[1024];
  const size_t len = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[len] = 0;
  char* l = std::strchr(buf, '(');
  char* r = std::strrchr(buf, ')');
  if (!l || !r || r < l) return false;
  const size_t nl = std::min<size_t>(size_t(r - l - 1), 19);
  std::memcpy(name, l + 1, nl);
  name[nl] = 0;
  // after ") ": state(3) ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime(14) stime(15)
  unsigned long long ut = 0, st = 0;
  if (std::sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return false;
  *ticks = ut + st;
  return true;
}

inline void DumpNow();
// kinds: 0 = renderer thread, 1 = record thread. Call once, from the renderer thread; record thread optional.
inline void Start(uintptr_t record_thread, bool have_record) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_running.exchange(true)) return;
  dl_iterate_phdr(PhdrCb, nullptr);
  g_samples = new Sample[kCap];
  g_targets[0] = pthread_self();
  g_kinds[0] = 0;
  g_ntargets = 1;
  if (have_record) {
    g_targets[1] = pthread_t(record_thread);
    g_kinds[1] = 1;
    g_ntargets = 2;
  }
  struct sigaction sa = {};
  sa.sa_sigaction = Handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, nullptr);
  if (const char* a = std::getenv("WWE2K14_NR_PC_SAMPLER_ALL"); a && a[0] == '1') g_all = true;
  std::atexit([] { DumpNow(); });
  if (g_all) {
    std::thread([] {
      const int self = int(syscall(SYS_gettid));
      auto t0 = std::chrono::steady_clock::now(), last_dump = t0;
      uint64_t seen_last_scan = 0;
      int snap = 0;
      while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto now = std::chrono::steady_clock::now();
        {
          if (DIR* d = opendir("/proc/self/task")) {
            while (dirent* e = readdir(d)) {
              const int tid = std::atoi(e->d_name);
              if (tid <= 0 || tid == self) continue;
              int slot = -1;
              for (int t = 0; t < g_nslots; ++t)
                if (g_slot_tid[t].load(std::memory_order_relaxed) == tid) slot = t;
              uint64_t ticks = 0;
              char name[20];
              if (!ReadTask(tid, &ticks, name)) continue;
              if (slot >= 0) {
                std::memcpy(g_slot_name[slot], name, sizeof(name));  // threads rename themselves after start
                continue;
              }
              if (g_nslots >= kMaxSlots) continue;
              slot = g_nslots;
              std::memcpy(g_slot_name[slot], name, sizeof(name));
              g_slot_tid[slot].store(tid, std::memory_order_relaxed);
              ++g_nslots;
              // Linux CPU-clock id of one thread: MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED) = (~tid << 3) | 4 | 2.
              const clockid_t clk = clockid_t((~unsigned(tid) << 3) | 6u);
              struct sigevent sev = {};
              sev.sigev_notify = SIGEV_THREAD_ID;
              sev.sigev_signo = SIGPROF;
              sev._sigev_un._tid = tid;
              timer_t timer;
              if (timer_create(clk, &sev, &timer) == 0) {
                struct itimerspec its = {};
                its.it_interval.tv_nsec = 1000000;
                its.it_value.tv_nsec = 1000000;
                timer_settime(timer, 0, &its, nullptr);
              }
            }
            closedir(d);
          }
          std::lock_guard<std::mutex> lk(g_mutex);
          DrainTo(seen_last_scan);  // entries claimed one second ago are complete
          seen_last_scan = g_count.load(std::memory_order_acquire);
        }
        if (now - last_dump > std::chrono::seconds(kDumpSeconds)) {
          last_dump = now;
          DumpNow();
          const int sec = int(std::chrono::duration_cast<std::chrono::seconds>(now - t0).count());
          if (sec / 60 > snap && g_have_path.load()) {
            snap = sec / 60;
            std::string p;
            {
              std::lock_guard<std::mutex> lk(g_mutex);
              p = g_path;
            }
            const std::string snapname = p + ".t" + std::to_string(sec);
            std::FILE* src = std::fopen(p.c_str(), "rb");
            std::FILE* dst = src ? std::fopen(snapname.c_str(), "wb") : nullptr;
            if (src && dst) {
              char buf[65536];
              size_t k;
              while ((k = std::fread(buf, 1, sizeof(buf), src)) > 0) std::fwrite(buf, 1, k, dst);
            }
            if (src) std::fclose(src);
            if (dst) std::fclose(dst);
          }
        }
      }
    }).detach();
    std::fprintf(stderr, "[nr] pc sampler on: ALL threads, 1 sample per ms of each thread's CPU time (base %p)\n",
                 reinterpret_cast<void*>(g_base));
    return;
  }
  std::thread([] {
    auto last = std::chrono::steady_clock::now();
    const auto t0 = last;
    while (true) {
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      for (int t = 0; t < g_ntargets; ++t) {
        // First signal by pthread_kill (threads are alive at Start); then tgkill by learned tid, so a thread that has
        // exited (record thread joined at shutdown) gives ESRCH instead of a use-after-free in pthread_kill.
        const int tid = g_tids[t].load(std::memory_order_relaxed);
        if (tid) syscall(SYS_tgkill, getpid(), tid, SIGPROF);
        else if (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1)) pthread_kill(g_targets[t], SIGPROF);
      }
      if (const int tid = g_extra_set.load(std::memory_order_acquire) ? g_tids[3].load(std::memory_order_relaxed) : 0) {
        syscall(SYS_tgkill, getpid(), tid, SIGPROF);
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - last > std::chrono::seconds(kDumpSeconds)) {
        last = now;
        DumpNow();  // dladdr + file write happen here, not on the sampled threads
      }
    }
  }).detach();
  std::fprintf(stderr, "[nr] pc sampler on: %d threads, 2000 Hz (base %p)\n", g_ntargets, reinterpret_cast<void*>(g_base));
}

// Adds the CALLING thread as an extra sampled thread (one only; later calls are ignored). Usable before or after Start.
inline void AddCurrentThread(uint32_t kind) {
  if (g_extra_set.load()) return;
  g_targets[3] = pthread_self();
  g_kinds[3] = kind;
  g_tids[3].store(int(syscall(SYS_gettid)), std::memory_order_relaxed);
  g_extra_set.store(true, std::memory_order_release);
}

inline const char* Where(uint64_t pc, unsigned long long* off, const char** sym) {
  Dl_info info = {};
  if (pc && dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
    *off = pc - uintptr_t(info.dli_fbase);
    *sym = info.dli_sname ? info.dli_sname : "-";
    return info.dli_fname;
  }
  *off = pc;
  *sym = "-";
  return "?";
}

inline void DumpNow() {
  if (!g_running.load() || !g_have_path.load()) return;
  std::lock_guard<std::mutex> lk(g_mutex);
  std::map<std::array<uint64_t, 4>, uint64_t> local;
  if (!g_all) {
    const uint32_t n = std::min<uint32_t>(uint32_t(std::min<uint64_t>(g_count.load(), kCap)), kCap);
    for (uint32_t i = 0; i < n; ++i)
      ++local[{g_samples[i].kind + (SpikeEpoch(g_samples[i].epoch) ? 10u : 0u), g_samples[i].pc, g_samples[i].ra_sp, g_samples[i].ra_bp}];
  } else {
    if (FILE* f = std::fopen((g_path + "-threads.txt").c_str(), "w")) {
      std::fprintf(f, "# kind tid name samples (drained %llu lost %llu)\n", (unsigned long long)g_drained,
                   (unsigned long long)g_lost);
      for (int t = 0; t < g_nslots; ++t)
        std::fprintf(f, "%d %d %s %llu\n", 100 + t, g_slot_tid[t].load(), g_slot_name[t],
                     (unsigned long long)g_slot_samples[t].load());
      std::fclose(f);
    }
  }
  const auto& hist = g_all ? g_hist : local;
  if (FILE* f = std::fopen(g_path.c_str(), "w")) {
    for (auto& [k, c] : hist) {
      unsigned long long o0, o1, o2;
      const char *s0, *s1, *s2;
      const char* m0 = Where(k[1], &o0, &s0);
      const char* m1 = Where(k[2], &o1, &s1);
      const char* m2 = Where(k[3], &o2, &s2);
      std::fprintf(f, "%u %s %llx %s %s %llx %s %s %llx %s %llu\n", unsigned(k[0]), m0, o0, s0, m1, o1, s1, m2, o2, s2,
                   (unsigned long long)c);
    }
    std::fclose(f);
  }
}

// Called by the frontend every N frames: only records the output path. The histogram/dladdr/file work is done by the
// sampler thread (every kDumpSeconds) and at exit, off the command-processor thread.
inline void Dump(const std::string& path) {
  if (!g_running.load() || g_have_path.load()) return;
  std::lock_guard<std::mutex> lk(g_mutex);
  g_path = path;
  g_have_path.store(true);
}
}  // namespace nr::sampler
#elif defined(_WIN32) && !defined(__ANDROID__) && !defined(REX_UWP)  // card C3: no SuspendThread/ReadProcessMemory sampling in the Xbox app family
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace nr::sampler {
constexpr uint32_t kCap = 1u << 20;
struct Sample {
  uint32_t kind;
  uint64_t pc, ra_sp, ra_bp;
  uint32_t epoch;  // guest frame (NoteFrame)
};
inline Sample* g_samples = nullptr;
inline std::atomic<uint32_t> g_count{0};
inline HANDLE g_targets[4];
inline std::atomic<bool> g_extra_set{false};  // slot 3: a thread added by AddCurrentThread
inline uint32_t g_kinds[4];
inline int g_ntargets = 0;
inline std::atomic<bool> g_running{false};
inline std::mutex g_mutex;
inline std::string g_path;
inline std::atomic<bool> g_have_path{false};
constexpr int kDumpSeconds = 10;
// Stutter attribution (2026-10-17 night): every sample carries the guest frame it was taken in; NoteFrame (called by the
// game's flip hook, src/present_interval.cpp) marks frames longer than 45 ms. DumpNow writes such samples with kind + 10.
inline std::atomic<uint32_t> g_epoch{0};
inline std::atomic<uint64_t> g_spike_bits[1024];  // epoch % 65536
inline void NoteArm(uint32_t) {}
inline void NoteFrame(uint64_t dt_us) {
  const uint32_t e = g_epoch.fetch_add(1, std::memory_order_relaxed);
  const uint32_t n = (e + 1) & 0xFFFF;
  g_spike_bits[n >> 6].fetch_and(~(uint64_t(1) << (n & 63)), std::memory_order_relaxed);  // the next frame starts clean
  if (dt_us > 45000) g_spike_bits[(e & 0xFFFF) >> 6].fetch_or(uint64_t(1) << (e & 63), std::memory_order_relaxed);
}
inline bool SpikeEpoch(uint32_t e) {
  e &= 0xFFFF;
  return (g_spike_bits[e >> 6].load(std::memory_order_relaxed) >> (e & 63)) & 1;
}

inline bool ReadU64(uint64_t addr, uint64_t* out) {  // never faults: ReadProcessMemory returns FALSE for unmapped memory
  SIZE_T got = 0;
  return addr && (addr & 7) == 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, 8, &got) && got == 8;
}

// Runs with the target suspended: no heap allocation, no CRT I/O, no loader lock.
inline void SampleOne(int t) {
  HANDLE h = g_targets[t];
  if (SuspendThread(h) == DWORD(-1)) return;
  CONTEXT ctx;
  ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  uint64_t pc = 0, sp = 0, bp = 0, ra_sp = 0, ra_bp = 0;
  const bool ok = GetThreadContext(h, &ctx) != 0;
  if (ok) {
    pc = ctx.Rip;
    sp = ctx.Rsp;
    bp = ctx.Rbp;
    if (!ReadU64(sp, &ra_sp)) ra_sp = 0;
    if (!(bp > sp && bp - sp < (1u << 20) && ReadU64(bp + 8, &ra_bp))) ra_bp = 0;
  }
  ResumeThread(h);
  if (!ok) return;
  const uint32_t i = g_count.fetch_add(1, std::memory_order_relaxed);
  if (i >= kCap) return;
  g_samples[i] = {g_kinds[t], pc, ra_sp, ra_bp, g_epoch.load(std::memory_order_relaxed)};
}

// Adds the CALLING thread as an extra sampled thread (one only; later calls are ignored). Usable before or after Start.
inline void AddCurrentThread(uint32_t kind) {
  if (g_extra_set.load()) return;
  HANDLE h = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                       THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
    return;
  }
  g_targets[3] = h;
  g_kinds[3] = kind;
  g_extra_set.store(true, std::memory_order_release);
}

// Module basename and (pc - module base). Called only from DumpNow (nothing suspended).
inline const char* Where(uint64_t pc, unsigned long long* off, const char** sym, std::map<uint64_t, std::string>& names) {
  *sym = "-";
  HMODULE mod = nullptr;
  if (pc && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(pc), &mod) && mod) {
    auto& name = names[uint64_t(reinterpret_cast<uintptr_t>(mod))];
    if (name.empty()) {
      wchar_t w[MAX_PATH];
      const DWORD n = GetModuleFileNameW(mod, w, MAX_PATH);
      std::string b;
      size_t start = 0;
      for (DWORD i = 0; i < n; ++i) if (w[i] == L'\\' || w[i] == L'/') start = i + 1;
      for (DWORD i = start; i < n; ++i) b.push_back(w[i] < 128 && w[i] > 32 ? char(w[i]) : '_');
      name = b.empty() ? "?" : b;
    }
    *off = pc - uint64_t(reinterpret_cast<uintptr_t>(mod));
    return name.c_str();
  }
  *off = pc;
  return "?";
}

inline void DumpNow() {
  if (!g_running.load() || !g_have_path.load()) return;
  std::lock_guard<std::mutex> lk(g_mutex);
  const uint32_t n = std::min<uint32_t>(g_count.load(), kCap);
  std::map<std::array<uint64_t, 4>, uint32_t> hist;
  for (uint32_t i = 0; i < n; ++i)
    ++hist[{g_samples[i].kind + (SpikeEpoch(g_samples[i].epoch) ? 10u : 0u), g_samples[i].pc, g_samples[i].ra_sp, g_samples[i].ra_bp}];
  std::map<uint64_t, std::string> names;
  if (FILE* f = std::fopen(g_path.c_str(), "w")) {
    for (auto& [k, c] : hist) {
      unsigned long long o0, o1, o2;
      const char *s0, *s1, *s2;
      const char* m0 = Where(k[1], &o0, &s0, names);
      const char* m1 = Where(k[2], &o1, &s1, names);
      const char* m2 = Where(k[3], &o2, &s2, names);
      std::fprintf(f, "%u %s %llx %s %s %llx %s %s %llx %s %u\n", unsigned(k[0]), m0, o0, s0, m1, o1, s1, m2, o2, s2, c);
    }
    std::fclose(f);
  }
}

// record_thread = a Windows HANDLE (std::thread::native_handle) cast to uintptr_t, or 0.
// kinds: 0 = calling (command-processor) thread, 1 = record thread. Call once from the CP thread.
inline void Start(uintptr_t record_thread, bool have_record) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_running.exchange(true)) return;
  g_samples = new Sample[kCap];
  HANDLE self = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
    std::fprintf(stderr, "[nr] pc sampler: DuplicateHandle failed (%lu); sampler off\n", GetLastError());
    g_running.store(false);
    return;
  }
  g_targets[0] = self;
  g_kinds[0] = 0;
  g_ntargets = 1;
  if (have_record && record_thread) {
    HANDLE rec = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(record_thread), GetCurrentProcess(), &rec, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
      g_targets[1] = rec;
      g_kinds[1] = 1;
      g_ntargets = 2;
    }
  }
  std::atexit([] { DumpNow(); });
  std::thread([] {
    // Plain sleep loop: Sleep granularity is the system timer (default 15.6 ms unless the game raised it), so the
    // achieved rate can be well below 1 kHz; the true rate is samples/second in the dump, not this nominal figure.
    // A high-resolution waitable timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) is tried first; Sleep(1) is the fallback.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    auto last = std::chrono::steady_clock::now();
    while (true) {
      if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -10000;  // 1 ms, relative, 100 ns units
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, 10);
      } else {
        Sleep(1);
      }
      for (int t = 0; t < g_ntargets; ++t) SampleOne(t);
      if (g_extra_set.load(std::memory_order_acquire)) SampleOne(3);
      const auto now = std::chrono::steady_clock::now();
      if (now - last > std::chrono::seconds(kDumpSeconds)) {
        last = now;
        DumpNow();
      }
    }
  }).detach();
  std::fprintf(stderr, "[nr] pc sampler on: %d threads, ~1000 Hz (high-res waitable timer if available, else Sleep(1))\n", g_ntargets);
}

// Called by the frontend every N frames: only records the output path (see the Linux variant).
inline void Dump(const std::string& path) {
  if (!g_running.load() || g_have_path.load()) return;
  std::lock_guard<std::mutex> lk(g_mutex);
  g_path = path;
  g_have_path.store(true);
}
}  // namespace nr::sampler
#else
#include <cstdint>
#include <string>
namespace nr::sampler {
inline void Start(uintptr_t, bool) {}
inline void AddCurrentThread(uint32_t) {}
inline void Dump(const std::string&) {}
inline void NoteArm(uint32_t) {}
inline void NoteFrame(uint64_t) {}
}  // namespace nr::sampler
#endif
