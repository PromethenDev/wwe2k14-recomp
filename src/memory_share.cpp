// Shared-memory safety net (owner 2026-11-17: "let areas lend memory to each other"; after #154 CAS null VB, #122, #125).
// "N:line" = generated/wwe2k14_recomp.N.cpp. Default ON; WWE2K14_MEM_SHARE=0 switches it off (retail behaviour).
//
// The game splits memory into 56 fixed areas and fails a request when its own area is full, even while others are
// empty; several callers then use the null result (#154 VB lock 85:6648, #125 96-byte object). This file retries a
// failed request ONCE in a lender area, without changing any successful request:
//
// 1. Graphics memory (physical, GPU-visible). sub_8225F6C0(r3 size, r4 tag) (0:37410) picks window W = the pool record's
//    data word (0x836EE480 + 40*pool + 8, pool = tag byte 1) or, when that is -1, the calling thread's heap
//    (sub_82C31100). Window W = 60-byte descriptor at 0x836F0C80 + 60*W (+4 low, +8 high physical address). It asks
//    the kernel (sub_82261740 -> MmAllocatePhysicalMemoryEx, 0:42451) for pages inside [low, high] only, so a window
//    is a RANGE, not a reservation. Freeing is by address (sub_8225F7E8 -> sub_822617B8 MmFreePhysicalMemory, 0:42526),
//    never by window, and sub_8225F600 (window lookup in the free path) only reads. So a block taken from another
//    window's range frees correctly. Retry ranges: window 55 (the leftover), then the span of windows 14..55.
//    Post-steps copied from sub_8225F6C0 0:37528-37560: align = sub_8225EAF0(tag), protect = *(0x835DDB40 +
//    4*((tag >> 28) & 3)), and when tag & 0x40000000 the block is zeroed (sub_82C7CE20 if (tag >> 28) & 3 == 1, else
//    sub_82261A58) over MmQueryAllocationSize (sub_822617B0) bytes.
// 2. General memory (block heaps). sub_82C31518(r3 size, r4 flags) (80:~26200, 3194 call sites, the game's new),
//    sub_82C32DE8 (small blocks) and sub_82C32F58(r3 size, r4 align, r5) (aligned, 80:30008) all take the heap from
//    sub_82C31100 and lock it themselves; each stores the heap index in the block header (+12, block = header+16) and
//    the free path reads it back from the block (sub_82C316D8 80:26512), so a block from heap 55 frees into heap 55.
//    Retry: the whole call again with sub_82C31100 answering 55 on this host thread (same mechanism as the #125 fix,
//    lane A gh125-spare-1/2 PASS). sub_82C32DE8 called inside sub_82C32F58 is not retried on its own, because
//    sub_82C31A48 (aligned post-step) works on the thread's heap; the outer sub_82C32F58 retry covers it.
//
// Lifetime rule (review 11-14 risk A): at a layout change the game rebuilds heaps/windows 14 and up
// (sub_82C322B0 r3=14, sub_8225F480 arr,14). Only a request whose OWN area is >= 14 (mode-scoped, freed before the
// rebuild) may borrow, and only from areas >= 14. Requests from areas 0..13 keep the retail answer.
// Graphics loans are tracked (address -> size, removed in the free wrapper) and capped at kPhysNetCap bytes
// outstanding, so a caller that allocates until failure cannot drain the other windows.
//
// TEST-ONLY (WWE2K14_TEST_TOOLS builds): WWE2K14_TEST_MEM_SHARE=1 logs how full every window / heap is every 20 s
// (with the lowest free seen); WWE2K14_TEST_MEM_SHARE_EVERY=N sends every Nth eligible request straight to the
// lender (exercises loan + free in an ordinary run).
//
// T2 MEM-NEVERFAIL (2026-11-18):
// - Caller-named fail log (always on, failure path only): a request that still ends with 0 (own area full and no loan,
//   sharing off, lender full, cap reached) writes ONE "[mem-fail]" line per (kind, caller) - again at the 2nd, 4th,
//   8th ... failure of that caller - with the guest call chain (mem_fail_log.h). The chain's addresses are the return
//   addresses that out/analysis/alloc-null/sites.jsonl rows map to (alloc_fail_map.py: ret = the `ctx.lr = ...` line
//   after the row's bl), so every remaining failure names its scan row and class (DEREF / PASS-DEREF / checked ...).
// - Graphics windows 0-13 (WWE2K14_MEM_SHARE_LOW, default 0): the 14-55 limit above came from the block-heap lifetime
//   rule (a heap rebuild formats live blocks). Graphics windows are not formatted: the rebuild sub_8225F480 (0:37060)
//   only rewrites window descriptors and clears their tracking words (sub_8225E958; the alloc/free path F6C0/F7E8 never
//   reads them), frees no page, and blocks are freed by address (sub_8225F7E8; sub_8225F600's window answer is
//   discarded, its only two callers 0:37705/37786). So a long-lived loan cannot be corrupted by a rebuild; the
//   remaining cost of lending it from windows 14-54 is a block left inside the next layout's windows. LOW=1 therefore
//   lends windows 0-13 from the physical memory BELOW the window pool (no window, no heap, never rebuilt;
//   mem_fail_log.h BelowPool), and also uses it as the last resort for windows 14-55; capped at 24 MB of loans and a 16
//   MB free reserve. LOW=2 = CHECK: a window 0-13 failure takes the below-pool block, logs it, frees it and keeps the
//   retail answer 0.
// - Lender full: heap 55's region is 256 MB of the 0x40000000 virtual heap (0x40000000-0x7EFFFFFF, Memory::LookupHeap),
//   placed top-down; graphics loans must stay inside the 512 MB physical memory (GPU addresses are 29-bit), so their
//   ceiling is the free room of the windows + below-pool memory, whatever the PC has. The reservation logs the
//   virtual heap's free room once ("[mem-share] virtual heap ...") as the evidence for growing the region.
#include "memory_share.h"
#include "mem_fail_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace wwe2k14::mem {
thread_local int t_force_heap = -1;
GuestFn g_inner_new = nullptr;
GuestFn g_inner_small = nullptr;
}

