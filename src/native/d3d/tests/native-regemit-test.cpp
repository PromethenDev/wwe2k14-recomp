// N4 step 3 host test: native register-run writer (native_regemit_core.h) against the REAL generated guest bodies
// sub_82CC0D18 / 82CC06D8, copied by tools/nr/gen-regemit-ref.py into out/n4/regemit_ref.inc (ring reserve 82CBA798 = a stub
// both runs share: new segment of 32 words). Random non-zero masks (single bits, long runs, all ones), registers, values,
// cursors near the segment end (short-segment path, several reserves per run). Compares device, both ring segments, r3.
// Build + run: tools/nr/native-regemit-test.sh
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "native/d3d/native_regemit_core.h"

namespace {
constexpr uint32_t D = 0x30000000u, kDev = 14336, RING = 0x52000000u, RING2 = 0x53000000u, kRing = 0x4000;
uint8_t* g_base;
std::mt19937_64 rng(13579);
uint32_t R(uint32_t n) { return uint32_t(rng() % n); }
long g_traps = 0, g_reserves = 0;
}  // namespace

static void RefTrap(int) { ++g_traps; }
static void Stub(uint32_t, PPCContext& ctx, uint8_t* base) {  // ring reserve: next 32-word segment
  ++g_reserves;
  const uint32_t dev = ctx.r3.u32, cur = PPC_LOAD_U32(dev + 48);
  const uint32_t q = cur < RING2 ? RING2 : cur + 0x100;
  PPC_STORE_U32(dev + 48, q);
  PPC_STORE_U32(dev + 52, q + 0x80);
  ctx.r3.u64 = q;
}
#include "regemit_ref.inc"

namespace {
struct TestM {
  PPCContext& ctx;
  uint8_t* base;
  uint8_t L8(uint32_t a) { return PPC_LOAD_U8(a); }
  uint32_t L32(uint32_t a) { return PPC_LOAD_U32(a); }
  uint64_t L64(uint32_t a) { return PPC_LOAD_U64(a); }
  void S32(uint32_t a, uint32_t v) { PPC_STORE_U32(a, v); }
  uint64_t RingReserve(uint32_t dev, uint32_t lr) {
    ctx.r3.u64 = dev;
    ctx.lr = lr;
    Stub(0, ctx, base);
    return ctx.r3.u64;
  }
};
void W32(uint32_t a, uint32_t v) { uint8_t* base = g_base; PPC_STORE_U32(a, v); }
void Fill(uint32_t a, uint32_t n) { for (uint32_t i = 0; i < n; i += 4) W32(a + i, uint32_t(rng())); }
uint64_t Mask() {
  switch (R(6)) {
    case 0: return 1ull << R(64);
    case 1: return ~0ull;
    case 2: {  // a few runs
      uint64_t m = 0;
      for (uint32_t k = R(4) + 1; k; --k) {
        const uint32_t a = R(64), n = 1 + R(16);
        for (uint32_t i = a; i < a + n && i < 64; ++i) m |= 1ull << i;
      }
      return m;
    }
    case 3: return rng() & 0xFFF0000000000000ull;  // the draw entry's shapes
    case 4: return rng() & 0xFFFF000000000000ull;
    default: return rng();
  }
}
}  // namespace

int main(int argc, char** argv) {
  const long cases = argc > 1 ? std::atol(argv[1]) : 200000;
  void* mm = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mm == MAP_FAILED) return std::perror("mmap"), 2;
  g_base = static_cast<uint8_t*>(mm);
  uint8_t* base = g_base;
  std::vector<uint8_t> b_dev(kDev), b_r1(kRing), b_r2(kRing), r_dev, r_r1, r_r2;
  long bad = 0, slow = 0;
  for (long c = 0; c < cases; ++c) {
    uint64_t mask;
    do mask = Mask();
    while (mask == 0);
    Fill(D, kDev), Fill(RING, kRing), Fill(RING2, kRing);
    Fill(0x7F000000u - 0x400, 0x400);
    const uint32_t cur = RING + 4 * R(256);
    W32(D + 48, cur);
    W32(D + 52, R(3) == 0 ? cur + 4 * R(40) : RING + 0x3000);
    const uint64_t reg0 = R(2) ? 8192 + R(512) : (uint64_t(rng()) & 0xFFFFFFFFull);
    const uint32_t values = D + 4 * R(3000);
    std::memcpy(b_dev.data(), g_base + D, kDev), std::memcpy(b_r1.data(), g_base + RING, kRing);
    std::memcpy(b_r2.data(), g_base + RING2, kRing);
    const long res0 = g_reserves;
    PPCContext ctx{};
    ctx.r1.u64 = 0x7F000000u;
    ctx.r3.u64 = D, ctx.r4.u64 = mask, ctx.r5.u64 = reg0, ctx.r6.u64 = values;
    ref_sub_82CC0D18(ctx, g_base);
    const uint64_t ref_r3 = ctx.r3.u64;
    if (g_reserves != res0) ++slow;
    r_dev.assign(g_base + D, g_base + D + kDev), r_r1.assign(g_base + RING, g_base + RING + kRing);
    r_r2.assign(g_base + RING2, g_base + RING2 + kRing);
    std::memcpy(g_base + D, b_dev.data(), kDev), std::memcpy(g_base + RING, b_r1.data(), kRing);
    std::memcpy(g_base + RING2, b_r2.data(), kRing);
    PPCContext nctx{};
    nctx.r1.u64 = 0x7F000000u - 144;
    TestM tm{nctx, g_base};
    const uint64_t nat_r3 = wwe2k14::d3d::native_shader::RegEmit(tm, D, mask, reg0, values);
    const bool ok = nat_r3 == ref_r3 && !std::memcmp(r_dev.data(), g_base + D, kDev) &&
                    !std::memcmp(r_r1.data(), g_base + RING, kRing) && !std::memcmp(r_r2.data(), g_base + RING2, kRing);
    if (!ok && ++bad <= 10)
      std::printf("MISMATCH case %ld mask %016llX reg0 %llX cur %X lim %X r3 ref %llX nat %llX\n", c,
                  (unsigned long long)mask, (unsigned long long)reg0, cur, uint32_t(PPC_LOAD_U32(D + 52)),
                  (unsigned long long)ref_r3, (unsigned long long)nat_r3);
  }
  std::printf("native-regemit-test: %ld cases, %ld mismatches (short-segment path %ld, traps %ld)\n", cases, bad, slow,
              g_traps);
  return bad ? 1 : 0;
}
