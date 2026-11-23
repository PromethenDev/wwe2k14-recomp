// Native Resolve worker host test (overnight 11-18, docs/tasks/overnight-1118/): the native sub_82CB2320
// (native_resolve_core.h) against the REAL generated guest body and its callees, copied by tools/nr/gen-ref-closure.py into
// out/nresolve/rw_ref.inc. Random device / record / source / rect / flag states; both runs share deterministic stubs for
// the ring reserve, allocators, dispatchers, memcpy and the fill hook (each call is logged and the logs compared).
// Compares: the device block, both ring buffers, the record, the scratch and list areas, the stub call logs, and r3.
// Build + run: tools/nr/native-resolve-test.sh [cases]
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "native/d3d/native_resolve_core.h"
#include "rw_ref.inc"

namespace {
constexpr uint32_t D = 0x30000000u, kDev = 16384, RING = 0x50000000u, RING2 = 0x51000000u, kRing = 0x10000;
constexpr uint32_t REC = 0x40000000u, SRC = 0x40001000u, RECT = 0x40002000u, FMT = 0x40003000u, FACTOR = 0x40004000u;
constexpr uint32_t SCRATCH = 0x52000000u, LIST = 0x53000000u, STACK = 0x7F000000u, kStackBytes = 0x4000;
uint8_t* g_base;

struct StubLog {
  std::vector<std::string> calls;
  void Add(const char* name, const PPCContext& c, int n) {
    if (calls.size() > 4096) return;  // a looping guest path must not grow without bound
    char b[160];
    int k = std::snprintf(b, sizeof b, "%s", name);
    const uint64_t r[8] = {c.r3.u32, c.r4.u64, c.r5.u32, c.r6.u32, c.r7.u32, c.r8.u32, c.r9.u32, c.r10.u32};  // r4 64-bit (masks)
    for (int i = 0; i < n; ++i) k += std::snprintf(b + k, sizeof b - k, " %llx", (unsigned long long)r[i]);
    calls.emplace_back(b);
  }
};
StubLog* g_log;
uint32_t g_scratch_fail = 0;
uint64_t g_f1_bits = 0;
}  // namespace

// Shared stubs (both runs). Ring reserve: switch to RING2 with a roomy limit (same as native-rt-test).
static void ref_sub_82CBA798(PPCContext& __restrict ctx, uint8_t* base) {
  g_log->Add("reserve", ctx, 1);
  PPC_STORE_U32(ctx.r3.u32 + 56, RING2 + 0x8000);
  PPC_STORE_U32(ctx.r3.u32 + 52, RING2 + 0x8000);  // segment end: 82CC0D18's short path loops on it (11-18 OOM)
  ctx.r3.u64 = RING2;
}
static void ref_sub_82CB20C0(PPCContext& __restrict ctx, uint8_t*) { g_log->Add("flushdisp", ctx, 2); ctx.r3.u64 = 0; }
static void ref_sub_82CBBD68(PPCContext& __restrict ctx, uint8_t*) { g_log->Add("listres", ctx, 1); ctx.r3.u64 = LIST; }
static void ref_sub_82CB9588(PPCContext& __restrict ctx, uint8_t*) {
  g_log->Add("scratch", ctx, 3);
  ctx.r3.u64 = g_scratch_fail ? 0 : SCRATCH;
}
static void ref_sub_82CC0988(PPCContext& __restrict ctx, uint8_t* base) {
  g_log->Add("encoder", ctx, 3);
  ctx.r3.u64 = ctx.r4.u64 & 0x00FF00FF00FF00FFull;  // deterministic "remaining groups" mask
}
static void ref_sub_82CB5AE8(PPCContext& __restrict ctx, uint8_t*) { g_log->Add("cb5ae8", ctx, 1); }
static void ref_sub_82C7C8F0(PPCContext& __restrict ctx, uint8_t* base) {
  PPCContext lc = ctx;
  lc.r4.u64 = ctx.r4.u32;  // the guest address may be sign-extended (lis): compare the low 32 bits
  g_log->Add("memcpy", lc, 3);
  std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32);
}
static void ref_sub_82CB1FE8(PPCContext& __restrict ctx, uint8_t*) { g_log->Add("fillhook", ctx, 3); }

