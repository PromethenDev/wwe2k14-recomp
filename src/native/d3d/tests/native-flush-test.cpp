// N4 step 2c host test: native shader flush (native_flush_core.h) against the REAL generated guest bodies sub_82CC1E48 /
// 82CC0790 / 82CC1D08 / 82CC1200, copied by tools/nr/gen-flush-ref.py into out/n4/flush_ref.inc. The hooked callees (ring
// reserve, ShaderConstLoad, VS upload), memcpy and KeBugCheck are stubs both runs share; the stubs log every call (address,
// argument registers) and have device / ring / stack effects, so call order, arguments and the read-back of the upload's
// stack word are compared too. Random devices, VS / PS objects (two sub-blocks, cache keys, fences), declarations, globals.
// Compares device, objects, declaration, globals, resident code, ring segments, the call logs and the returned mask.
// Build + run: tools/nr/native-flush-test.sh
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "native/d3d/native_flush_core.h"

namespace {
constexpr uint32_t D = 0x30000000u, kDev = 14336, VS = 0x40000000u, kVs = 0x2000, PS = 0x41000000u, kPs = 0x400,
                   DECL = 0x43000000u, kDecl = 0x200, X = 0x44000000u, kX = 0x200, CODE = 0x45000000u, kCode = 0x8000,
                   GLOB = 0x83948060u, RING = 0x52000000u, RING2 = 0x53000000u, kRing = 0x2000, SP = 0x7F000000u;
uint8_t* g_base;
std::mt19937_64 rng(24680);
uint32_t R(uint32_t n) { return uint32_t(rng() % n); }
std::vector<uint64_t>* g_log = nullptr;
long g_bug = 0;
}  // namespace

// The generated atomic (mfmsr / lwarx / stwcx) takes the emulator's global lock: single-threaded stand-ins here.
namespace rex::thread {
std::atomic<bool> g_global_lock_census{false};
std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex m;
  return m;
}
std::unique_lock<std::recursive_mutex> global_critical_region::AcquireCensused() { return std::unique_lock(mutex()); }
}  // namespace rex::thread

static void RefTrap(int) {}
static void Stub(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  if (addr != 0x82C7C8F0u && g_log) {
    g_log->push_back(addr);
    g_log->push_back(ctx.r3.u64);  // the ring reserve and KeBugCheck read r3 only
    if (addr == 0x82CC1C58u || addr == 0x82CC1B00u) g_log->push_back(ctx.r4.u64), g_log->push_back(ctx.r5.u64);
    if (addr == 0x82CC1B00u)
      for (uint64_t r : {ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64}) g_log->push_back(r);
  }
  switch (addr) {
    case 0x82CBA798u:  // ring reserve: new segment
      PPC_STORE_U32(ctx.r3.u32 + 56, RING2 + 0x1800);
      ctx.r3.u64 = RING2 + 4 * (PPC_LOAD_U32(ctx.r3.u32 + 48) & 7);
      return;
    case 0x82CC1C58u: {  // const load: one 2-word packet
      const uint32_t dev = ctx.r3.u32, q = PPC_LOAD_U32(dev + 48);
      PPC_STORE_U32(q + 4, 0xC0022F00u);
      PPC_STORE_U32(q + 8, ctx.r4.u32 ^ ctx.r5.u32);
      PPC_STORE_U32(dev + 48, q + 8);
      ctx.r3.u64 = q + 8;
      return;
    }
    case 0x82CC1B00u: {  // VS upload: packet + D+11070 bit 7 + rewrites the r6 word when r4 != 0
      const uint32_t dev = ctx.r3.u32, q = PPC_LOAD_U32(dev + 48);
      PPC_STORE_U32(q + 4, 0xC0003B00u);
      PPC_STORE_U32(q + 8, ctx.r10.u32 ^ ctx.r8.u32);
      PPC_STORE_U32(dev + 48, q + 8);
      PPC_STORE_U8(dev + 11070, uint8_t((PPC_LOAD_U8(dev + 11070) & 0x7F) | (ctx.r4.u32 ? 0x80 : 0)));
      if (ctx.r4.u32) PPC_STORE_U32(ctx.r6.u32, (PPC_LOAD_U32(ctx.r6.u32) & ~0x00F00000u) | 0x00500000u);
      ctx.r3.u64 = q + 8;
      return;
    }
    case 0xBu:  // KeBugCheck
      ++g_bug;
      return;
    default:  // memcpy
      std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32);
  }
}
#include "flush_ref.inc"

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
  uint64_t Copy(uint32_t dst, uint32_t src, uint32_t n, uint32_t) {
    std::memmove(base + dst, base + src, n);
    return dst;
  }
  uint64_t Call(uint32_t addr, uint32_t lr) {
    ctx.lr = lr;
    Stub(addr, ctx, base);
    return ctx.r3.u64;
  }
  uint64_t RingReserve(uint32_t dev, uint32_t lr) {
    ctx.r3.u64 = dev;
    return Call(0x82CBA798u, lr);
  }
  void BugCheck(uint32_t code, uint32_t lr) {
    ctx.r3.u64 = code;
    Call(0xBu, lr);
  }
  void ConstLoad(uint32_t dev, uint32_t info, uint32_t r5, uint32_t lr) {
    ctx.r3.u64 = dev, ctx.r4.u64 = info, ctx.r5.u64 = r5;
    Call(0x82CC1C58u, lr);
  }
  void VsUpload(uint32_t dev, uint32_t r4, uint32_t vs, uint32_t r6, uint32_t decl, uint32_t r8, uint32_t r9, uint32_t idx,
                uint32_t lr) {
    ctx.r3.u64 = dev, ctx.r4.u64 = r4, ctx.r5.u64 = vs, ctx.r6.u64 = r6, ctx.r7.u64 = decl, ctx.r8.u64 = r8,
    ctx.r9.u64 = r9, ctx.r10.u64 = idx;
    Call(0x82CC1B00u, lr);
  }
  uint32_t AtomicInc(uint32_t a) {
    const uint32_t v = PPC_LOAD_U32(a) + 1;
    PPC_STORE_U32(a, v);
    return v;
  }
};

