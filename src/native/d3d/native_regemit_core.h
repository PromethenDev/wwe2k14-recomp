// N4 step 3 (fully native): native body of the register-run writer sub_82CC0D18 ("RegEmitHelper", generated/
// wwe2k14_recomp.85.cpp) and its short-segment path sub_82CC06D8.
//   RegEmit(D, mask r4 (64-bit, MSB = register reg0), reg0 r5, values r6): for each run of set bits, from the top: skip the
//   clear bits (advancing the value pointer and register), then write one Type-0 header ((run - 1) << 16 | first register)
//   and the run's values at the ring cursor D+48. When cursor + 4 * run is not below the segment end D+52, the run goes
//   through 82CC06D8 instead: as many values as fit (header each time), then the ring reserve 82CBA798 (hooked), repeat.
//   Returns the guest r3 (D, or the cursor the last short-segment call returned). A zero mask makes the guest loop forever
//   (header count -1): no caller passes one; the native body is not used for it (the wrapper runs the guest body).
// Pure: guest access through the policy M (see native_shader_core.h); M::RingReserve(dev, lr) -> cursor (guest 82CBA798).
#pragma once
#include <cstdint>

namespace wwe2k14::d3d::native_shader {

namespace regemit {
inline uint32_t Clz64(uint64_t v) { return v ? uint32_t(__builtin_clzll(v)) : 64u; }
inline uint64_t Shl64(uint64_t v, uint32_t n) { return n >= 64 ? 0 : v << n; }  // PPC sld: shifts of 64..127 give 0

// sub_82CC06D8(D, cursor, reg, values - 4, count, step 1) -> cursor.
template <class M>
uint32_t Short(M& m, uint32_t D, uint32_t q, uint64_t reg, uint32_t src, uint32_t n) {
  for (;;) {
    const int32_t avail = (int32_t(m.L32(D + 52) - q) >> 2) - 1;  // step 1: divw by 1
    if (avail > 0) {
      uint32_t cnt = uint32_t(avail);
      if (!(cnt < n)) cnt = n;
      n -= cnt;
      m.S32(q + 4, (((cnt - 1) << 16) & 0xFFFF0000u) | uint32_t(reg));
      q += 4;
      for (uint32_t i = 0; i < cnt; ++i) {
        src += 4;
        m.S32(q + 4, m.L32(src));
        q += 4;
      }
      reg += cnt;
    }
    if (n == 0) return q;
    m.S32(D + 48, q);
    m.RingReserve(D, 0x82CC0778u);
    q = m.L32(D + 48);
  }
}
}  // namespace regemit

// sub_82CC0D18. mask != 0.
template <class M>
uint64_t RegEmit(M& m, uint32_t D, uint64_t mask, uint64_t reg0, uint32_t values) {
  using namespace regemit;
  uint64_t r3 = D;
  uint32_t q = m.L32(D + 48);
  uint32_t p = values - 4;
  uint64_t reg = reg0;
  do {
    const uint32_t lz = Clz64(mask);
    const uint32_t lim = m.L32(D + 52);
    mask = Shl64(mask, lz);
    p += 4 * lz;
    const uint32_t run = Clz64(~mask);
    const uint64_t first = lz + reg;
    if (q + 4 * run < lim) {
      reg = run + first;
      m.S32(q + 4, (((run - 1) << 16) & 0xFFFF0000u) | uint32_t(first));
      q += 4;
      for (uint32_t k = run; k != 0; --k) {
        p += 4;
        mask <<= 1;
        m.S32(q + 4, m.L32(p));
        q += 4;
      }
    } else {
      r3 = q = Short(m, D, q, first, p, run);
      p += 4 * run;
      reg = run + first;
      mask = Shl64(mask, run);
    }
  } while (mask != 0);
  m.S32(D + 48, q);
  return r3;
}

}  // namespace wwe2k14::d3d::native_shader
