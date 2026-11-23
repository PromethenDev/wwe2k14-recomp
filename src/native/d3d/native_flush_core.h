// N4 step 2c (fully native): native body of the shader flush sub_82CC1E48 (generated/wwe2k14_recomp.85.cpp) with its two
// helpers sub_82CC0790 (pixel-shader kill / alpha state bits) and sub_82CC1D08 (in-place vertex-shader patch cache). The flush
// runs before draws whose shader or shader-state dirty bits are set:
//   - picks the VS sub-block (the second one when [vs+872] & 0x20 and no PS), loads PS constants + the PS code packet
//     (0xC0012700) when the PS changed, keeps D+10372 / 10536 / 10560 / 10580 / 11068-11071 / 13076 in step;
//   - loads VS constants and either patches the resident VS microcode in place (82CC1D08: skipped when the declaration id and
//     stream masks match the cached ones, refused while the GPU may still run that code) or uploads a patched copy inline
//     (82CC1B00, hooked); the in-place path emits the VS code packet (0xC0012700 / 0xC0012701) and stamps the fence word;
//   - with D+11068 bit 6 emits the state block (0xC0006000 / 0xC0006100 pairs, 0xC0015501 / 0xC0025501 register writes).
// Returns the 64-bit dirty mask. The control flow follows the guest's branch structure (labels name the guest addresses);
// KeBugCheck(245) is called where the guest calls it (assertions that never fire in a working game).
// Pure: guest access through the policy M (see native_shader_core.h) plus
//   M::RingReserve(dev, lr) -> cursor (guest 82CBA798), M::BugCheck(code, lr),
//   M::ConstLoad(dev, info, base, lr) (hooked 82CC1C58), M::VsUpload(dev, r4, vs, r6, decl, r8, r9, idx, lr) (hooked
//   82CC1B00), M::AtomicInc(addr) -> new value (lwarx/stwcx increment of a guest word).
#pragma once
#include <cstdint>

#include "native_vsupload_core.h"

