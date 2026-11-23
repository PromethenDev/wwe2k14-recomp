// Pool exhaustion (GitHub #125 WM23 Cena v Michaels, v1.5.5-test1 crash): log + spare-heap retry. Always on.
// "N:line" = generated/wwe2k14_recomp.N.cpp. Card docs/cards/GH125-wm23-pool.md.
//
// sub_82BCF0B0 (76:60862) makes a 96-byte object from the game's fixed 16-byte-block pools (sub_82C31518 or
// sub_82C32DE8; per-heap table at *(0x83930034) + 36*H). 11-14 correction: that table has 56 entries, not 24 - it is
// the same 56-heap layout as #122 (sub_82C31B38, 80:27170, called from sub_828005A0 with li r4,56 and the KiB array
// from sub_827DDC78). H is the CALLING THREAD's heap: sub_82C31100 (80:25646) looks the guest thread id up in a
// 24-slot table at 0x8392F6C0 (24 bytes each: u64 thread id, u32 heap at +8; slot 0's heap when not found). When no free run fits, both return 0
// (80:26362, 80:29925) and sub_82BCF0B0 returns 0; sub_82BAD598 then reads *(0) and the game crashes (75:59103).
// This hook only writes, the first few times it happens, which caller asked and the first three words of each of
// the 56 heap entries plus the 24-slot thread -> heap map, so a player's bug report shows which pool ran out and how big it is.
//
// 11-14 spare-heap retry: when the 96-byte allocation fails, it is tried ONCE more with the calling thread's heap
// switched to heap 55 (the in-match layout's leftover, 15.6 MB, unused in 23 #122 traces), by answering
// sub_82C31100 with 55 for that one nested call on this thread only. Freeing is safe: the free path sub_82C32048 takes
// the heap from the block itself (sub_82C316D8 80:26512: lbz r31,-1(ptr) = the heap byte both allocators store at header+12, block = header+16), not from the thread. If heap 55 has no
// room either the answer stays 0 = retail behaviour. Capped at kSpareMax objects (96 B each); off with
// WWE2K14_POOL_SPARE=0. TEST-ONLY WWE2K14_TEST_POOL_SPARE_EVERY=N sends every Nth allocation straight to the spare heap
// (exercises alloc + later free from heap 55 in an ordinary match).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "memory_share.h"

namespace wwe2k14::mem {
// Caller-named fail log, defined in memory_share.cpp (T2 MEM-NEVERFAIL; one [mem-fail] line per caller).
void NoteFail(uint8_t* base, const char* kind, uint32_t size, uint32_t area, const char* why, uint32_t lr, uint32_t sp,
              bool test);
}

namespace {
std::atomic<int> g_lines{0};
constexpr int kMaxLines = 8;
constexpr uint32_t kPoolTablePtr = 0x83930034;  // lis r9,-31853 (0x83930000) + lwz r11,52(r9) in sub_82C31518
constexpr uint32_t kPoolEntry = 36;
constexpr uint32_t kPools = 56;
constexpr uint32_t kThreadMap = 0x8392F6C0;  // lis r11,-31853 + addi r9,r11,-2368 in sub_82C31100
constexpr uint32_t kThreadSlots = 24;
constexpr uint32_t kSpareHeap = 55;
constexpr uint32_t kSpareMax = 50000;  // 4.8 MB of 96-byte objects at most
using wwe2k14::mem::t_force_heap;  // defined in memory_share.cpp (one sub_82C31100 override)
std::atomic<uint32_t> g_spare_used{0};
std::atomic<uint32_t> g_spare_failed{0};
std::atomic<uint64_t> g_calls{0};

bool SpareEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_POOL_SPARE");
    return !(e && e[0] == '0');
  }();
  return on;
}
uint64_t TestEvery() {
#ifdef WWE2K14_NO_TEST_TOOLS
  return 0;
#else
  static const uint64_t n = [] {
    const char* e = std::getenv("WWE2K14_TEST_POOL_SPARE_EVERY");
    return e ? std::strtoull(e, nullptr, 10) : 0ull;
  }();
  return n;
#endif
}
}  // namespace

extern "C" {
PPC_FUNC(__imp__sub_82BCF0B0);
PPC_FUNC(sub_82BCF0B0);
PPC_FUNC(__imp__sub_82C31100);
}

// sub_82C31100 (the calling thread's heap) is overridden in memory_share.cpp and honours t_force_heap.