namespace {
using Fn = void (*)(PPCContext&, uint8_t*);
Fn Lookup(uint32_t a) {
  switch (a) {
#define F(x) case 0x##x##u: return ref_sub_##x;
    F(82CBA798) F(82CB20C0) F(82CBBD68) F(82CB9588) F(82CC0988) F(82CB5AE8) F(82C7C8F0) F(82CB1FE8)
    F(82CAA500) F(82CAB1E8) F(82CB18F0) F(82CB1A50) F(82CAC5C0) F(82CC0D18) F(82CAA430) F(82CAA740)
#undef F
    default: return nullptr;
  }
}

struct TestM {
  uint8_t* base;
  uint32_t sp;  // guest r1 inside the worker frame (callees run with it)
  bool ok = true;
  uint8_t L8(uint32_t a) { return PPC_LOAD_U8(a); }
  uint16_t L16(uint32_t a) { return PPC_LOAD_U16(a); }
  uint32_t L32(uint32_t a) { return PPC_LOAD_U32(a); }
  uint64_t L64(uint32_t a) { return PPC_LOAD_U64(a); }
  void S8(uint32_t a, uint8_t v) { PPC_STORE_U8(a, v); }
  void S16(uint32_t a, uint16_t v) { PPC_STORE_U16(a, v); }
  void S32(uint32_t a, uint32_t v) { PPC_STORE_U32(a, v); }
  void S64(uint32_t a, uint64_t v) { PPC_STORE_U64(a, v); }
  uint64_t Call(uint32_t addr, uint32_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8,
                uint64_t r9, uint64_t r10, double f1 = 0.0) {
    Fn fn = Lookup(addr);
    if (!fn) {
      std::printf("native called unknown guest function %08X\n", addr);
      ok = false;
      return 0;
    }
    PPCContext c{};
    c.r1.u64 = sp;
    c.r3.u64 = r3, c.r4.u64 = r4, c.r5.u64 = r5, c.r6.u64 = r6, c.r7.u64 = r7, c.r8.u64 = r8, c.r9.u64 = r9, c.r10.u64 = r10;
    c.f1.f64 = f1;
    c.lr = lr;
    fn(c, base);
    return c.r3.u64;
  }
  uint64_t CallF(uint32_t addr, uint32_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8,
                 uint64_t r9, uint64_t r10, uint64_t f1_bits) {
    double d;
    std::memcpy(&d, &f1_bits, 8);
    return Call(addr, lr, r3, r4, r5, r6, r7, r8, r9, r10, d);
  }
  uint64_t CallV(uint32_t addr, uint32_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8,
                 uint64_t r9, uint64_t r10, uint32_t v1_addr) {
    (void)v1_addr;  // 82CB1FE8 is a logged stub in this test (its tree reaches the kernel)
    return Call(addr, lr, r3, r4, r5, r6, r7, r8, r9, r10);
  }
};

struct Regions {  // everything either run may write
  std::vector<uint8_t> dev, ring, ring2, rec, scratch, list;
  void Save(uint8_t* b) {
    dev.assign(b + D, b + D + kDev);
    ring.assign(b + RING, b + RING + kRing);
    ring2.assign(b + RING2, b + RING2 + kRing);
    rec.assign(b + REC, b + REC + 256);
    scratch.assign(b + SCRATCH, b + SCRATCH + 256);
    list.assign(b + LIST, b + LIST + 256);
  }
  void Load(uint8_t* b) const {
    std::memcpy(b + D, dev.data(), kDev);
    std::memcpy(b + RING, ring.data(), kRing);
    std::memcpy(b + RING2, ring2.data(), kRing);
    std::memcpy(b + REC, rec.data(), 256);
    std::memcpy(b + SCRATCH, scratch.data(), 256);
    std::memcpy(b + LIST, list.data(), 256);
  }
};

std::mt19937_64 rng(4321);
uint32_t R(uint32_t n) { return uint32_t(rng() % n); }
void W32(uint32_t a, uint32_t v) { uint8_t* base = g_base; PPC_STORE_U32(a, v); }
void W16(uint32_t a, uint16_t v) { uint8_t* base = g_base; PPC_STORE_U16(a, v); }

struct Args {
  uint32_t flags, rect, payload, caller_flag, rec, src, mode, stk468, stk476, stk492, stk500;
  double f1;
};

Args RandomState() {
  for (uint32_t i = 0; i < kDev; i += 4) W32(D + i, uint32_t(rng()));
  for (uint32_t i = 0; i < 256; i += 4) W32(REC + i, uint32_t(rng())), W32(SRC + i, uint32_t(rng()));
  // record fields that steer the body: format code (rec+32 low 6 bits), sample mode (rec+48 & 0x600), dims
  static const uint32_t codes[] = {6, 7, 16, 17, 22, 23, 26, 27, 28, 29, 30, 31, 32, 36, 37, 38, 50, 54, 55, 56, 57, 61, 63};
  W32(REC + 32, (uint32_t(rng()) & ~0x3Fu) | codes[R(sizeof codes / 4)]);
  W32(REC + 48, (uint32_t(rng()) & ~0x600u) | (R(4) << 9));
  W32(REC + 36, uint32_t(rng()) & 0x03FFFFFFu);
  W32(REC + 40, uint32_t(rng()) & 0x0000FFFFu);
  W32(REC + 28, uint32_t(rng()));
  // format objects behind D+12816 + 4 * (flags & 7)
  for (uint32_t k = 0; k < 5; ++k) {
    W32(D + 12816 + 4 * k, FMT + 64 * k);
    W16(FMT + 64 * k + 24, uint16_t(rng()));
  }
  W32(D + 13124, R(5));  // tile count
  W32(D + 48, RING + 4 * R(16));
  W32(D + 56, R(3) == 0 ? RING + 4 * R(24) : RING + 0x8000);
  W32(D + 52, RING + 0x8000);
  for (uint32_t k = 0; k < 4; ++k) W32(FACTOR + 4 * k, uint32_t(rng()));
  // game-image constants the body reads
  W32(0x83947020u, R(2)), W32(0x83947024u, R(2)), W32(0x83947028u, R(2));
  for (uint32_t i = 0; i < 128; ++i) { uint8_t* base = g_base; PPC_STORE_U8(0x820828A1u + i, uint8_t(1 + R(32))); }
  for (uint32_t i = 0; i < 16; i += 4) W32(0x82020BC0u + i, uint32_t(rng()));
  for (uint32_t i = 0; i < 48; i += 4) W32(0x82082B28u + i, uint32_t(rng()));
  for (uint32_t i = 0; i < 64; i += 4) W32(0x81C82A60u + i, uint32_t(rng()));
  const int32_t l = int32_t(R(1300)), t = int32_t(R(700));
  W32(RECT + 0, uint32_t(l)), W32(RECT + 4, uint32_t(t));
  W32(RECT + 8, uint32_t(l + 1 + int32_t(R(1300)))), W32(RECT + 12, uint32_t(t + 1 + int32_t(R(700))));
  g_scratch_fail = R(16) == 0;
  Args a{};
  a.flags = uint32_t(rng()) & 0x0000F3FFu;
  a.rect = R(4) == 0 ? 0 : RECT;
  a.payload = uint32_t(rng());
  a.caller_flag = R(3) == 0 ? 1 : 0;
  a.rec = REC;
  a.src = R(4) == 0 ? 0 : SRC;
  a.mode = R(3);
  a.stk468 = R(2) ? 0 : uint32_t(rng()) & 0xFFFF;
  a.stk476 = R(2) ? 0 : FACTOR;
  a.stk492 = R(4);
  a.stk500 = a.src;
  const float fs[] = {0.0f, 0.5f, 1.0f, 2.0f};
  a.f1 = fs[R(4)];
  return a;
}

void SetupCaller(uint8_t* base, const Args& a) {
  // The worker reads stack arguments from its caller's frame: caller sp + 84 / 92 / 100 / 108 / 116 (stk468.. after
  // its own 384-byte frame). Both runs start from the same caller frame.
  const uint32_t caller_sp = STACK + kStackBytes - 512;
  std::memset(base + STACK, 0, kStackBytes);
  PPC_STORE_U32(caller_sp + 84, a.stk468);
  PPC_STORE_U32(caller_sp + 92, a.stk476);
  PPC_STORE_U32(caller_sp + 100, 0);
  PPC_STORE_U32(caller_sp + 108, a.stk492);
  PPC_STORE_U32(caller_sp + 116, a.stk500);
}
}  // namespace

