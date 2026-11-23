// TEST-ONLY guest write catcher (UNIV import records, 2026-11-12). Question: which game function writes the
// wrestler-id -> custom-record map u16[500] at guest 0x835DDF90 (reader sub_822C1C68, 512 = none) and its sibling at
// 0x835DE378 (sub_822C1C90)? Static search found no writer (address computed at run time).
// WWE2K14_TEST_WATCH_GUEST=<hex addr>:<bytes> makes the host pages holding that range read-only. A write faults; the
// handler logs the host RIP (module-relative; map with addr2line -e wwe2k14 -> sub_XXXXXXXX) and the guest address,
// makes the page writable again and returns, so the write then succeeds. A helper thread re-arms every 50 ms.
// Faults outside the watched pages go to the previous handler (SDK crash handler). Linux only. Not in public builds.
#include <cstdint>

namespace wwe2k14::guest_watch {
void Note(uint8_t* base);
void Find(uint8_t* base, const char* tag);  // one-shot memory search (WWE2K14_TEST_FIND), test builds only
}

#if defined(WWE2K14_NO_TEST_TOOLS) || defined(_WIN32) || defined(REX_UWP)
namespace wwe2k14::guest_watch {
void Note(uint8_t*) {}
void Find(uint8_t*, const char*) {}
}
#else
#include <atomic>
#include <cstdio>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <dlfcn.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <rex/logging.h>

namespace wwe2k14::guest_watch {
namespace {
std::atomic<bool> g_started{false};
uint8_t* g_base = nullptr;
uintptr_t g_lo = 0, g_hi = 0;          // watched host page range [lo, hi)
uint32_t g_glo = 0, g_ghi = 0;         // watched guest byte range
uintptr_t g_module = 0;
struct sigaction g_prev {};
constexpr int kMax = 256;
struct Hit { uintptr_t rip; uint32_t gaddr; };
Hit g_hits[kMax];
std::atomic<int> g_nhits{0};
std::atomic<uint32_t> g_faults{0};

void Handler(int sig, siginfo_t* si, void* uc_void) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
  if (a >= g_lo && a < g_hi) {
    auto* uc = static_cast<ucontext_t*>(uc_void);
    const uintptr_t rip = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    const uint32_t gaddr = static_cast<uint32_t>(a - reinterpret_cast<uintptr_t>(g_base));
    g_faults.fetch_add(1);
    if (gaddr >= g_glo && gaddr < g_ghi) {
      bool seen = false;
      const int n = g_nhits.load();
      for (int i = 0; i < n && i < kMax; ++i) if (g_hits[i].rip == rip) { seen = true; break; }
      if (!seen && n < kMax) { g_hits[n] = {rip, gaddr}; g_nhits.store(n + 1); }
    }
    mprotect(reinterpret_cast<void*>(g_lo), g_hi - g_lo, PROT_READ | PROT_WRITE);
    return;
  }
  if (g_prev.sa_flags & SA_SIGINFO) { if (g_prev.sa_sigaction) g_prev.sa_sigaction(sig, si, uc_void); return; }
  if (g_prev.sa_handler == SIG_DFL || g_prev.sa_handler == SIG_IGN) { signal(sig, SIG_DFL); raise(sig); return; }
  g_prev.sa_handler(sig);
}

void Loop() {
  int logged = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int n = g_nhits.load();
    for (; logged < n; ++logged)
      REXLOG_WARN("[guest-watch] write to guest {:08X} from host rip module+0x{:x} (faults so far {})", g_hits[logged].gaddr,
                  g_hits[logged].rip - g_module, g_faults.load());
    mprotect(reinterpret_cast<void*>(g_lo), g_hi - g_lo, PROT_READ);
  }
}
}  // namespace

