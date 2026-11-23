// Public release builds (WWE2K14_TEST_TOOLS=OFF): test tool compiled to a no-op (owner rule 2026-10-26).
#ifdef WWE2K14_NO_TEST_TOOLS
namespace wwe2k14::prof {
void Start() {}
}  // namespace wwe2k14::prof
#else
// TEST-only continuous per-thread profiler - see game_profiler.h. Written for this project (lane all-1104, 2026-11-04).
// Suspended window discipline copied from hitch_catcher.cpp: between SuspendThread and ResumeThread only registers are
// read and the thread's own committed stack is scanned in place into preallocated storage (no heap, no locks).

#include "game_profiler.h"

#include <mutex>

#if defined(_WIN32) && !defined(__ANDROID__) && !defined(REX_UWP)  // Xbox: no thread snapshots or suspends (app family)

#include <windows.h>
#include <tlhelp32.h>
#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/logging.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace wwe2k14::prof {
namespace {

constexpr uint32_t kTickMs = 20;           // PC sample period for busy threads
constexpr uint32_t kTicksPerReport = 50;   // 1 s report window
constexpr uint32_t kMaxSampled = 12;       // threads PC-sampled per window
constexpr uint32_t kCandidates = 8;        // exe return-address candidates kept per sample
constexpr size_t kStackBytes = 32768;      // stack bytes scanned per sample
constexpr uint32_t kMaxSamplesPerThread = kTicksPerReport + 4;
constexpr double kLogPct = 5.0;            // log threads using >= 5 % of one CPU
constexpr double kSamplePct = 10.0;        // PC-sample threads that used >= 10 % of one CPU last window

std::once_flag g_once;
uintptr_t g_base = 0, g_end = 0;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool InExe(uint64_t a) { return a >= g_base && a < g_end; }

struct Sample {
  uint64_t pc;
  uint64_t cand[kCandidates];
  uint32_t ncand;
};

struct Thread {
  DWORD tid = 0;
  HANDLE h = nullptr;
  std::string name;
  uint64_t k0 = 0, u0 = 0;   // kernel / user 100 ns at the start of the window
  bool seen = false;         // present in this window's snapshot
  bool sample = false;       // PC-sampled during this window
  std::vector<Sample> samples;
};

std::string ThreadName(HANDLE h) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  std::string out;
  PWSTR w = nullptr;
  if (fn && SUCCEEDED(fn(h, &w)) && w) {
    for (const wchar_t* p = w; *p && out.size() < 40; ++p) out += (*p > 32 && *p < 127) ? char(*p) : '_';
    LocalFree(w);
  }
  return out.empty() ? "-" : out;
}

bool Times(HANDLE h, uint64_t& k, uint64_t& u) {
  FILETIME c, e, kf, uf;
  if (!GetThreadTimes(h, &c, &e, &kf, &uf)) return false;
  k = uint64_t(kf.dwHighDateTime) << 32 | kf.dwLowDateTime;
  u = uint64_t(uf.dwHighDateTime) << 32 | uf.dwLowDateTime;
  return true;
}

// Suspended window: registers + bounded in-place scan of the thread's own committed stack, nothing else.
bool SampleOne(HANDLE h, Sample& s) {
  if (SuspendThread(h) == DWORD(-1)) return false;
  CONTEXT ctx;
  ctx.ContextFlags = CONTEXT_CONTROL;
  const bool ok = GetThreadContext(h, &ctx) != 0;
  s.ncand = 0;
  if (ok) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(ctx.Rsp), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
      const uintptr_t region_end = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
      const size_t n = (std::min<size_t>)(kStackBytes, region_end - ctx.Rsp) / 8;
      const auto* w = reinterpret_cast<const uint64_t*>(ctx.Rsp);
      for (size_t i = 0; i < n && s.ncand < kCandidates; ++i) {
        if (InExe(w[i])) s.cand[s.ncand++] = w[i];
      }
    }
  }
  ResumeThread(h);
  if (!ok) return false;
  s.pc = ctx.Rip;
  return true;
}

