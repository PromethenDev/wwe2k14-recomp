// Native Clear tree host test (11-19): the native sub_82CB8BA8 -> 82CB8A70 -> 82CB83F0 (native_clear_core.h) against the
// REAL generated guest bodies and their callees (82CC0D18, 82CC06D8, 82CAC5C0), copied by tools/nr/gen-ref-closure.py into
// out/nclear/cl_ref.inc. Random device / rect / colour / flag / ring states; both runs share deterministic logged stubs for
// the ring reserve, the state encoder 82CC0988, memcpy and the plane emitter 82CB7E50 (its log carries r3..r10 as 64-bit,
// f1 bits, its stack argument 92(r1) and the 16 colour bytes at r10).
// Compares: the device block, both ring buffers, the surface area and the stub call logs.
// Build + run: tools/nr/native-clear-test.sh [cases]   (CL_VERBOSE=1 [CL_CASE=n] prints the first / chosen diff)
#include <sys/mman.h>
#include <xmmintrin.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "native/d3d/native_clear_core.h"
#include "cl_ref.inc"

namespace {
constexpr uint32_t D = 0x30000000u, kDev = 16384, RING = 0x50000000u, RING2 = 0x51000000u, kRing = 0x10000;
constexpr uint32_t SURF = 0x40000000u, RECTS = 0x40002000u, kSurf = 0x4000;
constexpr uint32_t STACK = 0x7F000000u, kStackBytes = 0x4000;
uint8_t* g_base;

struct StubLog {
  std::vector<std::string> calls;
  void Add(const std::string& s) {
    if (calls.size() < 4096) calls.push_back(s);  // a looping guest path must not grow without bound
  }
};
StubLog* g_log;
std::string Hex(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, " %llx", (unsigned long long)v);
  return b;
}
}  // namespace

