// Native Resolve worker sub_82CB2320 (native_resolve.h). Core: native_resolve_core.h (host test: 50,000 random states
// against the generated body, 0 mismatches). WWE2K14_D3D_NATIVE_RESOLVE 0 / 1 native (default on desktop) / 2 CHECK, _AB=N.
// CHECK WARNING (run 4, 11-19): restoring the device block clobbers other threads' updates and froze the game once.
//
// CHECK (test runs only): save every region the body and its callees can write (device block, ring window after the
// cursor, the resolve record, the deferred-list window, the worker's stack frame), run the NATIVE body, keep its results,
// restore the saved bytes and the registers, run the GUEST body live, compare. The guest result is what the game keeps.
// A call is only compared when no callee can have effects outside the saved regions: no ring reserve possible (cursor
// well below both D+52 and D+56), no flush dispatcher (caller flag set), no fill hook (flags & 0x100: its tree compiles
// shaders and allocates), no deferred-list reserve (list not full). Other calls run the guest body only and are counted
// as skipped. Callees with internal state outside these regions run twice in a compared call (accepted for a test mode).
#include "native_resolve.h"
#include "native_resolve_core.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "capture.h"
#include "native_setters_core.h"

extern "C" {
PPC_FUNC(sub_82CAA500);
PPC_FUNC(sub_82CAB1E8);
PPC_FUNC(sub_82CB18F0);
PPC_FUNC(sub_82CB1A50);
PPC_FUNC(sub_82CB1FE8);
PPC_FUNC(sub_82CAC5C0);
PPC_FUNC(sub_82CC0988);
PPC_FUNC(sub_82CC0D18);
PPC_FUNC(sub_82CB20C0);
PPC_FUNC(sub_82CBA798);
PPC_FUNC(sub_82C7C8F0);
PPC_FUNC(sub_82CB5AE8);
PPC_FUNC(sub_82CB9588);
PPC_FUNC(sub_82CAA430);
PPC_FUNC(sub_82CAA740);
PPC_FUNC(sub_82CBBD68);
}

namespace wwe2k14::d3d::native_resolve {
namespace {

using Fn = void (*)(PPCContext&, uint8_t*);
Fn Callee(uint32_t a) {
  switch (a) {
#define F(x) case 0x##x##u: return sub_##x;
    F(82CAA500) F(82CAB1E8) F(82CB18F0) F(82CB1A50) F(82CB1FE8) F(82CAC5C0) F(82CC0988) F(82CC0D18)
    F(82CB20C0) F(82CBA798) F(82C7C8F0) F(82CB5AE8) F(82CB9588) F(82CAA430) F(82CAA740) F(82CBBD68)
#undef F
    default: return nullptr;
  }
}

// Live policy: guest memory, guest callees through their (hooked) symbols with the guest return address.
struct GuestM {
  PPCContext& ctx;
  uint8_t* base;
  uint32_t sp;  // the worker's frame (caller r1 - 384): callees run with it, as under the guest body
  bool ok = true;
  uint8_t L8(uint32_t a) { return PPC_LOAD_U8(a); }
  uint16_t L16(uint32_t a) { return PPC_LOAD_U16(a); }
  uint32_t L32(uint32_t a) { return PPC_LOAD_U32(a); }
  uint64_t L64(uint32_t a) { return PPC_LOAD_U64(a); }
  void S8(uint32_t a, uint8_t v) { PPC_STORE_U8(a, v); }
  void S16(uint32_t a, uint16_t v) { PPC_STORE_U16(a, v); }
  void S32(uint32_t a, uint32_t v) { PPC_STORE_U32(a, v); }
  void S64(uint32_t a, uint64_t v) { PPC_STORE_U64(a, v); }
  void Regs(uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8, uint64_t r9, uint64_t r10) {
    ctx.r1.u64 = sp;
    ctx.r3.u64 = r3, ctx.r4.u64 = r4, ctx.r5.u64 = r5, ctx.r6.u64 = r6;
    ctx.r7.u64 = r7, ctx.r8.u64 = r8, ctx.r9.u64 = r9, ctx.r10.u64 = r10;
  }
  uint64_t Run(uint32_t addr, uint32_t lr) {
    Fn fn = Callee(addr);
    if (!fn) {
      ok = false;  // unreachable: every m.Call address of the core is listed
      return 0;
    }
    ctx.lr = lr;
    fn(ctx, base);
    return ctx.r3.u64;
  }
  uint64_t Call(uint32_t addr, uint32_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8,
                uint64_t r9, uint64_t r10) {
    Regs(r3, r4, r5, r6, r7, r8, r9, r10);
    return Run(addr, lr);
  }
  uint64_t CallF(uint32_t addr, uint32_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8,
                 uint64_t r9, uint64_t r10, uint64_t f1_bits) {
    Regs(r3, r4, r5, r6, r7, r8, r9, r10);
    ctx.fpscr.disableFlushMode();
    ctx.f1.u64 = f1_bits;  // fmr f1,f31 (9013)
    return Run(addr, lr);
  }
  uint64_t CallV(uint32_t addr, uint32_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8,
                 uint64_t r9, uint64_t r10, uint32_t v1_addr) {
    Regs(r3, r4, r5, r6, r7, r8, r9, r10);
    const uint32_t ea = v1_addr & ~0xFu;  // lvx128 v1,0,r17 (9031), as the generated code loads it
    simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)PPC_RAW_ADDR(ea)),
                                                                          simde_mm_load_si128((simde__m128i*)VectorMaskL)));
    return Run(addr, lr);
  }
};

