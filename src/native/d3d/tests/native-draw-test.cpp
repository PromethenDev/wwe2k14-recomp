// N3 host test: the native draw entry (native_draw_core.h) against the REAL generated guest body sub_82CB52D0, copied by
// tools/nr/gen-draw-ref.py into out/n3/draw_ref.inc with its 9 callees routed to deterministic stubs (both runs call the same
// stubs). Random device / index-buffer / stride-table states; compares the 14 KB device block, the ring and list regions,
// the stub call log (address, return address, argument registers) and the final r3. Build: tools/nr/native-draw-test.sh
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "native/d3d/native_draw_core.h"

namespace {
constexpr uint32_t D = 0x30000000u, kDev = 14336, RING = 0x50000000u, RING2 = 0x51000000u, kRing = 0x40000;
constexpr uint32_t LIST = 0x52000000u, LIST2 = 0x53000000u, kList = 0x10000, IBO = 0x40000000u;
constexpr uint32_t TABLE = wwe2k14::d3d::native_draw::kStrideTable;
uint8_t* g_base;

struct CallLog {
  uint32_t addr, lr;
  uint64_t r3, r4, r5, r6;
};
std::vector<CallLog> g_log;
uint64_t Mix(uint64_t x) {
  x ^= x >> 31; x *= 0x9E3779B97F4A7C15ull; x ^= x >> 29; return x;
}
}  // namespace

// Deterministic callee stubs: log the call, change some device bytes (including the body's dirty words and, for the
// encoders, the ring pointer as packet writers do), return a value derived from the inputs.
static void Stub(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  const int nargs = addr == 0x82CC10B0u || addr == 0x82CC0D18u ? 4 : addr == 0x82CC0988u ? 3
                  : addr == 0x82CC1E48u || addr == 0x82CC0F58u || addr == 0x82CC0DD8u ? 2 : 1;
  g_log.push_back({addr, uint32_t(ctx.lr), ctx.r3.u64, nargs > 1 ? ctx.r4.u64 : 0, nargs > 2 ? ctx.r5.u64 : 0,
                   nargs > 3 ? ctx.r6.u64 : 0});
  const uint64_t h = Mix(addr ^ ctx.r3.u64 * 3 ^ (nargs > 1 ? ctx.r4.u64 * 5 : 0) ^ PPC_LOAD_U32(D + 48));
  const uint32_t dev = D;
  if (addr == 0x82CBA798u) {  // ring reserve: new buffer, new limit
    PPC_STORE_U32(dev + 56, RING2 + 0x20000);
    ctx.r3.u64 = RING2 + 4 * (h & 7);
    return;
  }
  if (addr == 0x82CBA080u) {  // list block reserve (r3 = D+13616)
    PPC_STORE_U32(dev + 13632, LIST2 + 0x8000);
    ctx.r3.u64 = LIST2 + 16 * (h & 3);
    return;
  }
  // packet writer: a few words at the ring pointer, pointer advanced
  const uint32_t p = PPC_LOAD_U32(dev + 48);
  const uint32_t n = uint32_t(h % 4);
  for (uint32_t i = 1; i <= n; ++i) PPC_STORE_U32(p + 4 * i, uint32_t(h >> (8 * i)));
  PPC_STORE_U32(dev + 48, p + 4 * n);
  if (h & 0x100) PPC_STORE_U8(dev + 11068, uint8_t(PPC_LOAD_U8(dev + 11068) ^ 1));  // flips the list branch sometimes
  if (h & 0x200) PPC_STORE_U32(dev + 12812, IBO + 0x100 * ((h >> 12) & 3));        // index buffer object changes
  if (h & 0x400) PPC_STORE_U64(dev + 40, h);                                          // the 0988 gate word
  PPC_STORE_U32(dev + 10560, uint32_t(h >> 20));
  if (addr == 0x82CC1E48u || addr == 0x82CC0988u) {  // return the new dirty mask (64-bit)
    ctx.r3.u64 = ctx.r4.u64 ^ (h & 0x00F0F0F0F0F0FFFFull);
    return;
  }
  ctx.r3.u64 = h;
}