// Outside the exe: "module+0xoff" (module name cached per base).
std::string Where(uint64_t a) {
  char b[48];
  if (InExe(a)) {
    std::snprintf(b, sizeof(b), "+0x%llx", (unsigned long long)(a - g_base));
    return b;
  }
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(a), &m) || !m) {
    return "anon";
  }
  static std::unordered_map<uintptr_t, std::string> names;
  auto it = names.find(uintptr_t(m));
  if (it == names.end()) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* slash = std::strrchr(path, '\\');
    it = names.emplace(uintptr_t(m), slash ? slash + 1 : path).first;
  }
  std::snprintf(b, sizeof(b), "+0x%llx", (unsigned long long)(a - uintptr_t(m)));
  return it->second + b;
}

template <class Map>
std::string Top(const Map& m, size_t k) {
  std::vector<std::pair<std::string, uint32_t>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::string out;
  for (size_t i = 0; i < v.size() && i < k; ++i) out += (i ? "," : "") + v[i].first + ":" + std::to_string(v[i].second);
  return out.empty() ? "-" : out;
}

bool Enabled() {
  if (const char* e = std::getenv("WWE2K14_PROFILE"); e && std::strcmp(e, "1") == 0) return true;
  wchar_t path[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  if (!n || n >= std::size(path)) return false;
  std::wstring file(path, n);
  const size_t slash = file.find_last_of(L"\\/");
  file = (slash == std::wstring::npos ? std::wstring() : file.substr(0, slash + 1)) + L"TEST-PROFILE.txt";
  return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void Refresh(std::unordered_map<DWORD, Thread>& threads, DWORD self, DWORD pid) {
  for (auto& [tid, t] : threads) t.seen = false;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    auto it = threads.find(te.th32ThreadID);
    if (it != threads.end()) {
      it->second.seen = true;
      continue;
    }
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                          FALSE, te.th32ThreadID);
    if (!h) continue;
    Thread t;
    t.tid = te.th32ThreadID;
    t.h = h;
    t.name = ThreadName(h);
    t.seen = true;
    Times(h, t.k0, t.u0);
    t.samples.reserve(kMaxSamplesPerThread);
    threads.emplace(t.tid, std::move(t));
  }
  CloseHandle(snap);
  for (auto it = threads.begin(); it != threads.end();) {  // exited threads
    if (!it->second.seen || WaitForSingleObject(it->second.h, 0) == WAIT_OBJECT_0) {
      CloseHandle(it->second.h);
      it = threads.erase(it);
    } else {
      ++it;
    }
  }
}

// ---- once-per-second speedometer: separates "processor clock" from "path to memory" ----------------------------------
// alu: fixed integer loop inside L1 (core clock only). chase: random pointer chase through 64 MiB, far beyond the 16 MiB
// L3 (DRAM latency, ns per hop). wc: write-combined memset of 1 MiB, the same kind of write as the renderer's mapped
// upload memory (bypasses the caches: fabric + DRAM bandwidth). Each takes the best of several short runs, so a thread
// that gets preempted does not read as a slow machine. Total cost about 2 ms per second, on this thread only.
constexpr size_t kChaseBytes = size_t(64) << 20;
constexpr size_t kChaseStride = 64;
constexpr size_t kWcBytes = size_t(1) << 20;

struct Meter {
  uint8_t* chase = nullptr;
  uint8_t* wc = nullptr;
  uint32_t cur = 0;
  volatile uint64_t sink = 0;

  void Init() {
    chase = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChaseBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    wc = static_cast<uint8_t*>(VirtualAlloc(nullptr, kWcBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_WRITECOMBINE));
    if (chase) {
      // Sattolo shuffle: one single cycle through every node, so the prefetchers cannot follow it.
      const uint32_t n = uint32_t(kChaseBytes / kChaseStride);
      std::vector<uint32_t> next(n);
      for (uint32_t i = 0; i < n; ++i) next[i] = i;
      uint64_t x = 0x9E3779B97F4A7C15ull;
      for (uint32_t i = n - 1; i > 0; --i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const uint32_t j = uint32_t(x % i);
        std::swap(next[i], next[j]);
      }
      for (uint32_t i = 0; i < n; ++i) *reinterpret_cast<uint32_t*>(chase + size_t(i) * kChaseStride) = next[i];
    }
    REXLOG_INFO("[meter] init chase={} wc={}", chase ? "ok" : "FAILED", wc ? "ok" : "FAILED");
  }

