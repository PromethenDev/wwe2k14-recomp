// N4 (fully native): native bodies of the guest SetPixelShader sub_82CB42A0 and SetVertexShader sub_82CB44A8
// (generated/wwe2k14_recomp.85.cpp 13008-13254 and 13303-13560; the two differ only where marked `vs`).
//   1. Old shader release: if the device holds a shader, either stamp the current fence into old+8 (D+11036 != 0) or,
//      when (D+11040 & old+0) != 0, append an 8-byte entry {flags | old >> 2, 0xFFFFFFFF} to the deferred list at
//      D+13928 (limit D+13932; full -> guest helper 82CBBD68 returns the new slot). The entry's top bit comes from an
//      uninitialised stack word at (sp - 128) + 80, exactly as the guest builds it (bit 30 cleared, bit 31 kept).
//   2. D+13068 (PS) / D+13072 (VS) = shader; PS: dirty D+16 |= 0x100000 | 0x20000; VS: D+16 |= 0x80000 when the shader
//      is non-NULL (before step 1) and byte 11070 &= 0x7F.
//   3. Shader patch block t = info + [info+20] (info = PS+40 / VS+872): constant dirty mask D+8 (PS) / D+0 (VS) &= ~t[0];
//      t[8] != 0 -> D+32 |= 1 << 56; then the fetch-constant patch list at t+20 (length [t+16] bytes): a skipped run of
//      8-byte entries up to the first zero-count entry, direct copies (offset u16, count u16 words) into D+1152+offset,
//      then masked runs ((dev & mask) | value per word).
// Pure: every guest access goes through the policy M (see native_rt_core.h): L8/L16/L32/L64, S8/S32/S64,
// M::Reserve(dev, lr) -> slot (guest 82CBBD68), M::RingReserve(dev, lr) -> cursor (guest 82CBA798),
// M::Copy(dst, src, n, lr) -> r3 (guest memcpy 82C7C8F0 returns dst).
#pragma once
#include <cstdint>

