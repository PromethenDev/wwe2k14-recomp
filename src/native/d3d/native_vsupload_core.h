// N4 step 2b (fully native): native body of the inline vertex-shader upload sub_82CC1B00 and its whole call tree
// (generated/wwe2k14_recomp.85.cpp): the vertex-fetch patcher sub_82CC1200, the VS->PS interpolator fix-up sub_82CC19B0 and
// its two helpers sub_82CC1828 / sub_82CC18F8. sub_82CC0630 only flushes data-cache lines (dcbf + sync): no host effect.
//   VSUpload(D, flag r4, VS obj r5, r6, decl r7, r8, r9, sub-block idx r10):
//     blk = obj + [obj + 8 (idx + 112)]; len = [blk + 876] bytes (n = len / 4 dwords).
//     Ring reserve of n + 5 dwords (sub_82CBA9D0: cursor D+48 when cursor + 4 (n + 5) <= D+52, else the guest slow path).
//     Words at cursor+4..: 0xC0003B00, 256, 0xC0002B00 | ((n + 1) << 16 & 0x3FFF0000), 0, n & 0x3FFF; then the microcode
//     ([blk + 872] + [obj + 32], physical -> 0xC0000000 view) is copied after them; flag != 0 -> interpolator fix-up;
//     D+11070 bit 7 = (flag != 0); fetch patcher; D+48 = last code dword; D+12272 = [D+12912]; D+12264 = [D+12904].
//   Patcher (obj, code, decl, strides = D+12904 bytes, idx): for each of the [B+28] fetch slots (B = blk + 872, slot words at
//     B + 4 ([B+24] + 9)) the 12-byte vertex fetch at obj + 68 + 416 idx + 12 i is rebuilt from the matching declaration
//     element (decl + 52 + 12 k, matched on stream/usage nibbles of the slot word) or set to a default; grouped fetches are
//     sorted and merged (mini-fetch offsets), then each 12-byte fetch is written to code + 12 ([slot] & 0xFFF).
//   The guest patcher reads one uninitialised stack halfword (its sp + 80) into the swizzle accumulator; only its bit 0 can
//   survive there and that bit never reaches a stored word (it is shifted out of every masked field), so the native body
//   uses 0. The host test gives the guest body random stack bytes to prove it.
// Pure: every guest access goes through the policy M (see native_shader_core.h): L8/L16/L32/L64, S32/S64, S8,
// M::Copy(dst, src, n, lr) -> r3 (guest memcpy 82C7C8F0 returns dst), M::Reserve9D0(dev, ndw, lr) -> cursor or 0 (guest
// sub_82CBA9D0 slow path).
#pragma once
#include <cstdint>

