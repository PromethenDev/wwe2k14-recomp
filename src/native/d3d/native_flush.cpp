// N4 step 2c: native shader flush sub_82CC1E48 (native_flush_core.h). WWE2K14_D3D_NATIVE_FLUSH: 0 = guest body,
// 1 = native body (desktop default), 2 = CHECK; _AB=N runs mode 1 in odd blocks of N frames (cost-ab.py rule).
// The native body calls the hooked callees through their hooks (ShaderConstLoad 82CC1C58 and the VS upload 82CC1B00 carry
// the shader identity bookkeeping; the ring reserve 82CBA798 the segment bookkeeping), with the guest flush's stack frame
// (sp - 272) so the upload's r6 word (frame + 80) lives where it does under the guest.
// CHECK: before the guest body runs live, copies are taken of the device, the flush stack frame, the VS object head (in-place
// patch cache keys + fences), the declaration head (id), the three flush globals and the GPU's completed-fence word. The
// guest body's hooked callees record their arguments, the device and the stack frame after they return (RecScope). The
// native body then runs on the copies and replays the recorded callees in order: address, return address and argument
// registers must match, and the flush's own device fields must equal the recorded pre-call device; the recorded device and
// frame are then loaded into the copies. At the end the flush's fields, every word it stored outside the device and the
// stack frame (ring packets, cache keys, fences, declaration id, globals, in-place microcode patches) and the returned mask
// are compared with the live result.
#include "native_flush.h"
#include "native_flush_core.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>
#include <rex/thread/mutex.h>

#include "../nr/nr_pc_sampler.h"

extern "C" {
PPC_FUNC(sub_82CBA798);  // ring reserve (hooked)
PPC_FUNC(sub_82CC1C58);  // ShaderConstLoad (hooked: shader identity)
PPC_FUNC(sub_82CC1B00);  // inline VS upload (hooked: shader identity; native tree by default)
PPC_FUNC(__imp__KeBugCheck);
}

