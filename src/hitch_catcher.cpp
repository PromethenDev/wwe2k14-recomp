// Stall catcher - see hitch_catcher.h. Sampling discipline (same as the P31 PC sampler, nr_pc_sampler.h): between
// SuspendThread and ResumeThread the watchdog only reads registers and copies stack bytes into preallocated buffers
// (no heap, no locks, no unwinder - RtlLookupFunctionEntry can take a lock the suspended thread holds). Stack words are
// return-address CANDIDATES; triage.py keeps only those preceded by a call instruction in the stored exe.
#include "hitch_catcher.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <rex/logging.h>

#if defined(_WIN32) && !defined(__ANDROID__) && !defined(REX_UWP)  // Xbox: no thread snapshots or suspends (app family)
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;
#elif defined(__linux__)
#include <elf.h>
#include <link.h>

#include <cstdio>
#include <string>
#endif

namespace wwe2k14::hitch {
namespace {

std::atomic<uint64_t> g_last_present_ns{0};
// The stall being sampled: the present that ends it stores its gap here (later presents during a long round must not
// stretch the measured stall - CMP27-F logged 0.17 s "stalls" that were round time).
std::atomic<uint64_t> g_armed_ns{0}, g_armed_gap_ns{0};
std::once_flag g_once;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

#if defined(_WIN32) && !defined(__ANDROID__) && !defined(REX_UWP)  // Xbox: no thread snapshots or suspends (app family)

constexpr uint32_t kSampleMs = 10;
constexpr uint32_t kMaxSamples = 500;     // 5 s of samples per stall; the stall is still timed to its end
constexpr uint32_t kStackBytes = 65536;   // scanned per sample (driver frames are deep: 16 KB missed the exe frames)
constexpr uint32_t kCandidates = 12;      // return-address candidates kept per sample
constexpr uint32_t kMaxStalls = 200;      // per run
constexpr uint32_t kMaxThreadsLogged = 8;

uintptr_t g_base = 0, g_end = 0;

// "[hitch] build=" id: the exe's CodeView record (PDB GUID text + age), the id tools/diag/symbols.py stores under.
std::string ExeBuildId() {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(&__ImageBase);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  const auto* dbg = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(reinterpret_cast<const uint8_t*>(dos) + dir.VirtualAddress);
  for (size_t i = 0; dir.Size && i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
    if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !dbg[i].AddressOfRawData) continue;
    const uint8_t* cv = reinterpret_cast<const uint8_t*>(dos) + dbg[i].AddressOfRawData;
    if (std::memcmp(cv, "RSDS", 4) != 0) continue;
    GUID g;
    uint32_t age;
    std::memcpy(&g, cv + 4, sizeof(g));
    std::memcpy(&age, cv + 20, sizeof(age));
    char text[64];
    std::snprintf(text, sizeof(text), "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X", g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], age);
    return text;
  }
  return "unknown";
}

uint64_t ThreadCpuNs(HANDLE h) {
  FILETIME c, e, k, u;
  if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
  return ((uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime)) * 100;
}

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

struct Sample {
  uint64_t pc;
  uint64_t cand[kCandidates];
  uint32_t ncand;
};

struct Target {
  DWORD tid;
  HANDLE h;
  std::string name;
  uint64_t cpu0;
  std::vector<Sample> samples;
};


bool InExe(uint64_t a) { return a >= g_base && a < g_end; }

// Suspended window: registers + a bounded in-place scan of the thread's own committed stack (reads only; the stack
// cannot change or be freed while the thread is suspended), nothing else.
bool SampleOne(Target& t, Sample& s) {
  if (SuspendThread(t.h) == DWORD(-1)) return false;
  CONTEXT ctx;
  ctx.ContextFlags = CONTEXT_CONTROL;
  const bool ok = GetThreadContext(t.h, &ctx) != 0;
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
  ResumeThread(t.h);
  if (!ok) return false;
  s.pc = ctx.Rip;
  return true;
}