  double AluUs() {
    // About 1 ms of spinning first: an idle core needs that long to ramp up, and the reading should show what the
    // core can do, not how long it had been asleep.
    const uint64_t warm_until = NowNs() + 1000000;
    while (NowNs() < warm_until) sink = sink + 1;
    double best = 1e30;
    for (int r = 0; r < 5; ++r) {
      const uint64_t t0 = NowNs();
      uint64_t x = 0x123456789ABCDEFull + r;
      for (int i = 0; i < 40000; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
      }
      sink = x;
      best = (std::min)(best, double(NowNs() - t0) / 1000.0);
    }
    return best;
  }

  double ChaseNs() {
    if (!chase) return 0;
    constexpr int kHops = 2000;
    double best = 1e30;
    for (int r = 0; r < 4; ++r) {
      uint32_t i = cur;
      const uint64_t t0 = NowNs();
      for (int h = 0; h < kHops; ++h) i = *reinterpret_cast<const volatile uint32_t*>(chase + size_t(i) * kChaseStride);
      const uint64_t t1 = NowNs();
      cur = i;
      best = (std::min)(best, double(t1 - t0) / kHops);
    }
    return best;
  }

  double WcGbs() {
    if (!wc) return 0;
    double best = 0;
    for (int r = 0; r < 3; ++r) {
      const uint64_t t0 = NowNs();
      __m256i v = _mm256_set1_epi32(r);
      for (size_t o = 0; o + 32 <= kWcBytes; o += 32) _mm256_stream_si256(reinterpret_cast<__m256i*>(wc + o), v);
      _mm_sfence();
      best = (std::max)(best, double(kWcBytes) / double(NowNs() - t0));
    }
    return best;  // bytes per ns == GB/s
  }
};

// Windows' own view of the integrated GPU (the counters Task Manager reads). Optional: many drivers leave them empty.
// Only non-NVIDIA adapters are queried, so this can never wake a sleeping discrete GPU. Sizes are checked by the kernel:
// a mismatch returns an error and the query is simply dropped.
struct KmtAdapterInfo {
  UINT hAdapter;
  LUID AdapterLuid;
  ULONG NumOfSources;
  BOOL bPrecisePresentRegionsPreferred;
};
struct KmtEnum2 {
  ULONG NumAdapters;
  KmtAdapterInfo* pAdapters;
};
struct KmtQuery {
  UINT hAdapter;
  int Type;
  void* pPrivateDriverData;
  UINT PrivateDriverDataSize;
};
struct KmtDeviceIds {
  UINT32 PhysicalAdapterIndex;
  UINT32 VendorID, DeviceID, SubVendorID, SubSystemID, RevisionID, BusType;
};
struct KmtNodePerf {
  UINT32 NodeOrdinal;
  UINT32 PhysicalAdapterIndex;
  ULONGLONG Frequency, MaxFrequency, MaxFrequencyOC;
  ULONG Voltage, VoltageMax, VoltageMaxOC;
  ULONGLONG MaxTransitionLatency;
};
struct KmtAdapterPerf {
  UINT32 PhysicalAdapterIndex;
  ULONGLONG MemoryFrequency, MaxMemoryFrequency, MaxMemoryFrequencyOC, MemoryBandwidth, PCIEBandwidth;
  ULONG FanRPM, Power, Temperature;
  UCHAR PowerStateOverride;
};
constexpr int kKmtDeviceIds = 31, kKmtNodePerf = 61, kKmtAdapterPerf = 62;

