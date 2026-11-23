// N4: native SetPixelShader 82CB42A0 / SetVertexShader 82CB44A8, (step 2a) ShaderConstLoad 82CC1C58 (native_shader_core.h)
// and (step 2b) the inline VS upload tree 82CC1B00 (native_vsupload_core.h). ShaderConstLoad has its own switch
// WWE2K14_D3D_NATIVE_CONSTLOAD, the VS upload WWE2K14_D3D_NATIVE_VSUPLOAD (each 0 default / 1 / 2 CHECK, _AB).
// WWE2K14_D3D_NATIVE_SHADER: 0 = guest bodies, 1 = native bodies (desktop default), 2 = CHECK; _AB=N in-run A/B (mode 1 in odd
// blocks of N frames). CHECK: device copy + the 128-byte stack area below sp are taken before the guest body runs live; the
// native body then runs on the copies (other loads read guest memory: shader objects, which the body does not write) and
// every native store is compared with the live result: device bytes in the body's fields, every store outside the device
// (old shader fence word, deferred-list entry, stack scratch). A call that would need the list helper 82CBBD68 is not
// compared (counted as reserve_skips), as in N2-RT.
#include "native_shader.h"
#include "native_shader_core.h"
#include "native_vsupload_core.h"
#include "native_regemit_core.h"
#include "../nr/nr_pc_sampler.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

extern "C" PPC_FUNC(sub_82CBBD68);  // deferred-list reserve (guest)
extern "C" PPC_FUNC(sub_82CBA798);  // ring reserve (guest, hooked)
extern "C" PPC_FUNC(sub_82CBA9D0);  // ring reserve of N dwords in one segment (guest; its callees are hooked)