namespace wwe2k14::d3d::native_shader {

// Returns the guest r3 at the end of the body.
template <class M>
uint64_t SetShader(M& m, bool vs, uint32_t D, uint64_t r3in, uint64_t r4in, uint32_t sp) {
  uint64_t r3 = r3in;
  const uint32_t S = uint32_t(r4in);
  if (vs && S != 0) m.S64(D + 16, m.L64(D + 16) | 524288);
  const uint32_t old = m.L32(D + (vs ? 13072u : 13068u));
  if (old != 0) {
    const uint32_t fence = m.L32(D + 11036);
    if (fence != 0) {
      m.S32(old + 8, fence);
    } else if (int32_t(m.L32(D + 11040) & m.L32(old + 0)) != 0) {
      const uint32_t lim = m.L32(D + 13932);
      r3 = m.L32(D + 13928);
      if (!(uint32_t(r3) < lim)) r3 = m.Reserve(D, vs ? 0x82CB4518u : 0x82CB42FCu);
      const uint32_t nsp = sp - 128;
      const uint32_t w = m.L32(nsp + 80);
      const uint32_t merged = (((old >> 2) & 0x3FFFFFFFu) | (w & 0xC0000000u)) & 0xBFFFFFFFu;
      m.S32(nsp + 84, 0xFFFFFFFFu);
      m.S32(nsp + 80, merged);
      m.S64(uint32_t(r3), uint64_t(merged) << 32 | 0xFFFFFFFFu);
      m.S32(D + 13928, uint32_t(r3 + 8));
    }
  }
  if (vs) {
    const uint8_t b = m.L8(D + 11070);
    m.S32(D + 13072, S);
    m.S8(D + 11070, uint8_t(b & 0x7F));
  } else {
    m.S32(D + 13068, S);
    uint64_t d = m.L64(D + 16) | 1048576;
    m.S64(D + 16, d);
    m.S64(D + 16, d | 131072);
  }
  if (S == 0) return r3;
  const uint32_t info = S + (vs ? 872u : 40u);
  if (vs && int32_t(info) == 0) return r3;
  const uint32_t rel = m.L32(info + 20);
  if (rel == 0) return r3;
  const uint32_t t = rel + info;
  const uint32_t mask_off = vs ? 0u : 8u;
  m.S64(D + mask_off, m.L64(D + mask_off) & ~m.L64(t + 0));
  if (m.L64(t + 8) != 0) m.S64(D + 32, m.L64(D + 32) | (1ull << 56));
  const uint32_t len = m.L32(t + 16);
  uint32_t p = t + 20;
  const uint32_t end = len + p;
  const uint32_t fetch = D + 1152;
  if (!(p < end)) return r3;
  for (;;) {  // skipped run (8-byte entries) up to the first zero-count entry
    const uint32_t c = m.L16(p + 2);
    p += 4;
    if (c == 0) break;
    p += 4;
    if (!(p < end)) break;
  }
  if (!(p < end)) return r3;
  // direct copies until a zero-count entry
  for (;;) {
    const uint32_t c = m.L16(p + 2), off = m.L16(p + 0);
    p += 4;
    if (c == 0) break;
    const uint32_t n = (c << 2) & 0xFFFFFFFCu;
    r3 = m.Copy(off + fetch, p, n, vs ? 0x82CB4608u : 0x82CB43F0u);
    p = n + p;
    if (!(p < end)) break;
  }
  // masked runs: (dev & mask) | value, two words per step
  while (p < end) {
    uint32_t c = m.L16(p + 2);
    const uint32_t off = m.L16(p + 0);
    p += 4;
    if (c == 0) break;
    uint32_t q = off + fetch - 4;
    do {
      const uint32_t mask = m.L32(p);
      c = (c + 65536 - 2) & 0xFFFF;
      const uint32_t cur = m.L32(q + 4);
      const uint32_t val = m.L32(p + 4);
      p += 8;
      q += 4;
      m.S32(q, (cur & mask) | val);
    } while (c != 0);
  }
  return r3;
}

// N4 step 2a: ShaderConstLoad sub_82CC1C58 (generated 85.cpp 46527-46635), called by the shader flush with info = PS+40 or
// VS+872 and r5 = the shader's data base. For each entry {off u16, count u16, addr u32} of the table at t+20 (t = info +
// [info+20], length [t+16] bytes, stops at a zero count) it writes one constant-block load packet at the ring cursor D+48:
// 0xC0022F00, phys(addr + r5), off * 4, count (ring reserve 82CBA798 when the cursor is past the limit D+56). Returns r3.
template <class M>
uint64_t ConstLoad(M& m, uint32_t D, uint64_t r3in, uint64_t r4in, uint64_t r5in) {
  uint64_t r3 = r3in;
  const uint32_t info = uint32_t(r4in);
  const uint32_t rel = m.L32(info + 20);
  if (rel == 0) return r3;
  const uint64_t t = uint64_t(rel) + r4in;
  uint32_t p = uint32_t(t + 20);
  const uint32_t end = m.L32(uint32_t(t) + 16) + p;
  while (p < end) {
    const uint32_t c = m.L16(p + 2), off = m.L16(p + 0);
    if (c == 0) break;
    const uint64_t addr = uint64_t(m.L32(p + 4)) + r5in;
    p += 8;
    r3 = m.L32(D + 48);
    if (uint32_t(r3) > m.L32(D + 56)) r3 = m.RingReserve(D, 0x82CC1CC4u);
    const uint32_t a = uint32_t(addr);
    const uint32_t phys = (((a >> 20) & 0xFFFu) + 512u & 0x1000u) + (a & 0x1FFFFFFFu);
    uint32_t q = uint32_t(r3);
    m.S32(q + 4, 0xC0022F00u);
    m.S32(q + 8, phys);
    m.S32(q + 12, (off << 2) & 0xFFFFFFFCu);
    m.S32(q + 16, c);
    q += 16;
    r3 = (r3 & 0xFFFFFFFF00000000ull) | q;
    m.S32(D + 48, q);
  }
  return r3;
}

}  // namespace wwe2k14::d3d::native_shader