struct Config {
  int mode = 0;
  uint32_t ab = 0;
};
const Config& Cfg() {
  static const Config c = [] {
    Config k;
#if defined(__ANDROID__) || defined(REX_UWP)
    k.mode = 0;  // Android / Xbox app: guest body (same rule as the other native setters)
#else
    // Default ON on desktop since 11-19: host test 50,000 states 0 mismatches; game CHECK 11,264 calls (ring / record /
    // list identical); native smoke match nresolve-on-1 PASS (550,145 calls, picture normal); in-run A/B nresolve-ab-1:
    // all threads +0.04 ms/f, 6 of 13 pairs faster = no clear difference. WWE2K14_D3D_NATIVE_RESOLVE=0 restores the guest.
    k.mode = 1;
#endif
    if (const char* e = std::getenv("WWE2K14_D3D_NATIVE_RESOLVE"); e && *e) k.mode = std::atoi(e);
    if (k.mode < 0 || k.mode > 2) k.mode = 0;
    if (const char* e = std::getenv("WWE2K14_D3D_NATIVE_RESOLVE_AB"); e && *e) k.ab = uint32_t(std::strtoul(e, nullptr, 10));
    if (k.mode) REXLOG_INFO("[d3d-native-resolve] {} ab={}", k.mode == 1 ? "ON (native body)" : "CHECK", k.ab);
    return k;
  }();
  return c;
}

void RunNative(PPCContext& ctx, uint8_t* base) {
  const uint32_t caller_sp = ctx.r1.u32;
  GuestM m{ctx, base, caller_sp - 384u};
  Resolve(m, caller_sp, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
          ctx.f1.f64);
  ctx.r1.u64 = caller_sp;  // the guest epilogue's r1 (non-volatile registers were never touched)
}

// ---- CHECK ----
constexpr uint32_t kDev = 16384, kRingWin = 4096, kRec = 64, kList = 16, kFrameLo = 384, kFrameHi = 128;
std::atomic<uint64_t> g_calls{0}, g_compared{0}, g_skipped{0}, g_bad{0};
std::atomic<uint64_t> g_skip_flags{0}, g_skip_ring{0}, g_skip_list{0}, g_skip_range{0};
// Worst-case ring bytes of one call (packets of the body + the register groups of 82CC0D18 + scissor + encoder),
// measured from the generated body: < 1,200 bytes.
constexpr uint32_t kRingMargin = 1280;

struct Region {
  uint32_t addr = 0, len = 0;
  std::vector<uint8_t> bytes;
  void Save(uint8_t* base, uint32_t a, uint32_t n) {
    // Run 3 (11-19) crashed here: the deferred-list pointer D+13928 is 0 when no list is in use (guest page 0 is
    // protected). A region outside mapped guest memory is not saved (and not compared).
    addr = a, len = (a >= 0x10000u && GuestRangeOk(a, n)) ? n : 0;
    bytes.assign(base + a, base + a + len);
  }
  void Restore(uint8_t* base) const { std::memcpy(base + addr, bytes.data(), len); }
};

