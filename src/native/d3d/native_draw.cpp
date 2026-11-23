// N3: native draw entry sub_82CB52D0 (native_draw_core.h). WWE2K14_D3D_NATIVE_DRAW: 0 = guest body,
// 1 = native body (default on desktop), 2 = CHECK.
// CHECK: the guest body runs live while its direct callees record their arguments, return value and the device block after
// they return (RecScope at the top of each callee's hook). The native body then runs on a copy of the device taken before
// the guest body, replaying the recorded callees in order: each native call must match the recorded call (address, return
// address, argument registers the callee reads) and the body's own device fields at that point; the recorded device block is
// then loaded into the copy. At the end the body's fields, every ring / list word the native body stored and the final r3
// are compared with the live result.
#include "native_draw.h"
#include "native_draw_core.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

extern "C" {
PPC_FUNC(sub_82CC10B0);
PPC_FUNC(sub_82CC1E48);
PPC_FUNC(sub_82CC0988);
PPC_FUNC(sub_82CC0D18);
PPC_FUNC(sub_82CC0F58);
PPC_FUNC(sub_82CC0DD8);
PPC_FUNC(sub_82CB5AE8);
PPC_FUNC(sub_82CBA798);
PPC_FUNC(sub_82CBA080);
}

namespace wwe2k14::d3d::native_draw {
namespace {
constexpr uint32_t kDevBytes = 14336;  // every device field the body or its callees' results feed lies below this
// The body's own device stores: dirty words 0..39, ring pointer 48, linked-list block pointer 13628.
inline bool BodyField(uint32_t i) { return i < 40 || (i - 48u) < 4u || (i - 13628u) < 4u; }
constexpr uint32_t kPreLo = 64, kPreHiOff = 13628, kPreHi = 4;  // pre-call snapshot: bytes 0..63 and 13628..13631
constexpr uint32_t kMaxCalls = 96;

struct RecCall {
  uint32_t addr, lr;
  uint64_t r3, r4, r5, r6, ret;
  uint8_t pre[kPreLo + kPreHi];
};
struct Rec {
  uint32_t D = 0;
  int depth = 0;
  uint32_t n = 0;
  bool overflow = false;
  RecCall calls[kMaxCalls];
  std::vector<uint8_t> posts = std::vector<uint8_t>(size_t(kMaxCalls) * kDevBytes);
  uint8_t* Post(uint32_t i) { return posts.data() + size_t(i) * kDevBytes; }
};
thread_local Rec* t_rec = nullptr;

struct GuestM {
  PPCContext& ctx;
  uint8_t* base;
  uint8_t L8(uint32_t a) { return PPC_LOAD_U8(a); }
  uint32_t L32(uint32_t a) { return PPC_LOAD_U32(a); }
  uint64_t L64(uint32_t a) { return PPC_LOAD_U64(a); }
  void S32(uint32_t a, uint32_t v) { PPC_STORE_U32(a, v); }
  void S64(uint32_t a, uint64_t v) { PPC_STORE_U64(a, v); }
  void Trap() { ppc_trap(ctx, base, 0); }
  uint64_t Call(uint32_t addr, uint32_t lr, int nargs, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    ctx.r3.u64 = a3;
    if (nargs > 1) ctx.r4.u64 = a4;
    if (nargs > 2) ctx.r5.u64 = a5;
    if (nargs > 3) ctx.r6.u64 = a6;
    ctx.lr = lr;
    switch (addr) {
      case kAluConst: sub_82CC10B0(ctx, base); break;
      case kShaderFlush: sub_82CC1E48(ctx, base); break;
      case kStateEnc: sub_82CC0988(ctx, base); break;
      case kRegGroup: sub_82CC0D18(ctx, base); break;
      case kFetchConst: sub_82CC0F58(ctx, base); break;
      case kCoherency: sub_82CC0DD8(ctx, base); break;
      case kSmall5AE8: sub_82CB5AE8(ctx, base); break;
      case kRingReserve: sub_82CBA798(ctx, base); break;
      case kListReserve: sub_82CBA080(ctx, base); break;
      default: break;
    }
    return ctx.r3.u64;
  }
};

// Replay policy for CHECK (see the file comment).
struct CheckM {
  uint8_t* base;
  uint32_t D;
  uint8_t* dev;  // kDevBytes, guest byte order
  Rec& rec;
  uint32_t next = 0;
  std::vector<std::pair<uint32_t, uint32_t>> ring;
  uint32_t stray = 0;
  bool ok = true;           // false: diverged from the recorded calls (a mismatch)
  char why[160] = {};
  bool In(uint32_t a, uint32_t n) const { return a >= D && a - D + n <= kDevBytes; }
  uint8_t L8(uint32_t a) { return In(a, 1) ? dev[a - D] : PPC_LOAD_U8(a); }
  uint32_t L32(uint32_t a) {
    if (!In(a, 4)) return PPC_LOAD_U32(a);
    const uint8_t* p = dev + (a - D);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  }
  uint64_t L64(uint32_t a) { return uint64_t(L32(a)) << 32 | L32(a + 4); }
  void S32(uint32_t a, uint32_t v) {
    if (!In(a, 4)) {
      ring.emplace_back(a, v);
      return;
    }
    uint8_t* p = dev + (a - D);
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
    for (uint32_t i = 0; i < 4; ++i)
      if (!BodyField(a - D + i)) ++stray;
  }
  void S64(uint32_t a, uint64_t v) {
    S32(a, uint32_t(v >> 32));
    S32(a + 4, uint32_t(v));
  }
  void Fail(const char* fmt, uint64_t x, uint64_t y, uint64_t z) {
    if (!ok) return;
    ok = false;
    std::snprintf(why, sizeof why, fmt, (unsigned long long)x, (unsigned long long)y, (unsigned long long)z);
  }
  void Trap() { Fail("native trap (stride 0) call=%llu %llu %llu", next, 0, 0); }
  uint64_t Call(uint32_t addr, uint32_t lr, int nargs, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!ok) return 0;
    if (next >= rec.n) {
      Fail("extra native call %llX lr %llX (guest made %llu)", addr, lr, rec.n);
      return 0;
    }
    const RecCall& c = rec.calls[next];
    if (c.addr != addr || c.lr != lr) {
      Fail("call %llu: native %llX, guest %llX", next, uint64_t(addr) << 32 | lr, uint64_t(c.addr) << 32 | c.lr);
      return 0;
    }
    if (c.r3 != a3 || (nargs > 1 && c.r4 != a4) || (nargs > 2 && c.r5 != a5) || (nargs > 3 && c.r6 != a6)) {
      Fail("call %llu args: native r4 %llX guest r4 %llX", next, a4, c.r4);
      return 0;
    }
    for (uint32_t i = 0; i < kPreLo + kPreHi; ++i) {
      const uint32_t off = i < kPreLo ? i : kPreHiOff + (i - kPreLo);
      if (BodyField(off) && dev[off] != c.pre[i]) {
        Fail("call %llu: body field byte %llu native %llX differs from guest", next, off,
             uint64_t(dev[off]) << 8 | c.pre[i]);
        return 0;
      }
    }
    std::memcpy(dev, rec.Post(next), kDevBytes);
    ++next;
    return c.ret;
  }
};

#if defined(__ANDROID__) || defined(REX_UWP)
constexpr int kDefaultMode = 0;  // Android / Xbox app: guest body (same rule as N1 / N2-RT)
#else
// Default ON on desktop PC (2026-11-18 after n3-check PASS: 13,893,632 draws, 0 mismatches, 0 stray, 174 M ring words; n3-speed
// in-run A/B 600 s: game 5.16 -> 5.04, worker 6.18 -> 6.07 ms/f, 12 of 25 pairs = no clear difference, not slower).
constexpr int kDefaultMode = 1;
#endif

struct Mode {
  int mode = -1;
  uint32_t ab = 0;
  std::atomic<uint64_t> calls{0}, compared{0}, skipped{0}, bad{0}, ring_words{0}, stray_calls{0}, callees{0}, logged{0};
  void Init() {
    const char* e = std::getenv("WWE2K14_D3D_NATIVE_DRAW");
    mode = !e || !*e ? kDefaultMode : *e == '1' ? 1 : *e == '2' ? 2 : 0;  // NATIVE_DRAW=0 restores the guest body
    const char* a = std::getenv("WWE2K14_D3D_NATIVE_DRAW_AB");
    const long n = a && *a ? std::strtol(a, nullptr, 10) : 0;
    ab = n > 0 && n <= 1000000 ? uint32_t(n) : 0u;
    if (mode) REXLOG_INFO("[d3d-native-draw] {} (native draw body 82CB52D0) ab={}", mode == 2 ? "CHECK" : "ON", ab);
  }
  void Report(uint64_t n) {
    if ((n & 0x3FFFFu) != 0) return;
    REXLOG_INFO("[d3d-native-draw] {} calls={} compared={} skipped={} callees={} ring_words={} mismatch_calls={} "
                "mismatch_stray={}",
                mode == 2 ? "CHECK" : "ON", n, compared.load(), skipped.load(), callees.load(), ring_words.load(),
                bad.load(), stray_calls.load());
  }
};
Mode g_mode;
}  // namespace