extern "C" {
PPC_FUNC(__imp__sub_82C31100);
PPC_FUNC(sub_82C31100);
PPC_FUNC(__imp__sub_8225F6C0);
PPC_FUNC(sub_8225F6C0);
PPC_FUNC(__imp__sub_822617B8);
PPC_FUNC(sub_822617B8);
PPC_FUNC(__imp__sub_82C31518);
PPC_FUNC(sub_82C31518);
PPC_FUNC(__imp__sub_82C32DE8);
PPC_FUNC(sub_82C32DE8);
PPC_FUNC(__imp__sub_82C32F58);
PPC_FUNC(sub_82C32F58);
PPC_FUNC(__imp__sub_82C31B38);
PPC_FUNC(sub_82C31B38);
PPC_FUNC(__imp__sub_82C322B0);
PPC_FUNC(__imp__sub_82C314C8);
PPC_FUNC(sub_82C322B0);
PPC_FUNC(__imp__RtlEnterCriticalSection);
PPC_FUNC(__imp__RtlLeaveCriticalSection);
PPC_FUNC(__imp__sub_8225EAF0);
PPC_FUNC(__imp__sub_82261740);
PPC_FUNC(__imp__sub_822617B0);
PPC_FUNC(__imp__sub_82C7CE20);
PPC_FUNC(__imp__sub_82261A58);
}

namespace {
using wwe2k14::mem::t_force_heap;

constexpr uint32_t kAreas = 56;
constexpr uint32_t kSpare = 55;
constexpr uint32_t kFirstModeArea = 14;
constexpr uint32_t kPoolRecords = 0x836EE480;  // lis r11,-31889 + addi -7040 (0:37424); 40 bytes per pool, +8 data window
constexpr uint32_t kPoolRecordStride = 40;
constexpr uint32_t kWinDesc = 0x836F0C80;      // lis -31889 + addi 3200 (0:37436); 60 bytes, +4 low, +8 high
constexpr uint32_t kWinStride = 60;
constexpr uint32_t kProtectTable = 0x835DDB40;  // lis r10,-31906 + addi -9408 (0:37440)
constexpr uint32_t kHeapTablePtr = 0x83930034;  // lis r9,-31853 + lwz 52 (sub_82C31518); 36 bytes per heap
constexpr uint32_t kHeapEntry = 36;
constexpr uint64_t kPhysNetCap = 64ull << 20;

bool Enabled() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_MEM_SHARE");
    const bool v = !(e && e[0] == '0');
    REXLOG_INFO("[mem-share] {}", v ? "on: a full memory area may borrow from the spare areas"
                                    : "off (WWE2K14_MEM_SHARE=0): retail behaviour");
    return v;
  }();
  return on;
}

#ifdef WWE2K14_NO_TEST_TOOLS
constexpr bool ReportOn() { return false; }
constexpr uint64_t TestEvery() { return 0; }
constexpr bool TestFull(bool, uint32_t) { return false; }
#else
// WWE2K14_TEST_MEM_SHARE_FULL=g14,g34,h4,h54: treat these graphics windows (g) / general heaps (h) as full, so every
// request from them borrows while all other areas behave normally (fault injection; test builds only).
bool TestFull(bool graphics, uint32_t area) {
  static const std::pair<uint64_t, uint64_t> masks = [] {
    uint64_t g = 0, h = 0;
    const char* e = std::getenv("WWE2K14_TEST_MEM_SHARE_FULL");
    for (const char* p = e; p && *p;) {
      const char k = *p++;
      const unsigned long v = std::strtoul(p, const_cast<char**>(&p), 10);
      if (v < 64) (k == 'g' ? g : h) |= 1ull << v;
      while (*p == ',' || *p == ' ') ++p;
    }
    if (g | h) REXLOG_INFO("[mem-share] TEST: areas treated as full: graphics {:016X} general {:016X}", g, h);
    return std::make_pair(g, h);
  }();
  return area < 64 && (((graphics ? masks.first : masks.second) >> area) & 1);
}
bool ReportOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_TEST_MEM_SHARE");
    return e && e[0] == '1';
  }();
  return on;
}
uint64_t TestEvery() {
  static const uint64_t n = [] {
    const char* e = std::getenv("WWE2K14_TEST_MEM_SHARE_EVERY");
    return e ? std::strtoull(e, nullptr, 10) : 0ull;
  }();
  return n;
}
#endif

struct Scan {
  uint64_t free_bytes = 0, largest = 0;
  bool ok = false;
};
Scan ScanHeapRange(rex::memory::BaseHeap* heap, uint32_t lo, uint32_t hi) {
  Scan s;
  if (!heap || hi <= lo) return s;
  uint64_t a = lo;
  for (int i = 0; a < hi && i < 200000; ++i) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(uint32_t(a), &info) || info.region_size == 0) return s;
    const uint64_t len = std::min<uint64_t>(info.region_size, hi - a);
    if (info.state == 0) {
      s.free_bytes += len;
      s.largest = std::max(s.largest, len);
    }
    a += len;
  }
  s.ok = a >= hi;
  return s;
}
rex::memory::Memory* GuestMemory() {
  auto* ks = rex::system::kernel_state();
  return ks ? ks->memory() : nullptr;
}
Scan ScanPhysical(uint32_t lo, uint32_t hi) {
  auto* mem = GuestMemory();
  return ScanHeapRange(mem ? mem->GetPhysicalHeap() : nullptr, lo, hi);
}