namespace {
// sub_82BCF0B0 reads no argument registers (it starts with bl sub_82C31A20, then li r3,96 / li r3,6), so it can be
// called again with the same context.
void SpareTry(PPCContext& ctx, uint8_t* base, uint32_t caller, bool test) {
  if (g_spare_used.load() >= kSpareMax) return;
  // Review 11-14 risk A: sub_82C322B0 (layout change, e.g. 47.cpp:9745 li r3,14) rebuilds heaps 14 and up as one
  // free block each. Heap 55 is one of them, so only lend it to a thread whose own heap is also >= 14 (same lifetime);
  // a lower heap survives layout changes and could keep a pointer into a rebuilt heap 55.
  __imp__sub_82C31100(ctx, base);
  const uint32_t own = ctx.r3.u32;
  // Risk B: heap 55 must exist in this layout (start set, end above start).
  const uint32_t table = PPC_LOAD_U32(kPoolTablePtr);
  bool usable = own >= 14 && own < kPools && own != kSpareHeap && table >= 0x40000000u && table < 0xF0000000u;
  if (usable) {
    const uint32_t start = PPC_LOAD_U32(table + kSpareHeap * kPoolEntry);
    const uint32_t end = PPC_LOAD_U32(table + kSpareHeap * kPoolEntry + 4);
    usable = start >= 0x40000000u && end > start;
  }
  if (!usable) {
    ctx.r3.u64 = 0;
    if (g_spare_failed.fetch_add(1) < 3)
      REXLOG_WARN("[pool-spare] not used (thread heap {}, caller {:08X}): retail answer 0", own, caller);
    return;
  }
  struct Force {  // the override can never stay set on this host thread
    Force() { t_force_heap = static_cast<int>(kSpareHeap); }
    ~Force() { t_force_heap = -1; }
  } force;
  __imp__sub_82BCF0B0(ctx, base);
  if (ctx.r3.u32 != 0) {
    const uint32_t n = g_spare_used.fetch_add(1) + 1;   // lifetime count (frees are not tracked)
    if (n <= 3 || (n & (n - 1)) == 0)
      REXLOG_WARN("[pool-spare] 96-byte object taken from spare heap {} for thread heap {} (caller {:08X}, {} so far{})",
                  kSpareHeap, own, caller, n, test ? ", test switch" : "");
  } else if (g_spare_failed.fetch_add(1) < 3) {
    REXLOG_WARN("[pool-spare] spare heap {} full too (caller {:08X}): retail answer 0", kSpareHeap, caller);
  }
}
}  // namespace

extern "C" PPC_FUNC(sub_82BCF0B0) {
  const uint32_t caller = static_cast<uint32_t>(ctx.lr), sp = ctx.r1.u32;
  const uint64_t every = TestEvery();
  if (every && SpareEnabled() && (g_calls.fetch_add(1) + 1) % every == 0) {
    SpareTry(ctx, base, caller, true);
    if (ctx.r3.u32 != 0) return;
  }
  __imp__sub_82BCF0B0(ctx, base);
  if (ctx.r3.u32 != 0) return;
  const bool log = g_lines.fetch_add(1) < kMaxLines;
  if (SpareEnabled()) SpareTry(ctx, base, caller, false);
  if (ctx.r3.u32 == 0)  // the block allocators inside already logged their own failure; this names the object's caller
    wwe2k14::mem::NoteFail(base, "pool96 object", 96, 0xFFFFFFFFu, SpareEnabled() ? "spare retry failed" : "spare off",
                           caller, sp, false);
  if (!log) return;
  std::string pools;
  const uint32_t table = PPC_LOAD_U32(kPoolTablePtr);
  if (table >= 0x40000000u && table < 0xF0000000u) {
    for (uint32_t h = 0; h < kPools; ++h) {
      const uint32_t e = table + h * kPoolEntry;
      char item[48];
      std::snprintf(item, sizeof(item), " %u:%08X/%08X/%08X", h, PPC_LOAD_U32(e), PPC_LOAD_U32(e + 4),
                    PPC_LOAD_U32(e + 8));
      pools += item;
    }
  } else {
    char item[48];
    std::snprintf(item, sizeof(item), " table pointer %08X not readable", table);
    pools = item;
  }
  std::string map;
  for (uint32_t i = 0; i < kThreadSlots; ++i) {
    const uint32_t e = kThreadMap + i * 24;
    const uint32_t tid = PPC_LOAD_U32(e + 4);
    if (tid == 0) continue;
    char item[32];
    std::snprintf(item, sizeof(item), " %X->%u", tid, PPC_LOAD_U32(e + 8));
    map += item;
  }
  REXLOG_WARN("[pool-fail] thread heaps:{}", map);
  REXLOG_WARN("[pool-fail] 96-byte game object could not be allocated (caller {:08X}); spare retry {}; pools (#125):{}",
              caller, ctx.r3.u32 ? "OK" : "failed/off", pools);
}
