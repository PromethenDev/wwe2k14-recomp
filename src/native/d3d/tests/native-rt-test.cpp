// N2-RT host test: the native SetRenderTarget tree (native_rt_core.h) against the REAL generated guest bodies, copied by
// tools/nr/gen-rt-ref.py into out/n2rt/rt_ref.inc. Random device / surface / default-viewport states; compares the 14 KB
// device block and the ring bytes after both runs. Build (from the repo root, after gen-rt-ref.py):
//   tools/nr/native-rt-test.sh
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "native/d3d/native_rt_core.h"
#include "rt_ref.inc"

namespace {
constexpr uint32_t D = 0x30000000u, kDev = 14336, RING = 0x50000000u, RING2 = 0x51000000u, kRing = 0x10000;
constexpr uint32_t SURF = 0x40000000u;
uint8_t* g_base;

// Stub ring reserve: switch to a second buffer with a new limit (both runs see the same behaviour).
uint32_t StubReserve(uint8_t* base, uint32_t dev) {
  PPC_STORE_U32(dev + 56, RING2 + 0x8000);
  return RING2;
}
}  // namespace
static void ref_sub_82CBA798(PPCContext& __restrict ctx, uint8_t* base) { ctx.r3.u64 = StubReserve(base, ctx.r3.u32); }

namespace {
struct TestM {
  uint8_t* base;
  bool ok = true;
  uint8_t L8(uint32_t a) { return PPC_LOAD_U8(a); }
  uint16_t L16(uint32_t a) { return PPC_LOAD_U16(a); }
  uint32_t L32(uint32_t a) { return PPC_LOAD_U32(a); }
  uint64_t L64(uint32_t a) { return PPC_LOAD_U64(a); }
  void S8(uint32_t a, uint8_t v) { PPC_STORE_U8(a, v); }
  void S32(uint32_t a, uint32_t v) { PPC_STORE_U32(a, v); }
  void S64(uint32_t a, uint64_t v) { PPC_STORE_U64(a, v); }
  uint32_t Reserve(uint32_t dev) { return StubReserve(base, dev); }
};

struct Regions {  // everything either run may write
  uint8_t dev[kDev], ring[kRing], ring2[kRing];
  void Save(uint8_t* b) { std::memcpy(dev, b + D, kDev); std::memcpy(ring, b + RING, kRing); std::memcpy(ring2, b + RING2, kRing); }
  void Load(uint8_t* b) const { std::memcpy(b + D, dev, kDev); std::memcpy(b + RING, ring, kRing); std::memcpy(b + RING2, ring2, kRing); }
};

std::mt19937_64 rng(1234);
uint32_t R(uint32_t n) { return uint32_t(rng() % n); }
void W32(uint32_t a, uint32_t v) { uint8_t* base = g_base; PPC_STORE_U32(a, v); }
void W8(uint32_t a, uint8_t v) { uint8_t* base = g_base; PPC_STORE_U8(a, v); }
uint32_t FloatBits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

void RandomState() {
  for (uint32_t i = 0; i < kDev; i += 4) W32(D + i, uint32_t(rng()));
  // fields that steer branches: keep them in interesting ranges
  W8(D + 11068, uint8_t(rng()));
  W8(D + 11071, uint8_t(rng()));
  W8(D + 11070, uint8_t(rng()));
  W32(D + 13548, uint32_t(rng()));
  W8(D + 12563, R(4) == 0 ? 1 : 0);
  W32(D + 13092, R(4) == 0 ? 1 : 0);
  W32(D + 12288, R(2));
  for (uint32_t k = 0; k < 5; ++k) {
    const uint32_t a = R(3) == 0 ? 0 : SURF + 0x100 * (1 + R(6));
    W32(D + 12816 + 4 * k, a);
    W32(D + 13104 + 4 * k, R(2) ? a : SURF + 0x100 * (1 + R(6)));
  }
  const uint32_t tiles = R(5);
  W32(D + 13124, tiles);
  for (uint32_t t = 0; t < 4; ++t) {
    const int32_t l = int32_t(R(1400)) - 100, tp = int32_t(R(800)) - 100;
    W32(D + 13128 + 16 * t, uint32_t(l));
    W32(D + 13132 + 16 * t, uint32_t(tp));
    W32(D + 13136 + 16 * t, uint32_t(l + int32_t(R(1400))));
    W32(D + 13140 + 16 * t, uint32_t(tp + int32_t(R(800))));
    W32(D + 13368 + 8 * t, R(1400));
    W32(D + 13372 + 8 * t, R(800));
  }
  W32(D + 13556, R(2000));
  W32(D + 13560, R(2000));
  for (uint32_t k = 0; k < 4; ++k) W32(D + 13052 + 4 * k, uint32_t(int32_t(R(2000)) - 200));
  // ring: pointer near the start, limit either far (no reserve) or tight (reserve inside the packets)
  W32(D + 48, RING + 4 * R(16));
  W32(D + 56, R(3) == 0 ? RING + 4 * R(24) : RING + 0x8000);
  for (uint32_t k = 0; k < 4; ++k) W32(D + 3102 * 4 + 4 * k, R(3));  // the 'v' words (0, 1, 2)
  // surfaces: info words with interesting formats
  for (uint32_t s = 1; s <= 6; ++s) {
    const uint32_t S = SURF + 0x100 * s;
    W32(S + 24, uint32_t(rng()));
    W32(S + 28, (uint32_t(rng()) & 0xFFF0FFFFu) | (R(16) << 16));
    W32(S + 36, uint32_t(rng()));
    W32(S + 32, uint32_t(rng()));  // depth surface word (SetDepthStencil: 10560 and the 11070 bit)
  }
  // default viewport + scissor + half constant
  const uint32_t V = wwe2k14::d3d::native_rt::kDefaultViewport;
  for (uint32_t k = 0; k < 4; ++k) W32(V + 4 * k, R(8) == 0 ? uint32_t(rng()) : R(1500));
  const float zs[] = {0.0f, 1.0f, 0.25f, -1.0f, 1e30f, std::nanf("")};
  W32(V + 16, FloatBits(zs[R(6)]));
  W32(V + 20, FloatBits(zs[R(6)]));
  for (uint32_t k = 0; k < 4; ++k) W32(V + 24 + 4 * k, uint32_t(int32_t(R(2000)) - 200));
  W32(wwe2k14::d3d::native_rt::kHalf, FloatBits(R(4) == 0 ? 0.375f : 0.5f));
}
}  // namespace