// WWE2K14_MEM_SHARE_LOW: 1 (default since night 11-18) = lend graphics windows 0-13 from below the window pool (and last
// resort for 14-55); 0 = retail answer; 2 = CHECK (take + free a below-pool block on a failure, log, keep the retail
// answer). Evidence for the default: memnf-1 (match 600 s) and memsoak-caw-full (CAS) with six areas forced full: no crash,
// 10 loans each; realistic soak (chamber, caw, creations, thirty, universe): 0 loans, 0 failures (it only acts on a failure).
int LowMode() {
  static const int m = [] {
    const char* e = std::getenv("WWE2K14_MEM_SHARE_LOW");
    const int v = (e && *e) ? ((e[0] == '1' || e[0] == '2') ? e[0] - '0' : 0) : 1;
    if (v) REXLOG_INFO("[mem-share] LOW={}: graphics windows 0-13 {}", v,
                       v == 1 ? "borrow from the memory below the window pool (also last resort for 14-55)"
                              : "CHECK: below-pool block taken + freed on a failure, retail answer kept");
    return v;
  }();
  return m;
}

// ---------------------------------------------------------------- caller-named fail log (failure path only)
namespace fl = wwe2k14::mem::faillog;
fl::Registry g_fail_real, g_fail_test;
constexpr size_t kTestNoteCallers = 64;  // test notes (forced loans) log at most this many distinct callers

bool GuestReadable(uint32_t addr, uint32_t bytes) {
  auto* mem = GuestMemory();
  auto* heap = mem ? mem->LookupHeap(addr) : nullptr;
  return heap && addr + bytes > addr && heap->QueryRangeAccess(addr, addr + bytes - 1) != rex::memory::PageAccess::kNoAccess;
}

uint32_t KindId(const char* kind) {
  uint32_t h = 2166136261u;
  for (const char* p = kind; *p; ++p) h = (h ^ uint8_t(*p)) * 16777619u;
  return h;
}

}  // namespace

namespace wwe2k14::mem {
// kind = "general new" / "graphics" / "pool96" ...; area = own heap / window; why = reason; lr / sp = ctx.lr / ctx.r1
// at the hooked function's entry. test = a forced loan (fault injection), logged as a chain check, not a failure.
void NoteFail(uint8_t* base, const char* kind, uint32_t size, uint32_t area, const char* why, uint32_t lr, uint32_t sp,
              bool test) {
  fl::Registry& reg = test ? g_fail_test : g_fail_real;
  if (test && reg.distinct() >= kTestNoteCallers) return;
  const uint32_t n = reg.Hit(KindId(kind), lr);
  if (!fl::LogThisHit(n)) return;
  const fl::Chain c = fl::Walk(
      lr, sp, [base](uint32_t a) { return PPC_LOAD_U32(a); },
      [](uint32_t a, uint32_t b) { return GuestReadable(a, b); });
  char chain[16 * fl::kFrames];
  fl::FormatChain(c, chain, sizeof(chain));
  if (test)
    REXLOG_INFO("[mem-fail] TEST chain check ({} {} B, area {}: {}) chain {}", kind, size, area, why, chain);
  else
    REXLOG_WARN("[mem-fail] {} {} B for area {} failed: {} | caller {:08X} chain {} | caller failures {}", kind, size, area,
                why, lr, chain, n);
}
}  // namespace wwe2k14::mem

