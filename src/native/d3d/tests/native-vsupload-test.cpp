// N4 step 2b host test: native inline VS upload tree (native_vsupload_core.h) against the REAL generated guest bodies
// sub_82CC1B00 / 82CC1200 / 82CC19B0 / 82CC1828 / 82CC18F8 / 82CC0630, copied by tools/nr/gen-vsupload-ref.py into
// out/n4/vsupload_ref.inc (ring reserve 82CBA9D0 and memcpy 82C7C8F0 routed to stubs both runs share). Random devices,
// VS objects (fetch slots, groups, sub-blocks), vertex declarations, VS->PS link tables, ring states (fast / slow / failed
// reserve), lookup tables, and random guest stack bytes (the guest patcher reads one uninitialised stack halfword; the
// native body uses 0). Compares device, ring segments, objects, declaration, the fix-up output word and the final r3.
// Build + run: tools/nr/native-vsupload-test.sh
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "native/d3d/native_vsupload_core.h"

namespace {
constexpr uint32_t D = 0x30000000u, kDev = 14336, OBJ = 0x40000000u, kObj = 0x2000, VSI = 0x41000000u, kVsi = 0x1000,
                   PSO = 0x42000000u, kPso = 0x400, DECL = 0x43000000u, kDecl = 0x200, OUT = 0x44000000u, RING = 0x52000000u,
                   RING2 = 0x53000000u, kRing = 0x8000, SP = 0x7F000000u, SRC1 = 0xC0100000u, SRC2 = 0xDFE01000u;
uint8_t* g_base;
std::mt19937_64 rng(97531);
uint32_t R(uint32_t n) { return uint32_t(rng() % n); }
}  // namespace

static void Stub(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  if (addr == 0x82CBA9D0u) {  // ring reserve of r4 dwords: fast path, else new segment, or failure (0)
    const uint32_t dev = ctx.r3.u32, cur = PPC_LOAD_U32(dev + 48);
    if (cur + 4 * ctx.r4.u32 <= PPC_LOAD_U32(dev + 52)) { ctx.r3.u64 = cur; return; }
    if ((cur >> 2) & 1) { ctx.r3.u64 = 0; return; }
    PPC_STORE_U32(dev + 48, RING2 + 4 * ((cur >> 3) & 7));
    PPC_STORE_U32(dev + 52, RING2 + 0x1800);
    ctx.r3.u64 = PPC_LOAD_U32(dev + 48);
    return;
  }
  std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32);  // memcpy: returns dst (r3 unchanged)
}
#include "vsupload_ref.inc"

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
  uint64_t Reserve9D0(uint32_t dev, uint32_t ndw, uint32_t lr) {
    ctx.r3.u64 = dev;
    ctx.r4.u64 = ndw;
    ctx.lr = lr;
    Stub(0x82CBA9D0u, ctx, base);
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
uint32_t L32h(uint32_t a) { uint8_t* base = g_base; return PPC_LOAD_U32(a); }
void Fill(uint32_t a, uint32_t n) { for (uint32_t i = 0; i < n; i += 4) W32(a + i, uint32_t(rng())); }

struct Region { uint32_t a, n; };
const Region kRegions[] = {{D, kDev}, {OBJ, kObj}, {VSI, kVsi}, {PSO, kPso}, {DECL, kDecl}, {OUT, 16}, {RING, kRing},
                           {RING2, kRing}};
struct Snap {
  std::vector<std::vector<uint8_t>> r;
  void Save(uint8_t* b) {
    r.clear();
    for (const auto& g : kRegions) r.emplace_back(b + g.a, b + g.a + g.n);
  }
  void Load(uint8_t* b) const {
    for (size_t i = 0; i < r.size(); ++i) std::memcpy(b + kRegions[i].a, r[i].data(), kRegions[i].n);
  }
};

// A slot word: slot index < 0x400 (so every 12-byte write stays inside the ring snapshot) + run end bit (0x1000) + random
// key/nibble bits above.
uint32_t SlotWord(uint32_t maxslot, bool end_often) {
  const uint32_t slot = (R(4) << 8 | R(256)) % maxslot;
  // bits 16-27 = run offset index of an interpolator entry: keep every run start inside the filled region
  return (uint32_t(rng()) & 0xF0000000u) | R(64) << 16 | (end_often && R(3) == 0 ? 0x1000u : 0u) | slot;
}
}  // namespace