// Outside the exe (driver, Windows): "module!nearest_export+0xoff" or "module+0xoff", resolved after the stall from the
// loaded image's export table (cached per module).
std::string ExtName(uint64_t a) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(a), &m) || !m) {
    return "anon";
  }
  struct Mod {
    std::string name;
    std::vector<std::pair<uint32_t, std::string>> exports;  // sorted by rva
  };
  static std::unordered_map<uintptr_t, Mod> cache;
  auto it = cache.find(uintptr_t(m));
  if (it == cache.end()) {
    Mod mod;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* slash = std::strrchr(path, '\\');
    mod.name = slash ? slash + 1 : path;
    const auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (d.VirtualAddress && d.Size) {
      const auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + d.VirtualAddress);
      const auto* funcs = reinterpret_cast<const uint32_t*>(base + ex->AddressOfFunctions);
      const auto* names = reinterpret_cast<const uint32_t*>(base + ex->AddressOfNames);
      const auto* ords = reinterpret_cast<const uint16_t*>(base + ex->AddressOfNameOrdinals);
      for (uint32_t i = 0; i < ex->NumberOfNames; ++i) {
        const uint32_t rva = funcs[ords[i]];
        if (rva >= d.VirtualAddress && rva < d.VirtualAddress + d.Size) continue;  // forwarder
        mod.exports.emplace_back(rva, reinterpret_cast<const char*>(base + names[i]));
      }
      std::sort(mod.exports.begin(), mod.exports.end());
    }
    it = cache.emplace(uintptr_t(m), std::move(mod)).first;
  }
  const uint32_t off = uint32_t(a - uintptr_t(m));
  const auto& ex = it->second.exports;
  auto e = std::upper_bound(ex.begin(), ex.end(), std::make_pair(off, std::string("\xff")));
  char b[48];
  if (e != ex.begin() && off - std::prev(e)->first < 0x10000) {
    --e;
    std::snprintf(b, sizeof(b), "+0x%x", off - e->first);
    return it->second.name + "!" + e->second + b;
  }
  std::snprintf(b, sizeof(b), "+0x%x", off);
  return it->second.name + b;
}

bool Ignored(const std::string& name) {
  static const char* const kSkip[] = {"Audio", "XMA", "TimerQueue", "Census", "hitch", "spdlog", "freeze"};
  for (const char* k : kSkip)
    if (name.find(k) != std::string::npos) return true;
  return false;
}

std::vector<DWORD> ignored;  // thread ids skipped by name (ids are not reused while the thread lives)

void RefreshTargets(std::vector<Target>& out) {
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  for (size_t i = 0; i < out.size();) {  // drop exited threads
    if (WaitForSingleObject(out[i].h, 0) == WAIT_OBJECT_0) {
      CloseHandle(out[i].h);
      out[i] = std::move(out.back());
      out.pop_back();
    } else {
      ++i;
    }
  }
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    if (std::any_of(out.begin(), out.end(), [&](const Target& t) { return t.tid == te.th32ThreadID; })) continue;
    if (std::find(ignored.begin(), ignored.end(), te.th32ThreadID) != ignored.end()) continue;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                          FALSE, te.th32ThreadID);
    if (!h) continue;
    std::string name = ThreadName(h);
    if (Ignored(name)) {  // never sampled: cannot hold up a frame (and every suspend costs, ~1.5 ms each on the server)
      CloseHandle(h);
      ignored.push_back(te.th32ThreadID);
      continue;
    }
    Target t{te.th32ThreadID, h, std::move(name), 0, {}};
    t.samples.reserve(kMaxSamples);
    out.push_back(std::move(t));
  }
  CloseHandle(snap);
}

std::string Rva(uint64_t a) {
  char b[24];
  if (!InExe(a)) return ExtName(a);
  std::snprintf(b, sizeof(b), "+0x%llx", (unsigned long long)(a - g_base));
  return b;
}