namespace {
using wwe2k14::mem::NoteFail;

bool Pow2OrSmall(uint32_t n) { return n <= 4 || (n & (n - 1)) == 0; }

// ---------------------------------------------------------------- graphics loans
std::mutex g_loan_mu;
struct Loan {
  uint32_t bytes;
  bool low;  // below-pool loan (counted in g_low_net, not g_loan_net)
};
std::unordered_map<uint32_t, Loan> g_loans;  // physical address (low 29 bits) -> loan
std::atomic<uint64_t> g_loan_net{0};
std::atomic<uint64_t> g_low_net{0};
std::atomic<uint32_t> g_low_count{0};
std::atomic<uint32_t> g_loan_count{0};
std::atomic<uint64_t> g_phys_calls{0};

struct Range {
  uint32_t lo = 0, hi = 0;
  bool ok() const { return hi > lo; }
};
Range Window(uint8_t* base, uint32_t w) {
  const uint32_t d = kWinDesc + kWinStride * w;
  return {PPC_LOAD_U32(d + 4), PPC_LOAD_U32(d + 8)};
}
Range ModeSpan(uint8_t* base) {
  Range r{0xFFFFFFFFu, 0};
  for (uint32_t w = kFirstModeArea; w < kAreas; ++w) {
    const Range x = Window(base, w);
    if (!x.ok()) continue;
    r.lo = std::min(r.lo, x.lo);
    r.hi = std::max(r.hi, x.hi);
  }
  if (r.hi <= r.lo) return Range{};
  for (uint32_t w = 0; w < kFirstModeArea; ++w) {  // a long-lived window inside the span would break the lifetime rule
    const Range x = Window(base, w);
    if (x.ok() && x.lo < r.hi && x.hi > r.lo) return Range{};
  }
  return r;
}

uint32_t ThreadHeap(PPCContext& ctx, uint8_t* base) {
  __imp__sub_82C31100(ctx, base);  // read-only lookup in the 24-slot thread -> heap map (80:25646)
  return ctx.r3.u32;
}

uint32_t OwnWindow(PPCContext& ctx, uint8_t* base, uint32_t tag) {
  const uint32_t word = PPC_LOAD_U32(kPoolRecords + kPoolRecordStride * ((tag >> 16) & 0xFFu) + 8);
  return word == 0xFFFFFFFFu ? ThreadHeap(ctx, base) : word;
}

// sub_8225F6C0's allocation + post-steps with another physical range. Returns the guest address or 0.
uint32_t PhysAllocIn(PPCContext& ctx, uint8_t* base, uint32_t size, uint32_t tag, Range r, bool low = false) {
  ctx.r3.u64 = tag;
  __imp__sub_8225EAF0(ctx, base);
  const uint32_t align = ctx.r3.u32;
  const uint32_t protect = PPC_LOAD_U32(kProtectTable + 4 * ((tag >> 28) & 3u));  // rlwinm r9,r30,6,28,29 = ((tag>>28)&3)*4
  ctx.r3.u64 = size;
  ctx.r4.u64 = r.lo;
  ctx.r5.u64 = r.hi;
  ctx.r6.u64 = align;
  ctx.r7.u64 = protect;
  __imp__sub_82261740(ctx, base);
  const uint32_t ptr = ctx.r3.u32;
  if (ptr == 0) return 0;
  if ((ptr & 0x1FFFFFFFu) < r.lo || (ptr & 0x1FFFFFFFu) >= r.hi) {  // same sanity check as 0:37528 (low bound)
    ctx.r3.u64 = ptr;
    __imp__sub_822617B8(ctx, base);
    return 0;
  }
  ctx.r3.u64 = ptr;
  __imp__sub_822617B0(ctx, base);
  const uint32_t got = ctx.r3.u32;
  if (tag & 0x40000000u) {
    ctx.r3.u64 = ptr;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = got;
    if (((tag >> 28) & 3u) == 1)
      __imp__sub_82C7CE20(ctx, base);
    else
      __imp__sub_82261A58(ctx, base);
  }
  {
    std::lock_guard<std::mutex> lk(g_loan_mu);
    g_loans[ptr & 0x1FFFFFFFu] = Loan{got, low};
  }
  (low ? g_low_net : g_loan_net).fetch_add(got);
  return ptr;
}

Range BelowPoolRange(uint8_t* base) {
  uint32_t lo[kAreas], hi[kAreas];
  for (uint32_t w = 0; w < kAreas; ++w) {
    const Range x = Window(base, w);
    lo[w] = x.lo;
    hi[w] = x.hi;
  }
  const fl::Span s = fl::BelowPool(lo, hi, kAreas);
  return {s.lo, s.hi};
}

// Below-pool loan (LOW=1/2). Returns the guest address or 0 with *why set.
uint32_t LowLend(PPCContext& ctx, uint8_t* base, uint32_t size, uint32_t tag, const char** why) {
  const Range r = BelowPoolRange(base);
  if (!r.ok()) {
    *why = "no below-pool span (window descriptors empty)";
    return 0;
  }
  const Scan sc = ScanPhysical(r.lo, r.hi);
  if (!sc.ok) {
    *why = "below-pool scan failed";
    return 0;
  }
  if ((*why = fl::LowRefusal(sc.free_bytes, sc.largest, size, g_low_net.load()))) return 0;
  const uint32_t p = PhysAllocIn(ctx, base, size, tag, r, true);
  if (!p) *why = "kernel refused the below-pool block";
  return p;
}

// Drop a loan record (the block itself is freed by the caller).
void ForgetLoan(uint32_t ptr) {
  std::lock_guard<std::mutex> lk(g_loan_mu);
  auto it = g_loans.find(ptr & 0x1FFFFFFFu);
  if (it == g_loans.end()) return;
  (it->second.low ? g_low_net : g_loan_net).fetch_sub(it->second.bytes);
  g_loans.erase(it);
}

// Returns the loan or 0. own = the window that failed.
uint32_t PhysLend(PPCContext& ctx, uint8_t* base, uint32_t size, uint32_t tag, uint32_t own, uint32_t caller,
                  uint32_t sp, bool test) {
  const char* why = nullptr;
  if (own >= kAreas) why = "window index out of range";
  else if (own < kFirstModeArea) {
    // Long-lived window (0-13): only the below-pool memory may serve it (see the T2 note in the header).
    const int low = LowMode();
    if (low == 0) {
      why = "window below 14 (long-lived; WWE2K14_MEM_SHARE_LOW=1 lends these from below the window pool)";
    } else {
      const uint32_t p = LowLend(ctx, base, size, tag, &why);
      if (p && low == 2) {
        static std::atomic<uint32_t> s_checks{0};
        const uint32_t k = s_checks.fetch_add(1) + 1;
        if (Pow2OrSmall(k))
          REXLOG_WARN("[mem-share] CHECK low: {} B for window {} could be lent from below the pool at {:08X} (caller "
                      "{:08X}; check {}{}); freed, retail answer kept", size, own, p, caller, k,
                      test ? ", test switch" : "");
        ForgetLoan(p);
        ctx.r3.u64 = p;
        __imp__sub_822617B8(ctx, base);
        why = "LOW=2 CHECK keeps the retail answer";
      } else if (p) {
        const uint32_t n = g_low_count.fetch_add(1) + 1;
        if (Pow2OrSmall(n))
          REXLOG_WARN("[mem-share] graphics: {} B for window {} lent from below the window pool at {:08X} (caller "
                      "{:08X}; low loan {}, {} KiB out{})", size, own, p, caller, n, g_low_net.load() >> 10,
                      test ? ", test switch" : "");
        return p;
      }
    }
  }
  if (why) {
    if (!test) NoteFail(base, "graphics", size, own, why, caller, sp, false);
    return 0;
  }
  const bool capped = g_loan_net.load() + size > kPhysNetCap;
  // Lender = the mode window with the most room (memshare-1: window 55 is 100 % used by the game itself in a match,
  // window 54 had ~31 MB free). The last good lender is tried first; scan only when it cannot serve the request.
  static std::atomic<uint32_t> s_last{0};
  char fromBuf[16] = "55";
  const char* from = fromBuf;
  uint32_t ptr = 0;
  const uint32_t last = s_last.load();
  if (!capped && last >= kFirstModeArea && last < kSpare && last != own) {
    ptr = PhysAllocIn(ctx, base, size, tag, Window(base, last));
    if (ptr) std::snprintf(fromBuf, sizeof(fromBuf), "%u", last);
  }
  if (!ptr && !capped) {
    uint32_t best = 0;
    uint64_t best_free = 0;
    for (uint32_t w = kFirstModeArea; w < kSpare; ++w) {
      if (w == own) continue;
      const Range r = Window(base, w);
      if (!r.ok() || (r.hi - r.lo) < size) continue;
      const Scan sc = ScanPhysical(r.lo, r.hi);
      if (sc.ok && sc.largest >= size && sc.free_bytes > best_free) {
        best = w;
        best_free = sc.free_bytes;
      }
    }
    if (best) {
      ptr = PhysAllocIn(ctx, base, size, tag, Window(base, best));
      if (ptr) {
        s_last.store(best);
        std::snprintf(fromBuf, sizeof(fromBuf), "%u", best);
      }
    }
  }
  const Range spare = Window(base, kSpare);
  if (!ptr && !capped && own != kSpare && spare.ok()) {
    ptr = PhysAllocIn(ctx, base, size, tag, spare);
    from = "55";
  }
  if (!ptr && !capped) {
    const Range span = ModeSpan(base);
    if (span.ok()) {
      ptr = PhysAllocIn(ctx, base, size, tag, span);
      from = "14-55";
    }
  }
  if (!ptr && LowMode() == 1) {  // last resort: below the window pool (never rebuilt, so any window may use it)
    const char* low_why = nullptr;
    const uint32_t p = LowLend(ctx, base, size, tag, &low_why);
    if (p) {
      const uint32_t n = g_low_count.fetch_add(1) + 1;
      if (Pow2OrSmall(n))
        REXLOG_WARN("[mem-share] graphics: {} B for window {} lent from below the window pool at {:08X} (caller {:08X}; "
                    "low loan {}, {} KiB out{})", size, own, p, caller, n, g_low_net.load() >> 10,
                    test ? ", test switch" : "");
      return p;
    }
  }
  if (!ptr) {
    if (!test)
      NoteFail(base, "graphics", size, own, capped ? "loan cap (64 MB) reached" : "windows 14-55 full too", caller, sp,
               false);
    return 0;
  }
  const uint32_t n = g_loan_count.fetch_add(1) + 1;
  if (Pow2OrSmall(n))
    REXLOG_WARN("[mem-share] graphics: {} B for window {} lent from window {} at {:08X} (caller {:08X}; loan {}, {} KiB "
                "out{})",
                size, own, from, ptr, caller, n, g_loan_net.load() >> 10, test ? ", test switch" : "");
  return ptr;
}

// ---------------------------------------------------------------- general (block heap) loans
thread_local int t_f58_depth = 0;
std::atomic<uint32_t> g_heap_loans{0};
std::atomic<uint64_t> g_heap_calls{0};
std::atomic<uint32_t> g_sampled_heap[64];  // test switch: own heap of each sampled call (why loans did / did not happen)

// ---------------------------------------------------------------- step 2: heap 55 outside the 512 MB
// Heap 55 is never used by the game itself (15.2 of 15.3 MB free in memshare-1/-2; "never used in 23 traces" #122).
// Its 36-byte table entry holds +0 start, +4 end; the free list lives inside the blocks (sub_82C31518 80:26259,
// free sub_82C32048 reads only +0 of the heap's entry and the blocks), so pointing +0/+4 at a bigger range and
// formatting it like the rebuild does (sub_82C322B0 80:28226 loc_82C323B4: block 0 = {1,0,1,0}, block 1 = {total,0,
// total,0}, total = (end-start)/16) gives a 256 MB heap 55 in guest virtual memory (the 0x40000000 heap; host RAM, not
// the console's 512 MB physical memory). The region is formatted ONCE and never again: before each rebuild
// (sub_82C322B0) heap 55's original range is put back so the game formats that one, and the big range is re-pointed
// after. Loans therefore survive layout changes, so with the region active every heap (also 0..13) may borrow.
// Off: WWE2K14_MEM_SHARE_BIG=0 (step-1 behaviour: heap 55 in place, only heaps >= 14 borrow).
constexpr uint32_t kHeapLocks = 0x8392F900;  // lis -31853 + addi -1792 (80:30020); 28-byte critical section per heap
constexpr uint32_t kBigSize = 256u << 20;
std::atomic<uint32_t> g_big{0};      // guest address of the region (0 = none yet)
std::atomic<bool> g_big_active{false};
bool g_big_formatted = false;         // guarded by heap 55's critical section
uint32_t g_orig55_lo = 0, g_orig55_hi = 0;

bool BigEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_MEM_SHARE_BIG");
    return !(e && e[0] == '0');
  }();
  return on;
}