bool Comparable(const PPCContext& ctx, uint8_t* base) {
  const uint32_t D = ctx.r3.u32;
  if (ctx.r7.u32 != 0 || (ctx.r4.u32 & 0x100u)) {  // flush dispatcher / fill hook
    g_skip_flags.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t cur = PPC_LOAD_U32(D + 48), seg = PPC_LOAD_U32(D + 52), lim = PPC_LOAD_U32(D + 56);
  if (uint64_t(cur) + kRingMargin > seg || uint64_t(cur) + kRingMargin > lim) {  // a ring reserve could happen
    g_skip_ring.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // Deferred-list entry (FallbackAndList): only when D+11036 == 0 and (D+11040 & rec[0]) != 0; then a full list
  // (D+13928 >= D+13932) calls the list reserve. Run 2 (11-19) skipped every call on the unconditional test.
  const bool list_path = PPC_LOAD_U32(D + 11036) == 0 && (PPC_LOAD_U32(D + 11040) & PPC_LOAD_U32(ctx.r8.u32)) != 0;
  if (list_path && !(PPC_LOAD_U32(D + 13928) < PPC_LOAD_U32(D + 13932))) {  // deferred-list reserve
    g_skip_list.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!(GuestRangeOk(cur, kRingWin) && GuestRangeOk(D, kDev) && GuestRangeOk(ctx.r8.u32, kRec))) {
    g_skip_range.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void Summary(uint64_t calls) {
  if ((calls & 0x3FFu) != 0) return;
  REXLOG_INFO("[d3d-native-resolve] CHECK calls={} compared={} skipped={} (flags {} ring {} list {} range {}) mismatch={}",
              calls, g_compared.load(std::memory_order_relaxed), g_skipped.load(std::memory_order_relaxed),
              g_skip_flags.load(std::memory_order_relaxed), g_skip_ring.load(std::memory_order_relaxed),
              g_skip_list.load(std::memory_order_relaxed), g_skip_range.load(std::memory_order_relaxed),
              g_bad.load(std::memory_order_relaxed));
}

void RunCheck(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*)) {
  const uint64_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!Comparable(ctx, base)) {
    g_skipped.fetch_add(1, std::memory_order_relaxed);
    original(ctx, base);
    Summary(calls);
    return;
  }
  const uint32_t D = ctx.r3.u32, sp = ctx.r1.u32;
  thread_local Region dev, ring, rec, list, frame;
  dev.Save(base, D, kDev);
  ring.Save(base, PPC_LOAD_U32(D + 48), kRingWin);
  rec.Save(base, ctx.r8.u32, kRec);
  list.Save(base, PPC_LOAD_U32(D + 13928), kList);
  frame.Save(base, sp - kFrameLo, kFrameLo + kFrameHi);
  const PPCContext entry = ctx;
  RunNative(ctx, base);
  thread_local std::vector<uint8_t> nd, nr, nrec, nl;
  nd.assign(base + D, base + D + dev.len);
  nr.assign(base + ring.addr, base + ring.addr + ring.len);
  nrec.assign(base + rec.addr, base + rec.addr + rec.len);
  nl.assign(base + list.addr, base + list.addr + list.len);
  frame.Restore(base), list.Restore(base), rec.Restore(base), ring.Restore(base), dev.Restore(base);
  ctx = entry;
  original(ctx, base);  // the game keeps the guest result
  g_compared.fetch_add(1, std::memory_order_relaxed);
  auto first_diff = [&](const std::vector<uint8_t>& n, uint32_t a) -> int64_t {
    for (uint32_t i = 0; i < n.size(); ++i)
      if (n[i] != base[a + i]) return i;
    return -1;
  };
  const int64_t dd = first_diff(nd, D), rd = first_diff(nr, ring.addr), cd = first_diff(nrec, rec.addr),
                ld = first_diff(nl, list.addr);
  if (dd >= 0 || rd >= 0 || cd >= 0 || ld >= 0) {
    const uint64_t bad = g_bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad <= 16)
      REXLOG_WARN("[d3d-native-resolve] CHECK mismatch flags={:08X} mode={} dev+{} ring+{} rec+{} list+{} (-1 = equal)",
                  entry.r4.u32, entry.r10.u32, dd, rd, cd, ld);
  }
  Summary(calls);
}

}  // namespace

bool Body(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame) {
  const Config& cfg = Cfg();
  if (cfg.mode == 0) return false;
  if (cfg.mode == 1 && !native_setters::AbNewArm(frame, cfg.ab)) return false;
  if (cfg.mode == 2) {
    RunCheck(ctx, base, original);
    return true;
  }
  RunNative(ctx, base);
  return true;
}

}  // namespace wwe2k14::d3d::native_resolve