void Note(uint8_t* base) {
  if (g_started.load(std::memory_order_relaxed)) return;
  if (g_started.exchange(true)) return;
  const char* e = std::getenv("WWE2K14_TEST_WATCH_GUEST");
  if (!e || !*e) return;
  char* end = nullptr;
  g_glo = static_cast<uint32_t>(std::strtoul(e, &end, 16));
  const uint32_t len = (end && *end == ':') ? static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 0)) : 4;
  g_ghi = g_glo + len;
  g_base = base;
  const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
  g_lo = (reinterpret_cast<uintptr_t>(base) + g_glo) & ~(page - 1);
  g_hi = (reinterpret_cast<uintptr_t>(base) + g_ghi + page - 1) & ~(page - 1);
  if (const char* d = std::getenv("WWE2K14_TEST_DUMP_GUEST")) {  // "<hex addr>:<bytes>" one-time raw dump to the log (hex)
    char* e2 = nullptr;
    const uint32_t a = static_cast<uint32_t>(std::strtoul(d, &e2, 16));
    const uint32_t n = (e2 && *e2 == ':') ? static_cast<uint32_t>(std::strtoul(e2 + 1, nullptr, 0)) : 64;
    static const char* hx = "0123456789abcdef";
    for (uint32_t off = 0; off < n; off += 64) {
      char line[129] = {};
      for (uint32_t i = 0; i < 64 && off + i < n; ++i) {
        const uint8_t b = base[a + off + i];
        line[2 * i] = hx[b >> 4]; line[2 * i + 1] = hx[b & 15];
      }
      REXLOG_WARN("[guest-dump] {:08X} {}", a + off, line);
    }
  }
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&Note), &info)) g_module = reinterpret_cast<uintptr_t>(info.dli_fbase);
  struct sigaction sa {};
  sa.sa_sigaction = Handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &g_prev);
  REXLOG_WARN("[guest-watch] TEST: watching guest {:08X}..{:08X} (host pages {:x}..{:x}), module base {:x}", g_glo, g_ghi,
              g_lo, g_hi, g_module);
  std::thread(Loop).detach();
}
void Find(uint8_t* base, const char* tag) {
  static std::atomic<int> runs{0};
  const char* e = std::getenv("WWE2K14_TEST_FIND");
  if (!e || !*e || runs.fetch_add(1) >= 3) return;
  char* end = nullptr;
  const uint32_t a = static_cast<uint32_t>(std::strtoul(e, &end, 0));
  uint32_t bs[8]; int nb = 0;
  while (end && (*end == ':' || *end == ',') && nb < 8) bs[nb++] = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0));
  FILE* mem = std::fopen("/proc/self/mem", "rb");
  if (!mem) return;
  static uint8_t buf[1 << 20];
  int found = 0;
  const uint32_t ranges[][2] = {{0x40000000u, 0x7F000000u}, {0x80000000u, 0x90000000u}, {0xA0000000u, 0xFFF00000u}};
  for (auto& r : ranges) {
    for (uint64_t g = r[0]; g < r[1] && found < 40; g += sizeof(buf)) {
      if (fseeko(mem, static_cast<off_t>(reinterpret_cast<uintptr_t>(base) + g), SEEK_SET) != 0) continue;
      const size_t n = std::fread(buf, 1, sizeof(buf), mem);
      if (n < 32) { std::clearerr(mem); continue; }
      for (size_t i = 0; i + 18 < n && found < 40; i += 2) {
        if (((buf[i] << 8) | buf[i + 1]) != a) continue;
        for (size_t k = 2; k <= 16; k += 2) {
          const uint32_t v = (buf[i + k] << 8) | buf[i + k + 1];
          for (int b = 0; b < nb; ++b)
            if (v == bs[b]) {
              char hex[97] = {};
              static const char* hx = "0123456789abcdef";
              for (int q = 0; q < 48 && i + q < n; ++q) { hex[2*q] = hx[buf[i+q] >> 4]; hex[2*q+1] = hx[buf[i+q] & 15]; }
              REXLOG_WARN("[guest-find] {} {:08X}: {} then {} at +{} | {}", tag, static_cast<uint32_t>(g + i), a, v, k, hex);
              ++found; k = 99; break;
            }
        }
      }
    }
  }
  std::fclose(mem);
  REXLOG_WARN("[guest-find] {} done, {} hits", tag, found);
}
}  // namespace wwe2k14::guest_watch
#endif