namespace wwe2k14::d3d::native_shader {

namespace vsup {
inline uint32_t Rotl(uint32_t v, unsigned s) { return s ? (v << s) | (v >> (32 - s)) : v; }
constexpr uint32_t kTab16 = 0x820831B8u;  // u16[8]: component masks per swizzle select (patcher)
constexpr uint32_t kTab8 = 0x820831C8u;   // u8[64]: dwords per fetch format (patcher sort/merge)
// The guest buffer: frame 592, entries from sp + 96; its prologue saves r20-r31 + lr in [sp + 488, sp + 592), so entry 32
// and up would overwrite the caller's saved registers: a fetch count > 32 cannot occur in a working game (body falls back).
constexpr uint32_t kMaxFetch = 32;

// Fetch slot count of the VS sub-block (the patcher's loop count; > kMaxFetch would overrun the guest's own stack buffer).
template <class M>
uint32_t FetchCount(M& m, uint32_t obj, uint32_t idx) {
  const uint32_t B = m.L32(obj + ((idx + 112u) << 3)) + obj + 872u;
  return m.L32(B + 28);
}

// One 3-bit swizzle select field of w (at bit `at`) mapped through the component mask table, filtered by the accumulator.
template <class M>
uint32_t Sel(M& m, uint32_t w, unsigned at, uint32_t acc) {
  return m.L16(kTab16 + (((w >> at) & 7u) << 1)) & (acc & 0xFFFFu);
}

// sub_82CC1828: write the default fetch {0xC8000000, 0, 0x02000000} to every slot of interpolator entry e's slot run.
template <class M>
void InterpDefault(M& m, uint32_t obj, uint32_t code, uint32_t e) {
  uint32_t p = obj + 4u * (((m.L32(obj + 20) >> 5) & 0x1Fu) + (m.L16(e) & 0xFFFu) + m.L32(obj + 28) + m.L32(obj + 24) + 9u);
  for (;;) {
    const uint32_t w = m.L32(p);
    const uint32_t dst = (w & 0xFFFu) * 12u + code;
    m.S32(dst + 0, 0xC8000000u);
    m.S32(dst + 4, 0);
    m.S32(dst + 8, 0x02000000u);
    if (w & 0x1000u) break;
    p += 4;
  }
}

// sub_82CC18F8: copy the PS register nibble ([q] >> 8 & 0xF) into the low nibble of word 0 of every slot of entry e's run.
template <class M>
void InterpRemap(M& m, uint32_t obj, uint32_t code, uint32_t e, uint32_t q) {
  uint32_t p = obj + 4u * (((m.L32(obj + 20) >> 5) & 0x1Fu) + (m.L16(e) & 0xFFFu) + m.L32(obj + 28) + m.L32(obj + 24) + 9u);
  for (;;) {
    const uint32_t dst = (m.L32(p) & 0xFFFu) * 12u + code;
    const uint32_t w0 = m.L32(dst);
    m.S32(dst, (w0 & ~0xFu) | ((m.L32(q) >> 8) & 0xFu));
    if (m.L32(p) & 0x1000u) break;
    p += 4;
  }
}

// sub_82CC19B0(vs = r3, code = r4, out = r5, ps = r6): match the VS outputs to the PS inputs.
template <class M>
void InterpFixup(M& m, uint32_t vs, uint32_t code, uint32_t out, uint32_t ps) {
  if ((m.L8(vs + 8) & 7u) == 7u) return;
  const uint32_t f = m.L32(vs + 20);
  if (f & 0x40000u) return;
  if (ps != 0 && (m.L32(ps + 20) & 0x20000u)) return;
  const uint32_t nvs = (f >> 5) & 0x1Fu;
  uint32_t p = vs + 4u * (m.L32(vs + 28) + m.L32(vs + 24) + 9u);
  uint32_t i = 0, q = 0, nps = 0, last = 0;
  if (ps != 0) {
    const uint32_t g = m.L32(ps + 20);
    q = ps + 32;
    last = g & 0x1Fu;
    nps = (g >> 5) & 0x1Fu;
    if (last != 0) last -= 1;
  }
  for (uint32_t k = nps; k != 0; --k) {
    const uint32_t want = m.L8(q + 3);
    while (i < nvs && m.L8(p + 3) < want) {
      InterpDefault(m, vs, code, p);
      ++i;
      p += 4;
    }
    if ((m.L32(p) ^ m.L32(q)) & 0xF00u) InterpRemap(m, vs, code, p, q);
    p += 4;
    q += 4;
    ++i;
  }
  for (; i < nvs; ++i, p += 4) InterpDefault(m, vs, code, p);
  m.S32(out, (m.L32(out) & ~0x00F00000u) | ((last << 20) & 0x00F00000u));
}

inline int32_t Off(uint32_t w2) { return int32_t(w2 << 1) >> 9; }  // signed fetch offset field of word 2

// sub_82CC1200(obj, code, decl, strides, idx). Returns the guest r3 at its end (obj, or the last memcpy destination).
template <class M>
uint64_t PatchFetches(M& m, uint32_t obj, uint64_t code64, uint32_t decl, uint32_t strides, uint32_t idx, uint64_t r3in) {
  const uint32_t code = uint32_t(code64);
  const uint32_t B = m.L32(obj + ((idx + 112u) << 3)) + obj + 872u;
  uint32_t n = m.L32(B + 28);
  if (n == 0) return r3in;
  if (n > kMaxFetch) n = kMaxFetch;  // the guest would overrun its own frame (impossible in a working game); stay in bounds
  const uint32_t list = B + 4u * (m.L32(B + 24) + 9u);
  const uint32_t ndecl = m.L32(decl + 24);
  const uint32_t src = obj + idx * 416u + 68u;
  uint32_t buf[kMaxFetch][3];
  uint32_t acc = 0;  // guest: uninitialised stack u16 (only bit 0 survives and never reaches a stored word; see top)
  // Phase 1: rebuild each fetch from its declaration element.
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t w0 = m.L32(src + 12 * i), w1 = m.L32(src + 12 * i + 4), w2 = m.L32(src + 12 * i + 8);
    uint32_t k = 0, e = decl + 52;
    if (ndecl != 0) {
      const uint32_t slot = m.L32(list + 4 * i);
      for (; k < ndecl; ++k, e += 12)
        if (m.L8(e + 9) == ((slot >> 12) & 0xFu) && m.L8(e + 10) == ((slot >> 16) & 0xFu)) break;
    }
    uint32_t a, b, c;  // new words 0 / 1 / 2 (word 1 gets the swizzle pass below)
    if (ndecl == 0 || k >= ndecl) {
      w0 = (w0 & ~0x3FF00000u) | 0x05F00000u;
      acc = (acc & 0xFu) | 0x9250u;
      a = w0;
      b = (w1 & 0xBFC6CFFFu) | 0x60000u;
      c = (w2 & 0x80000000u) | m.L8(strides);
    } else {
      const uint32_t t = m.L32(e + 4);
      uint32_t s = ((((((t >> 10) & 0x38u) | ((t >> 16) & 7u)) << 3) | ((t >> 19) & 7u)) << 1 | (t & 0xFC00u)) << 3;
      acc = (s | (acc & 0xFu)) & 0xFFFFu;
      if ((t & 0xC0u) == 64) {  // flip the top bit of each 3-bit select <= 3 (four fields)
        if ((acc & 0xE000u) <= 0x6000u) acc = ((acc & 0xE000u) ^ 0x2000u) | (acc & 0x1FFFu);
        if ((acc & 0x1C00u) <= 0xC00u) acc = ((acc & 0x1C00u) ^ 0x400u) | (acc & 0xE3FFu);
        if ((acc & 0x380u) <= 0x180u) acc = ((acc & 0x380u) ^ 0x80u) | (acc & 0xFC7Fu);
        if ((acc & 0x70u) <= 0x30u) acc = ((acc & 0x70u) ^ 0x10u) | (acc & 0xFF8Fu);
      }
      const uint32_t stream = m.L16(e + 0), offset = m.L16(e + 2);
      const uint32_t mul = (95u - stream) * 0x01560056u;
      const uint32_t r20 = (mul & ~0x00F80000u) | (Rotl(mul, 11) & 0x00F80000u);
      w0 = (w0 & ~0x07F00000u) | (Rotl(r20, 1) & 0x07F00000u);
      a = w0 & 0xC7FFFFFFu;
      b = ((((t & 0x3Fu) << 12) | (t & 0x300u)) << 4) | (w1 & 0xBFC0CFFFu);
      c = ((offset << 6) & 0x7FFFFF00u) | m.L8(strides + stream) | (w2 & 0x80000000u);
    }
    buf[i][0] = a;
    buf[i][2] = c;
    acc = (acc & 0xFFFFu) | 14u;
    // Swizzle pass over the four 3-bit selects of word 1 (bits 0-2, 3-5, 6-8, 9-11).
    const uint32_t v0 = Sel(m, b, 0, acc);
    uint32_t v = v0;
    for (int k = 0; k < 4; ++k) v = (v >> 3) | v0;
    b = (b & ~7u) | ((v >> 1) & 7u);
    v = Sel(m, b, 3, acc);
    uint32_t x = ((((v >> 3) | v) >> 3 | v) >> 3 | v) >> 1 | ((v << 2) & 0x3FFFCu);
    b = (b & ~0x38u) | (x & 0x38u);
    v = Sel(m, b, 6, acc);
    x = (((v >> 3) | v) >> 3 | v) >> 1 | (((((v << 3) & 0x7FFF8u) | v) << 2));
    b = (b & ~0x1C0u) | (x & 0x1C0u);
    v = Sel(m, b, 9, acc);
    x = (((v >> 3) | v) >> 1) | (((((((v << 3) & 0x7FFF8u) | v) << 3) | v) << 2));
    b = (b & ~0xE00u) | (x & 0xE00u);
    buf[i][1] = b;
  }
  // Phase 2: groups between flagged slot words (bits 20-21): sort by register key, then merge adjacent mini-fetches.
  const uint32_t end = list + 4 * n;
  uint32_t p = list;
  auto flagged = [&](uint32_t a) { return (m.L32(a) & 0x00300000u) != 0; };
  auto key = [](uint32_t w) { return int32_t(((w >> 18) & 0x7Cu) | ((w >> 25) & 3u)); };
  while (p < end) {
    while (p < end && !flagged(p)) p += 4;
    if (!(p < end)) break;
    const uint32_t g0 = p;
    uint32_t q = p + 4;
    if (!(q < end)) break;
    while (q < end && !flagged(q)) q += 4;
    if (!(q < end)) break;
    const uint32_t i0 = (g0 - list) >> 2;
    const uint32_t gn = ((q - g0) >> 2) + 1;  // entries g0..q inclusive
    uint32_t (*g)[3] = buf + i0;
    p = q + 4;
    uint32_t outer = gn - 1, sortn = gn;
    int32_t pinned = -1;
    if ((m.L32(q) & 0x00300000u) == 0x00300000u) {  // the closing entry is pinned last
      pinned = key(buf[(q - list) >> 2][0]);
      outer -= 1;
      sortn = gn - 1;
    }
    for (uint32_t j = 1, o = 0; o < outer; ++o, ++j) {
      if (j >= sortn) continue;
      for (uint32_t r = j; r < sortn; ++r) {
        uint32_t (&x)[3] = g[j - 1];
        uint32_t (&y)[3] = g[r];
        const int32_t kx = key(x[0]), ky = key(y[0]);
        int32_t d = ky - kx;
        bool swap;
        if (d != 0) {
          if (pinned == kx) swap = true;
          else {
            if (pinned == ky) d = -1;
            swap = d > 0;
          }
        } else {
          d = Off(x[2]) - Off(y[2]);
          if (d == 0) d = int32_t(m.L8(kTab8 + ((x[1] >> 16) & 0x3Fu))) - int32_t(m.L8(kTab8 + ((y[1] >> 16) & 0x3Fu)));
          swap = d > 0;
        }
        if (swap)
          for (int w = 0; w < 3; ++w) {
            const uint32_t t = x[w];
            x[w] = y[w];
            y[w] = t;
          }
      }
    }
    // Merge: a fetch that continues the previous one's register and fits within 8 dwords becomes a mini-fetch of it.
    uint32_t prev = 0;
    for (uint32_t r = 1; r < gn; ++r) {
      uint32_t* cur = g[r];
      uint32_t* pv = g[prev];
      auto reg = [](uint32_t w) { return ((w & ~0xC0000u) | ((w >> 7) & 0xC0000u)) & 0x01FC0000u; };
      bool merged = false;
      if (reg(pv[0]) == reg(cur[0])) {
        const uint32_t po = uint32_t(Off(pv[2])), co = uint32_t(Off(cur[2]));
        if (!(po > co)) {
          uint32_t span = m.L8(kTab8 + ((cur[1] >> 16) & 0x3Fu)) - po + co;
          if (!(span > 8)) {
            const uint32_t have = (pv[0] >> 27) & 7u;
            span -= 1;
            if (have > span) span = have;
            pv[0] = (pv[0] & ~0x38000000u) | ((span << 27) & 0x38000000u);
            cur[1] |= 0x40000000u;
            merged = true;
          }
        }
      }
      if (!merged) prev = r;
    }
  }
  // Phase 3: write each fetch to its slot in the uploaded code.
  uint64_t r3 = r3in;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t dst = (m.L32(list + 4 * i) & 0xFFFu) * 12u + code;
    m.S32(dst + 0, buf[i][0]);
    m.S32(dst + 4, buf[i][1]);
    m.S32(dst + 8, buf[i][2]);
    r3 = dst;
  }
  return r3;
}
}  // namespace vsup