static int g_ref_traps = 0;
static void RefTrap(int) { ++g_ref_traps; }
#include "draw_ref.inc"

namespace {
struct TestM {
  PPCContext& ctx;
  uint8_t* base;
  bool trapped = false;
  uint8_t L8(uint32_t a) { return PPC_LOAD_U8(a); }
  uint32_t L32(uint32_t a) { return PPC_LOAD_U32(a); }
  uint64_t L64(uint32_t a) { return PPC_LOAD_U64(a); }
  void S32(uint32_t a, uint32_t v) { PPC_STORE_U32(a, v); }
  void S64(uint32_t a, uint64_t v) { PPC_STORE_U64(a, v); }
  void Trap() { trapped = true; }
  uint64_t Call(uint32_t addr, uint32_t lr, int nargs, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    ctx.r3.u64 = a3;
    if (nargs > 1) ctx.r4.u64 = a4;
    if (nargs > 2) ctx.r5.u64 = a5;
    if (nargs > 3) ctx.r6.u64 = a6;
    ctx.lr = lr;
    Stub(addr, ctx, base);
    return ctx.r3.u64;
  }
};

struct Regions {
  std::vector<uint8_t> dev = std::vector<uint8_t>(kDev), ring = std::vector<uint8_t>(kRing),
                       ring2 = std::vector<uint8_t>(kRing), list = std::vector<uint8_t>(kList),
                       list2 = std::vector<uint8_t>(kList);
  void Save(uint8_t* b) {
    std::memcpy(dev.data(), b + D, kDev), std::memcpy(ring.data(), b + RING, kRing);
    std::memcpy(ring2.data(), b + RING2, kRing), std::memcpy(list.data(), b + LIST, kList);
    std::memcpy(list2.data(), b + LIST2, kList);
  }
  void Load(uint8_t* b) const {
    std::memcpy(b + D, dev.data(), kDev), std::memcpy(b + RING, ring.data(), kRing);
    std::memcpy(b + RING2, ring2.data(), kRing), std::memcpy(b + LIST, list.data(), kList);
    std::memcpy(b + LIST2, list2.data(), kList);
  }
  bool Same(const Regions& o) const {
    return dev == o.dev && ring == o.ring && ring2 == o.ring2 && list == o.list && list2 == o.list2;
  }
};

std::mt19937_64 rng(5678);
uint32_t R(uint32_t n) { return uint32_t(rng() % n); }
void W32(uint32_t a, uint32_t v) { uint8_t* base = g_base; PPC_STORE_U32(a, v); }
void W64(uint32_t a, uint64_t v) { uint8_t* base = g_base; PPC_STORE_U64(a, v); }
void W8(uint32_t a, uint8_t v) { uint8_t* base = g_base; PPC_STORE_U8(a, v); }

void RandomState() {
  for (uint32_t i = 0; i < kDev; i += 4) W32(D + i, uint32_t(rng()));
  for (uint32_t k = 0; k < 5; ++k) W64(D + 8 * k, R(3) == 0 ? 0 : rng() & (R(2) ? ~0ull : (0xFFFull << (12 * R(5)))));
  W64(D + 40, R(2) ? rng() : 0);
  W8(D + 11068, uint8_t(rng()));
  W8(D + 11072, uint8_t(R(3) == 0 ? 0 : rng()));
  W8(D + 12563, uint8_t(R(3) == 0 ? 0 : R(2) ? 1 : R(64)));
  W32(D + 12812, IBO + 0x100 * R(4));
  for (uint32_t k = 0; k < 4; ++k) {
    W32(IBO + 0x100 * k, uint32_t(rng()));
    W32(IBO + 0x100 * k + 24, uint32_t(rng()));
  }
  for (uint32_t s = 0; s < 64; ++s) {
    const uint32_t stride = 1 + R(4);
    W32(TABLE + 8 * s, stride);
    W32(TABLE + 8 * s + 4, R(stride + 1));  // overlap carried into the next chunk (< chunk size: the loop ends)
  }
  W32(D + 48, RING + 4 * R(64));
  W32(D + 56, R(3) == 0 ? RING + 4 * R(64) : RING + 0x30000);
  W32(D + 13628, LIST + 16 * R(16));
  W32(D + 13632, R(3) == 0 ? LIST + 16 * R(16) : LIST + 0x8000);
}
}  // namespace

