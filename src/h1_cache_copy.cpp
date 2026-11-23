// H1e (docs/tasks/H1e-design.md; WWE2K14_H1_COPY_SKIP, default 1, 0 = off): Move-Set hang fix. When the end-process job's optional cache copy
// cannot get memory and nothing can be evicted, skip the copy instead of waiting on itself (generated 81.cpp:48091-48102). The game
// already skips oversized copies at the same call (81.cpp:48381-48383) and both callers discard the result (81.cpp:48432, 80.cpp:64026).
// Value 2 (test builds): also delays large jobs at sub_82C40D00 entry to force the bad order for the owner test.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "h1_cache_copy_core.h"

extern "C" PPC_FUNC(sub_82C31100);
extern "C" PPC_FUNC(sub_82C31668);
extern "C" PPC_FUNC(sub_82C32F58);
extern "C" PPC_FUNC(sub_82C581C0);
extern "C" PPC_FUNC(__imp__sub_82C57CE8);  // the original: a diagnostic override may read the waiting caller's registers
extern "C" PPC_FUNC(__imp__sub_82C58498);
extern "C" PPC_FUNC(__imp__sub_82C58578);
extern "C" PPC_FUNC(__imp__sub_82C40D00);

namespace {
using namespace wwe2k14::h1e;
constexpr uint32_t kAllocWord = 0x8362A100u;  // saved, zeroed and restored around the allocation (81.cpp:48062-48069); meaning unknown

int Mode() {
  static const int m = [] {
    int v = 1;  // on for players (owner 11-05): it only acts where the game would otherwise wait on itself forever
    if (const char* e = std::getenv("WWE2K14_H1_COPY_SKIP"); e && (*e == '0' || *e == '2')) v = *e - '0';
#ifdef WWE2K14_NO_TEST_TOOLS
    if (v == 2) v = 1;
#endif
    REXLOG_INFO("[h1-fix] mode={}", v);
    return v;
  }();
  return m;
}

thread_local uint32_t t_prealloc = 0;
thread_local uint32_t t_loader_size = 0;  // > 0 while the loader path of sub_82C58498 runs on this thread
thread_local uint32_t t_loader_iter = 0;
thread_local std::chrono::steady_clock::time_point t_job_start{};
std::atomic<uint32_t> g_lines{0};

struct GameOps {
  PPCContext& ctx;
  uint8_t* base;
  uint32_t cache;
  uint32_t Alloc(uint32_t size, uint32_t align) {
    const uint32_t old = PPC_LOAD_U32(kAllocWord);
    PPC_STORE_U32(kAllocWord, 0);
    ctx.r3.u64 = size;
    ctx.r4.u64 = align;
    ctx.r5.u64 = 0;
    ctx.r11.u64 = 0;
    sub_82C32F58(ctx, base);
    PPC_STORE_U32(kAllocWord, old);
    return ctx.r3.u32;
  }
  int32_t EvictableCount() {
    ctx.r3.u64 = cache;
    __imp__sub_82C57CE8(ctx, base);
    return ctx.r3.s32;
  }
  int32_t EvictOne() {
    ctx.r3.u64 = cache;
    sub_82C581C0(ctx, base);
    return ctx.r3.s32;
  }
};

void LogCopy(const char* what, uint32_t size, uint8_t* base, uint32_t req, int32_t evictable) {
  if (size < kLogMinBytes || g_lines.fetch_add(1, std::memory_order_relaxed) >= 200) return;
  char path[64] = {};
  for (uint32_t i = 0; i + 1 < sizeof path; ++i) {
    const char c = char(PPC_LOAD_U8(req + 50 + i));
    if (!c || c < 32 || c > 126) break;
    path[i] = c;
  }
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_job_start).count();
  REXLOG_INFO("[h1-fix] {} size={} evictable_at_end={} job_ms={} path={}", what, size, evictable, ms, path);
}
}  // namespace

