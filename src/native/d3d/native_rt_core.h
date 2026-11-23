// N2-RT (fully native, stage F2): native body of the guest SetRenderTarget tree. Map and sources: docs/tasks/N2-RT-native.md.
//   sub_82CAF5E8 SetRenderTarget(dev, index, surface)  generated 85:1410
//   sub_82CAF558 (surface state)  -> sub_82CAF4B8 (default viewport to float) -> sub_82CAF250 (viewport)
//   sub_82CAEB70 (scissor from viewport)  -> sub_82CAC5C0 (scissor packets into the Xbox ring, generated 84:68173)
// Pure: every guest access goes through the memory policy M, so the host test runs it on a fake memory against the generated
// bodies, the CHECK mode runs it on a device copy, and the live path runs it on guest memory.
//   M::L8/L16/L32/L64(addr), M::S8/S32/S64(addr, v): big-endian guest loads/stores (addresses are guest addresses).
//   M::Reserve(dev) -> new ring pointer: the guest ring reserve sub_82CBA798 (the CHECK policy aborts instead).
//   M::ok: false once Reserve aborted (the CHECK result is then not comparable).
#pragma once
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wwe2k14::d3d::native_rt {

// Constants in the game image (read through M, verified at runtime by the CHECK mode against the guest result).
constexpr uint32_t kDefaultViewport = 0x82082920u;  // {int X, Y, W, H; float MinZ, MaxZ}; +24: 4 words of default scissor
constexpr uint32_t kHalf = 0x8202B72Cu;             // float used by the viewport scale (expected 0.5)

inline float AsF(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint32_t AsU(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
// fctiwz as the generated code does it (NaN -> 0x80000000, > INT_MAX -> INT_MAX, else truncate; < INT_MIN -> INT_MIN).
inline int32_t Fctiwz(double d) {
  if (std::isnan(d)) return INT32_MIN;
  if (d > double(INT_MAX)) return INT_MAX;
  if (d < double(INT_MIN)) return INT32_MIN;
  return int32_t(d);
}
inline double F(double d) { return double(float(d)); }  // frsp

// "Tiled" predicate shared by the viewport, the scissor packets and SetRenderTarget's 11068 bit 0 (the 5 bound surfaces
// equal the tiling set at 13104.., or are NULL).
template <class M>
bool TiledSurfaces(M& m, uint32_t D) {
  const uint32_t b = m.L8(D + 11068);
  if (b & 0x10) return true;
  if (!(b & 0x20)) return false;
  for (uint32_t k = 0; k < 5; ++k) {
    const uint32_t a = m.L32(D + 12816 + 4 * k), t = m.L32(D + 13104 + 4 * k);
    if (t != a && a != 0) return false;
  }
  return true;
}

inline uint32_t Pack14(int32_t x, int32_t y) { return ((uint32_t(y) << 16) & 0x3FFF0000u) | (uint32_t(x) & 0x3FFFu); }

// sub_82CAC5C0(D, x0, y0, x1, y1): scissor packets. Words are pre-increment stores (stwu).
template <class M>
void ScissorPackets(M& m, uint32_t D, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  uint32_t p = m.L32(D + 48);
  if (p > m.L32(D + 56)) {
    p = m.Reserve(D);
    if (!m.ok) return;
  }
  auto put = [&](uint32_t w) { p += 4; m.S32(p, w); };
  if (!TiledSurfaces(m, D)) {
    put(0x00022080u);
    put(0);
    put(Pack14(x0, y0));
    put(Pack14(x1, y1));
  } else {
    put(0xC0006100u);
    put(0);
    for (uint32_t t = 0; t < m.L32(D + 13124); ++t) {
      const uint32_t rt = D + 13128 + 16 * t, ro = D + 13368 + 8 * t;
      int32_t cx0 = x0, cy0 = y0, cx1, cy1;
      const int32_t L = int32_t(m.L32(rt)), T = int32_t(m.L32(rt + 4)), R = int32_t(m.L32(rt + 8)), B = int32_t(m.L32(rt + 12));
      const uint32_t ox = m.L32(ro), oy = m.L32(ro + 4);
      if (!(x0 > L)) cx0 = L;
      if (!(y0 > T)) cy0 = T;
      cx1 = R;
      if (x1 < R) cx1 = x1;
      cy1 = B;
      if (y1 < B) cy1 = y1;
      if (!(cx0 < cx1) || !(cy0 < cy1)) cx0 = cy0 = cx1 = cy1 = 0;
      put(0xC0006000u);
      const uint32_t sh = (2 * t) & 0x3Fu;
      put(sh & 0x20 ? 0u : (3u << sh));
      put(0xC0035501u);
      put(0x2080u);
      put((((0u - oy) << 16) & 0x7FFF0000u) | ((0u - ox) & 0x7FFFu));
      put(Pack14(cx0, cy0));
      put(Pack14(cx1, cy1));
      m.S32(D + 48, p);
      if (p > m.L32(D + 56)) {
        p = m.Reserve(D);
        if (!m.ok) return;
      }
    }
    if ((m.L8(D + 11071) & 0x20) && (m.L8(D + 11068) & 0x40)) {
      put(0xC0006000u);
      put(1);
      put(0xC0025501u);
      put(0x2081u);
      put(Pack14(x0, y0));
      put(Pack14(x1, y1));
    }
    put(0xC0006000u);
    put(m.L32(D + 13084));
    put(0xC0006100u);
    put(m.L32(D + 13088));
  }
  m.S32(D + 48, p);
}

// sub_82CAEB70(D, R = D + 13052): scissor from the stored viewport, clamped to R when D+12288 != 0.
template <class M>
void ScissorFromViewport(M& m, uint32_t D) {
  const uint32_t R = D + 13052;
  int32_t x0 = Fctiwz(double(AsF(m.L32(D + 13024))));
  const int32_t w = Fctiwz(double(AsF(m.L32(D + 13032))));
  const int32_t h = Fctiwz(double(AsF(m.L32(D + 13036))));
  int32_t y0 = Fctiwz(double(AsF(m.L32(D + 13028))));
  const uint32_t r0 = m.L32(R), r1 = m.L32(R + 4), r2 = m.L32(R + 8), r3 = m.L32(R + 12);
  m.S32(D + 13052, r0);
  m.S32(D + 13056, r1);
  m.S32(D + 13060, r2);
  m.S32(D + 13064, r3);
  int32_t x1 = int32_t(uint32_t(w) + uint32_t(x0)), y1 = int32_t(uint32_t(h) + uint32_t(y0));
  if (m.L32(D + 12288) != 0) {
    if (!(x0 > int32_t(r0))) x0 = int32_t(r0);
    if (!(y0 > int32_t(r1))) y0 = int32_t(r1);
    if (!(x1 < int32_t(r2))) x1 = int32_t(r2);
    if (!(y1 < int32_t(r3))) y1 = int32_t(r3);
  }
  const uint32_t a = m.L32(D + 10436), b = m.L32(D + 10440);
  m.S32(D + 10436, (a & 0x80008000u) | ((uint32_t(y0) << 16) & 0x7FFF0000u) | (uint32_t(x0) & 0x7FFFu));
  m.S32(D + 10440, (b & 0x80008000u) | ((uint32_t(y1) << 16) & 0x7FFF0000u) | (uint32_t(x1) & 0x7FFFu));
  ScissorPackets(m, D, x0, y0, x1, y1);
}

// sub_82CAF250(D, x, y, w, h, minz, maxz): viewport; arguments are already float-rounded doubles.
template <class M>
void Viewport(M& m, uint32_t D, double x, double y, double w, double h, double minz, double maxz) {
  uint32_t surf = m.L32(D + 12816);
  if (surf == 0) {
    surf = m.L32(D + 12832);
    if (surf == 0) return;
  }
  int32_t LW, LH;
  if (TiledSurfaces(m, D)) {
    LW = int32_t(m.L32(D + 13556));
    LH = int32_t(m.L32(D + 13560));
  } else {
    const uint32_t info = m.L32(surf + 36);
    LW = int32_t(((info >> 18) & 0x3FFFu) + 1);
    LH = int32_t(((info >> 3) & 0x7FFFu) + 1);
  }
  const int32_t ix = Fctiwz(x), iw = Fctiwz(w), iy = Fctiwz(y), ih = Fctiwz(h);
  int32_t r = int32_t(uint32_t(iw) + uint32_t(ix));
  if (r < LW) {} else r = LW;  // r = min(ix + iw, LW), signed
  int32_t b = int32_t(uint32_t(ih) + uint32_t(iy));
  if (!(b < LH)) b = LH;
  r = int32_t(uint32_t(r) - uint32_t(ix));
  b = int32_t(uint32_t(b) - uint32_t(iy));
  if (r < 0 || b < 0) r = b = 0;
  m.S32(D + 13024, AsU(float(x)));
  m.S32(D + 13028, AsU(float(y)));
  const double fh = F(double(int64_t(b)));
  m.S32(D + 13040, AsU(float(minz)));
  m.S32(D + 13044, AsU(float(maxz)));
  m.S32(D + 13036, AsU(float(fh)));
  m.S32(D + 13048, 0);
  const double fw = F(double(int64_t(r)));
  m.S32(D + 13032, AsU(float(fw)));
  ScissorFromViewport(m, D);
  const double half = double(AsF(m.L32(kHalf)));
  m.S32(D + 10520, AsU(float(F(maxz - minz))));
  m.S32(D + 10524, AsU(float(minz)));
  const double sx = F(fw * half);
  m.S32(D + 10504, AsU(float(sx)));
  const double sy = F(fh * half);
  m.S32(D + 10508, AsU(float(F(sx + x))));
  m.S32(D + 10512, AsU(float(-sy)));
  m.S32(D + 10516, AsU(float(F(sy + y))));
  m.S64(D + 16, m.L64(D + 16) | 0x7E00000ull);  // six single-bit ORs (0x4000000 .. 0x200000) in the guest
}

// sub_82CAF558(D, S) + sub_82CAF4B8(D, kDefaultViewport).
template <class M>
void SurfaceState(M& m, uint32_t D, uint32_t S) {
  const uint32_t s24 = m.L32(S + 24);
  const uint32_t i = (s24 >> 16) & 3u;
  m.S32(D + 10684, i == 0 ? 0u : i == 1 ? 0x8001u : 0xC003u);
  m.S32(D + 10368, s24);
  m.S64(D + 16, m.L64(D + 16) | (1ull << 57));
  m.S64(D + 32, m.L64(D + 32) | (1ull << 36));
  for (uint32_t k = 0; k < 4; ++k) m.S32(D + 13052 + 4 * k, m.L32(kDefaultViewport + 24 + 4 * k));
  const uint32_t V = kDefaultViewport;
  // lwz zero-extends: the ints convert as unsigned 32-bit values (int64 of a uint32).
  const double x = F(double(int64_t(m.L32(V + 0)))), y = F(double(int64_t(m.L32(V + 4))));
  const double w = F(double(int64_t(m.L32(V + 8)))), h = F(double(int64_t(m.L32(V + 12))));
  const double minz = double(AsF(m.L32(V + 16))), maxz = double(AsF(m.L32(V + 20)));
  Viewport(m, D, x, y, w, h, minz, maxz);
}

// Dirty bit for EDRAM-info word k (k = 0 colour 0, k = index + 1 otherwise): dword at D + ((k + 0x20107) >> 16) * 8, bit
// 63 - ((k + 7) & 0xFF) (guest: rotate of 1 << 63 right by that amount).
template <class M>
void DirtyInfo(M& m, uint32_t D, uint32_t k) {
  const uint32_t r3 = k + 0x20107u;
  const uint32_t off = uint32_t(int32_t(r3) >> 16) << 3;
  const uint32_t sh = r3 & 0xFFu;
  const uint64_t bit = (sh & 0x40) ? 0 : ((1ull << 63) >> (sh & 0x7F));
  m.S64(D + off, m.L64(D + off) | bit);
}

// sub_82CAF5E8 SetRenderTarget(D, index, S).
template <class M>
void SetRenderTarget(M& m, uint32_t D, uint32_t index, uint32_t S) {
  const uint32_t slot = ((index + 3204u) << 2);
  m.S32(D + slot, S);
  if (S != 0) {
    const uint32_t k = index == 0 ? 0u : index + 1u;
    m.S32(D + ((k + 2593u) << 2), m.L32(S + 28));
    DirtyInfo(m, D, k);
    const uint32_t s_now = m.L32(D + slot);
    const uint32_t v_off = (index + 3102u) << 2;
    const uint32_t v = m.L32(D + v_off);
    m.S32(D + v_off, v);  // the guest rewrites the same value
    if (s_now != 0) {
      const uint32_t k2 = index == 0 ? 0u : index + 1u;
      const uint32_t i_off = (k2 + 2593u) << 2;
      const uint32_t info = m.L32(D + i_off);
      const uint32_t f = (info >> 16) & 0xFu;
      if ((f == 2 || f == 3 || f == 10 || f == 12) && ((((info >> 19) & 1u) ^ v) != 0)) {
        const uint32_t vm1 = v - 1u;
        const uint32_t hi = (~vm1 << 16) & ((f + 3u) << 17);
        const uint32_t lo = ((((f >> 1) - 3u) & vm1) << 16);
        m.S32(D + i_off, (info & 0xFFF0FFFFu) | ((hi | lo) & 0xF0000u));
        DirtyInfo(m, D, k2);
      }
    }
  }
  if (index == 0) {
    if (S == 0) {
      const uint32_t w = m.L32(D + 10372);
      const uint32_t f = (w >> 16) & 0xFu;
      if (f == 5 || f == 7 || f == 15) {
        m.S32(D + 10372, w & 0xFFF0FFFFu);
        m.S64(D + 16, m.L64(D + 16) | (1ull << 56));
      }
    }
    uint32_t b = m.L8(D + 11071) & 0xF7u;
    m.S8(D + 11071, uint8_t(b));
    uint32_t surf = S;
    if (S != 0) {
      const uint32_t f = m.L16(S + 28) & 0xFu;
      if (f == 5 || f == 7 || f == 15) {
        m.S8(D + 11071, uint8_t(b | 8u));
        m.S64(D + 16, m.L64(D + 16) | 0x100000ull);
      }
    } else {
      surf = m.L32(D + 12832);
    }
    if (surf != 0) {
      SurfaceState(m, D, surf);
      if (!m.ok) return;
    }
    uint32_t bit = 0;
    if (m.L32(D + 13092) == 0) {
      const uint32_t f = m.L8(D + 11068);
      if (!(f & 8) && !(f & 4) && m.L8(D + 12563) == 0) bit = TiledSurfaces(m, D) ? 1u : 0u;
    }
    m.S8(D + 11068, uint8_t((m.L8(D + 11068) & 0xFEu) | bit));
    const uint32_t r9 = m.L32(D + 12292);
    const uint32_t mask = m.L32(D + 12816) != 0 ? 0xFFFFFFFFu : 0u;
    const uint32_t w10460 = m.L32(D + 10460);
    m.S32(D + 12292, r9);
    m.S32(D + 10460, (w10460 & 0xFFFFFFF0u) | (mask & r9 & 0xFu));
  } else if (index >= 1 && index <= 3) {
    const uint32_t a = m.L32(D + 12816 + 4 * index), r10 = m.L32(D + 12292 + 4 * index);
    const uint32_t mask = a != 0 ? 0xFFFFFFFFu : 0u;
    const uint32_t w = m.L32(D + 10460);
    m.S32(D + 12292 + 4 * index, r10);
    const uint32_t sh = 4 * index, nib = 0xFu << sh;
    m.S32(D + 10460, (w & ~nib) | (((mask & r10) << sh) & nib));
  } else {
    return;  // other indices: no 10460 update and no dirty bit 37 (guest loc_82CAF970)
  }
  m.S64(D + 16, m.L64(D + 16) | (1ull << 37));
}

// sub_82CAF978 SetDepthStencil(D, Z)  (generated 85; map in docs/tasks/N2-RT-native.md)
template <class M>
void SetDepthStencil(M& m, uint32_t D, uint32_t Z) {
  m.S32(D + 12832, Z);
  if (Z == 0) {
    m.S32(D + 10560, m.L32(D + 10560) & 0xFFFFFFF0u);
    m.S8(D + 11070, uint8_t(m.L8(D + 11070) & 0xDFu));
    m.S64(D + 16, m.L64(D + 16) | 0x100ull);
  } else {
    if (m.L32(D + 12816) == 0) {
      SurfaceState(m, D, Z);
      if (!m.ok) return;
    }
    m.S32(D + 10376, m.L32(Z + 28));
    const uint32_t b70 = m.L8(D + 11070), b71 = m.L8(D + 11071) & 0x20u;
    m.S32(D + 10560, m.L32(Z + 32));
    const uint32_t z32 = m.L32(Z + 32);
    m.S8(D + 11070, uint8_t((b70 & 0xDFu) | ((z32 & 1u) << 5)));  // rotl(z32, 5) & 0x20 = bit 0 of z32
    if (b71 && TiledSurfaces(m, D)) {
      const uint32_t w = m.L32(D + 10368), t = m.L32(D + 13548);
      m.S32(D + 10368, (t << 18) | (w & 0x3FFFFu));  // no dirty bit for 0x2000 here (capture needs an overlay)
    }
    m.S64(D + 16, m.L64(D + 16) | (1ull << 55) | 0x100ull | 0x20000ull);
  }
  uint32_t bit = 0;
  if (m.L32(D + 13092) == 0) {
    const uint32_t f = m.L8(D + 11068);
    if (!(f & 8) && !(f & 4) && m.L8(D + 12563) == 0) bit = TiledSurfaces(m, D) ? 1u : 0u;
  }
  m.S8(D + 11068, uint8_t((m.L8(D + 11068) & 0xFEu) | bit));
  const uint32_t v08 = m.L32(D + 12308);
  const uint32_t r48 = m.L32(D + 10548);
  const uint32_t mask1 = m.L32(D + 12832) != 0 ? 0xFFFFFFFFu : 0u;
  m.S32(D + 12308, v08);
  m.S32(D + 10548, (((mask1 & v08) & 1u) << 1) | (r48 & 0xFFFFFFFDu));
  m.S64(D + 16, m.L64(D + 16) | 0x800ull | 0x20000ull);
  const uint32_t v12 = m.L32(D + 12312);
  const uint32_t r48b = m.L32(D + 10548);
  const uint32_t mask2 = m.L32(D + 12832) != 0 ? 0xFFFFFFFFu : 0u;
  m.S32(D + 12312, v12);
  m.S32(D + 10548, (r48b & 0xFFFFFFFEu) | (mask2 & v12 & 1u));
  m.S64(D + 16, m.L64(D + 16) | 0x800ull | 0x20000ull);
}

}  // namespace wwe2k14::d3d::native_rt
