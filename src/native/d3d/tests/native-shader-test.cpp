// N4 host test: native SetPixelShader / SetVertexShader (native_shader_core.h) against the REAL generated guest bodies,
// copied by tools/nr/gen-shader-ref.py into out/n4/shader_ref.inc (list helper 82CBBD68 and memcpy 82C7C8F0 routed to
// stubs both runs share). Random devices, old/new shaders, fences, deferred-list states and patch tables; compares the
// device block, shader objects, list regions, the stack scratch below sp and the final r3. Build: tools/nr/native-shader-test.sh
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "native/d3d/native_shader_core.h"

namespace {
constexpr uint32_t D = 0x30000000u, kDev = 14336, SHD = 0x40000000u, kShd = 0x10000, LIST = 0x50000000u,
                   LIST2 = 0x51000000u, kList = 0x1000, SP = 0x7F000000u;
uint8_t* g_base;
std::mt19937_64 rng(4321);
uint32_t R(uint32_t n) { return uint32_t(rng() % n); }
}  // namespace

static void Stub(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  if (addr == 0x82CBA798u) {  // ring reserve: new segment, new limit
    PPC_STORE_U32(ctx.r3.u32 + 56, 0x52000000u + 0x800);
    ctx.r3.u64 = 0x52000000u + 4 * (PPC_LOAD_U32(ctx.r3.u32 + 48) & 7);
    return;
  }
  if (addr == 0x82CBBD68u) {  // deferred-list reserve: new block
    PPC_STORE_U32(ctx.r3.u32 + 13932, LIST2 + 0x800);
    ctx.r3.u64 = LIST2 + 8 * (ctx.lr & 7);
    return;
  }
  // memcpy: returns dst
  std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32);
}
#include "shader_ref.inc"

namespace {
struct TestM {
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
    Stub(0x82CBBD68u, ctx, base);
    return ctx.r3.u64;
  }
  uint64_t RingReserve(uint32_t dev, uint32_t lr) {
    ctx.r3.u64 = dev;
    ctx.lr = lr;
    Stub(0x82CBA798u, ctx, base);
    return ctx.r3.u64;
  }
  uint64_t Copy(uint32_t dst, uint32_t src, uint32_t n, uint32_t) {
    std::memmove(base + dst, base + src, n);
    return dst;
  }
};

void W8(uint32_t a, uint8_t v) { uint8_t* base = g_base; PPC_STORE_U8(a, v); }
void W16(uint32_t a, uint16_t v) { uint8_t* base = g_base; PPC_STORE_U16(a, v); }
void W32(uint32_t a, uint32_t v) { uint8_t* base = g_base; PPC_STORE_U32(a, v); }
void W64(uint32_t a, uint64_t v) { uint8_t* base = g_base; PPC_STORE_U64(a, v); }

// Shader object k at SHD + 0x1000 k; its patch table at info + rel (info = +40 PS, +872 VS).
void BuildShader(uint32_t S, bool vs) {
  for (uint32_t i = 0; i < 0x1000; i += 4) W32(S + i, uint32_t(rng()));
  const uint32_t info = S + (vs ? 872u : 40u);
  if (R(6) == 0) { W32(info + 20, 0); return; }
  const uint32_t rel = 32 + 4 * R(16);
  W32(info + 20, rel);
  const uint32_t t = info + rel;
  W64(t, R(3) == 0 ? 0 : uint64_t(rng()));
  W64(t + 8, R(2) ? 0 : uint64_t(rng()));
  uint32_t p = t + 20;
  const uint32_t nskip = R(3);
  for (uint32_t i = 0; i < nskip; ++i) { W16(p, uint16_t(rng())); W16(p + 2, uint16_t(1 + R(9))); W32(p + 4, uint32_t(rng())); p += 8; }
  W32(p, 0); p += 4;  // end of skipped run
  const uint32_t ncopy = R(4);
  for (uint32_t i = 0; i < ncopy; ++i) {
    const uint32_t c = 1 + R(12);
    W16(p, uint16_t(4 * R(160))); W16(p + 2, uint16_t(c)); p += 4;
    for (uint32_t k = 0; k < c; ++k) { W32(p, uint32_t(rng())); p += 4; }
  }
  W32(p, 0); p += 4;  // end of copies
  const uint32_t nmask = R(4);
  for (uint32_t i = 0; i < nmask; ++i) {
    const uint32_t c = 2 * (1 + R(8));
    W16(p, uint16_t(4 * R(160))); W16(p + 2, uint16_t(c)); p += 4;
    for (uint32_t k = 0; k < c / 2; ++k) { W32(p, uint32_t(rng())); W32(p + 4, uint32_t(rng())); p += 8; }
  }
  if (R(2)) { W32(p, 0); p += 4; }
  uint32_t len = p - (t + 20);
  if (R(8) == 0) len = 4 * R(len / 4 + 1);  // truncated table: every loop's end test
  W32(t + 16, len);
}

struct Snap {
  std::vector<uint8_t> dev = std::vector<uint8_t>(kDev), shd = std::vector<uint8_t>(kShd), l1 = std::vector<uint8_t>(kList),
                       l2 = std::vector<uint8_t>(kList), st = std::vector<uint8_t>(512),
                       r1 = std::vector<uint8_t>(0x1000), r2 = std::vector<uint8_t>(0x1000);
  void Save(uint8_t* b) {
    std::memcpy(dev.data(), b + D, kDev), std::memcpy(shd.data(), b + SHD, kShd);
    std::memcpy(l1.data(), b + LIST, kList), std::memcpy(l2.data(), b + LIST2, kList);
    std::memcpy(st.data(), b + SP - 256, 512);
    std::memcpy(r1.data(), b + 0x51800000u, 0x1000), std::memcpy(r2.data(), b + 0x52000000u, 0x1000);
  }
  void Load(uint8_t* b) const {
    std::memcpy(b + D, dev.data(), kDev), std::memcpy(b + SHD, shd.data(), kShd);
    std::memcpy(b + LIST, l1.data(), kList), std::memcpy(b + LIST2, l2.data(), kList);
    std::memcpy(b + SP - 256, st.data(), 512);
    std::memcpy(b + 0x51800000u, r1.data(), 0x1000), std::memcpy(b + 0x52000000u, r2.data(), 0x1000);
  }
};
}  // namespace

