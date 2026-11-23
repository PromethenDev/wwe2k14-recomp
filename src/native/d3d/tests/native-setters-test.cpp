// F1 host test. Build: g++ -std=c++20 -I src src/native/d3d/tests/native-setters-test.cpp -o t && ./t
// Reference = literal model of the generated guest body (lvlx/lvrx merge row by row, stvx to an aligned address).
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
#include "native/d3d/native_setters_core.h"
using namespace wwe2k14::d3d::native_setters;
static void Lvlx(const uint8_t* m, uint32_t a, uint8_t* out) {  // bytes from a up to the next 16-byte boundary, left-justified
  uint32_t n = 16 - (a & 15);
  std::memset(out, 0, 16);
  std::memcpy(out, m + a, n);
}
static void Lvrx(const uint8_t* m, uint32_t a, uint8_t* out) {  // bytes before a in its 16-byte block, right-justified; 0 if aligned
  std::memset(out, 0, 16);
  uint32_t n = a & 15;
  if (n) std::memcpy(out + 16 - n, m + (a & ~15u), n);
}
static void GuestBody(std::vector<uint8_t>& m, uint32_t dev, uint32_t first, uint32_t src, uint32_t count, uint64_t bits, bool ps) {
  uint32_t off = ((first + ConstBaseRow(ps)) << 4) & 0xFFFFFFF0u;
  uint32_t d = off + dev, s = src;
  for (uint32_t i = 0; i < count; ++i, s += 16, d += 16) {
    uint8_t a[16], b[16], v[16];
    Lvlx(m.data(), s, a);
    Lvrx(m.data(), s + 16, b);
    for (int k = 0; k < 16; ++k) v[k] = a[k] | b[k];
    std::memcpy(m.data() + (d & ~15u), v, 16);
  }
  OrDirtyBe(m.data() + dev + DirtyOffset(ps), bits);
}
static void Native(std::vector<uint8_t>& m, uint32_t dev, uint32_t first, uint32_t src, uint32_t count, uint64_t bits, bool ps) {
  const uint32_t dest = ConstDest(dev, first, ps);
  for (uint32_t i = 0; i < count; ++i) std::memcpy(m.data() + dest + i * 16, m.data() + src + i * 16, 16);
  OrDirtyBe(m.data() + dev + DirtyOffset(ps), bits);
}
int main() {
  std::mt19937 r(1234);
  const uint32_t dev = 0x1000;
  std::vector<uint8_t> a(0x10000);
  for (auto& x : a) x = uint8_t(r());
  int cases = 0;
  for (int t = 0; t < 200000; ++t) {
    const bool ps = t & 1;
    uint32_t count = (t % 7 == 0) ? 0 : 1 + r() % ((t % 11 == 0) ? 256 : 8);
    uint32_t first = count >= kMaxRows ? 0 : r() % (kMaxRows - count + 1);
    uint32_t src = 0x8000 + r() % 0x4000;  // any alignment, outside the device
    uint64_t bits = (uint64_t(r()) << 32) | r();
    assert(ConstArgsOk(dev, first, count));
    auto ga = a, na = a;
    GuestBody(ga, dev, first, src, count, bits, ps);
    Native(na, dev, first, src, count, bits, ps);
    assert(ga == na);
    if (t % 500 == 0) a = ga;
    ++cases;
  }
  assert(ConstDest(0x1000, 0, false) == 0x1000 + 1920);
  assert(ConstDest(0x1000, 0, true) == 0x1000 + 6016);
  assert(ConstDest(0x1000, 255, true) == 0x1000 + 6016 + 255 * 16);
  uint8_t w[8] = {0, 0, 0, 0, 0, 0, 0, 1};
  OrDirtyBe(w, 0x8000000000000002ull);
  assert(w[0] == 0x80 && w[7] == 3);
  assert(!ConstArgsOk(0x1004, 0, 1) && !ConstArgsOk(0x1000, 200, 57) && ConstArgsOk(0x1000, 200, 56));
  assert(!AbNewArm(0, 600) && !AbNewArm(599, 600) && AbNewArm(600, 600) && !AbNewArm(1200, 600) && AbNewArm(5, 0));
  // F1 step 3: stream fetch words against a literal model of generated 85:4 (rlwinm + add sequence).
  auto rotl32 = [](uint32_t v, unsigned n) { return n ? (v << n) | (v >> (32 - n)) : v; };
  for (int t = 0; t < 2000000; ++t) {
    const uint32_t buffer = (uint32_t(r()) << 1) ^ r(), offset = r();
    const uint32_t a = buffer + offset;
    const uint32_t top = rotl32(a, 12) & 0xFFFu;
    const uint32_t model = (rotl32(top + 512u, 0) & 0x1000u) + (a & 0x1FFFFFFFu);
    assert(StreamFetchAddr(buffer, offset) == model);
  }
  for (uint32_t st = 0; st <= 17; ++st) {
    const uint32_t r11 = 17u - st;
    assert(StreamFetchAddrOffset(st) == (rotl32(r11 + 222u, 3) & 0xFFFFFFF8u));
    assert(StreamFetchSizeOffset(st) == (rotl32(r11, 3) & 0xFFFFFFF8u) + 1780u);
  }
  assert(StreamFetchAddrOffset(0) == 1912 && StreamFetchSizeOffset(0) == 1916);  // 8 bytes apart: one fetch constant
  // F1 step 4: fetch merge against a literal model of generated 84:67956 (register by register, same masks and order).
  for (int t = 0; t < 2000000; ++t) {
    TexIn i{};
    i.t28 = r(); i.t32 = r(); i.t36 = r(); i.t40 = r(); i.t44 = r(); i.t48 = r();
    i.w0 = r(); i.w4 = r(); i.w12 = r(); i.w16 = r(); i.w20 = r();
    i.min_clamp = uint8_t(r() % 16); i.max_clamp = uint8_t(r() % 16);
    uint64_t r27 = i.t48, r7 = i.t32, r26 = i.t44, r25 = i.t28;
    uint64_t r9 = rotl32(uint32_t(r27), 12) & 0xFFF, r10 = rotl32(uint32_t(r7), 12) & 0xFFF;
    r9 += 512; r10 += 512;
    uint64_t r30 = r7 & 0x1FFFFFFF, r8 = rotl32(uint32_t(r9), 0) & 0x1000, r3 = rotl32(uint32_t(r10), 0) & 0x1000;
    r7 = rotl32(uint32_t(r27), 0) & 0x1FFFFE00;
    r10 = r26;
    r7 = r8 + r7;
    r3 = r3 + r30;
    r10 = (i.w16 & 0xFFFFFFFFFFFFFC03ull) | (r10 & 0x3FC);
    r25 = (i.w0 & 0x3FFC00) | (r25 & 0xFFFFFFFFFFC003FFull);
    r3 = (i.w4 & 0x800) | (r3 & 0xFFFFFFFFFFFFF7FFull);
    uint64_t r23 = (i.w12 & 0x7FF80000) | (i.t40 & 0xFFFFFFFF8007FFFFull);
    r7 = (i.w20 & 0x1FF) | (r7 & 0xFFFFFFFFFFFFFE00ull);
    uint64_t lo = rotl32(uint32_t(r26), 30) & 0xF, m8 = i.min_clamp;
    if (lo > m8) m8 = rotl32(uint32_t(r26), 30) & 0xF;
    r10 = (rotl32(uint32_t(m8), 2) & 0x3C) | (r10 & 0xFFFFFFFFFFFFFFC3ull);
    uint64_t m9 = i.max_clamp, hi = rotl32(uint32_t(r26), 26) & 0xF;
    if (hi < m9) m9 = hi;
    r10 = (rotl32(uint32_t(m9), 6) & 0x3C0) | (r10 & 0xFFFFFFFFFFFFFC3Full);
    const TexOut o = MergeTextureFetch(i);
    assert(o.w0 == uint32_t(r25) && o.w4 == uint32_t(r3) && o.w8 == i.t36 && o.w12 == uint32_t(r23));
    assert(o.w16 == uint32_t(r10) && o.w20 == uint32_t(r7));
  }
  printf("native-setters-test PASS (%d cases)\n", cases);
}