uint32_t BigRegion() {
  uint32_t r = g_big.load();
  if (r) return r;
  static std::mutex mu;
  std::lock_guard<std::mutex> lk(mu);
  if ((r = g_big.load())) return r;
  auto* ks = rex::system::kernel_state();
  auto* mem = ks ? ks->memory() : nullptr;
  auto* heap = mem ? mem->LookupHeapByType(false, 64 * 1024) : nullptr;
  uint32_t addr = 0;
  // Lender-full evidence (T2 c): free room of the 0x40000000 virtual heap (Memory::LookupHeap: 0x40000000-0x7EFFFFFF)
  // before the region is taken - what a bigger region could use without touching the 512 MB. Once per process.
  const Scan before = ScanHeapRange(heap, 0x40000000u, 0x7F000000u);
  REXLOG_INFO("[mem-share] virtual heap 40000000-7F000000 before the region: {} MB free, largest run {} MB{}",
              before.free_bytes >> 20, before.largest >> 20, before.ok ? "" : " (scan incomplete)");
  if (!heap || !heap->Alloc(kBigSize, 64 * 1024, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                            rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, true, &addr) ||
      addr == 0) {
    REXLOG_WARN("[mem-share] could not reserve the {} MB spare region; heap 55 stays as it is", kBigSize >> 20);
    return 0;
  }
  g_big.store(addr);
  return addr;
}

void LockHeap(PPCContext& ctx, uint8_t* base, uint32_t h, bool enter) {
  ctx.r3.u64 = kHeapLocks + 28 * h;
  if (enter)
    __imp__RtlEnterCriticalSection(ctx, base);
  else
    __imp__RtlLeaveCriticalSection(ctx, base);
}