namespace wwe2k14::d3d::native_shader {
namespace {
constexpr uint32_t kDevBytes = 14336;
constexpr uint32_t kStack = 128;

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
  uint64_t Reserve(uint32_t dev, uint32_t lr) {
    ctx.r3.u64 = dev;
    ctx.lr = lr;
    sub_82CBBD68(ctx, base);
    return ctx.r3.u64;
  }
  uint64_t RingReserve(uint32_t dev, uint32_t lr) {
    ctx.r3.u64 = dev;
    ctx.lr = lr;
    sub_82CBA798(ctx, base);
    return ctx.r3.u64;
  }
  uint64_t Reserve9D0(uint32_t dev, uint32_t ndw, uint32_t lr) {
    ctx.r3.u64 = dev;
    ctx.r4.u64 = ndw;
    ctx.lr = lr;
    sub_82CBA9D0(ctx, base);
    return ctx.r3.u64;
  }
  // The guest memcpy 82C7C8F0 on guest memory (big-endian bytes copied as bytes); returns dst like the guest.
  uint64_t Copy(uint32_t dst, uint32_t src, uint32_t n, uint32_t) {
    std::memmove(base + dst, base + src, n);
    return dst;
  }
};

// Device fields the bodies write (CHECK compares only these; other threads write other device words).
bool InShaderTree(uint32_t i) {
  return i < 40 || (i - 1152u) < 768u || i == 11070 || (i - 13068u) < 8u || (i - 13928u) < 4u;
}

struct CopyM {
  uint8_t* base;
  uint32_t D;
  uint8_t* dev;       // kDevBytes copy (guest byte order)
  uint32_t stack_lo;  // sp - kStack
  uint8_t* stack;     // kStack copy
  std::unordered_map<uint32_t, uint8_t> out;  // byte stores outside the device (final value per address; reads see them)
  bool (*in_tree)(uint32_t);
  uint32_t stray = 0;
  bool ok = true;
  uint8_t* Ptr(uint32_t a) {
    if (a >= D && a - D < kDevBytes) return dev + (a - D);
    if (a >= stack_lo && a - stack_lo < kStack) return stack + (a - stack_lo);
    return nullptr;
  }
  uint8_t B(uint32_t a) {
    if (uint8_t* p = Ptr(a)) return *p;
    if (!out.empty())
      if (auto it = out.find(a); it != out.end()) return it->second;
    return PPC_LOAD_U8(a);
  }
  uint8_t L8(uint32_t a) { return B(a); }
  uint16_t L16(uint32_t a) { return uint16_t(B(a) << 8 | B(a + 1)); }
  uint32_t L32(uint32_t a) { return uint32_t(L16(a)) << 16 | L16(a + 2); }
  uint64_t L64(uint32_t a) { return uint64_t(L32(a)) << 32 | L32(a + 4); }
  void SB(uint32_t a, uint8_t v) {
    if (a >= D && a - D < kDevBytes) {
      dev[a - D] = v;
      if (!in_tree(a - D)) ++stray;
      return;
    }
    if (uint8_t* p = Ptr(a)) *p = v;
    out[a] = v;
  }
  void S8(uint32_t a, uint8_t v) { SB(a, v); }
  void S32(uint32_t a, uint32_t v) {
    for (int i = 0; i < 4; ++i) SB(a + i, uint8_t(v >> (24 - 8 * i)));
  }
  void S64(uint32_t a, uint64_t v) {
    S32(a, uint32_t(v >> 32));
    S32(a + 4, uint32_t(v));
  }
  uint64_t Reserve(uint32_t, uint32_t) {
    ok = false;
    return 0;
  }
  uint64_t RingReserve(uint32_t, uint32_t) {
    ok = false;
    return 0;
  }
  uint64_t Reserve9D0(uint32_t, uint32_t, uint32_t) {
    ok = false;
    return 0;
  }
  uint64_t Copy(uint32_t dst, uint32_t src, uint32_t n, uint32_t) {
    for (uint32_t i = 0; i < n; ++i) SB(dst + i, B(src + i));
    return dst;
  }
};

bool InConstLoadTree(uint32_t i) { return (i - 48u) < 4u; }  // ring cursor only; the packets are outside the device
// Register-run writer: ring cursor only (its packets are outside the device; the short-segment reserve aborts the compare).
bool InRegEmitTree(uint32_t i) { return (i - 48u) < 4u; }
// VS upload: ring cursor, byte 11070, the copies D+12264..12279 (the code and packets are outside the device).
bool InVsUploadTree(uint32_t i) { return (i - 48u) < 4u || i == 11070 || (i - 12264u) < 16u; }

#if defined(__ANDROID__) || defined(REX_UWP)
constexpr int kShaderDefault = 0, kConstLoadDefault = 0;
constexpr int kVsUploadDefault = 0;
constexpr int kRegEmitDefault = 0;
#else
// SetPixelShader/SetVertexShader default ON on desktop PC (2026-11-18 after n4-check PASS: 9,437,184 calls, 0 mismatches,
// 0 stray; n4-speed in-run A/B 600 s: game thread 4.96 -> 4.96 ms/f, no clear difference). WWE2K14_D3D_NATIVE_SHADER=0
// restores the guest bodies. ShaderConstLoad (N4 step 2a) default ON on desktop since 11-18 night: CHECK clean (N4 2a),
// combined in-run A/B dscl-ab-1 (with native SetDepthStencil, 300 s in ring, lane A, Qwen stopped): all threads -0.02 ms/f,
// new faster in 6 of 14 pairs, "no clear difference" (the earlier +0.2 readings were record-thread noise).
// WWE2K14_D3D_NATIVE_CONSTLOAD=0 restores the guest body.
constexpr int kShaderDefault = 1, kConstLoadDefault = 1;
// Inline VS upload tree (N4 step 2b) default ON on desktop PC (2026-11-18 after n4b-check PASS: 1,048,576 uploads, 0 mismatches,
// 0 stray; n4b-speed game thread 4.99 -> 4.99 ms/f, n4b-prof -0.15; the record-thread +0.16 of n4b-speed was -0.13 on the
// repeat and absent in the per-arm profile). WWE2K14_D3D_NATIVE_VSUPLOAD=0 restores the guest body.
constexpr int kVsUploadDefault = 1;
// Register-run writer (N4 step 3) default ON on desktop PC (2026-11-18 after n4r-check PASS: 2,359,296 calls, 0 mismatches,
// 0 skips; n4r-speed in-run A/B 600 s: game 4.94 -> 4.96 ms/f, worker +0.01, record -0.08, all threads -0.05).
// WWE2K14_D3D_NATIVE_REGEMIT=0 restores the guest body.
constexpr int kRegEmitDefault = 1;
#endif

struct Mode {
  const char* tag;
  const char* env;
  const char* env_ab;
  int default_mode;
  bool (*in_tree)(uint32_t);
  uint32_t frame_bytes;  // the guest body's stack frame (stwu r1,-N(r1))
  int mode = -1;
  uint32_t ab = 0;
  std::atomic<uint64_t> calls{0}, compared{0}, reserve_skips{0}, bad{0}, out_bytes{0}, stray_calls{0}, logged{0};
  void Init() {
    const char* e = std::getenv(env);
    mode = !e || !*e ? default_mode : *e == '1' ? 1 : *e == '2' ? 2 : 0;
    const char* a = std::getenv(env_ab);
    const long n = a && *a ? std::strtol(a, nullptr, 10) : 0;
    ab = n > 0 && n <= 1000000 ? uint32_t(n) : 0u;
    if (mode) REXLOG_INFO("[{}] {} (native body) ab={}", tag, mode == 2 ? "CHECK" : "ON", ab);
  }
  void Report(uint64_t n) {
    if ((n & 0x3FFFFu) != 0) return;
    REXLOG_INFO("[{}] {} calls={} compared={} reserve_skips={} out_bytes={} mismatch_calls={} mismatch_stray={}", tag,
                mode == 2 ? "CHECK" : "ON", n, compared.load(), reserve_skips.load(), out_bytes.load(), bad.load(),
                stray_calls.load());
  }
};
Mode g_shader{"d3d-native-shader", "WWE2K14_D3D_NATIVE_SHADER", "WWE2K14_D3D_NATIVE_SHADER_AB", kShaderDefault,
              InShaderTree, 128};
Mode g_constload{"d3d-native-constload", "WWE2K14_D3D_NATIVE_CONSTLOAD", "WWE2K14_D3D_NATIVE_CONSTLOAD_AB",
                 kConstLoadDefault, InConstLoadTree, 144};
Mode g_regemit{"d3d-native-regemit", "WWE2K14_D3D_NATIVE_REGEMIT", "WWE2K14_D3D_NATIVE_REGEMIT_AB", kRegEmitDefault,
               InRegEmitTree, 144};
Mode g_vsupload{"d3d-native-vsupload", "WWE2K14_D3D_NATIVE_VSUPLOAD", "WWE2K14_D3D_NATIVE_VSUPLOAD_AB", kVsUploadDefault,
                InVsUploadTree, 192};

// One call: guest body (false), native body, or CHECK (device + stack copies -> guest body live -> native on the copies).
template <class Tree>
bool Run(Mode& md, std::once_flag& once, PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*),
         uint32_t frame, const char* what, Tree tree) {
  std::call_once(once, [&] { md.Init(); });
  const int mode = md.mode;
  if (mode == 0) return false;
  if (mode == 1 && md.ab) {
    const uint32_t arm = (frame / md.ab) & 1u;
    nr::sampler::NoteArm(arm);  // sampler ALL mode splits CPU by arm (no-op unless the sampler runs)
    if (arm == 0) return false;  // even block: guest body (cost-ab.py rule)
  }
  const uint64_t calls = md.calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t D = ctx.r3.u32, sp = ctx.r1.u32;
  const uint64_t a4 = ctx.r4.u64;
  if (mode == 1) {
    // Same frame as the guest body so a helper's frame lands where it does under the guest.
    const uint64_t sp64 = ctx.r1.u64;
    PPC_STORE_U32(sp - md.frame_bytes, sp);
    ctx.r1.u32 = sp - md.frame_bytes;
    GuestM m{ctx, base};
    const uint64_t r3 = tree(m, sp);
    ctx.r1.u64 = sp64;
    ctx.r3.u64 = r3;
    md.Report(calls);
    return true;
  }
  uint8_t dev[kDevBytes], stack[kStack];
  std::memcpy(dev, base + D, kDevBytes);
  std::memcpy(stack, base + sp - kStack, kStack);
  // tree() reads its inputs from ctx before the guest body changes them: capture them first.
  PPCContext in = ctx;
  original(ctx, base);
  const uint64_t guest_r3 = ctx.r3.u64;
  CopyM m{base, D, dev, sp - kStack, stack, {}, md.in_tree};
  const uint64_t r3 = tree.check(m, in, sp);
  if (!m.ok) {
    md.reserve_skips.fetch_add(1, std::memory_order_relaxed);
    md.Report(calls);
    return true;
  }
  md.compared.fetch_add(1, std::memory_order_relaxed);
  md.out_bytes.fetch_add(m.out.size(), std::memory_order_relaxed);
  int first = -1;
  for (uint32_t i = 0; i < kDevBytes && first < 0; ++i)
    if (md.in_tree(i) && base[D + i] != dev[i]) first = int(i);
  uint32_t out_bad = 0, out_first = 0;
  for (const auto& [a, v] : m.out)
    if (PPC_LOAD_U8(a) != v && !out_bad++) out_first = a;
  const bool r3_bad = r3 != guest_r3;
  if (m.stray) md.stray_calls.fetch_add(1, std::memory_order_relaxed);
  if (first >= 0 || out_bad || r3_bad) md.bad.fetch_add(1, std::memory_order_relaxed);
  if ((first >= 0 || out_bad || r3_bad || m.stray) && md.logged.fetch_add(1, std::memory_order_relaxed) < 20)
    REXLOG_WARN("[{}] CHECK mismatch {} r4={:08X} first_device_byte={} (live {:02X} native {:02X}) out_bytes={} out_bad={} "
                "first_out={:08X} r3 native {:X} guest {:X} stray={}",
                md.tag, what, uint32_t(a4), first, first >= 0 ? base[D + first] : 0, first >= 0 ? dev[first] : 0,
                m.out.size(), out_bad, out_first, r3, guest_r3, m.stray);
  md.Report(calls);
  return true;
}