namespace wwe2k14::d3d::native_flush {
namespace {
using native_shader::Flush;
constexpr uint32_t kDevBytes = 14336;
constexpr uint32_t kFrame = 272;     // the guest flush's stack frame
constexpr uint32_t kVsHead = 0x200;  // VS object bytes holding the in-place cache keys / fences (vs + 416 i + 40 .. 68)
constexpr uint32_t kDeclHead = 64;
constexpr uint32_t kGlob = native_shader::flush::kMask, kGlobBytes = 12;
constexpr uint32_t kMaxCalls = 16;
constexpr uint32_t kRing = 0x82CBA798u, kConstLoad = 0x82CC1C58u, kVsUpload = 0x82CC1B00u;

// The flush's own device fields (CHECK compares only these; callees and other threads write others).
inline bool Field(uint32_t i) {
  return (i - 48u) < 4u || (i - 10372u) < 4u || (i - 10528u) < 12u || (i - 10560u) < 4u || (i - 10580u) < 4u ||
         (i - 11068u) < 4u || (i - 12264u) < 16u || (i - 13076u) < 4u;
}

struct RecCall {
  uint32_t addr, lr;
  uint64_t r[8];  // r3..r10
  uint64_t ret;   // r3 at return
};
struct Rec {
  uint32_t D = 0, fsp = 0;
  int depth = 0;
  uint32_t n = 0;
  bool overflow = false;
  RecCall calls[kMaxCalls];
  std::vector<uint8_t> pre = std::vector<uint8_t>(size_t(kMaxCalls) * kDevBytes);
  std::vector<uint8_t> post = std::vector<uint8_t>(size_t(kMaxCalls) * kDevBytes);
  std::vector<uint8_t> post_frame = std::vector<uint8_t>(size_t(kMaxCalls) * kFrame);
};
thread_local Rec* t_rec = nullptr;

struct GuestM {
  PPCContext& ctx;
  uint8_t* base;
  uint8_t L8(uint32_t a) { return PPC_LOAD_U8(a); }
  uint16_t L16(uint32_t a) { return PPC_LOAD_U16(a); }
  uint32_t L32(uint32_t a) { return PPC_LOAD_U32(a); }
  uint64_t L64(uint32_t a) { return PPC_LOAD_U64(a); }
  void S8(uint32_t a, uint8_t v) { PPC_STORE_U8(a, v); }
  void S32(uint32_t a, uint32_t v) { PPC_STORE_U32(a, v); }
  void S64(uint32_t a, uint64_t v) { PPC_STORE_U64(a, v); }
  uint64_t Copy(uint32_t dst, uint32_t src, uint32_t n, uint32_t) {
    std::memmove(base + dst, base + src, n);
    return dst;
  }
  uint64_t RingReserve(uint32_t dev, uint32_t lr) {
    ctx.r3.u64 = dev;
    ctx.lr = lr;
    sub_82CBA798(ctx, base);
    return ctx.r3.u64;
  }
  void BugCheck(uint32_t code, uint32_t lr) {
    ctx.r3.u64 = code;
    ctx.lr = lr;
    __imp__KeBugCheck(ctx, base);
  }
  void ConstLoad(uint32_t dev, uint32_t info, uint32_t r5, uint32_t lr) {
    ctx.r3.u64 = dev, ctx.r4.u64 = info, ctx.r5.u64 = r5;
    ctx.lr = lr;
    sub_82CC1C58(ctx, base);
  }
  void VsUpload(uint32_t dev, uint32_t r4, uint32_t vs, uint32_t r6, uint32_t decl, uint32_t r8, uint32_t r9, uint32_t idx,
                uint32_t lr) {
    ctx.r3.u64 = dev, ctx.r4.u64 = r4, ctx.r5.u64 = vs, ctx.r6.u64 = r6, ctx.r7.u64 = decl, ctx.r8.u64 = r8,
    ctx.r9.u64 = r9, ctx.r10.u64 = idx;
    ctx.lr = lr;
    sub_82CC1B00(ctx, base);
  }
  // The guest's lwarx/stwcx increment under the emulator's global lock (as the generated code does).
  uint32_t AtomicInc(uint32_t a) {
    auto lock = rex::thread::global_critical_region::AcquireDirect();
    auto* p = reinterpret_cast<uint32_t*>(base + a);
    uint32_t old = __atomic_load_n(p, __ATOMIC_SEQ_CST), v;
    do v = __builtin_bswap32(__builtin_bswap32(old) + 1);
    while (!__atomic_compare_exchange_n(p, &old, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return __builtin_bswap32(v);
  }
};

// CHECK policy: copies taken before the guest ran; stores outside the device and the frame go to an overlay.
struct CheckM {
  uint8_t* base;
  uint32_t D, fsp, vs, decl, fence_addr;
  uint8_t* dev;    // kDevBytes
  uint8_t* frame;  // kFrame at fsp
  uint8_t* vs_head;
  uint8_t* decl_head;
  uint8_t* glob;
  uint32_t fence_pre;
  Rec& rec;
  uint32_t next = 0;
  std::unordered_map<uint32_t, uint8_t> out;
  uint32_t stray = 0;
  bool ok = true, skip = false;
  char why[160] = {};
  uint8_t* Ptr(uint32_t a) {
    if (a - D < kDevBytes) return dev + (a - D);
    if (a - fsp < kFrame) return frame + (a - fsp);
    if (vs && a - vs < kVsHead) return vs_head + (a - vs);
    if (decl && a - decl < kDeclHead) return decl_head + (a - decl);
    if (a - kGlob < kGlobBytes) return glob + (a - kGlob);
    return nullptr;
  }
  uint8_t B(uint32_t a) {
    if (!out.empty())
      if (auto it = out.find(a); it != out.end()) return it->second;
    if (uint8_t* p = Ptr(a)) return *p;
    if (fence_addr && a - fence_addr < 4) return uint8_t(fence_pre >> (24 - 8 * (a - fence_addr)));
    return PPC_LOAD_U8(a);
  }
  void SB(uint32_t a, uint8_t v) {
    if (a - D < kDevBytes) {
      dev[a - D] = v;
      if (!Field(a - D)) ++stray;
      return;
    }
    if (a - fsp < kFrame) {  // the frame is scratch: not compared
      frame[a - fsp] = v;
      return;
    }
    out[a] = v;
  }
  uint8_t L8(uint32_t a) { return B(a); }
  uint16_t L16(uint32_t a) { return uint16_t(B(a) << 8 | B(a + 1)); }
  uint32_t L32(uint32_t a) { return uint32_t(L16(a)) << 16 | L16(a + 2); }
  uint64_t L64(uint32_t a) { return uint64_t(L32(a)) << 32 | L32(a + 4); }
  void S8(uint32_t a, uint8_t v) { SB(a, v); }
  void S32(uint32_t a, uint32_t v) {
    for (int i = 0; i < 4; ++i) SB(a + i, uint8_t(v >> (24 - 8 * i)));
  }
  void S64(uint32_t a, uint64_t v) {
    S32(a, uint32_t(v >> 32));
    S32(a + 4, uint32_t(v));
  }
  uint64_t Copy(uint32_t dst, uint32_t src, uint32_t n, uint32_t) {
    for (uint32_t i = 0; i < n; ++i) SB(dst + i, B(src + i));
    return dst;
  }
  void Fail(const char* fmt, uint64_t x, uint64_t y, uint64_t z) {
    if (!ok) return;
    ok = false;
    std::snprintf(why, sizeof why, fmt, (unsigned long long)x, (unsigned long long)y, (unsigned long long)z);
  }
  uint64_t Replay(uint32_t addr, uint32_t lr, int nargs, const uint64_t* args) {
    if (!ok || skip) return 0;
    if (next >= rec.n) return Fail("extra native call %llX lr %llX (guest made %llu)", addr, lr, rec.n), 0;
    const RecCall& c = rec.calls[next];
    if (c.addr != addr || c.lr != lr)
      return Fail("call %llu: native %llX, guest %llX", next, uint64_t(addr) << 32 | lr, uint64_t(c.addr) << 32 | c.lr), 0;
    for (int i = 0; i < nargs; ++i)
      if (uint32_t(c.r[i]) != uint32_t(args[i]))
        return Fail("call %llu arg r%llu native/guest %llX", next, i + 3, uint64_t(uint32_t(args[i])) << 32 | uint32_t(c.r[i])), 0;
    const uint8_t* pre = rec.pre.data() + size_t(next) * kDevBytes;
    for (uint32_t i = 0; i < kDevBytes; ++i)
      if (Field(i) && dev[i] != pre[i])
        return Fail("call %llu: flush field byte %llu native/guest %llX", next, i, uint64_t(dev[i]) << 8 | pre[i]), 0;
    std::memcpy(dev, rec.post.data() + size_t(next) * kDevBytes, kDevBytes);
    std::memcpy(frame, rec.post_frame.data() + size_t(next) * kFrame, kFrame);
    return rec.calls[next++].ret;
  }
  uint64_t RingReserve(uint32_t dev_addr, uint32_t lr) {
    const uint64_t a[1] = {dev_addr};
    return Replay(kRing, lr, 1, a);
  }
  void BugCheck(uint32_t, uint32_t) { skip = true; }
  void ConstLoad(uint32_t dev_addr, uint32_t info, uint32_t r5, uint32_t lr) {
    const uint64_t a[3] = {dev_addr, info, r5};
    Replay(kConstLoad, lr, 3, a);
  }
  void VsUpload(uint32_t dev_addr, uint32_t r4, uint32_t v, uint32_t r6, uint32_t dl, uint32_t r8, uint32_t r9, uint32_t idx,
                uint32_t lr) {
    const uint64_t a[8] = {dev_addr, r4, v, r6, dl, r8, r9, idx};
    Replay(kVsUpload, lr, 8, a);
  }
  uint32_t AtomicInc(uint32_t a) {
    const uint32_t v = L32(a) + 1;
    S32(a, v);
    return v;
  }
};

#if defined(__ANDROID__) || defined(REX_UWP)
constexpr int kFlushDefault = 0;  // Android / Xbox app: guest body (same rule as N1-N4)
#else
// Default ON on desktop PC (2026-11-18 after n4c-check PASS: 5,242,880 flushes, 0 mismatches, 0 stray, 0 skipped;
// n4c-speed in-run A/B 600 s: game thread 5.02 -> 5.02 ms/f, worker +0.02, record -0.02, all threads +0.00).
constexpr int kFlushDefault = 1;
#endif

struct Mode {
  int mode = -1;
  uint32_t ab = 0;
  std::atomic<uint64_t> calls{0}, compared{0}, skipped{0}, bad{0}, out_bytes{0}, stray_calls{0}, callees{0}, logged{0};
  void Init() {
    const char* e = std::getenv("WWE2K14_D3D_NATIVE_FLUSH");
    mode = !e || !*e ? kFlushDefault : *e == '1' ? 1 : *e == '2' ? 2 : 0;  // NATIVE_FLUSH=0 restores the guest body
    const char* a = std::getenv("WWE2K14_D3D_NATIVE_FLUSH_AB");
    const long n = a && *a ? std::strtol(a, nullptr, 10) : 0;
    ab = n > 0 && n <= 1000000 ? uint32_t(n) : 0u;
    if (mode) REXLOG_INFO("[d3d-native-flush] {} (native shader flush 82CC1E48) ab={}", mode == 2 ? "CHECK" : "ON", ab);
  }
  void Report(uint64_t n) {
    if ((n & 0x3FFFFu) != 0) return;
    REXLOG_INFO("[d3d-native-flush] {} calls={} compared={} skipped={} callees={} out_bytes={} mismatch_calls={} "
                "mismatch_stray={}",
                mode == 2 ? "CHECK" : "ON", n, compared.load(), skipped.load(), callees.load(), out_bytes.load(), bad.load(),
                stray_calls.load());
  }
};
Mode g_mode;
std::once_flag g_once;
}  // namespace

void RecScope::Begin(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  Rec* r = t_rec;
  if (!r) return;
  ctx_ = &ctx;
  base_ = base;
  if (r->depth++ != 0) return;  // nested inside a callee: not a direct callee of the flush
  top_ = true;
  if (r->n >= kMaxCalls) {
    r->overflow = true;
    return;
  }
  RecCall& c = r->calls[r->n];
  c.addr = addr;
  c.lr = uint32_t(ctx.lr);
  const uint64_t regs[8] = {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  std::memcpy(c.r, regs, sizeof regs);
  std::memcpy(r->pre.data() + size_t(r->n) * kDevBytes, base + r->D, kDevBytes);
}

void RecScope::End() {
  Rec* r = t_rec;
  if (!r) return;
  --r->depth;
  if (!top_ || r->overflow || r->n >= kMaxCalls) return;
  uint8_t* base = base_;
  std::memcpy(r->post.data() + size_t(r->n) * kDevBytes, base + r->D, kDevBytes);
  std::memcpy(r->post_frame.data() + size_t(r->n) * kFrame, base + r->fsp, kFrame);
  r->calls[r->n].ret = ctx_->r3.u64;
  ++r->n;
}

bool Body(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame) {
  std::call_once(g_once, [] { g_mode.Init(); });
  const int mode = g_mode.mode;
  if (mode == 0) return false;
  if (mode == 1 && g_mode.ab) {
    const uint32_t arm = (frame / g_mode.ab) & 1u;
    nr::sampler::NoteArm(arm);
    if (arm == 0) return false;
  }
  const uint64_t calls = g_mode.calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t D = ctx.r3.u32, sp = ctx.r1.u32, fsp = sp - kFrame;
  const uint64_t r4 = ctx.r4.u64;
  if (mode == 1) {
    const uint64_t sp64 = ctx.r1.u64;
    PPC_STORE_U32(fsp, sp);  // back chain, as the guest's stwu
    ctx.r1.u32 = fsp;
    GuestM m{ctx, base};
    const uint64_t f = Flush(m, D, r4, fsp);
    ctx.r1.u64 = sp64;
    ctx.r3.u64 = f;
    g_mode.Report(calls);
    return true;
  }
  // CHECK.
  static thread_local Rec* rec = new Rec();
  static thread_local std::vector<uint8_t> dev(kDevBytes), frm(kFrame), vsh(kVsHead), dch(kDeclHead), glb(kGlobBytes);
  const uint32_t vs = PPC_LOAD_U32(D + 13072), decl = PPC_LOAD_U32(D + 12240), fence_addr = PPC_LOAD_U32(D + 11024);
  std::memcpy(dev.data(), base + D, kDevBytes);
  std::memcpy(frm.data(), base + fsp, kFrame);
  if (vs) std::memcpy(vsh.data(), base + vs, kVsHead);
  if (decl) std::memcpy(dch.data(), base + decl, kDeclHead);
  std::memcpy(glb.data(), base + kGlob, kGlobBytes);
  const uint32_t fence_pre = fence_addr ? PPC_LOAD_U32(fence_addr) : 0;
  rec->D = D, rec->fsp = fsp, rec->depth = 0, rec->n = 0, rec->overflow = false;
  t_rec = rec;
  original(ctx, base);
  t_rec = nullptr;
  const uint64_t guest_f = ctx.r3.u64;
  if (rec->overflow) {
    g_mode.skipped.fetch_add(1, std::memory_order_relaxed);
    g_mode.Report(calls);
    return true;
  }
  CheckM m{base, D, fsp, vs, decl, fence_addr, dev.data(), frm.data(), vsh.data(), dch.data(), glb.data(), fence_pre, *rec};
  const uint64_t f = Flush(m, D, r4, fsp);
  if (m.skip) {
    g_mode.skipped.fetch_add(1, std::memory_order_relaxed);
    g_mode.Report(calls);
    return true;
  }
  g_mode.compared.fetch_add(1, std::memory_order_relaxed);
  g_mode.callees.fetch_add(m.next, std::memory_order_relaxed);
  g_mode.out_bytes.fetch_add(m.out.size(), std::memory_order_relaxed);
  if (m.ok && m.next != rec->n) m.Fail("native made %llu calls, guest %llu", m.next, rec->n, 0);
  int first = -1;
  for (uint32_t i = 0; i < kDevBytes && first < 0; ++i)
    if (Field(i) && base[D + i] != dev[i]) first = int(i);
  uint32_t out_bad = 0, out_first = 0;
  for (const auto& [a, v] : m.out)
    if (PPC_LOAD_U8(a) != v && !out_bad++) out_first = a;
  const bool f_bad = f != guest_f;
  if (m.stray) g_mode.stray_calls.fetch_add(1, std::memory_order_relaxed);
  const bool bad = !m.ok || first >= 0 || out_bad || f_bad;
  if (bad) g_mode.bad.fetch_add(1, std::memory_order_relaxed);
  if ((bad || m.stray) && g_mode.logged.fetch_add(1, std::memory_order_relaxed) < 20)
    REXLOG_WARN("[d3d-native-flush] CHECK mismatch r4={:X} {} first_field={} (live {:02X} native {:02X}) out={} out_bad={} "
                "first_out={:08X} mask native {:X} guest {:X} calls {}/{} stray={}",
                r4, m.ok ? "" : m.why, first, first >= 0 ? base[D + first] : 0, first >= 0 ? dev[first] : 0, m.out.size(),
                out_bad, out_first, f, guest_f, m.next, rec->n, m.stray);
  g_mode.Report(calls);
  return true;
}

}  // namespace wwe2k14::d3d::native_flush