// After a carve / rebuild: point heap 55 at the big region (format it the first time only).
void AdoptBig(PPCContext& ctx, uint8_t* base) {
  if (!Enabled() || !BigEnabled()) return;
  const uint32_t table = PPC_LOAD_U32(kHeapTablePtr);
  if (table < 0x40000000u || table >= 0xF0000000u) return;
  const uint32_t e = table + kSpare * kHeapEntry;
  const uint32_t lo = PPC_LOAD_U32(e), hi = PPC_LOAD_U32(e + 4);
  const uint32_t big = BigRegion();
  if (!big || lo == big || lo < 0x40000000u || hi <= lo) return;
  LockHeap(ctx, base, kSpare, true);
  g_orig55_lo = lo;
  g_orig55_hi = hi;
  if (!g_big_formatted) {
    const uint32_t total = kBigSize >> 4;
    PPC_STORE_U32(big + 0, 1);
    PPC_STORE_U32(big + 4, 0);
    PPC_STORE_U32(big + 8, 1);
    PPC_STORE_U32(big + 12, 0);
    PPC_STORE_U32(big + 16, total);
    PPC_STORE_U32(big + 20, 0);
    PPC_STORE_U32(big + 24, total);
    PPC_STORE_U32(big + 28, 0);
    g_big_formatted = true;
  }
  PPC_STORE_U32(e, big);
  PPC_STORE_U32(e + 4, big + kBigSize);
  LockHeap(ctx, base, kSpare, false);
  g_big_active.store(true);
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 6)
    REXLOG_INFO("[mem-share] spare heap 55 -> {} MB at {:08X} (its own {:08X}-{:08X} left to the game)", kBigSize >> 20,
                big, lo, hi);
}

// Before a rebuild: give the game heap 55's own range back so it formats that one, not the region with live loans.
void ReturnOrig55(PPCContext& ctx, uint8_t* base) {
  if (!g_big_active.load()) return;
  const uint32_t table = PPC_LOAD_U32(kHeapTablePtr);
  if (table < 0x40000000u || table >= 0xF0000000u) return;
  const uint32_t e = table + kSpare * kHeapEntry;
  if (PPC_LOAD_U32(e) != g_big.load()) return;
  LockHeap(ctx, base, kSpare, true);
  PPC_STORE_U32(e, g_orig55_lo);
  PPC_STORE_U32(e + 4, g_orig55_hi);
  LockHeap(ctx, base, kSpare, false);
}

bool SpareHeapExists(uint8_t* base) {
  const uint32_t table = PPC_LOAD_U32(kHeapTablePtr);
  if (table < 0x40000000u || table >= 0xF0000000u) return false;
  const uint32_t s = PPC_LOAD_U32(table + kSpare * kHeapEntry), e = PPC_LOAD_U32(table + kSpare * kHeapEntry + 4);
  return s >= 0x40000000u && e > s;
}

struct Args {
  uint64_t r3, r4, r5, r6;
};
Args Save(PPCContext& ctx) { return {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64}; }
void Restore(PPCContext& ctx, const Args& a) {
  ctx.r3.u64 = a.r3;
  ctx.r4.u64 = a.r4;
  ctx.r5.u64 = a.r5;
  ctx.r6.u64 = a.r6;
}

using wwe2k14::mem::GuestFn;

// Is this request allowed to borrow from heap 55? (own heap returned through *own)
bool HeapMayBorrow(PPCContext& ctx, uint8_t* base, uint32_t* own, bool sample = false) {
  if (t_force_heap >= 0) return false;  // already inside a retry (this file or the #125 one)
  *own = ThreadHeap(ctx, base);
  if (sample) g_sampled_heap[std::min<uint32_t>(*own, 63)].fetch_add(1, std::memory_order_relaxed);
  if (*own >= kAreas || *own == kSpare || !SpareHeapExists(base)) return false;
  return *own >= kFirstModeArea || g_big_active.load();  // a persistent region also serves the long-lived heaps
}

// Runs fn once with the thread's heap answered as 55. Restores args first. Returns the result (r3).
uint32_t CallOnSpare(PPCContext& ctx, uint8_t* base, const Args& a, GuestFn fn) {
  struct Force {
    Force() { t_force_heap = static_cast<int>(kSpare); }
    ~Force() { t_force_heap = -1; }
  } force;
  Restore(ctx, a);
  fn(ctx, base);
  return ctx.r3.u32;
}

// Why HeapMayBorrow said no (failure path only).
const char* HeapRefusal(uint8_t* base, uint32_t own) {
  if (own >= kAreas) return "heap index out of range";
  if (own == kSpare) return "own heap is the lender (55)";
  if (!SpareHeapExists(base)) return "heap 55 does not exist in this layout";
  return "heap below 14 and no persistent region (WWE2K14_MEM_SHARE_BIG=0 or region not reserved)";
}

void HeapNote(uint8_t* base, const char* name, uint32_t size, uint32_t own, const char* why, uint32_t caller,
              uint32_t sp, bool test) {
  char kind[24];
  std::snprintf(kind, sizeof(kind), "general %s", name);
  NoteFail(base, kind, size, own, why, caller, sp, test);
}