int main(int argc, char** argv) {
  const long cases = argc > 1 ? std::atol(argv[1]) : 200000;
  void* mm = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mm == MAP_FAILED) return std::perror("mmap"), 2;
  g_base = static_cast<uint8_t*>(mm);
  static Snap before, ref, nat;
  long bad = 0, released = 0, listed = 0, reserved = 0, constloads = 0, constpk = 0;
  for (long c = 0; c < cases; ++c) {
    const int kind = R(3);  // 0 PS, 1 VS, 2 const load
    const bool vs = kind == 1;
    for (uint32_t i = 0; i < kDev; i += 4) W32(D + i, uint32_t(rng()));
    for (uint32_t k = 0; k < 8; ++k) BuildShader(SHD + 0x1000 * k, vs);
    const uint32_t oldsh = R(3) == 0 ? 0 : SHD + 0x1000 * R(8);
    W32(D + (vs ? 13072 : 13068), oldsh);
    W32(D + 11036, R(2) ? 0 : uint32_t(rng()));
    W32(D + 11040, R(3) == 0 ? 0 : uint32_t(rng()));
    W32(D + 13928, LIST + 8 * R(64));
    W32(D + 13932, R(3) == 0 ? LIST + 8 * R(64) : LIST + 0x800);
    for (uint32_t i = 0; i < 512; i += 4) W32(SP - 256 + i, uint32_t(rng()));
    W32(D + 48, 0x51800000u + 4 * R(64));
    W32(D + 56, R(3) == 0 ? 0x51800000u + 4 * R(64) : 0x51800000u + 0x800);
    const uint64_t S = R(5) == 0 ? 0 : SHD + 0x1000 * R(8);
    if (oldsh) ++released;
    before.Save(g_base);
    PPCContext ctx{};
    ctx.r1.u64 = SP;
    ctx.r3.u64 = D, ctx.r4.u64 = S;
    const uint64_t info = SHD + 0x1000 * R(8) + (R(2) ? 40u : 872u), r5 = uint32_t(rng());
    if (kind == 2) {
      ctx.r4.u64 = info, ctx.r5.u64 = r5;
      ref_sub_82CC1C58(ctx, g_base);
    } else if (vs) {
      ref_sub_82CB44A8(ctx, g_base);
    } else {
      ref_sub_82CB42A0(ctx, g_base);
    }
    const uint64_t ref_r3 = ctx.r3.u64;
    ref.Save(g_base);
    if (std::memcmp(ref.l2.data(), before.l2.data(), kList)) ++reserved;
    if (std::memcmp(ref.l1.data(), before.l1.data(), kList) || std::memcmp(ref.l2.data(), before.l2.data(), kList)) ++listed;
    before.Load(g_base);
    PPCContext nctx{};
    nctx.r1.u64 = SP - 128;  // the native body's helper calls run in the guest frame
    TestM tm{nctx, g_base};
    const uint64_t nat_r3 = kind == 2 ? wwe2k14::d3d::native_shader::ConstLoad(tm, D, D, info, r5)
                                      : wwe2k14::d3d::native_shader::SetShader(tm, vs, D, D, S, SP);
    nat.Save(g_base);
    // Stack: compare only the two scratch words the body itself writes (sp-48, sp-44); the guest prologue's register
    // saves and the stwu back chain are frame bookkeeping, not body output.
    // (ShaderConstLoad writes no stack scratch; its prologue saves r25..r31 over that slot, so it is not compared there.)
    const bool st_ok = kind == 2 || !std::memcmp(nat.st.data() + 256 - 48, ref.st.data() + 256 - 48, 8);
    const bool ok = nat.dev == ref.dev && nat.shd == ref.shd && nat.l1 == ref.l1 && nat.l2 == ref.l2 && st_ok &&
                    nat.r1 == ref.r1 && nat.r2 == ref.r2 && nat_r3 == ref_r3;
    if (kind == 2) ++constloads;
    if (kind == 2 && nat.r1 != before.r1) ++constpk;
    if (!ok && ++bad <= 10) {
      int where = -1;
      for (uint32_t i = 0; i < kDev && where < 0; ++i) if (nat.dev[i] != ref.dev[i]) where = int(i);
      std::printf("MISMATCH case %ld %s S=%llX old=%X dev_byte=%d shd=%d lists=%d stack=%d r3 ref %llX nat %llX\n", c,
                  vs ? "VS" : "PS", (unsigned long long)S, oldsh, where, int(nat.shd == ref.shd),
                  int(nat.l1 == ref.l1 && nat.l2 == ref.l2), int(st_ok), (unsigned long long)ref_r3,
                  (unsigned long long)nat_r3);
    }
  }
  std::printf("native-shader-test: %ld cases, %ld mismatches (old shader %ld, list entry %ld, list reserve %ld, const loads %ld "
              "with packets %ld)\n", cases, bad, released, listed, reserved, constloads, constpk);
  return bad ? 1 : 0;
}