int main(int argc, char** argv) {
  const long cases = argc > 1 ? std::atol(argv[1]) : 200000;
  void* m = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) return std::perror("mmap"), 2;
  g_base = static_cast<uint8_t*>(m);
  static Regions before, ref;
  long bad = 0, reserves = 0, tiled = 0;
  for (long c = 0; c < cases; ++c) {
    RandomState();
    const uint32_t index = R(6), S = R(4) == 0 ? 0 : SURF + 0x100 * (1 + R(6));
    const bool ds = R(2) == 0;  // SetDepthStencil(D, S) or SetRenderTarget(D, index, S)
    before.Save(g_base);
    PPCContext ctx{};
    ctx.r1.u64 = 0x7F000000u;  // stack for the guest bodies
    if (ds) {
      ctx.r3.u64 = D, ctx.r4.u64 = S;
      ref_sub_82CAF978(ctx, g_base);
    } else {
      ctx.r3.u64 = D, ctx.r4.u64 = index, ctx.r5.u64 = S;
      ref_sub_82CAF5E8(ctx, g_base);
    }
    ref.Save(g_base);
    before.Load(g_base);
    TestM tm{g_base};
    if (ds) wwe2k14::d3d::native_rt::SetDepthStencil(tm, D, S);
    else wwe2k14::d3d::native_rt::SetRenderTarget(tm, D, index, S);
    if (std::memcmp(g_base + RING2, before.ring2, 16) != 0 || std::memcmp(ref.ring2, before.ring2, 16) != 0) ++reserves;
    if (wwe2k14::d3d::native_rt::TiledSurfaces(tm, D)) ++tiled;
    int where = -1;
    for (uint32_t i = 0; i < kDev && where < 0; ++i) if (g_base[D + i] != ref.dev[i]) where = int(i);
    const bool ring_ok = !std::memcmp(g_base + RING, ref.ring, kRing) && !std::memcmp(g_base + RING2, ref.ring2, kRing);
    if (where >= 0 || !ring_ok) {
      if (++bad <= 10)
        std::printf("MISMATCH case %ld %s index %u surface %08X: first device byte %d (guest %02X native %02X) ring %s\n", c, ds ? "DS" : "RT", index, S,
                    where, where >= 0 ? ref.dev[where] : 0, where >= 0 ? g_base[D + where] : 0, ring_ok ? "ok" : "DIFF");
    }
  }
  std::printf("native-rt-test: %ld cases, %ld mismatches (tiled-state cases %ld, ring-reserve cases %ld)\n", cases, bad, tiled,
              reserves);
  return bad ? 1 : 0;
}