void W8(uint32_t a, uint8_t v) { uint8_t* base = g_base; PPC_STORE_U8(a, v); }
void W16(uint32_t a, uint16_t v) { uint8_t* base = g_base; PPC_STORE_U16(a, v); }
void W32(uint32_t a, uint32_t v) { uint8_t* base = g_base; PPC_STORE_U32(a, v); }
void W64(uint32_t a, uint64_t v) { uint8_t* base = g_base; PPC_STORE_U64(a, v); }
uint32_t L32h(uint32_t a) { uint8_t* base = g_base; return PPC_LOAD_U32(a); }
uint64_t L64h(uint32_t a) { uint8_t* base = g_base; return PPC_LOAD_U64(a); }
void Fill(uint32_t a, uint32_t n) { for (uint32_t i = 0; i < n; i += 4) W32(a + i, uint32_t(rng())); }
uint32_t Bits(uint32_t keep_prob_n) { return R(keep_prob_n) == 0 ? uint32_t(rng()) : 0; }

struct Region { uint32_t a, n; };
const Region kRegions[] = {{D, kDev}, {VS, kVs}, {PS, kPs}, {DECL, kDecl}, {X, kX}, {CODE, kCode}, {GLOB, 16},
                           {RING, kRing}, {RING2, kRing}};
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