// sub_82CC1B00. Returns the guest r3 at the end of the body (0 when the ring reserve fails).
template <class M>
uint64_t VSUpload(M& m, uint32_t D, uint64_t r4, uint32_t obj, uint32_t r6, uint32_t decl, uint32_t r8, uint32_t r9,
                  uint32_t idx) {
  const uint32_t blk = m.L32(obj + ((idx + 112u) << 3)) + obj;
  const uint32_t len = m.L32(blk + 876);
  const uint32_t n = len >> 2;
  uint32_t r3 = m.L32(D + 48);
  if (r3 + 4u * (n + 5u) > m.L32(D + 52)) r3 = uint32_t(m.Reserve9D0(D, n + 5u, 0x82CC1B4Cu));
  if (r3 == 0) return 0;
  m.S32(r3 + 4, 0xC0003B00u);
  m.S32(r3 + 8, 256u);
  m.S32(r3 + 12, 0xC0002B00u | (((n + 1u) << 16) & 0x3FFF0000u));
  m.S32(r3 + 16, 0);
  m.S32(r3 + 20, n & 0x3FFFu);
  const uint32_t code = r3 + 24;
  const uint32_t a = m.L32(blk + 872) + m.L32(obj + 32);
  const uint32_t src = ((((a >> 20) & 0xFFFu) + 512u) & 0x1000u) + (a & 0x1FFFFFFFu) + 0xC0000000u;
  m.Copy(code, src, len, 0x82CC1BD8u);
  if (uint32_t(r4) != 0) vsup::InterpFixup(m, r8, code, r6, r9);
  m.S8(D + 11070, uint8_t((m.L8(D + 11070) & 0x7Fu) | (uint32_t(r4) != 0 ? 0x80u : 0u)));
  const uint64_t r3out = vsup::PatchFetches(m, obj, code, decl, D + 12904, idx, obj);
  m.S32(D + 48, r3 + 20 + 4 * n);
  m.S64(D + 12272, m.L64(D + 12912));
  m.S64(D + 12264, m.L64(D + 12904));
  return r3out;
}

}  // namespace wwe2k14::d3d::native_shader