// Tree adaptors: operator() for the live path (reads ctx), check() for CHECK (reads the saved entry registers).
struct ShaderTree {
  PPCContext& ctx;
  bool vs;
  template <class M> uint64_t operator()(M& m, uint32_t sp) { return SetShader(m, vs, ctx.r3.u32, ctx.r3.u64, ctx.r4.u64, sp); }
  template <class M> uint64_t check(M& m, const PPCContext& in, uint32_t sp) {
    return SetShader(m, vs, in.r3.u32, in.r3.u64, in.r4.u64, sp);
  }
};
struct ConstLoadTree {
  PPCContext& ctx;
  template <class M> uint64_t operator()(M& m, uint32_t) { return ConstLoad(m, ctx.r3.u32, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64); }
  template <class M> uint64_t check(M& m, const PPCContext& in, uint32_t) {
    return ConstLoad(m, in.r3.u32, in.r3.u64, in.r4.u64, in.r5.u64);
  }
};
struct RegEmitTree {
  PPCContext& ctx;
  template <class M> uint64_t operator()(M& m, uint32_t) { return RegEmit(m, ctx.r3.u32, ctx.r4.u64, ctx.r5.u64, ctx.r6.u32); }
  template <class M> uint64_t check(M& m, const PPCContext& in, uint32_t) {
    return RegEmit(m, in.r3.u32, in.r4.u64, in.r5.u64, in.r6.u32);
  }
};
struct VsUploadTree {
  PPCContext& ctx;
  template <class M> uint64_t operator()(M& m, uint32_t) { return Call(m, ctx); }
  template <class M> uint64_t check(M& m, const PPCContext& in, uint32_t) { return Call(m, in); }
  template <class M> static uint64_t Call(M& m, const PPCContext& c) {
    return VSUpload(m, c.r3.u32, c.r4.u64, c.r5.u32, c.r6.u32, c.r7.u32, c.r8.u32, c.r9.u32, c.r10.u32);
  }
};
}  // namespace