void RecScope::Begin(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  Rec* r = t_rec;
  if (!r) return;
  ctx_ = &ctx;  // active: the destructor balances the depth
  base_ = base;
  if (r->depth++ != 0) return;  // nested call inside a callee: not a direct callee of the draw body
  top_ = true;
  if (r->n >= kMaxCalls) {
    r->overflow = true;
    return;
  }
  RecCall& c = r->calls[r->n];
  c.addr = addr;
  c.lr = uint32_t(ctx.lr);
  c.r3 = ctx.r3.u64, c.r4 = ctx.r4.u64, c.r5 = ctx.r5.u64, c.r6 = ctx.r6.u64;
  std::memcpy(c.pre, base + r->D, kPreLo);
  std::memcpy(c.pre + kPreLo, base + r->D + kPreHiOff, kPreHi);
}

void RecScope::End() {
  Rec* r = t_rec;
  if (!r) return;
  --r->depth;
  if (!top_ || r->overflow) return;
  RecCall& c = r->calls[r->n];
  c.ret = ctx_->r3.u64;
  std::memcpy(r->Post(r->n), base_ + r->D, kDevBytes);
  ++r->n;
}

bool Body(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame) {
  static std::once_flag once;
  std::call_once(once, [] { g_mode.Init(); });
  const int mode = g_mode.mode;
  if (mode == 0) return false;
  if (mode == 1 && g_mode.ab && ((frame / g_mode.ab) & 1u) == 0) return false;  // even block: guest body (cost-ab.py rule)
  const uint64_t calls = g_mode.calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t a3 = ctx.r3.u64, a4 = ctx.r4.u64, a5 = ctx.r5.u64, a6 = ctx.r6.u64, a7 = ctx.r7.u64;
  if (mode == 1) {
    // Same stack frame as the guest body (stwu r1,-208(r1)): callees' frames land where they do under the guest body.
    const uint64_t sp = ctx.r1.u64;
    const uint32_t nsp = ctx.r1.u32 - 208;
    PPC_STORE_U32(nsp, ctx.r1.u32);
    ctx.r1.u32 = nsp;
    GuestM m{ctx, base};
    const uint64_t r3 = DrawIndexedVertices(m, a3, a4, a5, a6, a7);
    ctx.r1.u64 = sp;
    ctx.r3.u64 = r3;
    g_mode.Report(calls);
    return true;
  }
  // CHECK.
  const uint32_t D = uint32_t(a3);
  thread_local Rec* rec = new Rec();
  uint8_t dev[kDevBytes];
  std::memcpy(dev, base + D, kDevBytes);
  rec->D = D, rec->depth = 0, rec->n = 0, rec->overflow = false;
  t_rec = rec;
  original(ctx, base);
  t_rec = nullptr;
  const uint64_t guest_r3 = ctx.r3.u64;
  if (rec->overflow) {
    g_mode.skipped.fetch_add(1, std::memory_order_relaxed);
    g_mode.Report(calls);
    return true;
  }
  CheckM m{base, D, dev, *rec};
  const uint64_t r3 = DrawIndexedVertices(m, a3, a4, a5, a6, a7);
  if (m.ok && m.next != rec->n) m.Fail("native made %llu callee calls, guest %llu %llu", m.next, rec->n, 0);
  if (m.ok && r3 != guest_r3) m.Fail("final r3 native %llX guest %llX %llu", r3, guest_r3, 0);
  if (m.ok) {
    uint8_t live[kDevBytes];
    std::memcpy(live, base + D, kDevBytes);
    for (uint32_t i = 0; i < kDevBytes && m.ok; ++i)
      if (BodyField(i) && live[i] != dev[i]) m.Fail("end: body field byte %llu live %llX native %llX", i, live[i], dev[i]);
  }
  uint32_t ring_bad = 0;
  if (m.ok)
    for (const auto& [a, v] : m.ring)
      if (PPC_LOAD_U32(a) != v && !ring_bad++)
        m.Fail("ring word at %llX native %llX live %llX", a, v, PPC_LOAD_U32(a));
  g_mode.compared.fetch_add(1, std::memory_order_relaxed);
  g_mode.callees.fetch_add(rec->n, std::memory_order_relaxed);
  g_mode.ring_words.fetch_add(m.ring.size(), std::memory_order_relaxed);
  if (m.stray) g_mode.stray_calls.fetch_add(1, std::memory_order_relaxed);
  if (!m.ok) g_mode.bad.fetch_add(1, std::memory_order_relaxed);
  if ((!m.ok || m.stray) && g_mode.logged.fetch_add(1, std::memory_order_relaxed) < 20)
    REXLOG_WARN("[d3d-native-draw] CHECK mismatch prim={:X} r5={:08X} start={} count={} callees={} ring_words={} stray={}: {}",
                uint32_t(a4), uint32_t(a5), uint32_t(a6), uint32_t(a7), rec->n, m.ring.size(), m.stray, m.why);
  g_mode.Report(calls);
  return true;
}

}  // namespace wwe2k14::d3d::native_draw

// CHECK recording for the draw body's direct callees that have no hook of their own (the others carry a RecScope in
// hooks.cpp). Behaviour unchanged: each calls its original body.
extern "C" {
PPC_FUNC(__imp__sub_82CC0988);
PPC_FUNC(__imp__sub_82CC0DD8);
PPC_FUNC(__imp__sub_82CB5AE8);
PPC_FUNC(__imp__sub_82CBA080);
}
#define N3_REC_OVERRIDE(addr)                                                       \
  extern "C" PPC_FUNC(sub_##addr) {                                                 \
    wwe2k14::d3d::native_draw::RecScope rs_(0x##addr##u, ctx, base);                \
    __imp__sub_##addr(ctx, base);                                                   \
  }
N3_REC_OVERRIDE(82CC0988)
N3_REC_OVERRIDE(82CC0DD8)
N3_REC_OVERRIDE(82CB5AE8)
N3_REC_OVERRIDE(82CBA080)
#undef N3_REC_OVERRIDE