// Top-3 values of a histogram as "key:count,..." (keys already formatted).
template <class Map>
std::string Top(const Map& m, size_t k) {
  std::vector<std::pair<std::string, uint32_t>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::string out;
  for (size_t i = 0; i < v.size() && i < k; ++i) out += (i ? "," : "") + v[i].first + ":" + std::to_string(v[i].second);
  return out.empty() ? "-" : out;
}

void Report(uint32_t n, uint64_t gap_ms, std::vector<Target>& targets, uint32_t rounds, uint32_t ok, uint32_t fails,
            DWORD first_error, uint64_t round_us) {
  struct Row {
    Target* t;
    uint64_t cpu_ms;
    bool in_exe;
  };
  std::vector<Row> rows;
  for (Target& t : targets) {
    const uint64_t cpu = ThreadCpuNs(t.h);
    bool in_exe = false;
    for (const Sample& s : t.samples) in_exe |= InExe(s.pc) || s.ncand > 0;
    rows.push_back({&t, cpu > t.cpu0 ? (cpu - t.cpu0) / 1000000 : 0, in_exe});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cpu_ms > b.cpu_ms; });
  REXLOG_WARN("[hitch] n={} ms={} threads={} rounds={} samples={} failed={} first_error={} round_us_max={}", n, gap_ms,
              targets.size(), rounds, ok, fails, first_error, round_us);
  uint32_t logged = 0;
  for (const Row& r : rows) {
    if (logged >= kMaxThreadsLogged) break;
    // busy threads (>= 20 % of the stall on CPU) and threads parked inside game/renderer code
    if (r.cpu_ms * 5 < gap_ms && !r.in_exe) continue;
    if (r.t->samples.empty()) continue;
    std::unordered_map<std::string, uint32_t> leaf, stack;
    for (const Sample& s : r.t->samples) {
      ++leaf[Rva(s.pc)];
      std::string sig;
      for (uint32_t i = 0; i < s.ncand && i < 6; ++i) sig += (i ? ">" : "") + Rva(s.cand[i]);
      ++stack[sig.empty() ? "-" : sig];
    }
    REXLOG_WARN("[hitch] n={} tid={} name={} cpu_ms={} samples={} leaf={} stack={}", n, r.t->tid, r.t->name,
                r.cpu_ms, r.t->samples.size(), Top(leaf, 3), Top(stack, 2));
    ++logged;
  }
}

// Sampling starts once a frame is kStartMs late (so the stall's beginning is seen); a record is written only when the
// gap reaches the threshold.
constexpr uint64_t kStartMs = 50;

void Watchdog(uint64_t threshold_ns) {
  uint32_t stalls = 0, fails = 0;
  DWORD first_error = 0;
  std::vector<Target> targets;
  RefreshTargets(targets);
  uint64_t refreshed = NowNs();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const uint64_t last = g_last_present_ns.load(std::memory_order_acquire);
    const uint64_t now = NowNs();
    if (!last || now - last < kStartMs * 1000000) {
      if (now - refreshed > 1000000000ull) {
        RefreshTargets(targets);
        refreshed = NowNs();
      }
      continue;
    }
    if (stalls >= kMaxStalls) {
      if (stalls++ == kMaxStalls) REXLOG_WARN("[hitch] limit of {} stalls reached; catcher stops recording", kMaxStalls);
      while (g_last_present_ns.load(std::memory_order_acquire) == last) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    g_armed_gap_ns.store(0, std::memory_order_relaxed);
    g_armed_ns.store(last, std::memory_order_release);
    if (g_last_present_ns.load(std::memory_order_acquire) != last) continue;  // raced: the frame just arrived
    for (Target& t : targets) {
      t.samples.clear();
      t.cpu0 = ThreadCpuNs(t.h);
    }
    uint64_t round_ns = 0;
    uint32_t rounds = 0, ok = 0;
    fails = 0;
    first_error = 0;
    do {
      if (rounds < kMaxSamples) {
        const uint64_t r0 = NowNs();
        for (Target& t : targets) {
          Sample smp;
          if (SampleOne(t, smp)) {
            t.samples.push_back(smp);
            ++ok;
          } else if (!fails++) {
            first_error = GetLastError();
          }
        }
        round_ns = (std::max)(round_ns, NowNs() - r0);
        ++rounds;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSampleMs));
    } while (g_last_present_ns.load(std::memory_order_acquire) == last);
    uint64_t gap_ns = g_armed_gap_ns.load(std::memory_order_acquire);
    g_armed_ns.store(0, std::memory_order_relaxed);
    if (!gap_ns) gap_ns = g_last_present_ns.load(std::memory_order_acquire) - last;  // fallback: should not happen
    if (gap_ns < threshold_ns) continue;
    Report(++stalls, gap_ns / 1000000, targets, rounds, ok, fails, first_error, round_ns / 1000);
  }
}