bool Body(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame, bool vs) {
  static std::once_flag once;
  return Run(g_shader, once, ctx, base, original, frame, vs ? "VS" : "PS", ShaderTree{ctx, vs});
}

bool BodyConstLoad(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame) {
  static std::once_flag once;
  return Run(g_constload, once, ctx, base, original, frame, "constload", ConstLoadTree{ctx});
}

}  // namespace wwe2k14::d3d::native_shader

namespace wwe2k14::d3d::native_shader {
bool BodyVSUpload(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame) {
  static std::once_flag once;
  std::call_once(once, [] { g_vsupload.Init(); });
  if (g_vsupload.mode == 0) return false;
  // A fetch count above the guest's own buffer would corrupt the guest stack (never in a working game): guest body.
  GuestM g{ctx, base};
  if (vsup::FetchCount(g, ctx.r5.u32, ctx.r10.u32) > vsup::kMaxFetch) return false;
  return Run(g_vsupload, once, ctx, base, original, frame, "vsupload", VsUploadTree{ctx});
}
}  // namespace wwe2k14::d3d::native_shader

namespace wwe2k14::d3d::native_shader {
bool BodyRegEmit(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame) {
  if (ctx.r4.u64 == 0) return false;  // the guest loops forever on a zero mask; never passed - keep the guest's behaviour
  static std::once_flag once;
  return Run(g_regemit, once, ctx, base, original, frame, "regemit", RegEmitTree{ctx});
}
}  // namespace wwe2k14::d3d::native_shader