namespace wwe2k14::d3d::native_shader {

namespace flush {
constexpr uint32_t kMaskOnce = 0x83948064u;  // bit 0: kMask initialised
constexpr uint32_t kMask = 0x83948060u;      // PS/VS interpolator compare mask (0x603FF)
constexpr uint32_t kDeclIds = 0x83948068u;   // vertex declaration id counter

// sub_82CC0790(D, mask) -> mask | 256.
template <class M>
uint64_t KillState(M& m, uint32_t D, uint64_t r4) {
  const uint32_t ps = m.L32(D + 13068);
  uint32_t ps_bit = 0;
  if (ps) ps_bit = (m.L32(m.L32(ps + 64) + ps + 48) >> 27) & 1u;
  const uint32_t r10 = m.L32(D + 10548);
  const uint32_t r7 = r10 >> 1;
  const uint32_t r9 = (m.L32(D + 12284) >> 20) & 0xFFFu;
  uint32_t w = (m.L32(D + 10560) & 0xFFFFFFFEu) | ((uint32_t(m.L8(D + 11070)) >> 5) & r7 & 1u);
  m.S32(D + 10560, w);
  uint32_t v;
  if (!(r9 & 2)) {
    v = r9 & 1u;
  } else {
    const uint32_t sel = (r10 >> 4) & 7u;
    const uint32_t ok = (w & 0x10) ? (sel == 4 || sel == 6 || sel == 2) : (sel == 1 || sel == 3 || sel == 2);
    v = w & ~ps_bit & r7 & ok & 1u;
    if (r10 & 1) {
      v &= uint32_t((r10 & 0x3800u) == 0) & uint32_t((r10 & 0xE0000u) == 0);
      if (r10 & 0x80) v &= uint32_t((r10 & 0xE0000000u) == 0) & uint32_t((r10 & 0x03800000u) == 0);
    }
  }
  w = (m.L32(D + 10560) & ~2u) | ((v << 1) & 2u);
  m.S32(D + 10560, w);
  if (w & 2) {
    const uint8_t b = m.L8(D + 11069);
    if ((b & 4) && (m.L32(D + 13904) == 0 || m.L32(D + 13912) == 1)) {
      m.S8(D + 11069, uint8_t(b & 0xFB));
      uint32_t q = m.L32(D + 48);
      if (q > m.L32(D + 56)) q = uint32_t(m.RingReserve(D, 0x82CC08F8u));
      m.S32(q + 4, 0xC0004600u);
      m.S32(q + 8, 15);
      m.S32(D + 48, q + 8);
    }
  } else if ((r7 & 1) && (r10 & 4)) {
    const uint32_t sel = (r10 >> 4) & 7u;
    bool set;
    if (sel == 7 || sel == 5) set = true;
    else if (!(w & 0x10)) set = sel == 4 || sel == 6;
    else set = sel == 1 || sel == 3;
    if (set) m.S8(D + 11069, uint8_t(m.L8(D + 11069) | 4));
  }
  return r4 | 256;
}

// sub_82CC1D08(D, vs, decl, idx) -> 1 = the resident VS sub-block code fits the current declaration + streams (patched now
// or already), 0 = it would need patching while the GPU may still run it (the caller uploads a patched copy instead).
template <class M>
uint32_t PatchInPlace(M& m, uint32_t D, uint32_t vs, uint32_t decl, uint32_t idx) {
  const uint32_t o = vs + idx * 416u;
  bool hit = false;
  if (m.L32(o + 40) == m.L32(decl + 48)) {
    const uint64_t x = ((m.L64(o + 56) ^ m.L64(D + 12912)) & m.L64(decl + 40)) |
                       ((m.L64(o + 48) ^ m.L64(D + 12904)) & m.L64(decl + 32));
    hit = x == 0;
  }
  if (!hit) {
    const uint32_t fence = m.L32(o + 64);
    if (fence != 0) {
      const uint32_t cur = m.L32(D + 11036);
      if (cur - fence < cur - m.L32(m.L32(D + 11024))) return 0;
    }
    const uint32_t code = m.L32(m.L32(vs + ((idx + 112u) << 3)) + vs + 872) + m.L32(vs + 32);
    vsup::PatchFetches(m, vs, code, decl, D + 12904, idx, vs);
    if (m.L32(decl + 48) == 0) {
      uint32_t id;
      do id = m.AtomicInc(kDeclIds);
      while (id + 1u < 2u);
      m.S32(decl + 48, id);
    }
    m.S32(o + 40, m.L32(decl + 48));
    m.S64(o + 48, m.L64(D + 12904));
    m.S64(o + 56, m.L64(D + 12912));
  }
  m.S64(D + 12264, m.L64(D + 12904));
  m.S64(D + 12272, m.L64(D + 12912));
  return 1;
}

// One VS code packet {hdr, physical code address, dwords} for sub-block i at q + 4; stamps the fence at vs + 416 i + 64.
template <class M>
uint32_t VsCodePacket(M& m, uint32_t D, uint32_t vs, uint32_t i, uint32_t hdr, uint32_t q) {
  m.S32(q + 4, hdr);
  const uint32_t blk = m.L32(vs + ((i + 112u) << 3)) + vs;
  const uint32_t a = m.L32(blk + 872) + m.L32(vs + 32);
  m.S32(q + 8, ((((a >> 20) & 0xFFFu) + 512u) & 0x1000u) + (a & 0x1FFFFFFFu));
  m.S32(q + 12, m.L32(m.L32(vs + ((i + 112u) << 3)) + vs + 876) >> 2);
  m.S32(vs + i * 416u + 64, m.L32(D + 11036));
  return q + 12;
}
}  // namespace flush

// sub_82CC1E48(D, mask) -> mask. fsp = the flush's own stack frame (entry sp - 272): the word at fsp + 80 is passed to the
// VS upload (r6), which may rewrite it (interpolator count), so it lives in guest memory.
template <class M>
uint64_t Flush(M& m, uint32_t D, uint64_t r4, uint32_t fsp) {
  using namespace flush;
  constexpr uint32_t kNone = 0xFFFFFFFFu;
  uint64_t f = r4;
  uint32_t s84 = kNone, s88 = kNone, s100 = kNone, s104 = kNone, s92 = kNone, s96 = 0;
  uint32_t inplace = kNone;  // r15: VS sub-block patched in place -> its code packet is emitted
  uint32_t idx = 0;          // r23
  uint32_t pinfo = 0, p48 = 0, p52 = 0, p48b = 0;  // r27, r18, r16, r19 (PS info, its two words, word 48 kept)
  uint32_t mdiff = 0;        // r28: PS / VS interpolator layouts differ
  uint32_t r24 = 0, r26 = 0, r29 = 0, upload_r4 = 0;
  uint32_t vs, decl, ps, B;
  if (r4 & 0x20000u) f = KillState(m, D, r4);
  vs = m.L32(D + 13072);
  decl = m.L32(D + 12240);
  ps = m.L32(D + 13068);
  if (vs == 0) return f;
  B = m.L32(vs + 896) + vs + 872;
  if (ps != 0) goto ps_path;
  // No pixel shader.
  if (m.L8(D + 11071) & 2) {
    uint32_t q = m.L32(D + 48);
    if (q > m.L32(D + 56)) q = uint32_t(m.RingReserve(D, 0x82CC1EF4u));
    m.S32(q + 4, 1480);
    m.S32(q + 8, 0x20000u);
    m.S32(D + 48, q + 8);
    m.S8(D + 11071, uint8_t(m.L8(D + 11071) & 0xFC));
  }
  pinfo = 0, p48 = 0, p48b = 0;
  if (m.L32(vs + 872) & 0x20) {
    idx = 1;
    B = m.L32(vs + 904) + vs + 872;
  }
  if (m.L8(D + 11071) & 8) {
    const uint32_t w = m.L32(D + 10372);
    f |= 1ull << 56;
    m.S32(D + 10372, w & ~0xF0000u);
  }
  {
    const uint32_t w = m.L32(D + 10580);
    if ((w & 7) != 5) {
      f |= 0x80008u;
      m.S32(D + 10580, (w & ~7u) | 5u);
    }
  }
  mdiff = (m.L32(B + 20) & 0x1F) != 0;
check_bug:  // loc_82CC1F90
  if (m.L32(D + 13904) != 0) goto tail;
  if (!(m.L32(vs + 872) & 0x10)) goto tail;
  m.BugCheck(245, 0x82CC1FB0u);
ps_path:  // loc_82CC1FB0
  {
    const uint32_t pi = m.L32(ps + 64) + ps;
    pinfo = pi + 40;
    p48 = m.L32(pi + 48);
    p52 = m.L32(pi + 52);
    p48b = p48;
    const uint32_t t = (p48 & 0x70000000u) > 0x10000000u ? 1u : 0u;
    m.S8(D + 11070, uint8_t(m.L8(D + 11070) | (t << 3)));
    m.S8(D + 11071, uint8_t(m.L8(D + 11071) | (t << 1)));
  }
  if (f & 0x100000u) {
    m.ConstLoad(D, ps + 40, m.L32(ps + 24), 0x82CC201Cu);
    uint32_t q = m.L32(D + 48);
    if (q > m.L32(D + 56)) q = uint32_t(m.RingReserve(D, 0x82CC2034u));
    m.S32(q + 4, 0xC0012700u);
    f |= 0x4000u;
    const uint32_t pi = m.L32(ps + 64) + ps;
    const uint32_t a = m.L32(pi + 40) + m.L32(ps + 24);
    m.S32(q + 8, (((((a >> 20) & 0xFFFu) + 512u) & 0x1000u) + (a & 0x1FFFFFFEu)) | 1u);
    m.S32(q + 12, m.L32(m.L32(ps + 64) + ps + 44) >> 2);
    m.S32(D + 48, q + 12);
    const uint8_t b = m.L8(D + 11071);
    m.S32(D + 10536, m.L32(pinfo + 24));
    if (b & 8) {
      const uint32_t x = m.L32(D + 12816);
      f |= 1ull << 56;
      m.S32(D + 10372, m.L32(x + 28));
    }
    const uint32_t w = m.L32(D + 10580);
    if ((w & 7) != 4) {
      f |= 0x80008u;
      m.S32(D + 10580, (w & ~7u) | 4u);
    }
  }
  {  // loc_82CC20D8
    const uint32_t pw = m.L32(pinfo + 20), vw = m.L32(B + 20);
    uint32_t mask;
    const uint32_t once = m.L32(kMaskOnce);
    if (!(once & 1)) {
      m.S32(kMaskOnce, once | 1);
      mask = 0x603FFu;
      m.S32(kMask, mask);
    } else {
      mask = m.L32(kMask);
    }
    mdiff = ((pw & mask) - (vw & mask)) != 0;
  }
  if (m.L32(D + 13904) != 0) goto check_bug;
  if (!(m.L32(ps + 40) & 0x10)) goto check_bug;
  m.BugCheck(245, 0x82CC2144u);
tail:  // loc_82CC2144
  {
    const uint32_t s80 = m.L32(B + 8);
    const uint8_t b68 = m.L8(D + 11068);
    r26 = m.L32(D + 10580);
    r24 = m.L32(B + 12);
    r29 = r26;
    m.S32(fsp + 80, s80);
    if (b68 & 0x40) {
      bool five = pinfo == 0;
      if (!five && !(m.L32(D + 10556) & 8)) {
        const uint32_t w = m.L32(pinfo + 28);
        if (!(w & 0x10) && !(w & 0x20)) {
          const uint8_t b = m.L8(D + 11071);
          p48b = 0;
          m.S8(D + 11071, uint8_t(b & 0xFE));
          s96 = b & 1u;
          five = true;
        }
      }
      if (five) r29 = (r29 & ~7u) | 5u;
      if (f & 0x40000u) {
        s84 = s80 | p48b;
        s88 = r24 | p52;
      }
      if (m.L8(D + 11071) & 8) {
        uint32_t w = m.L32(D + 10372);
        if ((r29 & 7) == 5) w &= ~0xF0000u;
        s104 = w;
      }
      if (r29 != m.L32(D + 13076) || (f & 8)) {
        m.S32(D + 13076, r29);
        f |= 8;
        s100 = r29;
      }
    }
  }
  {  // loc_82CC2210
    const uint8_t b71 = m.L8(D + 11071), b70 = m.L8(D + 11070);
    const uint32_t t = (p48b & 0x70000000u) > 0x10000000u ? 1u : 0u;
    m.S8(D + 11071, uint8_t(b71 | t));
    if (!(b70 & 0x80) && mdiff == 0) {
      if (!(f & 0x80000u)) goto emit;
      const uint32_t info = vs + 872;
      m.ConstLoad(D, info, m.L32(vs + 32), 0x82CC2268u);
      const uint8_t bm = uint8_t(m.L8(D + 11070) & 0xBF);
      m.S8(D + 11070, bm);
      if (decl == 0 || (m.L32(info) & 0x40)) {  // loc_82CC2330: the resident code is used as is
        const uint32_t o = vs + 416u * idx;
        inplace = idx;
        m.S64(D + 12264, m.L64(o + 48));
        m.S64(D + 12272, m.L64(o + 56));
        if (((r29 ^ r26) & 7) == 0 || !(m.L32(info) & 0x20)) goto emit;
        s92 = 1;
        m.S8(D + 11070, uint8_t(bm | 0x40));
        goto emit;
      }
      if (m.L8(D + 11068) & 0x80) {
        upload_r4 = 0;
        goto upload;
      }
      if (PatchInPlace(m, D, vs, decl, idx)) inplace = idx;
      else m.VsUpload(D, 0, vs, fsp + 80, decl, B, pinfo, idx, 0x82CC22E4u);
      if (((r29 ^ r26) & 7) == 0 || !(m.L32(info) & 0x20)) goto emit;
      if (!PatchInPlace(m, D, vs, decl, 1)) goto emit;
      s92 = 1;
      m.S8(D + 11070, uint8_t(m.L8(D + 11070) | 0x40));
      goto emit;
    }
    // loc_82CC236C
    if (!(f & 0x180000u)) goto emit;
    m.S8(D + 11070, uint8_t(b70 & 0xBF));
    m.ConstLoad(D, vs + 872, m.L32(vs + 32), 0x82CC2390u);
    upload_r4 = mdiff;
  }
upload:  // loc_82CC2398
  m.VsUpload(D, upload_r4, vs, fsp + 80, decl, B, pinfo, idx, 0x82CC23B4u);
emit:  // loc_82CC23B4
  if (f & 0x180000u) {
    const uint32_t s80 = m.L32(fsp + 80);
    const uint8_t b = m.L8(D + 11070);
    f |= 0x10000u | 0x8000u;
    m.S32(D + 10532, r24 | p52);
    m.S32(D + 10528, s80 | p48);
    if (b & 0x40) {
      const uint32_t x = m.L32(vs + 904) + vs;
      s84 = m.L32(x + 880) | p48b;
      s88 = m.L32(x + 884) | p52;
    } else if (p48b == 0 && p48 != 0) {
      s84 = s80;
      s88 = r24;
    }
  }
  if (m.L8(D + 11068) & 0x40) {  // state block
    uint32_t q = m.L32(D + 48);
    if (q > m.L32(D + 56)) q = uint32_t(m.RingReserve(D, 0x82CC244Cu));
    m.S32(q + 4, 0xC0006000u);
    m.S32(q + 8, 0x15555555u);
    m.S32(q + 12, 0xC0006100u);
    m.S32(q + 16, 0);
    q += 16;
    if (f & 8) {
      m.S32(q + 4, 8712);
      f &= ~uint64_t(8);
      m.S32(q + 8, m.L32(D + 10580));
      q += 8;
    }
    if (s96 != 0) {
      m.S32(q + 4, 0xC0002601u);
      m.S32(q + 8, 0);
      q += 8;
    }
    if (s100 != kNone) {
      m.S32(q + 4, 0xC0015501u);
      m.S32(q + 8, 8712);
      m.S32(q + 12, s100);
      q += 12;
    }
    if (f & (1ull << 56)) {
      m.S32(q + 4, 8193);
      f &= ~(1ull << 56);
      m.S32(q + 8, m.L32(D + 10372));
      q += 8;
    }
    if (s104 != kNone) {
      m.S32(q + 4, 0xC0015501u);
      m.S32(q + 8, 8193);
      m.S32(q + 12, s104);
      q += 12;
    }
    if (f & 0x10000u) {
      m.S32(q + 4, 0x12180u);
      f &= 0xFFFFFFFFFFFE7FFFull;
      m.S32(q + 8, m.L32(D + 10528));
      m.S32(q + 12, m.L32(D + 10532));
      q += 12;
    }
    if (s84 != kNone) {
      m.S32(q + 4, 0xC0025501u);
      m.S32(q + 8, 8576);
      m.S32(q + 12, s84);
      m.S32(q + 16, s88);
      q += 16;
    }
    if (s92 != kNone) {
      q = VsCodePacket(m, D, vs, s92, 0xC0012701u, q);
      if (inplace != kNone) {
        m.S32(q + 4, 0xC0006000u);
        m.S32(q + 8, 0x2AAAAAAAu);
        q = VsCodePacket(m, D, vs, inplace, 0xC0012701u, q + 8);
      }
    } else if (inplace != kNone) {
      q = VsCodePacket(m, D, vs, inplace, 0xC0012700u, q);
    }
    m.S32(q + 4, 0xC0006000u);
    m.S32(q + 8, m.L32(D + 13084));
    m.S32(q + 12, 0xC0006100u);
    m.S32(q + 16, m.L32(D + 13088));
    m.S32(D + 48, q + 16);
    return f;
  }
  if (inplace == kNone) return f;  // loc_82CC273C
  {
    uint32_t q = m.L32(D + 48);
    if (q > m.L32(D + 56)) q = uint32_t(m.RingReserve(D, 0x82CC275Cu));
    m.S32(D + 48, VsCodePacket(m, D, vs, inplace, 0xC0012700u, q));
  }
  return f;
}

}  // namespace wwe2k14::d3d::native_shader