// VS sub-block i: blk = VS + 0x800 + 0x400 i (B = blk + 872: code addr, len, word 8, word 12, interp word 20, c24, n, list).
void BuildSubBlock(uint32_t i, uint32_t decl_nd) {
  const uint32_t blk = VS + 0x800 + 0x400 * i, B = blk + 872;
  W32(VS + 896 + 8 * i, blk - VS);
  W32(B + 0, CODE + 0x100 * R(8) - L32h(VS + 32));  // resident code = [B] + [vs+32] = CODE + 0x100 k
  W32(B + 4, 4 * R(400));
  W32(B + 8, uint32_t(rng())), W32(B + 12, uint32_t(rng()));
  W32(B + 20, R(3) ? (uint32_t(rng()) & ~0x1Fu) : uint32_t(rng()));
  const uint32_t c24 = R(4), n = R(5) == 0 ? 0 : 1 + R(32);
  W32(B + 24, c24), W32(B + 28, n);
  for (uint32_t k = 0; k < n; ++k) {
    uint32_t w = (uint32_t(rng()) & 0xFFC00000u) | (R(3) == 0 ? (1 + R(3)) << 20 : 0) | R(4) << 16 | R(4) << 12;
    w |= R(4) << 8 | R(256);
    W32(B + 4 * (c24 + 9) + 4 * k, w);
  }
  // cache key / fence of the in-place patch (vs + 416 i + 40 .. 68)
  const uint32_t o = VS + 416 * i;
  W32(o + 40, R(2) ? L32h(DECL + 48) : uint32_t(rng()));
  W64(o + 48, R(2) ? L64h(D + 12904) : uint64_t(rng()));
  W64(o + 56, R(2) ? L64h(D + 12912) : uint64_t(rng()));
  W32(o + 64, R(3) == 0 ? 0 : L32h(D + 11036) - R(8));
  (void)decl_nd;
}
}  // namespace