struct GpuPerf {
  using EnumFn = LONG(APIENTRY*)(KmtEnum2*);
  using QueryFn = LONG(APIENTRY*)(KmtQuery*);
  QueryFn query = nullptr;
  std::vector<std::pair<UINT, uint32_t>> adapters;  // handle, vendor
  bool node_ok = true, mem_ok = true;
  uint64_t last_bw = 0;

  void Init() {
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) return;
    const auto enum2 = reinterpret_cast<EnumFn>(GetProcAddress(gdi, "D3DKMTEnumAdapters2"));
    query = reinterpret_cast<QueryFn>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    if (!enum2 || !query) {
      query = nullptr;
      return;
    }
    KmtEnum2 e{};
    if (enum2(&e) < 0 || !e.NumAdapters) return;
    std::vector<KmtAdapterInfo> list(e.NumAdapters);
    e.pAdapters = list.data();
    if (enum2(&e) < 0) return;
    for (ULONG i = 0; i < e.NumAdapters; ++i) {
      KmtDeviceIds ids{};
      KmtQuery q{list[i].hAdapter, kKmtDeviceIds, &ids, sizeof(ids)};
      const LONG st = query(&q);
      REXLOG_INFO("[gpu-clk] adapter {} status=0x{:x} vendor=0x{:x} device=0x{:x}", i, uint32_t(st), ids.VendorID, ids.DeviceID);
      if (st >= 0 && ids.VendorID != 0x10DE && ids.VendorID != 0x1414) adapters.emplace_back(list[i].hAdapter, ids.VendorID);
    }
  }

  void Log(uint32_t window) {
    if (!query) return;
    for (const auto& [h, vendor] : adapters) {
      KmtNodePerf n{};
      KmtAdapterPerf a{};
      LONG ns = -1, as = -1;
      if (node_ok) {
        KmtQuery q{h, kKmtNodePerf, &n, sizeof(n)};
        ns = query(&q);
        if (ns < 0) {
          node_ok = false;
          REXLOG_INFO("[gpu-clk] engine clock not available (status 0x{:x})", uint32_t(ns));
        }
      }
      if (mem_ok) {
        KmtQuery q{h, kKmtAdapterPerf, &a, sizeof(a)};
        as = query(&q);
        if (as < 0) {
          mem_ok = false;
          REXLOG_INFO("[gpu-clk] memory clock not available (status 0x{:x})", uint32_t(as));
        }
      }
      if (ns < 0 && as < 0) continue;
      const uint64_t bw = as >= 0 ? a.MemoryBandwidth : 0;
      const uint64_t dbw = bw >= last_bw ? bw - last_bw : 0;
      last_bw = bw;
      REXLOG_INFO("[gpu-clk] s={} vendor=0x{:x} gfx_mhz={} gfx_max_mhz={} gfx_mv={} mem_mhz={} mem_max_mhz={} mem_mb_s={} power={} temp={} fan={}",
                  window, vendor, n.Frequency / 1000000, n.MaxFrequency / 1000000, n.Voltage, a.MemoryFrequency / 1000000,
                  a.MaxMemoryFrequency / 1000000, dbw >> 20, a.Power, a.Temperature, a.FanRPM);
    }
  }
};

void LogPowerStatus(uint32_t window, const Meter&, double alu, double chase_ns, double wc) {
  SYSTEM_POWER_STATUS ps{};
  GetSystemPowerStatus(&ps);
  REXLOG_INFO("[meter] s={} alu_us={:.1f} chase_ns={:.0f} wc_gbs={:.2f} ac={} battery_pct={} charging={}", window, alu, chase_ns, wc,
              int(ps.ACLineStatus), int(ps.BatteryLifePercent), (ps.BatteryFlag & 8) ? 1 : 0);
}