void HeapCall(PPCContext& ctx, uint8_t* base, GuestFn fn, const char* name) {
  const Args a = Save(ctx);
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint32_t sp = ctx.r1.u32;
  uint32_t own = 0;
  const uint64_t every = TestEvery();
  bool force = false;
  if (Enabled() && (every || ReportOn())) {
    const bool hit = every && (g_heap_calls.fetch_add(1) + 1) % every == 0;
    if (!hit && ReportOn() && t_force_heap < 0) {
      own = ThreadHeap(ctx, base);
      force = TestFull(false, own);
      Restore(ctx, a);
    }
    force = force || hit;
  }
  if (force && HeapMayBorrow(ctx, base, &own, true)) {
    const uint32_t p = CallOnSpare(ctx, base, a, fn);
    if (p != 0) {
      const uint32_t n = g_heap_loans.fetch_add(1) + 1;
      if (Pow2OrSmall(n))
        REXLOG_WARN("[mem-share] general: {} (r3={}) for heap {} lent from heap 55 at {:08X} (caller {:08X}; loan {}, "
                    "test switch)", name, uint32_t(a.r3), own, p, caller, n);
      if (ReportOn()) HeapNote(base, name, uint32_t(a.r3), own, "forced loan", caller, sp, true);
      return;
    }
  }
  Restore(ctx, a);  // HeapMayBorrow / a failed test loan clobbered r3
  fn(ctx, base);
  if (ctx.r3.u32 != 0 || a.r3 == 0 || t_force_heap >= 0) return;
  if (!Enabled()) {
    HeapNote(base, name, uint32_t(a.r3), ThreadHeap(ctx, base), "sharing off (WWE2K14_MEM_SHARE=0)", caller, sp, false);
    ctx.r3.u64 = 0;
    return;
  }
  if (!HeapMayBorrow(ctx, base, &own)) {
    HeapNote(base, name, uint32_t(a.r3), own, HeapRefusal(base, own), caller, sp, false);
    ctx.r3.u64 = 0;
    return;
  }
  const uint32_t ptr = CallOnSpare(ctx, base, a, fn);
  if (ptr) {
    const uint32_t n = g_heap_loans.fetch_add(1) + 1;
    if (Pow2OrSmall(n))
      REXLOG_WARN("[mem-share] general: {} (r3={}) for heap {} lent from heap 55 at {:08X} (caller {:08X}; loan {})", name,
                  uint32_t(a.r3), own, ptr, caller, n);
  } else {
    HeapNote(base, name, uint32_t(a.r3), own, g_big_active.load() ? "heap 55 region (256 MB) full too" : "heap 55 full too",
             caller, sp, false);
  }
}

// ---------------------------------------------------------------- test-only fill report
#ifndef WWE2K14_NO_TEST_TOOLS
std::atomic<int64_t> g_next_report_ms{0};
uint64_t g_min_free_win[kAreas];
uint64_t g_min_free_heap[kAreas];
std::once_flag g_min_once;

// Block heap h: walk its free list read-only (layout from sub_82C31518 80:~26200: head index = word 2 of block 0;
// free run at index i ends at (word0 & 0x0FFFFFFF), next free = word 2). Racy by design (test-only, no lock).
Scan ScanHeap(uint8_t* base, uint32_t table, uint32_t h, uint64_t* total) {
  Scan s;
  const uint32_t e = table + h * kHeapEntry;
  const uint32_t b = PPC_LOAD_U32(e), end = PPC_LOAD_U32(e + 4);
  *total = 0;
  if (b < 0x40000000u || end <= b) return s;
  const uint32_t blocks = (end - b) >> 4;
  *total = uint64_t(end - b);
  uint32_t i = PPC_LOAD_U32(b + 8);
  for (int n = 0; i < blocks && n < 200000; ++n) {
    const uint32_t stop = PPC_LOAD_U32(b + i * 16) & 0x0FFFFFFFu;
    if (stop <= i || stop > blocks) return s;
    const uint64_t run = uint64_t(stop - i - 1) * 16;
    s.free_bytes += run;
    s.largest = std::max(s.largest, run);
    i = PPC_LOAD_U32(b + i * 16 + 8);
  }
  s.ok = true;
  return s;
}

void MaybeReport(uint8_t* base) {
  if (!ReportOn()) return;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t due = g_next_report_ms.load();
  if (now < due || !g_next_report_ms.compare_exchange_strong(due, now + 20000)) return;
  std::call_once(g_min_once, [] {
    std::fill(std::begin(g_min_free_win), std::end(g_min_free_win), ~0ull);
    std::fill(std::begin(g_min_free_heap), std::end(g_min_free_heap), ~0ull);
  });
  std::string line;
  char item[96];
  for (uint32_t w = 0; w < kAreas; ++w) {
    const Range r = Window(base, w);
    if (!r.ok()) continue;
    const Scan s = ScanPhysical(r.lo, r.hi);
    if (!s.ok) continue;
    g_min_free_win[w] = std::min(g_min_free_win[w], s.free_bytes);
    std::snprintf(item, sizeof(item), " %u:%llu/%llu(big %llu,min %llu)", w, (unsigned long long)(s.free_bytes >> 10),
                  (unsigned long long)((r.hi - r.lo) >> 10), (unsigned long long)(s.largest >> 10),
                  (unsigned long long)(g_min_free_win[w] >> 10));
    line += item;
  }
  const Scan below = ScanPhysical(0, 0x0F800000u);  // under the window pool (boot heaps, system)
  const Range bp = BelowPoolRange(base);
  const Scan below2 = bp.ok() ? ScanPhysical(bp.lo, bp.hi) : Scan{};
  REXLOG_INFO("[mem-share] fill graphics windows free/size KiB:{} | below-pool free {} KiB (span 0-{:08X}: {} KiB, big {} "
              "KiB) | loans {} ({} KiB out) | low loans {} ({} KiB out) | fail-log callers {} (+{} over cap)",
              line, below.free_bytes >> 10, bp.hi, below2.free_bytes >> 10, below2.largest >> 10, g_loan_count.load(),
              g_loan_net.load() >> 10, g_low_count.load(), g_low_net.load() >> 10, g_fail_real.distinct(),
              g_fail_real.overflow());
  line.clear();
  const uint32_t table = PPC_LOAD_U32(kHeapTablePtr);
  if (table >= 0x40000000u && table < 0xF0000000u) {
    for (uint32_t h = 0; h < kAreas; ++h) {
      uint64_t total = 0;
      const Scan s = ScanHeap(base, table, h, &total);
      if (!total || !s.ok) continue;
      g_min_free_heap[h] = std::min(g_min_free_heap[h], s.free_bytes);
      std::snprintf(item, sizeof(item), " %u:%llu/%llu(big %llu,min %llu)", h, (unsigned long long)(s.free_bytes >> 10),
                    (unsigned long long)(total >> 10), (unsigned long long)(s.largest >> 10),
                    (unsigned long long)(g_min_free_heap[h] >> 10));
      line += item;
    }
  }
  std::string sampled;
  for (uint32_t h = 0; h < 64; ++h) {
    const uint32_t n = g_sampled_heap[h].load(std::memory_order_relaxed);
    if (n) sampled += " " + std::to_string(h) + ":" + std::to_string(n);
  }
  REXLOG_INFO("[mem-share] fill general heaps free/size KiB:{} | loans {} | calls {} | sampled own heaps:{}", line,
              g_heap_loans.load(), g_heap_calls.load(), sampled);
}
#else
inline void MaybeReport(uint8_t*) {}
#endif
}  // namespace