int main(int argc, char** argv) {
  const long cases = argc > 1 ? std::atol(argv[1]) : 200000;
  void* m = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) return std::perror("mmap"), 2;
  g_base = static_cast<uint8_t*>(m);
  static Regions before, ref, nat;
  long bad = 0, chunked = 0, listed = 0, wide = 0, reserves = 0, calls_total = 0;
  for (long c = 0; c < cases; ++c) {
    RandomState();
    const uint64_t r4 = R(64) | (R(4) == 0 ? uint64_t(rng()) << 32 : 0) /* table index: low word */, r5 = uint32_t(rng());
    const uint64_t r6 = R(4) == 0 ? uint32_t(rng()) : R(100000);
    const uint64_t r7 = R(6) == 0 ? 65536 + R(300000) : R(8) == 0 ? 65535 + R(2) : 1 + R(5000);
    if (r7 > 65535) ++chunked;
    if (g_base[D + 11068] & 1) ++listed;
    {
      uint8_t* base = g_base;
      if (PPC_LOAD_U32(PPC_LOAD_U32(D + 12812)) & 0x80000000u) ++wide;
    }
    before.Save(g_base);
    g_log.clear();
    PPCContext ctx{};
    ctx.r1.u64 = 0x7F000000u;
    ctx.r3.u64 = D, ctx.r4.u64 = r4, ctx.r5.u64 = r5, ctx.r6.u64 = r6, ctx.r7.u64 = r7;
    ref_sub_82CB52D0(ctx, g_base);
    const uint64_t ref_r3 = ctx.r3.u64;
    const std::vector<CallLog> ref_log = g_log;
    ref.Save(g_base);
    before.Load(g_base);
    g_log.clear();
    PPCContext nctx{};
    nctx.r1.u64 = 0x7F000000u;
    TestM tm{nctx, g_base};
    const uint64_t nat_r3 = wwe2k14::d3d::native_draw::DrawIndexedVertices(tm, D, r4, r5, r6, r7);
    nat.Save(g_base);
    calls_total += long(ref_log.size());
    for (const CallLog& l : ref_log) if (l.addr == 0x82CBA798u) { ++reserves; break; }
    bool log_ok = ref_log.size() == g_log.size();
    for (size_t i = 0; log_ok && i < g_log.size(); ++i)
      log_ok = !std::memcmp(&ref_log[i], &g_log[i], sizeof(CallLog));
    const bool mem_ok = nat.Same(ref);
    if (!mem_ok || !log_ok || nat_r3 != ref_r3 || tm.trapped || g_ref_traps) {
      g_ref_traps = 0;
      if (++bad <= 10) {
        int where = -1;
        for (uint32_t i = 0; i < kDev && where < 0; ++i) if (nat.dev[i] != ref.dev[i]) where = int(i);
        std::printf("MISMATCH case %ld: r4=%llX r7=%llu dev_byte=%d (ref %02X nat %02X) rings_ok=%d log %zu/%zu ok=%d r3 "
                    "ref %llX nat %llX trap=%d\n",
                    c, (unsigned long long)r4, (unsigned long long)r7, where, where >= 0 ? ref.dev[where] : 0,
                    where >= 0 ? nat.dev[where] : 0,
                    int(nat.ring == ref.ring && nat.ring2 == ref.ring2 && nat.list == ref.list && nat.list2 == ref.list2),
                    ref_log.size(), g_log.size(), int(log_ok), (unsigned long long)ref_r3, (unsigned long long)nat_r3,
                    int(tm.trapped));
      }
    }
  }
  std::printf("native-draw-test: %ld cases, %ld mismatches (chunked %ld, linked-list %ld, 32-bit indices %ld, ring reserve "
              "%ld, callee calls %ld)\n", cases, bad, chunked, listed, wide, reserves, calls_total);
  return bad ? 1 : 0;
}