// Allocation of the copy path (call at 81.cpp:48186): hand over the block the override below already got. The loader path
// (80.cpp:46804) keeps the original, whose wait is legitimate there.
extern "C" PPC_FUNC(__imp__sub_82C58498);
extern "C" PPC_FUNC(sub_82C58498) {
  if (t_prealloc) {
    const uint32_t cache = ctx.r3.u32;
    ctx.r3.u64 = t_prealloc;
    t_prealloc = 0;
    PPC_STORE_U32(cache + 36 + 28, 0);  // the original clears the lock's flag word after its allocation (81.cpp r31+28)
    return;
  }
  // SELCAP hang diagnostic (11-15, docs/tasks/SELCAP1-launch.md): the loader path can spin here forever (alloc fails,
  // nothing evictable). Log big requests (size r4, align r5, heap r3+76) so the last line before a hang names it.
  if (ctx.r4.u32 >= (1u << 20) && g_lines.fetch_add(1, std::memory_order_relaxed) < 400) {
    REXLOG_INFO("[h1-alloc] size={} align={} heap={:08X} lr={:08X}", ctx.r4.u32, ctx.r5.u32, PPC_LOAD_U32(ctx.r3.u32 + 76),
                uint32_t(ctx.lr));
  }
  // SELCAP-8 (11-15): the endless load spin is this loop with a ZERO size (freeze stack inside it, no [h1-loop] line because
  // t_loader_size was 0): a 0-byte read buffer (rounded to 2048 by sub_82C39BB0, 80.cpp:46729, so 0 stays 0) never
  // allocates, and the loop retries forever. Give such a request the smallest normal read buffer (2048 bytes, the
  // caller's own rounding unit); nothing is read into it. WWE2K14_ZERO_READ_FIX=0 = old behaviour.
  static const bool zero_fix = [] { const char* e = std::getenv("WWE2K14_ZERO_READ_FIX"); return !(e && *e == '0'); }();
  if (ctx.r4.u32 == 0 && zero_fix) {
    static std::atomic<uint32_t> zero_lines{0};
    if (zero_lines.fetch_add(1, std::memory_order_relaxed) < 20)
      REXLOG_WARN("[h1-alloc] zero-size read buffer (lr={:08X}) -> 2048 bytes", uint32_t(ctx.lr));
    ctx.r4.u64 = 2048;
  }
  t_loader_size = ctx.r4.u32 ? ctx.r4.u32 : 1;
  t_loader_iter = 0;
  __imp__sub_82C58498(ctx, base);
  t_loader_size = 0;
}

// SELCAP-7 diagnostic (11-15): inside the loader retry loop above, log what each failed round sees: evictable count
// (sub_82C57CE8) and evict result (sub_82C581C0). First 60 rounds per process, then every 2^20th, so a spin shows
// whether the request can never fit (count 0, evict 0) or the evict frees nothing (count > 0 forever).
std::atomic<uint32_t> g_loop_lines{0};
bool LoopLog() {
  if (!t_loader_size) return false;
  ++t_loader_iter;
  return g_loop_lines.load(std::memory_order_relaxed) < 60 || (t_loader_iter & 0xFFFFF) == 0;
}
extern "C" PPC_FUNC(sub_82C57CE8) {
  __imp__sub_82C57CE8(ctx, base);
  if (LoopLog()) {
    g_loop_lines.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("[h1-loop] size={} round={} evictable={}", t_loader_size, t_loader_iter, ctx.r3.s32);
  }
}
extern "C" PPC_FUNC(__imp__sub_82C581C0);
extern "C" PPC_FUNC(sub_82C581C0) {
  __imp__sub_82C581C0(ctx, base);
  if (t_loader_size && g_loop_lines.load(std::memory_order_relaxed) < 60) {
    g_loop_lines.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("[h1-loop] size={} round={} evict={}", t_loader_size, t_loader_iter, ctx.r3.s32);
  }
}

// r3 cache, r4 request; the caller holds the cache lock (81.cpp:48361), so the allocator's inner lock is not needed here.
extern "C" PPC_FUNC(sub_82C58578) {
  if (Mode() == 0 || !ctx.r3.u32) {
    __imp__sub_82C58578(ctx, base);
    return;
  }
  const uint32_t cache = ctx.r3.u32, req = ctx.r4.u32;
  const uint64_t lr = ctx.lr;
  const uint32_t size = CopySize(PPC_LOAD_U32(req + 44));
  sub_82C31100(ctx, base);
  const uint64_t saved_heap = ctx.r3.u64;
  ctx.r3.u64 = PPC_LOAD_U32(cache + 76);
  sub_82C31668(ctx, base);
  GameOps ops{ctx, base, cache};
  const uint32_t p = TryAllocNoWait(ops, size, kCopyAlign);
  ctx.r3.u64 = saved_heap;
  sub_82C31668(ctx, base);
  ctx.lr = lr;
  if (!p) {
    LogCopy("copy-skip", size, base, req, 0);
    ctx.r3.u64 = 0;
    return;
  }
  LogCopy("copy-ok", size, base, req, -1);
  t_prealloc = p;
  ctx.r3.u64 = cache;
  ctx.r4.u64 = req;
  __imp__sub_82C58578(ctx, base);
}

// End-process job: time base for job_ms. Mode 2 delays big jobs before any guest lock is taken (first lock 80.cpp:63941).
extern "C" PPC_FUNC(sub_82C40D00) {
  const int mode = Mode();
  if (mode) t_job_start = std::chrono::steady_clock::now();
  if (mode == 2) {
    const uint32_t req = PPC_LOAD_U32(ctx.r3.u32 + 40);
    if (req && CopySize(PPC_LOAD_U32(req + 44)) >= kLogMinBytes) std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
  __imp__sub_82C40D00(ctx, base);
}