static void ref_sub_82CBA798(PPCContext& __restrict ctx, uint8_t* base) {
  g_log->Add("reserve" + Hex(ctx.lr) + Hex(ctx.r3.u32));
  PPC_STORE_U32(ctx.r3.u32 + 56, RING2 + 0x8000);
  PPC_STORE_U32(ctx.r3.u32 + 52, RING2 + 0x8000);  // 82CC0D18's short path loops on D+52 (11-18 OOM)
  ctx.r3.u64 = RING2;
}
static void ref_sub_82CC0988(PPCContext& __restrict ctx, uint8_t*) {
  g_log->Add("encoder" + Hex(ctx.lr) + Hex(ctx.r3.u32) + Hex(ctx.r4.u64) + Hex(ctx.r5.u32));
  ctx.r3.u64 = ctx.r4.u64 & 0x00FF00FF00FF00FFull;
}
static void ref_sub_82C7C8F0(PPCContext& __restrict ctx, uint8_t* base) {
  g_log->Add("memcpy" + Hex(ctx.lr) + Hex(ctx.r3.u32) + Hex(ctx.r4.u32) + Hex(ctx.r5.u32));
  std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32);
}
static void ref_sub_82CB7E50(PPCContext& __restrict ctx, uint8_t* base) {
  std::string s = "plane" + Hex(ctx.lr);
  const uint64_t r[8] = {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  for (uint64_t v : r) s += Hex(v);
  s += " f1" + Hex(ctx.f1.u64) + " sp92" + Hex(PPC_LOAD_U32(ctx.r1.u32 + 92)) + " col";
  for (uint32_t i = 0; i < 16; i += 4) s += Hex(PPC_LOAD_U32(ctx.r10.u32 + i));
  g_log->Add(s);
}

namespace {
using Fn = void (*)(PPCContext&, uint8_t*);
Fn Lookup(uint32_t a) {
  switch (a) {
#define F(x) case 0x##x##u: return ref_sub_##x;
    F(82CBA798) F(82CC0988) F(82C7C8F0) F(82CB7E50) F(82CC0D18) F(82CAC5C0)
#undef F
    default: return nullptr;
  }
}

struct TestM {
  uint8_t* base;
  uint32_t sp;  // guest r1 of the 83F0 frame (callees run with it)
  bool ok = true;
  uint8_t L8(uint32_t a) { return PPC_LOAD_U8(a); }
  uint16_t L16(uint32_t a) { return PPC_LOAD_U16(a); }
  uint32_t L32(uint32_t a) { return PPC_LOAD_U32(a); }
  uint64_t L64(uint32_t a) { return PPC_LOAD_U64(a); }
  void S8(uint32_t a, uint8_t v) { PPC_STORE_U8(a, v); }
  void S16(uint32_t a, uint16_t v) { PPC_STORE_U16(a, v); }
  void S32(uint32_t a, uint32_t v) { PPC_STORE_U32(a, v); }
  void S64(uint32_t a, uint64_t v) { PPC_STORE_U64(a, v); }
  uint64_t CallF(uint32_t addr, uint32_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8,
                 uint64_t r9, uint64_t r10, uint64_t f1_bits) {
    Fn fn = Lookup(addr);
    if (!fn) {
      std::printf("native called unknown guest function %08X\n", addr);
      ok = false;
      return 0;
    }
    PPCContext c{};
    c.fpscr.InitHost();
    c.r1.u64 = sp;
    c.r3.u64 = r3, c.r4.u64 = r4, c.r5.u64 = r5, c.r6.u64 = r6, c.r7.u64 = r7, c.r8.u64 = r8, c.r9.u64 = r9, c.r10.u64 = r10;
    c.f1.u64 = f1_bits;
    c.lr = lr;
    fn(c, base);
    return c.r3.u64;
  }
  uint64_t Call(uint32_t addr, uint32_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8,
                uint64_t r9, uint64_t r10) {
    return CallF(addr, lr, r3, r4, r5, r6, r7, r8, r9, r10, 0);
  }
};

struct Regions {  // everything either run may write or read
  std::vector<uint8_t> dev, ring, ring2, surf, stack;
  void Save(uint8_t* b) {
    dev.assign(b + D, b + D + kDev);
    ring.assign(b + RING, b + RING + kRing);
    ring2.assign(b + RING2, b + RING2 + kRing);
    surf.assign(b + SURF, b + SURF + kSurf);
    stack.assign(b + STACK, b + STACK + kStackBytes);
  }
  void Load(uint8_t* b) const {
    std::memcpy(b + D, dev.data(), kDev);
    std::memcpy(b + RING, ring.data(), kRing);
    std::memcpy(b + RING2, ring2.data(), kRing);
    std::memcpy(b + SURF, surf.data(), kSurf);
    std::memcpy(b + STACK, stack.data(), kStackBytes);
  }
};

std::mt19937_64 rng(1119);
uint32_t R(uint32_t n) { return uint32_t(rng() % n); }
void W8(uint32_t a, uint8_t v) { uint8_t* base = g_base; PPC_STORE_U8(a, v); }
void W32(uint32_t a, uint32_t v) { uint8_t* base = g_base; PPC_STORE_U32(a, v); }
void W64(uint32_t a, uint64_t v) { uint8_t* base = g_base; PPC_STORE_U64(a, v); }
uint32_t FB(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

uint32_t RandFloatBits(float lo, float hi, uint32_t odds = 64) {  // mostly ordinary values, sometimes special ones
  switch (R(odds)) {
    case 0: return 0x7FC00000u | (uint32_t(rng()) & 0x3FFFFFu);      // QNaN
    case 1: return 0x7F800001u | (uint32_t(rng()) & 0x3FFFFFu);      // SNaN
    case 2: return R(2) ? 0x7F800000u : 0xFF800000u;                 // inf
    case 3: return uint32_t(rng()) & 0x807FFFFFu;                    // denormal / zero
    case 4: return uint32_t(rng());                                  // anything
    case 5: return FB(float(int32_t(rng())) * 4.0f);                 // out of int range
    case 6: return R(2) ? 0x00800000u : 0x80800000u;                 // smallest normal
    default: return FB(lo + (hi - lo) * float(R(1u << 20)) / float(1u << 20));
  }
}

struct Args {
  uint32_t count, rects, colour, opaque;
  uint64_t flags, f1;
  uint32_t caller_sp;
};

Args RandomState() {
  for (uint32_t i = 0; i < kDev; i += 4) W32(D + i, uint32_t(rng()));
  for (uint32_t i = 0; i < kSurf; i += 4) W32(SURF + i, uint32_t(rng()));
  for (uint32_t i = 0; i < kStackBytes; i += 4) W32(STACK + i, uint32_t(rng()));
  for (uint32_t i = 0; i < 64; i += 4) W32(i, uint32_t(rng()));  // 82CB8A70 reads *(NULL+36) when no surface is bound
  // Bound surfaces D+12816..12832 and the recorded set D+13104..13120 (82CB8A70's bound-set test, the plane loop).
  for (uint32_t k = 0; k < 5; ++k) {
    const uint32_t cur = R(k == 4 ? 4 : 3) == 0 ? 0 : SURF + 256 * (1 + R(8));  // D+12832 mostly bound
    W32(D + 12816 + 4 * k, cur);
    const uint32_t q = R(8);
    W32(D + 13104 + 4 * k, q < 5 ? cur : q == 5 ? 0 : SURF + 256 * (1 + R(8)));
  }
  W8(D + 11068, uint8_t(rng()));
  W32(D + 13556, R(16) == 0 ? uint32_t(rng()) : 1 + R(1400));  // 82CB8A70's override right / bottom
  W32(D + 13560, R(16) == 0 ? uint32_t(rng()) : 1 + R(800));
  // viewport x0, y0, w, h (f32) and the scissor enable + clamps
  W32(D + 13024, RandFloatBits(-64.f, 200.f, 256));
  W32(D + 13028, RandFloatBits(-64.f, 150.f, 256));
  W32(D + 13032, RandFloatBits(200.f, 1400.f, 256));
  W32(D + 13036, RandFloatBits(150.f, 800.f, 256));
  W32(D + 12288, R(2) ? 0 : uint32_t(rng()));
  W32(D + 13052, R(32) == 0 ? uint32_t(rng()) : uint32_t(int32_t(R(300)) - 50));
  W32(D + 13056, R(32) == 0 ? uint32_t(rng()) : uint32_t(int32_t(R(200)) - 50));
  W32(D + 13060, R(32) == 0 ? uint32_t(rng()) : uint32_t(int32_t(R(1400)) + 100));
  W32(D + 13064, R(32) == 0 ? uint32_t(rng()) : uint32_t(int32_t(R(800)) + 100));
  // dirty words: zero, sparse or dense
  auto dirty = [] {
    switch (R(4)) {
      case 0: return uint64_t(0);
      case 1: return uint64_t(1) << R(64);
      case 2: return rng() & rng() & rng();
      default: return uint64_t(rng());
    }
  };
  W64(D + 16, dirty()), W64(D + 24, dirty()), W64(D + 32, dirty()), W64(D + 40, dirty());
  // ring: cursor + a limit that may trip any of the four reserve checks
  W32(D + 48, RING + 4 * R(16));
  W32(D + 56, R(4) == 0 ? RING + 0x8000 : RING + 4 * R(240));
  W32(D + 52, RING + 0x8000);
  W32(D + 13124, R(5));
  // game-image constants
  for (uint32_t i = 0; i < 16; i += 4) W32(0x82020BC0u + i, uint32_t(rng()));
  for (uint32_t i = 0; i < 144; i += 4) W32(0x82082F10u + i, uint32_t(rng()));
  for (uint32_t i = 0; i < 16; i += 4) W32(0x82082FC0u + i, RandFloatBits(0.f, 40000.f));
  for (uint32_t i = 0; i < 16; i += 4) W32(0x82082FB0u + i, RandFloatBits(-40000.f, 0.f));
  W32(0x82000A9Cu, RandFloatBits(0.f, 1.f));
  // rect array
  for (uint32_t i = 0; i < 8; ++i) {
    const int32_t l = int32_t(R(700)) - 60, t = int32_t(R(400)) - 40;
    W32(RECTS + 16 * i + 0, uint32_t(l)), W32(RECTS + 16 * i + 4, uint32_t(t));
    W32(RECTS + 16 * i + 8, uint32_t(l + int32_t(R(900)) - 30)), W32(RECTS + 16 * i + 12, uint32_t(t + int32_t(R(500)) - 20));
    if (R(16) == 0) W32(RECTS + 16 * i + 4 * R(4), uint32_t(rng()));
  }
  Args a{};
  const uint32_t nc = R(8);
  a.count = nc < 1 ? 0 : nc < 4 ? 1 : 2 + R(3);
  a.rects = R(3) == 0 ? 0 : RECTS + 16 * R(4);
  a.flags = R(4) == 0 ? rng() : (rng() & 0xFFFF);
  if (R(4) == 0) a.flags &= ~0xFull;
  if (R(4) == 0) a.flags &= ~0xF0ull;
  a.colour = uint32_t(rng());
  a.opaque = uint32_t(rng());
  const double fs[] = {0.0, 1.0, 0.5, -2.0};
  std::memcpy(&a.f1, &fs[R(4)], 8);
  if (R(4) == 0) a.f1 = rng();
  a.caller_sp = STACK + kStackBytes - 1024 + 4 * R(4);  // r1 mod 16 = 0/4/8/12 -> lvlx offsets 4/8/12/0
  return a;
}
}  // namespace

int main(int argc, char** argv) {
  const long cases = argc > 1 ? std::atol(argv[1]) : 50000;
  void* m = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) return std::perror("mmap"), 2;
  g_base = static_cast<uint8_t*>(m);
  static Regions before, ref;
  long bad = 0, planes = 0, reserves = 0, encoders = 0, early = 0;
  std::vector<std::pair<std::string, long>> sites;  // coverage per stub call site (lr)
  for (long c = 0; c < cases; ++c) {
    // Random device words make the guest code touch pages anywhere in the 4 GB space; give them back every case
    // (the 11-18 night runs grew to 22 GB and the kernel OOM killer took the whole session down).
    madvise(g_base, 1ull << 32, MADV_DONTNEED);
    const Args a = RandomState();
    before.Save(g_base);
    StubLog log_ref, log_nat;
    g_log = &log_ref;
    _mm_setcsr(0x1F80);
    PPCContext ctx{};
    ctx.fpscr.InitHost();
    ctx.r1.u64 = a.caller_sp;
    ctx.r3.u64 = D, ctx.r4.u64 = a.count, ctx.r5.u64 = a.rects, ctx.r6.u64 = a.flags, ctx.r7.u64 = a.colour;
    ctx.r8.u64 = rng(), ctx.r9.u64 = a.opaque;
    ctx.f1.u64 = a.f1;
    ref_sub_82CB8BA8(ctx, g_base);
    ref.Save(g_base);
    before.Load(g_base);
    g_log = &log_nat;
    _mm_setcsr(0x1F80);
    TestM tm{g_base, a.caller_sp - 528};
    wwe2k14::d3d::native_clear::Clear(tm, a.caller_sp, D, a.count, a.rects, a.flags, a.colour, a.opaque, a.f1);
    Regions nat;
    nat.Save(g_base);
    for (auto& s : log_ref.calls) {
      planes += s.rfind("plane", 0) == 0;
      reserves += s.rfind("reserve", 0) == 0;
      encoders += s.rfind("encoder", 0) == 0;
      const std::string key = s.substr(0, s.find(' ', s.find(' ') + 1));
      auto it = std::find_if(sites.begin(), sites.end(), [&](auto& p) { return p.first == key; });
      if (it == sites.end()) sites.emplace_back(key, 1); else ++it->second;
    }
    early += log_ref.calls.empty();
    std::string what;
    if (!tm.ok) what += " guest-call";
    if (nat.dev != ref.dev)
      for (uint32_t i = 0; i < kDev; ++i)
        if (nat.dev[i] != ref.dev[i]) { what += " dev+" + std::to_string(i); break; }
    if (nat.ring != ref.ring || nat.ring2 != ref.ring2) what += " ring";
    if (nat.surf != ref.surf) what += " surf";
    if (log_nat.calls != log_ref.calls)
      what += " calls(" + std::to_string(log_ref.calls.size()) + "/" + std::to_string(log_nat.calls.size()) + ")";
    if (!what.empty() && std::getenv("CL_VERBOSE") &&
        (std::getenv("CL_CASE") ? c == std::atol(std::getenv("CL_CASE")) : bad == 0)) {
      auto be = [](const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
      int n = 0;
      for (uint32_t i = 0; i < kDev && n < 12; i += 4)
        if (std::memcmp(&nat.dev[i], &ref.dev[i], 4)) std::printf("  dev+%u ref=%08X nat=%08X\n", i, be(&ref.dev[i]), be(&nat.dev[i])), ++n;
      auto ringdiff = [&](const std::vector<uint8_t>& r, const std::vector<uint8_t>& q, uint32_t ba, const char* nm) {
        int k = 0;
        for (uint32_t i = 0; i < kRing && k < 12; i += 4)
          if (std::memcmp(&r[i], &q[i], 4)) std::printf("  %s %08X ref=%08X nat=%08X\n", nm, ba + i, be(&r[i]), be(&q[i])), ++k;
      };
      ringdiff(ref.ring, nat.ring, RING, "ring");
      ringdiff(ref.ring2, nat.ring2, RING2, "ring2");
      for (size_t i = 0; i < std::max(log_ref.calls.size(), log_nat.calls.size()); ++i)
        std::printf("  call %zu\n    ref[%s]\n    nat[%s]\n", i, i < log_ref.calls.size() ? log_ref.calls[i].c_str() : "-",
                    i < log_nat.calls.size() ? log_nat.calls[i].c_str() : "-");
    }
    if (!what.empty() && ++bad <= 10)
      std::printf("case %ld count=%u rects=%d flags=%llx sp%%16=%u: MISMATCH%s\n", c, a.count, a.rects != 0,
                  (unsigned long long)a.flags, a.caller_sp & 15, what.c_str());
  }
  for (auto& p : sites) std::printf("  site %-24s %ld\n", p.first.c_str(), p.second);
  std::printf("native-clear: %ld cases, %ld mismatching (coverage: %ld plane calls, %ld reserves, %ld encoder calls, %ld "
              "cases without any stub call)\n%s\n",
              cases, bad, planes, reserves, encoders, early, bad ? "NATIVE_CLEAR_TEST_FAIL" : "NATIVE_CLEAR_TEST_PASS");
  return bad ? 1 : 0;
}