// The calling thread's heap index (r3). Answers the forced heap only inside a scoped retry on this host thread.
extern "C" PPC_FUNC(sub_82C31100) {
  if (t_force_heap >= 0) {
    ctx.r3.u64 = static_cast<uint32_t>(t_force_heap);
    return;
  }
  __imp__sub_82C31100(ctx, base);
}

extern "C" PPC_FUNC(sub_8225F6C0) {
  const uint32_t size = ctx.r3.u32, tag = ctx.r4.u32, caller = static_cast<uint32_t>(ctx.lr), sp = ctx.r1.u32;
  MaybeReport(base);
  const uint64_t every = TestEvery();
  bool force = false;
  uint32_t fown = 0;
  if (Enabled() && size && (every || ReportOn())) {
    force = every && (g_phys_calls.fetch_add(1) + 1) % every == 0;
    if (!force && ReportOn()) {
      const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
      fown = OwnWindow(ctx, base, tag);
      force = TestFull(true, fown);
      ctx.r3.u64 = r3;
      ctx.r4.u64 = r4;
    }
  }
  if (force) {
    const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
    const uint32_t own = OwnWindow(ctx, base, tag);
    const uint32_t p = PhysLend(ctx, base, size, tag, own, caller, sp, true);
    if (p) {
      if (ReportOn()) NoteFail(base, "graphics", size, own, "forced loan", caller, sp, true);
      ctx.r3.u64 = p;
      return;
    }
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
  }
  __imp__sub_8225F6C0(ctx, base);
  if (ctx.r3.u32 != 0 || size == 0) return;
  const uint32_t own = OwnWindow(ctx, base, tag);
  if (!Enabled()) {
    NoteFail(base, "graphics", size, own, "sharing off (WWE2K14_MEM_SHARE=0)", caller, sp, false);
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r3.u64 = PhysLend(ctx, base, size, tag, own, caller, sp, false);
}

// Game wrapper around MmFreePhysicalMemory (r3 = address), used by the pooled free sub_8225F7E8: drop the loan record.
extern "C" PPC_FUNC(sub_822617B8) {
  if ((g_loan_net.load(std::memory_order_relaxed) | g_low_net.load(std::memory_order_relaxed)) != 0)
    ForgetLoan(ctx.r3.u32);
  __imp__sub_822617B8(ctx, base);
}

extern "C" PPC_FUNC(sub_82C31518) {
  HeapCall(ctx, base, wwe2k14::mem::g_inner_new ? wwe2k14::mem::g_inner_new : __imp__sub_82C31518, "new");
}

extern "C" PPC_FUNC(sub_82C32DE8) {
  if (t_f58_depth > 0) {  // inside the aligned allocator: its own outer retry covers this
    (wwe2k14::mem::g_inner_small ? wwe2k14::mem::g_inner_small : __imp__sub_82C32DE8)(ctx, base);
    return;
  }
  HeapCall(ctx, base, wwe2k14::mem::g_inner_small ? wwe2k14::mem::g_inner_small : __imp__sub_82C32DE8, "small");
}

namespace {
void F58Inner(PPCContext& ctx, uint8_t* base) {
  ++t_f58_depth;
  __imp__sub_82C32F58(ctx, base);
  --t_f58_depth;
}
}  // namespace

extern "C" PPC_FUNC(sub_82C32F58) { HeapCall(ctx, base, F58Inner, "aligned"); }

// Boot carve of all 56 heaps (one caller, 47:9038 sub_828005A0) and layout rebuilds (5 callers, heaps 14+ or 44+).
// Review 11-17 (race): the rebuild locks all 64 heap critical sections (sub_82C314C8, 80:~26200) and releases them at
// its tail (80:~28690). The put-back, the rebuild and the re-point must happen inside ONE such lock-all, or another
// thread could free/borrow a heap-55 block against the wrong range in between. The sections are recursive.
namespace {
void LeaveAll(PPCContext& ctx, uint8_t* base) {
  for (uint32_t i = 0; i < 64; ++i) {
    ctx.r3.u64 = kHeapLocks + 28 * i;
    __imp__RtlLeaveCriticalSection(ctx, base);
  }
}
}  // namespace

// Boot carve: single-threaded, and it initialises the heap locks itself (sub_82C30F40 first), so no lock-all here.
extern "C" PPC_FUNC(sub_82C31B38) {
  const uint64_t lr = ctx.lr;
  __imp__sub_82C31B38(ctx, base);
  const uint64_t r3 = ctx.r3.u64;
  AdoptBig(ctx, base);
  ctx.r3.u64 = r3;
  ctx.lr = lr;
}

extern "C" PPC_FUNC(sub_82C322B0) {
  const Args a = Save(ctx);
  const uint64_t lr = ctx.lr;
  __imp__sub_82C314C8(ctx, base);
  ReturnOrig55(ctx, base);
  Restore(ctx, a);
  ctx.lr = lr;
  __imp__sub_82C322B0(ctx, base);
  const uint64_t r3 = ctx.r3.u64;
  AdoptBig(ctx, base);
  LeaveAll(ctx, base);
  ctx.r3.u64 = r3;
  ctx.lr = lr;
}
