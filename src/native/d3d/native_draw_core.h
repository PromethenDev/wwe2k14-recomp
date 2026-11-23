// N3 (fully native, stage F3): native body of the guest draw entry sub_82CB52D0 (DrawIndexedVertices).
// Source: generated/wwe2k14_recomp.85.cpp (body 15469-16141), map docs/tasks/N3-draw-entry-start.md.
//   1. Dirty-group upload preamble: the five 64-bit dirty words D+0/8/16/24/32 select the guest encoders (ALU constants
//      82CC10B0, shader flush 82CC1E48, state encoder 82CC0988, register groups 82CC0D18, fetch constants 82CC0F58,
//      coherency 82CC0DD8, 82CB5AE8); each word is cleared after its uploads.
//   2. Draw packet loop: per chunk of at most 65535 indices (a multiple of the primitive's table stride) it writes
//      0x00002102, r5, then DRAW_INDX 0xC0032201 + 4 words (normal path) or, when D+11068 bit 0 is set, the 14-word
//      linked-list form with a block pointer stored at D+13628.
// The callees stay guest functions (M::Call); the body itself is pure: every guest access goes through the policy M, so the
// host test runs it on fake memory against the generated body, CHECK replays it on a device copy, the live path on guest
// memory. Register arithmetic copies the generated expressions (64-bit where the generated code is 64-bit).
//   M::L8/L32/L64(addr), M::S32/S64(addr, v): big-endian guest loads/stores.
//   M::Call(addr, lr, nargs, r3, r4, r5, r6) -> r3: guest call with the guest call site's return address.
//   M::Trap(): the divide-by-zero trap of the chunk split (twllei).
#pragma once
#include <cstdint>