int main(int argc, char** argv) {
  const long cases = argc > 1 ? std::atol(argv[1]) : 50000;
  void* m = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) return std::perror("mmap"), 2;
  g_base = static_cast<uint8_t*>(m);
  static Regions before, ref;
  long bad = 0;
  const uint32_t caller_sp = STACK + kStackBytes - 512;
  for (long c = 0; c < cases; ++c) {
    // Random device words make the guest code touch pages anywhere in the 4 GB space; give them back every 64 cases
    // (the 11-18 night runs grew to 22 GB and the kernel OOM killer took the whole session down).
    madvise(g_base, 1ull << 32, MADV_DONTNEED);
    const Args a = RandomState();
    SetupCaller(g_base, a);
    before.Save(g_base);
    StubLog log_ref, log_nat;
    g_log = &log_ref;
    PPCContext ctx{};
    ctx.r1.u64 = caller_sp;
    ctx.r3.u64 = D, ctx.r4.u64 = a.flags, ctx.r5.u64 = a.rect, ctx.r6.u64 = a.payload, ctx.r7.u64 = a.caller_flag;
    ctx.r8.u64 = a.rec, ctx.r9.u64 = a.src, ctx.r10.u64 = a.mode;
    ctx.f1.f64 = a.f1;
    std::memcpy(&g_f1_bits, &a.f1, 8);
    if (std::getenv("RW_PROGRESS")) std::fprintf(stderr, "case %ld ref\n", c);
    ref_sub_82CB2320(ctx, g_base);
    if (std::getenv("RW_PROGRESS")) std::fprintf(stderr, "case %ld native\n", c);
    ref.Save(g_base);
    before.Load(g_base);
    SetupCaller(g_base, a);
    g_log = &log_nat;
    TestM tm{g_base, caller_sp - 384};
    wwe2k14::d3d::native_resolve::Resolve(tm, caller_sp, D, a.flags, a.rect, a.payload, a.caller_flag, a.rec, a.src,
                                          a.mode, a.f1);
    Regions nat;
    nat.Save(g_base);
    std::string what;
    if (!tm.ok) what += " guest-call";
    if (nat.dev != ref.dev) {
      for (uint32_t i = 0; i < kDev; ++i)
        if (nat.dev[i] != ref.dev[i]) { what += " dev+" + std::to_string(i); break; }
    }
    if (nat.ring != ref.ring || nat.ring2 != ref.ring2) what += " ring";
    if (nat.rec != ref.rec) what += " record";
    if (nat.scratch != ref.scratch) what += " scratch";
    if (nat.list != ref.list) what += " list";
    if (log_nat.calls != log_ref.calls) what += " calls(" + std::to_string(log_ref.calls.size()) + "/" +
                                                std::to_string(log_nat.calls.size()) + ")";
    if (!what.empty() && std::getenv("RW_VERBOSE") && (std::getenv("RW_CASE") ? c == std::atol(std::getenv("RW_CASE")) : bad == 0)) {
      int n = 0;
      for (uint32_t i = 0; i < kDev && n < 12; i += 4)
        if (std::memcmp(&nat.dev[i], &ref.dev[i], 4)) {
          auto be = [](const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
          std::printf("  dev+%u ref=%08X nat=%08X\n", i, be(&ref.dev[i]), be(&nat.dev[i]));
          ++n;
        }
      auto ringdiff = [&](const std::vector<uint8_t>& r, const std::vector<uint8_t>& q, uint32_t baseaddr, const char* nm) {
        int k = 0;
        for (uint32_t i = 0; i < kRing && k < 12; i += 4)
          if (std::memcmp(&r[i], &q[i], 4)) {
            auto be = [](const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
            std::printf("  %s %08X ref=%08X nat=%08X\n", nm, baseaddr + i, be(&r[i]), be(&q[i]));
            ++k;
          }
      };
      ringdiff(ref.ring, nat.ring, RING, "ring");
      for (uint32_t i = 0; i < 256; i += 4)
        if (std::memcmp(&ref.scratch[i], &nat.scratch[i], 4))
          std::printf("  scratch+%u ref=%02X%02X%02X%02X nat=%02X%02X%02X%02X\n", i, ref.scratch[i], ref.scratch[i+1], ref.scratch[i+2], ref.scratch[i+3], nat.scratch[i], nat.scratch[i+1], nat.scratch[i+2], nat.scratch[i+3]);
      ringdiff(ref.ring2, nat.ring2, RING2, "ring2");
      for (size_t i = 0; i < std::max(log_ref.calls.size(), log_nat.calls.size()); ++i)
        std::printf("  call %zu ref[%s] nat[%s]\n", i, i < log_ref.calls.size() ? log_ref.calls[i].c_str() : "-",
                    i < log_nat.calls.size() ? log_nat.calls[i].c_str() : "-");
    }
    if (!what.empty() && ++bad <= 10)
      std::printf("case %ld flags=%08X rect=%d src=%d caller=%u mode=%u: MISMATCH%s\n", c, a.flags, a.rect != 0,
                  a.src != 0, a.caller_flag, a.mode, what.c_str());
  }
  std::printf("native-resolve: %ld cases, %ld mismatching\n%s\n", cases, bad,
              bad ? "NATIVE_RESOLVE_TEST_FAIL" : "NATIVE_RESOLVE_TEST_PASS");
  return bad ? 1 : 0;
}