int main(int argc, char** argv) {
  const long cases = argc > 1 ? std::atol(argv[1]) : 200000;
  void* mm = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mm == MAP_FAILED) return std::perror("mmap"), 2;
  g_base = static_cast<uint8_t*>(mm);
  static Snap before, ref, nat;
  std::vector<uint64_t> log_ref, log_nat;
  long bad = 0, no_vs = 0, no_ps = 0, uploads = 0, inplace = 0, state_blocks = 0, reserves = 0;
  for (long c = 0; c < cases; ++c) {
    Fill(D, kDev);
    Fill(SP - 0x1000, 0x1000);
    for (uint32_t i = 0; i < 16; i += 2) W16(0x820831B8u + i, uint16_t(rng()));
    for (uint32_t i = 0; i < 64; ++i) W8(0x820831C8u + i, uint8_t(R(3) ? 1 + R(4) : R(10)));
    Fill(GLOB, 16);
    if (R(2)) W32(GLOB + 4, L32h(GLOB + 4) | 1);
    if (R(8) == 0) W32(GLOB + 8, 0xFFFFFFFEu - R(2));
    Fill(CODE, kCode), Fill(X, kX), Fill(RING, kRing), Fill(RING2, kRing);
    // Device fields the flush reads.
    W32(D + 13072, R(12) == 0 ? 0 : VS);
    W32(D + 13068, R(4) == 0 ? 0 : PS);
    W32(D + 12240, R(6) == 0 ? 0 : DECL);
    W32(D + 13904, R(2) ? 0 : 1 + R(3));
    W32(D + 13912, R(3));
    W32(D + 11024, X + 0x100);
    W32(D + 12816, X + 0x40);
    W32(D + 11036, uint32_t(rng()));
    W32(X + 0x100, L32h(D + 11036) - R(16));
    const uint32_t cur = RING + 4 * R(64);
    W32(D + 48, cur);
    W32(D + 56, R(5) == 0 ? cur - 4 : RING + 0x1800);
    if (R(2)) W32(D + 13076, L32h(D + 10580));
    // Declaration.
    Fill(DECL, kDecl);
    const uint32_t nd = R(5);
    W32(DECL + 24, nd);
    if (R(4) == 0) W32(DECL + 48, 0);
    for (uint32_t k = 0; k < nd; ++k) {
      const uint32_t e = DECL + 52 + 12 * k;
      W16(e + 0, uint16_t(R(16)));
      uint32_t t = uint32_t(rng());
      if (R(2)) t = (t & ~0xC0u) | 0x40u;
      W32(e + 4, t), W8(e + 9, uint8_t(R(4))), W8(e + 10, uint8_t(R(4)));
    }
    // VS object.
    Fill(VS, kVs);
    W32(VS + 32, R(2) ? 0 : 0x10 * R(16));
    W32(VS + 872, (uint32_t(rng()) & ~0x70u) | (R(3) == 0 ? 0x20u : 0) | (R(3) == 0 ? 0x40u : 0) | (R(3) == 0 ? 0x10u : 0));
    BuildSubBlock(0, nd);
    BuildSubBlock(1, nd);
    // PS object: pi = PS + 0x100 (code addr, len, p48, p52; pinfo = pi + 40: interp word 20, 24, flags 28).
    Fill(PS, kPs);
    W32(PS + 64, 0x100);
    W32(PS + 40, uint32_t(rng()) & (R(3) == 0 ? ~0u : ~0x10u));
    W32(PS + 0x100 + 48, R(2) ? (uint32_t(rng()) & 0x8FFFFFFFu) | R(8) << 28 : uint32_t(rng()));
    W32(PS + 0x100 + 40 + 20, R(2) ? L32h(VS + 0x800 + 872 + 20) : uint32_t(rng()));
    W32(PS + 0x100 + 40 + 28, uint32_t(rng()) & (R(2) ? ~0x30u : ~0u));
    // A bug check that the guest would loop on (13904 == 0 with the 0x10 asserts set) is not a working-game state.
    if (L32h(D + 13904) == 0) {
      W32(VS + 872, L32h(VS + 872) & ~0x10u);
      W32(PS + 40, L32h(PS + 40) & ~0x10u);
    }
    uint64_t mask = rng() & (0x01000000001FFFFFull | Bits(4));
    if (R(2)) mask |= 0x20000u;
    before.Save(g_base);
    // Guest.
    log_ref.clear();
    g_log = &log_ref;
    PPCContext ctx{};
    ctx.r1.u64 = SP;
    ctx.r3.u64 = D, ctx.r4.u64 = mask;
    ref_sub_82CC1E48(ctx, g_base);
    const uint64_t ref_r3 = ctx.r3.u64;
    ref.Save(g_base);
    // Native (fresh stack bytes: it must not depend on them).
    before.Load(g_base);
    Fill(SP - 0x1000, 0x1000);
    log_nat.clear();
    g_log = &log_nat;
    PPCContext nctx{};
    nctx.r1.u64 = SP - 272;
    TestM tm{nctx, g_base};
    const uint64_t nat_r3 = wwe2k14::d3d::native_shader::Flush(tm, D, mask, SP - 272);
    nat.Save(g_base);
    g_log = nullptr;
    bool ok = nat_r3 == ref_r3 && log_nat == log_ref;
    int where = -1;
    for (size_t i = 0; i < nat.r.size(); ++i)
      if (nat.r[i] != ref.r[i]) { ok = false; if (where < 0) where = int(i); }
    if (!L32h(D + 13072)) ++no_vs;
    else if (!L32h(D + 13068)) ++no_ps;
    for (size_t i = 0; i < log_ref.size(); ++i)
      if (log_ref[i] == 0x82CC1B00u) { ++uploads; break; }
    for (size_t i = 0; i < log_ref.size(); ++i)
      if (log_ref[i] == 0x82CBA798u) { ++reserves; break; }
    if (ref.r[5] != before.r[5]) ++inplace;
    if (L32h(D + 11068) & 0x40000000u) ++state_blocks;
    if (!ok && ++bad <= 10) {
      uint32_t off = 0;
      if (where >= 0)
        for (uint32_t i = 0; i < kRegions[where].n; ++i)
          if (nat.r[where][i] != ref.r[where][i]) { off = i; break; }
      for (size_t i = 0; i < log_ref.size() && i < log_nat.size(); ++i)
        if (log_ref[i] != log_nat[i]) {
          std::printf("  log[%zu] ref %llX nat %llX\n", i, (unsigned long long)log_ref[i], (unsigned long long)log_nat[i]);
          break;
        }
      std::printf("MISMATCH case %ld region %d (%08X+%X) mask %llX r3 ref %llX nat %llX log ref %zu nat %zu\n", c, where,
                  where >= 0 ? kRegions[where].a : 0, off, (unsigned long long)mask, (unsigned long long)ref_r3,
                  (unsigned long long)nat_r3, log_ref.size(), log_nat.size());
    }
  }
  std::printf("native-flush-test: %ld cases, %ld mismatches (no VS %ld, no PS %ld, VS uploads %ld, in-place patches %ld, "
              "ring reserves %ld, bug checks %ld)\n", cases, bad, no_vs, no_ps, uploads, inplace, reserves, g_bug);
  return bad ? 1 : 0;
}