namespace wwe2k14::d3d::native_draw {

constexpr uint32_t kStrideTable = 0x82001B90u;  // -2113929216 + 7056: 8-byte entries by primitive selector (r20)

// Guest callees of the draw entry (all reached through their hooked / weak symbols).
enum Callee : uint32_t {
  kAluConst = 0x82CC10B0u,
  kShaderFlush = 0x82CC1E48u,
  kStateEnc = 0x82CC0988u,
  kRegGroup = 0x82CC0D18u,
  kFetchConst = 0x82CC0F58u,
  kCoherency = 0x82CC0DD8u,
  kSmall5AE8 = 0x82CB5AE8u,
  kRingReserve = 0x82CBA798u,
  kListReserve = 0x82CBA080u,
};

inline uint64_t Rotl64(uint64_t x, unsigned n) { return n ? (x << n) | (x >> (64 - n)) : x; }
// rlwinm as generated: rotl64(x.u32 | x.u64 << 32, n) & mask (32-bit masks here, so only the low word matters).
inline uint64_t Rlw(uint64_t x, unsigned n, uint64_t mask) {
  return Rotl64(uint64_t(uint32_t(x)) | (x << 32), n) & mask;
}

// Returns the guest r3 at the end of the body (the only caller, 82BB7A18 -> 82BB8C00, ignores it; kept exact anyway).
template <class M>
uint64_t DrawIndexedVertices(M& m, uint64_t r3in, uint64_t r4in, uint64_t r5in, uint64_t r6in, uint64_t r7in) {
  const uint32_t D = uint32_t(r3in);
  const uint64_t r31 = r3in;
  uint64_t r3 = r3in;
  // ---- 1. dirty-group uploads (loads of all five words come first, as in the generated prologue) ----
  const uint64_t d0 = m.L64(D + 0), d32 = m.L64(D + 32), d24 = m.L64(D + 24), d16 = m.L64(D + 16), d8 = m.L64(D + 8);
  if (d0) {
    r3 = m.Call(kAluConst, 0x82CB531Cu, 4, r3in, d0, 16384, r3in + 1920);
    m.S64(D + 0, 0);
  }
  if (d8) {
    r3 = m.Call(kAluConst, 0x82CB533Cu, 4, r31, d8, 17408, r31 + 6016);
    m.S64(D + 8, 0);
  }
  if (d16) {
    uint64_t r30 = d16;
    if (Rlw(r30, 0, 0x1E0000)) r30 = r3 = m.Call(kShaderFlush, 0x82CB5360u, 2, r31, r30, 0, 0);
    if (m.L64(D + 40) & r30) r30 = r3 = m.Call(kStateEnc, 0x82CB5384u, 3, r31, r30, m.L32(D + 10560), 0);
    if (r30 & 0xFFF)
      r3 = m.Call(kRegGroup, 0x82CB53A8u, 4, r31, Rotl64(r30, 52) & 0xFFF0000000000000ull, 8704, r31 + 10548);
    if (Rlw(r30, 0, 0x1F000))
      r3 = m.Call(kRegGroup, 0x82CB53C8u, 4, r31, Rotl64(r30, 47) & 0xF800000000000000ull, 8576, r31 + 10528);
    if (r30 & (Rotl64(65535, 42) & 0xFFFFFC0000000000ull))
      r3 = m.Call(kRegGroup, 0x82CB53F4u, 4, r31, Rotl64(r30, 6) & 0xFFFF000000000000ull, 8192, r31 + 10368);
    if (r30 & (uint64_t(int64_t(-2097152)) & 0x3FFFFFFFFFFull))
      r3 = m.Call(kRegGroup, 0x82CB541Cu, 4, r31, Rotl64(r30, 22) & 0xFFFFF80000000000ull, 8448, r31 + 10444);
    m.S64(D + 16, 0);
  }
  if (d24) {
    if (d24 & 0xFFFFFFFFull) r3 = m.Call(kFetchConst, 0x82CB5440u, 2, r31, Rotl64(d24, 32) & 0xFFFFFFFF00000000ull, 0, 0);
    if (d24 & (Rotl64(2031616 | 65535, 34) & 0xFFFFFFFC00000000ull))
      r3 = m.Call(kRegGroup, 0x82CB546Cu, 4, r31, Rotl64(d24, 9) & 0xFFFFF80000000000ull, 8832, r31 + 10596);
    m.S64(D + 24, 0);
  }
  if (d32) {
    if (d32 & (Rotl64(255, 38) & 0xFFFFFFC000000000ull))
      r3 = m.Call(kRegGroup, 0x82CB54A0u, 4, r31, Rotl64(d32, 18) & 0xFF00000000000000ull, 9088, r31 + 10832);
    if (d32 & (Rotl64(63, 49) & 0xFFFE000000000000ull))
      r3 = m.Call(kCoherency, 0x82CB54C0u, 2, r31, Rotl64(d32, 9) & 0xFC00000000000000ull, 0, 0);
    if (d32 & Rotl64(1, 56)) r3 = m.Call(kRegGroup, 0x82CB54E8u, 4, r31, uint64_t(int64_t(-16777216)), 18688, r31 + 10112);
    if (d32 & 0x3FFFFFFFFFull)
      r3 = m.Call(kRegGroup, 0x82CB5508u, 4, r31, Rotl64(d32, 26) & 0xFFFFFFFFFC000000ull, 8960, r31 + 10680);
    if (d32 & 0xC000000000000000ull) {
      const uint32_t b = m.L8(D + 11072);
      if ((b & 0xFFFFFF80u) || (b & 0x40u)) r3 = m.Call(kSmall5AE8, 0x82CB5530u, 1, r31, 0, 0, 0);
    }
    m.S64(D + 32, 0);
  }
  // ---- 2. draw packets ----
  const uint32_t r24 = m.L32(D + 12812);   // index buffer object: +0 flags (bit 31 = 32-bit indices), +24 base address
  const uint32_t r23 = D + 12498;          // byte table indexed by D+12563
  const uint64_t r22 = uint32_t(r4in) & 0x3F;  // primitive type
  const uint32_t r20 = kStrideTable;
  const uint64_t r18 = r4in, r17 = r5in;
  uint64_t r21 = r6in, r19 = r7in;
  for (;;) {
    r3 = m.L32(D + 48);
    if (uint32_t(r3) > m.L32(D + 56)) r3 = m.Call(kRingReserve, 0x82CB5560u, 1, r31, 0, 0, 0);
    uint64_t r27 = r19;
    const uint32_t p = uint32_t(r3);
    m.S32(p + 4, 8450);
    m.S32(p + 8, uint32_t(r17));
    r3 = (r3 & 0xFFFFFFFF00000000ull) | uint32_t(p + 8);
    const uint64_t r25 = r3;
    if (uint32_t(r19) > 65535u) {
      const uint32_t t = m.L32(uint32_t(Rlw(r18, 3, 0xFFFFFFF8)) + r20);
      uint32_t q = t ? 65535u / t : 0u;
      if (t == 0) m.Trap();
      q &= 0xFFFFFFFEu;
      r27 = uint64_t(int64_t(int32_t(q)) * int64_t(int32_t(t)));
    }
    uint64_t r10 = m.L32(r24 + 24);
    uint64_t r11 = Rlw(r21, 1, 0xFFFFFFFE) + r10;
    const uint32_t b12563 = m.L8(D + 12563);
    const uint64_t r7 = Rlw(r27, 16, 0xFFFF0000);
    const uint32_t r6 = m.L32(r24 + 0);
    const uint64_t r5 = b12563 != 0 ? 1 : 0;  // addic/subfe pair: carry of (b - 1) = (b != 0)
    uint64_t r9 = Rlw(r11, 12, 0xFFF);
    uint64_t r4 = m.L8(r23 + b12563);
    uint64_t r8 = r9 + 512;
    r9 = uint32_t(r11) & 0x1FFFFFFF;
    r4 = (Rlw(r5, 8, 0x100)) | (r4 & 0xFFFFFFFFFFFFFEFFull);
    const uint64_t r11c = Rlw(r6, 1, 0xC0000000);
    r8 = Rlw(r8, 0, 0x1000);
    const uint64_t r26 = r4 & 319;
    uint64_t r30 = r7 | r22;
    uint64_t r28 = r8 + r9;
    uint64_t r29 = (uint32_t(r27) & 0xFFFFFF) | r11c;
    if (r6 & 0x80000000u) {  // 32-bit indices
      r9 = Rlw(r21, 2, 0xFFFFFFFC);
      r8 = Rlw(r27, 1, 0xFFFFFE);
      r10 = r9 + r10;
      r29 = r8 | r11c;
      r9 = Rlw(r10, 12, 0xFFF);
      r10 = uint32_t(r10) & 0x1FFFFFFF;
      r11 = Rlw(r9 + 512, 0, 0x1000);
      r30 |= 2048;
      r28 = r11 + r10;
    }
    const uint32_t q = uint32_t(r25);
    uint32_t end;
    if (!(m.L8(D + 11068) & 1u)) {
      m.S32(q + 4, 0xC0032201u);
      m.S32(q + 8, uint32_t(r26));
      m.S32(q + 12, uint32_t(r30));
      m.S32(q + 16, uint32_t(r28));
      m.S32(q + 20, uint32_t(r29));
      end = q + 20;
    } else {  // linked-list (predicated) form
      uint64_t blk = m.L32(D + 13628);
      r3 = r31 + 13616;
      const uint32_t lim = m.L32(D + 13632);
      if (uint32_t(blk + 16) > lim) blk = r3 = m.Call(kListReserve, 0x82CB566Cu, 1, r31 + 13616, 0, 0, 0);
      m.S32(uint32_t(blk), q);
      m.S32(D + 13628, uint32_t(blk + 16));
      const uint64_t b4 = blk + 4;
      const uint32_t link = uint32_t((Rlw(Rlw(b4, 12, 0xFFF) + 512, 0, 0x1000) + Rlw(b4, 0, 0x1FFFFFFE)) | 1);
      const uint32_t w[14] = {0xC0006000u, 0xFFFFFFFFu,      0xC0032201u,      uint32_t(r26), uint32_t(r30),
                              uint32_t(r28), uint32_t(r29),  0xC0006000u,      0x80000000u,   0xC0015A01u,
                              26u,           link,           0xC0004601u,      25u};
      for (uint32_t i = 0; i < 14; ++i) m.S32(q + 4 + 4 * i, w[i]);
      end = q + 56;
    }
    const uint64_t rem = r19 - r27;
    m.S32(D + 48, end);
    if (int32_t(uint32_t(rem)) == 0) break;
    const uint64_t step = m.L32(uint32_t(Rlw(r18, 3, 0xFFFFFFF8)) + uint32_t(r20 + 4));
    r19 = step + rem;
    r21 = (r27 - step) + r21;
  }
  return r3;
}

}  // namespace wwe2k14::d3d::native_draw