void StartImpl() {
  HMODULE self = reinterpret_cast<HMODULE>(&__ImageBase);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
  g_base = uintptr_t(self);
  g_end = g_base + nt->OptionalHeader.SizeOfImage;
  bool on = true;
#ifdef WWE2K14_NO_TEST_TOOLS
  on = false;  // public builds: only the build id line
#endif
  if (const char* e = std::getenv("WWE2K14_HITCH"); e && std::strcmp(e, "0") == 0) on = false;
  uint32_t ms = 150;
  if (const char* e = std::getenv("WWE2K14_HITCH_MS"); e && std::atoi(e) > 0) ms = uint32_t(std::atoi(e));
  REXLOG_INFO("[hitch] build={} base=0x{:x} catcher={} threshold_ms={}", ExeBuildId(), g_base, on ? "on" : "off", ms);
  if (!on) return;
  std::thread([ms] {
    SetThreadDescription(GetCurrentThread(), L"hitch-catcher");
    Watchdog(uint64_t(ms) * 1000000);
  }).detach();
}

#else
void StartImpl() {}
#endif

}  // namespace

#if defined(_WIN32) && !defined(__ANDROID__) && !defined(REX_UWP)  // Xbox: no thread snapshots or suspends (app family)
std::string BuildId() { return ExeBuildId(); }
#elif defined(__linux__)
// Linux/Android: the main executable's GNU build-id note (hex), as `file` prints it.
std::string BuildId() {
  std::string id;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) -> int {
        auto* out = static_cast<std::string*>(data);
        for (int i = 0; i < info->dlpi_phnum; ++i) {
          const ElfW(Phdr)& ph = info->dlpi_phdr[i];
          if (ph.p_type != PT_NOTE) continue;
          const uint8_t* p = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
          const uint8_t* end = p + ph.p_memsz;
          while (p + sizeof(ElfW(Nhdr)) <= end) {
            const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(p);
            const uint8_t* name = p + sizeof(ElfW(Nhdr));
            const uint8_t* desc = name + ((note->n_namesz + 3) & ~3u);
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && std::memcmp(name, "GNU", 4) == 0 &&
                desc + note->n_descsz <= end) {
              char hex[3];
              for (uint32_t b = 0; b < note->n_descsz; ++b) {
                std::snprintf(hex, sizeof(hex), "%02x", desc[b]);
                *out += hex;
              }
              return 1;
            }
            p = desc + ((note->n_descsz + 3) & ~3u);
          }
        }
        return 1;  // only the first object (the main executable) is looked at
      },
      &id);
  return id.empty() ? "unknown" : id;
}
#else
std::string BuildId() { return "unknown"; }
#endif

void Start() { std::call_once(g_once, StartImpl); }

void NotePresent() {
  const uint64_t now = NowNs();
  const uint64_t prev = g_last_present_ns.exchange(now, std::memory_order_acq_rel);
  if (prev && prev == g_armed_ns.load(std::memory_order_relaxed)) g_armed_gap_ns.store(now - prev, std::memory_order_release);
}

}  // namespace wwe2k14::hitch
