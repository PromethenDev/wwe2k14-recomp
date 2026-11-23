// N2-RT: native SetRenderTarget tree (docs/tasks/N2-RT-native.md). WWE2K14_D3D_NATIVE_RT: 0 = guest body,
// 1 = native body (default on desktop), 2 = CHECK (device copy -> guest body runs live -> native body on the copy -> compare the device bytes,
// every ring word the native body wrote and the final ring pointer). Native calls of the ring reserve sub_82CBA798 set the
// same return address as the guest call sites; in CHECK a native reserve aborts the comparison (counted, not compared).
#include "native_rt.h"
#include "native_rt_core.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

extern "C" PPC_FUNC(sub_82CBA798);  // ring reserve (hooked in hooks.cpp; the guest calls this symbol too)

namespace wwe2k14::d3d::native_rt {
namespace {
constexpr uint32_t kDevBytes = 14336;  // every field the tree touches lies below this (same span as native_setters CHECK)

struct GuestM {
  PPCContext& ctx;
  uint8_t* base;
  bool ok = true;
  uint32_t reserve_site = 0x82CAC5F8u;
  uint8_t L8(uint32_t a) { return PPC_LOAD_U8(a); }
  uint16_t L16(uint32_t a) { return PPC_LOAD_U16(a); }
  uint32_t L32(uint32_t a) { return PPC_LOAD_U32(a); }
  uint64_t L64(uint32_t a) { return PPC_LOAD_U64(a); }
  void S8(uint32_t a, uint8_t v) { PPC_STORE_U8(a, v); }
  void S32(uint32_t a, uint32_t v) { PPC_STORE_U32(a, v); }
  void S64(uint32_t a, uint64_t v) { PPC_STORE_U64(a, v); }
  uint32_t Reserve(uint32_t dev) {
    ctx.r3.u64 = dev;
    ctx.lr = reserve_site;  // guest return address of the call (0x82CAC5F8 entry, 0x82CAC800 in the tile loop)
    sub_82CBA798(ctx, base);
    reserve_site = 0x82CAC800u;
    return ctx.r3.u32;
  }
};

// CHECK policy: device bytes come from (and go to) a copy taken before the guest body ran; other loads read guest memory
// (surface objects, game constants: the guest body does not write them); other stores are recorded (ring words).
struct CopyM {
  uint8_t* base;
  uint32_t D;
  uint8_t* dev;  // kDevBytes, guest byte order
  std::vector<std::pair<uint32_t, uint32_t>> ring;
  uint32_t stray = 0;  // byte stores outside the device (none expected): reported as a mismatch
  uint8_t wrote[kDevBytes] = {};  // device bytes the native tree stored (recorded at the store, no snapshot diff)
  bool ok = true;
  bool In(uint32_t a, uint32_t n) const { return a >= D && a - D + n <= kDevBytes; }
  uint8_t L8(uint32_t a) { return In(a, 1) ? dev[a - D] : PPC_LOAD_U8(a); }
  uint16_t L16(uint32_t a) {
    if (!In(a, 2)) return PPC_LOAD_U16(a);
    return uint16_t(dev[a - D] << 8 | dev[a - D + 1]);
  }
  uint32_t L32(uint32_t a) {
    if (!In(a, 4)) return PPC_LOAD_U32(a);
    const uint8_t* p = dev + (a - D);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  }
  uint64_t L64(uint32_t a) { return uint64_t(L32(a)) << 32 | L32(a + 4); }
  void S8(uint32_t a, uint8_t v) {
    if (In(a, 1)) dev[a - D] = v, wrote[a - D] = 1;
    else ++stray;
  }
  void S32(uint32_t a, uint32_t v) {
    if (!In(a, 4)) {
      ring.emplace_back(a, v);
      return;
    }
    uint8_t* p = dev + (a - D);
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
    std::memset(wrote + (a - D), 1, 4);
  }
  void S64(uint32_t a, uint64_t v) {
    S32(a, uint32_t(v >> 32));
    S32(a + 4, uint32_t(v));
  }
  uint32_t Reserve(uint32_t) {
    ok = false;
    return 0;
  }
};

// Device bytes each guest tree can write (docs/tasks/N2-RT-native.md maps). Other threads write other device fields (fence
// words near 11032, run n2rt-check), so CHECK compares only these and reports native writes outside them separately.
struct Span { uint32_t off, len; };
// Shared by both trees: the surface-state / viewport / scissor subtree (sub_82CAF558 ...) and the dirty words.
constexpr Span kSubtreeWrites[] = {
    {16, 8},     {32, 8},     {48, 4},                    // dirty words, ring pointer
    {10368, 4},  {10436, 8},  {10504, 24}, {10684, 4},    // rs 0x2000, scissor regs, viewport regs, 10684
    {11068, 1},  {13024, 44},                             // flag byte, viewport floats + scissor copy 13052..13064
};
constexpr Span kRtWrites[] = {
    {10372, 20}, {10460, 4}, {11071, 1}, {12292, 16}, {12408, 16}, {12816, 16},  // SetRenderTarget's own fields
};
constexpr Span kDsWrites[] = {
    {10376, 4}, {10548, 4}, {10560, 4}, {11070, 1}, {12308, 8}, {12832, 4},  // SetDepthStencil's own fields
};
template <size_t N>
bool InSpans(const Span (&spans)[N], uint32_t i) {
  for (const Span& sp : spans)
    if (i - sp.off < sp.len) return true;
  return false;
}

#if defined(__ANDROID__) || defined(REX_UWP)
constexpr bool kDesktop = false;
#else
constexpr bool kDesktop = true;
#endif

// One native setter: its switch, counters and CHECK field list.
struct Setter {
  const char* tag;       // log tag
  const char* env;       // mode switch
  const char* env_ab;    // in-run A/B switch
  int default_mode;
  bool (*in_tree)(uint32_t);
  int mode = -1;
  uint32_t ab = 0;
  std::atomic<uint64_t> calls{0}, compared{0}, reserve_skips{0}, bad{0}, ring_words{0}, stray_calls{0}, logged{0};
  void Init() {
    if (mode >= 0) return;
    const char* e = std::getenv(env);
    mode = !e || !*e ? default_mode : *e == '1' ? 1 : *e == '2' ? 2 : 0;
    const char* a = std::getenv(env_ab);
    const long n = a && *a ? std::strtol(a, nullptr, 10) : 0;
    ab = n > 0 && n <= 1000000 ? uint32_t(n) : 0u;
    if (mode) REXLOG_INFO("[{}] {} (native body) ab={}", tag, mode == 2 ? "CHECK" : "ON", ab);
  }
  void Report(uint64_t n) {
    if ((n & 0xFFFFu) != 0) return;
    REXLOG_INFO("[{}] {} calls={} compared={} reserve_skips={} ring_words={} mismatch_calls={} mismatch_stray={}", tag,
                mode == 2 ? "CHECK" : "ON", n, compared.load(), reserve_skips.load(), ring_words.load(), bad.load(),
                stray_calls.load());
  }
};

bool InRt(uint32_t i) { return InSpans(kSubtreeWrites, i) || InSpans(kRtWrites, i); }
bool InDs(uint32_t i) { return InSpans(kSubtreeWrites, i) || InSpans(kDsWrites, i); }

// SetRenderTarget default ON on desktop PC (owner 2026-11-18 after n2rt-check4 PASS: 1,048,576 calls, 0 mismatches, 0 stray,
// 3.76 M ring words, picture normal; n2rt-speed in-run A/B: game -0.09, worker -0.07 ms/f, 20 of 25 pairs faster).
// SetDepthStencil default ON on desktop since 11-18 night (N2-DS CHECK 851,968 calls 0 / 0; combined in-run A/B dscl-ab-1:
// all threads -0.02 ms/f, no clear difference). WWE2K14_D3D_NATIVE_DS=0 restores the guest body. Android / Xbox app: guest.
Setter g_rt{"d3d-native-rt", "WWE2K14_D3D_NATIVE_RT", "WWE2K14_D3D_NATIVE_RT_AB", kDesktop ? 1 : 0, InRt};
Setter g_ds{"d3d-native-ds", "WWE2K14_D3D_NATIVE_DS", "WWE2K14_D3D_NATIVE_DS_AB", kDesktop ? 1 : 0, InDs};

// Runs one setter call: guest body (false), native body, or CHECK (device copy -> guest body live -> native on the copy).
template <class Tree, class What>
bool Run(Setter& st, PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame, What what,
         Tree tree) {
  static std::once_flag once;
  std::call_once(once, [&] { st.Init(); });
  const int mode = st.mode;
  if (mode == 0) return false;
  if (mode == 1 && st.ab && ((frame / st.ab) & 1u) == 0) return false;  // even block: guest body (cost-ab.py rule)
  const uint32_t D = ctx.r3.u32;
  const uint64_t calls = st.calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (mode == 1) {
    GuestM m{ctx, base};
    tree(m);
    st.Report(calls);
    return true;
  }
  // CHECK. One read of the device before the guest body (n2rt-check3: a second copy 'pre = dev' was compiled as a second
  // read of the live device, so other threads' writes showed up as false 'stray' bytes).
  uint8_t dev[kDevBytes];
  std::memcpy(dev, PPC_RAW_ADDR(D), kDevBytes);
  original(ctx, base);
  CopyM m{base, D, dev};
  tree(m);
  if (!m.ok) {
    st.reserve_skips.fetch_add(1, std::memory_order_relaxed);
  } else {
    st.compared.fetch_add(1, std::memory_order_relaxed);
    st.ring_words.fetch_add(m.ring.size(), std::memory_order_relaxed);
    uint8_t live[kDevBytes];
    std::memcpy(live, PPC_RAW_ADDR(D), kDevBytes);  // one snapshot: compare and log the same bytes
    int first = -1;
    for (uint32_t i = 0; i < kDevBytes && first < 0; ++i)
      if (live[i] != dev[i] && st.in_tree(i)) first = int(i);
    int stray_first = -1;  // native writes outside the tree's fields (recorded at the store)
    for (uint32_t i = 0; i < kDevBytes; ++i)
      if (!st.in_tree(i) && m.wrote[i]) {
        if (stray_first < 0) stray_first = int(i);
        ++m.stray;
      }
    uint32_t ring_bad = 0, ring_first = 0, ring_want = 0, ring_have = 0;
    for (const auto& [a, v] : m.ring) {
      const uint32_t now = PPC_LOAD_U32(a);
      if (now != v) {
        if (!ring_bad) ring_first = a, ring_want = v, ring_have = now;
        ++ring_bad;
      }
    }
    if (m.stray) st.stray_calls.fetch_add(1, std::memory_order_relaxed);
    if (first >= 0 || ring_bad) st.bad.fetch_add(1, std::memory_order_relaxed);
    if ((first >= 0 || ring_bad || m.stray) && st.logged.fetch_add(1, std::memory_order_relaxed) < 20) {
      char wbuf[48];
      what(wbuf, sizeof wbuf);
      REXLOG_WARN("[{}] CHECK mismatch {} first_device_byte={} (live {:02X} native {:02X}) ring_words={} ring_bad={} first "
                  "ring {:08X} native {:08X} live {:08X} stray={} first_stray={}",
                  st.tag, wbuf, first, first >= 0 ? live[first] : 0, first >= 0 ? dev[first] : 0, m.ring.size(), ring_bad,
                  ring_first, ring_want, ring_have, m.stray, stray_first);
    }
  }
  st.Report(calls);
  return true;
}
}  // namespace

bool Body(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame) {
  const uint32_t D = ctx.r3.u32, index = ctx.r4.u32, S = ctx.r5.u32;
  return Run(g_rt, ctx, base, original, frame,
             [&](char* b, size_t n) { std::snprintf(b, n, "index=%u surface=%08X", index, S); },
             [&](auto& m) { SetRenderTarget(m, D, index, S); });
}

bool BodyDs(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame) {
  const uint32_t D = ctx.r3.u32, Z = ctx.r4.u32;
  return Run(g_ds, ctx, base, original, frame, [&](char* b, size_t n) { std::snprintf(b, n, "depth=%08X", Z); },
             [&](auto& m) { SetDepthStencil(m, D, Z); });
}

}  // namespace wwe2k14::d3d::native_rt