void Run() {
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  Meter meter;
  meter.Init();
  GpuPerf gpu_perf;
  gpu_perf.Init();
  const char* pc_env = std::getenv("WWE2K14_PROFILE_PC");
  const bool pc_sampling = pc_env && std::strcmp(pc_env, "1") == 0;
  REXLOG_INFO("[prof] thread PC sampling {} (WWE2K14_PROFILE_PC)", pc_sampling ? "on" : "off");
  std::unordered_map<DWORD, Thread> threads;
  Refresh(threads, self, pid);
  uint64_t window_start = NowNs();
  uint32_t window = 0;
  for (;;) {
    for (uint32_t tick = 0; tick < kTicksPerReport; ++tick) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
      for (auto& [tid, t] : threads) {
        if (!t.sample || t.samples.size() >= kMaxSamplesPerThread) continue;
        Sample s;
        if (SampleOne(t.h, s)) t.samples.push_back(s);
      }
    }
    const uint64_t now = NowNs();
    const double wall_100ns = double(now - window_start) / 100.0;
    window_start = now;
    ++window;
    struct Row {
      Thread* t;
      double pct, kpct;
    };
    std::vector<Row> rows;
    double total = 0;
    for (auto& [tid, t] : threads) {
      uint64_t k = 0, u = 0;
      if (!Times(t.h, k, u)) continue;
      const double dk = double(k - t.k0), du = double(u - t.u0);
      t.k0 = k;
      t.u0 = u;
      const double pct = 100.0 * (dk + du) / wall_100ns;
      total += pct;
      rows.push_back({&t, pct, 100.0 * dk / wall_100ns});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });
    uint32_t busy = 0;
    for (const Row& r : rows) busy += r.pct >= 50.0;
    REXLOG_INFO("[prof] s={} proc_cpu_pct={:.0f} threads={} busy50={}", window, total, rows.size(), busy);
    {
      const double alu = meter.AluUs(), chase_ns = meter.ChaseNs(), wc = meter.WcGbs();
      LogPowerStatus(window, meter, alu, chase_ns, wc);
      gpu_perf.Log(window);
    }
    for (const Row& r : rows) {
      if (r.pct < kLogPct) break;
      std::string leaf = "-", stack = "-";
      if (!r.t->samples.empty()) {
        std::unordered_map<std::string, uint32_t> lm, sm;
        for (const Sample& s : r.t->samples) {
          ++lm[Where(s.pc)];
          std::string sig;
          for (uint32_t i = 0; i < s.ncand && i < 5; ++i) sig += (i ? ">" : "") + Where(s.cand[i]);
          ++sm[sig.empty() ? "-" : sig];
        }
        leaf = Top(lm, 4);
        stack = Top(sm, 2);
      }
      REXLOG_INFO("[prof] s={} tid={} name={} cpu_pct={:.0f} kernel_pct={:.0f} samples={} leaf={} stack={}", window,
                  r.t->tid, r.t->name, r.pct, r.kpct, r.t->samples.size(), leaf, stack);
    }
    // Next window: sample the busiest threads of this one.
    uint32_t chosen = 0;
    for (auto& [tid, t] : threads) {
      t.sample = false;
      t.samples.clear();
    }
    for (const Row& r : rows) {
      if (!pc_sampling || chosen >= kMaxSampled || r.pct < kSamplePct) break;
      r.t->sample = true;
      ++chosen;
    }
    Refresh(threads, self, pid);
  }
}

void StartImpl() {
  if (!Enabled()) return;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(&__ImageBase);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
  g_base = uintptr_t(&__ImageBase);
  g_end = g_base + nt->OptionalHeader.SizeOfImage;
  REXLOG_WARN("[prof] TEST profiler on (TEST-PROFILE.txt): 1 s windows, speedometer on, PC sample period {} ms when enabled, base=0x{:x}",
              kTickMs, g_base);
  std::thread([] {
    SetThreadDescription(GetCurrentThread(), L"test-profiler");
    Run();
  }).detach();
}

}  // namespace

void Start() { std::call_once(g_once, StartImpl); }

}  // namespace wwe2k14::prof

#else

namespace wwe2k14::prof {
void Start() {}
}  // namespace wwe2k14::prof

#endif
#endif  // WWE2K14_NO_TEST_TOOLS