int main(int argc, char** argv) {
  const long cases = argc > 1 ? std::atol(argv[1]) : 200000;
  void* mm = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mm == MAP_FAILED) return std::perror("mmap"), 2;
  g_base = static_cast<uint8_t*>(mm);
  static Snap before, ref, nat;
  long bad = 0, failed = 0, slow = 0, fix = 0, matched = 0;
  for (long c = 0; c < cases; ++c) {
    Fill(D, kDev);
    Fill(SP - 0x1000, 0x1000);  // guest frames, incl. the patcher's uninitialised halfword
    Fill(SRC1, 0x1000), Fill(SRC2, 0x1000);
    for (uint32_t i = 0; i < 16; i += 2) W16(0x820831B8u + i, uint16_t(rng()));
    for (uint32_t i = 0; i < 64; ++i) W8(0x820831C8u + i, uint8_t(R(3) ? 1 + R(4) : R(10)));
    // Ring: cursor near the segment start; limit roomy, tight (slow path) or failing.
    const uint32_t cur = RING + 4 * R(64);
    W32(D + 48, cur);
    W32(D + 52, R(4) == 0 ? cur + 4 * R(64) : RING + 0x1800);
    // VS object + sub-block idx.
    Fill(OBJ, kObj);
    const uint32_t idx = R(4);
    const uint32_t ncode = R(301), len = 4 * ncode + (R(8) == 0 ? R(4) : 0);
    const uint32_t maxslot = ncode / 3 > 0 ? ncode / 3 : 1;
    for (uint32_t k = 0; k < 4; ++k) W32(OBJ + 896 + 8 * k, 0x800 + 0x200 * k);
    const uint32_t blk = OBJ + 0x800 + 0x200 * idx, B = blk + 872;
    const uint32_t add = R(2) ? 0 : 0x1000 * R(4);  // [obj+32] + [blk+872] = microcode address (two physical views)
    W32(OBJ + 32, add);
    W32(blk + 872, (R(2) ? 0x00100000u : 0x1FE00000u) - add);
    W32(blk + 876, len);
    const uint32_t c24 = R(5), n = R(6) == 0 ? 0 : 1 + R(32);
    W32(B + 24, c24);
    W32(B + 28, n);
    const uint32_t list = B + 4 * (c24 + 9);
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t w = (uint32_t(rng()) & 0xFFC00000u) | (R(3) == 0 ? (1 + R(3)) << 20 : 0) | R(4) << 16 | R(4) << 12;
      w |= (R(4) << 8 | R(256)) % maxslot;
      W32(list + 4 * i, w);
    }
    // Vertex declaration.
    Fill(DECL, kDecl);
    const uint32_t nd = R(5);
    W32(DECL + 24, nd);
    for (uint32_t k = 0; k < nd; ++k) {
      const uint32_t e = DECL + 52 + 12 * k;
      W16(e + 0, uint16_t(R(16)));
      uint32_t t = uint32_t(rng());
      if (R(2)) t = (t & ~0xC0u) | 0x40u;
      W32(e + 4, t);
      W8(e + 9, uint8_t(R(4)));
      W8(e + 10, uint8_t(R(4)));
    }
    // VS->PS link: VS info object (r8) and PS (r9).
    for (uint32_t i = 0; i < kVsi; i += 4) W32(VSI + i, SlotWord(maxslot, true) | ((i & 28) == 28 ? 0x1000u : 0u));
    W8(VSI + 8, uint8_t(R(6) == 0 ? 7 : R(7)));
    W32(VSI + 20, (uint32_t(rng()) & ~0x40000u & ~0x3E0u) | (R(8) == 0 ? 0x40000u : 0u) | R(9) << 5);
    W32(VSI + 24, R(4)), W32(VSI + 28, R(4));
    const uint32_t ps = R(4) == 0 ? 0 : PSO;
    Fill(PSO, kPso);
    W32(PSO + 20, (uint32_t(rng()) & ~0x203FFu) | (R(8) == 0 ? 0x20000u : 0u) | R(9) << 5 | R(6));
    Fill(OUT, 16);
    // Final state.
    before.Save(g_base);
    const uint32_t r4 = R(2) ? 0 : 1 + R(3), r6 = OUT;
    PPCContext ctx{};
    ctx.r1.u64 = SP;
    ctx.r3.u64 = D, ctx.r4.u64 = r4, ctx.r5.u64 = OBJ, ctx.r6.u64 = r6, ctx.r7.u64 = DECL, ctx.r8.u64 = VSI,
    ctx.r9.u64 = ps, ctx.r10.u64 = idx;
    ref_sub_82CC1B00(ctx, g_base);
    const uint64_t ref_r3 = ctx.r3.u64;
    ref.Save(g_base);
    before.Load(g_base);
    Fill(SP - 0x1000, 0x1000);  // the native body must not depend on stack contents
    PPCContext nctx{};
    nctx.r1.u64 = SP - 192;
    TestM tm{nctx, g_base};
    const uint64_t nat_r3 =
        wwe2k14::d3d::native_shader::VSUpload(tm, D, r4, OBJ, r6, DECL, VSI, ps, idx);
    nat.Save(g_base);
    bool ok = nat_r3 == ref_r3;
    int where = -1;
    for (size_t i = 0; i < nat.r.size(); ++i)
      if (nat.r[i] != ref.r[i]) { ok = false; if (where < 0) where = int(i); }
    if (ref_r3 == 0) ++failed;
    if (L32h(D + 48) >= RING2) ++slow;
    if (r4) ++fix;
    if (n && nd) ++matched;
    if (!ok && ++bad <= 10) {
      uint32_t off = 0;
      if (where >= 0)
        for (uint32_t i = 0; i < kRegions[where].n; ++i)
          if (nat.r[where][i] != ref.r[where][i]) { off = i; break; }
      if (where >= 0) {
        const uint32_t o = off & ~3u;
        auto w = [&](const Snap& sn) { const uint8_t* q = sn.r[where].data() + o; return uint32_t(q[0]) << 24 | q[1] << 16 | q[2] << 8 | q[3]; };
        std::printf("  word at +%X: before %08X ref %08X nat %08X (code off %d)\n", o, w(before), w(ref), w(nat),
                    int(kRegions[where].a + o) - int(cur + 24));
      }
      std::printf("MISMATCH case %ld region %d (%08X+%X) n=%u ncode=%u nd=%u r4=%u ps=%X r3 ref %llX nat %llX\n", c, where,
                  where >= 0 ? kRegions[where].a : 0, off, n, ncode, nd, r4, ps, (unsigned long long)ref_r3,
                  (unsigned long long)nat_r3);
    }
  }
  std::printf("native-vsupload-test: %ld cases, %ld mismatches (reserve failed %ld, new segment %ld, interp fix-up %ld, "
              "fetches+decl %ld)\n", cases, bad, failed, slow, fix, matched);
  return bad ? 1 : 0;
}
