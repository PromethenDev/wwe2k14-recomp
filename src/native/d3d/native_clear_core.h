// Native body of the guest D3D Clear tree (owner 11-19: fully native):
//   sub_82CB8BA8 (Clear entry: colour unpack, 0/1/n rects) -> sub_82CB8A70 (default rect, f1 default)
//   -> sub_82CB83F0 (clip, dirty-register flush, clear packets, plane emitter calls, scissor).
// Plan + maps: out/qwen/DS-CL-SPEC/SPEC.md, out/qwen/DS-CL{1,2,3}/MAP.md; DeepSeek drafts DS-CL-CODE-A/B/C merged and
// corrected by Claude against generated/wwe2k14_recomp.85.cpp 22966-24244. Verified only by
// src/native/d3d/tests/native-clear-test.cpp (tools/nr/native-clear-test.sh) against the generated bodies.
// Pure templates on a memory policy M (same as native_resolve_core.h):
//   M::L8/L16/L32/L64, S8/S16/S32/S64 (big-endian guest memory),
//   M::Call(addr, lr, r3..r10) -> r3 (64-bit), M::CallF(.., f1 as double bits) for 82CB7E50.
// Guest callees stay guest code (KEEP-GUEST seams): 82CC0988 (state encoder), 82CC0D18 (register runs),
// 82CBA798 (ring reserve), 82C7C8F0 (memcpy), 82CB7E50 (plane emitter, reads one stack word at 92(r1)),
// 82CAC5C0 (scissor packets). M::Call must run them with r1 = caller_sp - 528 (the 83F0 frame, see Clear()).
#pragma once
#include <cstdint>
#include <cstring>

namespace wwe2k14::d3d::native_clear {

constexpr uint32_t kStateEncoder = 0x82CC0988u;
constexpr uint32_t kRegEmit = 0x82CC0D18u;
constexpr uint32_t kRingReserve = 0x82CBA798u;  // returns the new cursor in r3; does NOT store D+48
constexpr uint32_t kMemcpy = 0x82C7C8F0u;
constexpr uint32_t kPlaneEmit = 0x82CB7E50u;
constexpr uint32_t kScissorPackets = 0x82CAC5C0u;
// Game-image constants (lis/addi pairs at 22997/23001, 23441/23449, 24148/24152/24159, 24100/24103).
constexpr uint32_t kDefaultColour = 0x82020BC0u;  // 16 B, 83F0's r6 == 0 (dead from 8BA8, which always passes &stk80)
constexpr uint32_t kTemplate36 = 0x82082F10u;     // r29 = lis -32248 + 12048
constexpr uint32_t kTemplate108 = 0x82082F38u;    // r29 + 40
constexpr uint32_t kUnpackScale = 0x82082FC0u;    // lvx v0  (lis -32248 + 12224)
constexpr uint32_t kUnpackBias = 0x82082FB0u;     // lvx v13 (lis -32248 + 12208)
constexpr uint32_t kDefaultF1 = 0x82000A9Cu;      // lfs f1 (lis -32256 + 2716)

inline uint64_t Rotl64(uint64_t v, unsigned n) {
  n &= 63u;
  return n ? ((v << n) | (v >> (64u - n))) : v;
}
// rlwinm rX,rX,0,28,23 as generated: rotl64(lo | lo << 32, 0) & 0xFFFFFFFFFFFFFF0F - the upper half becomes a copy of
// the low word (r27 is passed whole to 82CB7E50 in r4).
inline uint64_t ClearNibble1(uint64_t r) { return ((r & 0xFFFFFFFFull) | (r << 32)) & 0xFFFFFFFFFFFFFF0Full; }
inline uint64_t Sext32(uint32_t v) { return uint64_t(int64_t(int32_t(v))); }

// lfs: f32 -> f64 bits exactly as x86 cvtss2sd without DAZ (signalling NaN quietened), independent of host MXCSR.
inline uint64_t F32ToF64Bits(uint32_t x) {
  const uint64_t s = uint64_t(x >> 31) << 63;
  const uint32_t e = (x >> 23) & 0xFFu, f = x & 0x7FFFFFu;
  if (e == 0xFFu) return s | (0x7FFull << 52) | (uint64_t(f) << 29) | (f ? (1ull << 51) : 0);
  if (e == 0) {
    if (f == 0) return s;
    int sh = 0;
    uint32_t mm = f;
    while (!(mm & 0x800000u)) mm <<= 1, ++sh;  // normalise the denormal
    return s | (uint64_t(1023 - 126 - sh) << 52) | (uint64_t(mm & 0x7FFFFFu) << 29);
  }
  return s | (uint64_t(e - 127 + 1023) << 52) | (uint64_t(f) << 29);
}
inline double AsDouble(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }
inline uint64_t DBits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
inline uint32_t FBitsOf(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

// fctiwz as generated (23033): NaN -> 0x80000000, > INT_MAX -> INT_MAX, else cvttsd2si (out of range -> 0x80000000).
// The low word of the stfd'd result is what lwz reads back (23037/23051/23059/23061).
inline uint32_t Fctiwz(uint32_t f32) {
  const double d = AsDouble(F32ToF64Bits(f32));
  if (d != d) return 0x80000000u;
  if (d > 2147483647.0) return 0x7FFFFFFFu;
  if (d < -2147483648.0) return 0x80000000u;  // cvttsd2si: integer indefinite
  return uint32_t(int32_t(d));
}

// One lane of x86 mulps/addps under FTZ+DAZ (enableFlushModeUnconditional at 24181), computed in software so the
// result does not depend on the host MXCSR: DAZ on inputs, first-operand NaN propagation (quietened), default NaN
// 0xFFC00000 for invalid operations, flush-to-zero when the result is tiny after rounding (x86 detects tininess
// after rounding). float*float is exact in double; float+float rounded through double is correctly rounded.
inline uint32_t Daz(uint32_t x) { return (x & 0x7F800000u) == 0 ? (x & 0x80000000u) : x; }
inline bool IsNaN32(uint32_t x) { return (x & 0x7FFFFFFFu) > 0x7F800000u; }
inline uint32_t RoundFtz(double exact) {
  const double a = exact < 0 ? -exact : exact;
  // smallest normal 2^-126; values >= 2^-126 - 2^-151 round (24-bit, unbounded exponent) to >= 2^-126.
  constexpr double tiny = 0x1p-126 - 0x1p-151;
  if (a != 0 && a < tiny) return exact < 0 ? 0x80000000u : 0u;
  return FBitsOf(float(exact));
}
inline uint32_t X86MulFtz(uint32_t a, uint32_t b) {
  a = Daz(a), b = Daz(b);
  if (IsNaN32(a)) return a | 0x400000u;
  if (IsNaN32(b)) return b | 0x400000u;
  float fa, fb;
  std::memcpy(&fa, &a, 4), std::memcpy(&fb, &b, 4);
  const double p = double(fa) * double(fb);
  if (p != p) return 0xFFC00000u;
  return RoundFtz(p);
}
inline uint32_t X86AddFtz(uint32_t a, uint32_t b) {
  a = Daz(a), b = Daz(b);
  if (IsNaN32(a)) return a | 0x400000u;
  if (IsNaN32(b)) return b | 0x400000u;
  float fa, fb;
  std::memcpy(&fa, &a, 4), std::memcpy(&fb, &b, 4);
  const double s = double(fa) + double(fb);
  if (s != s) return 0xFFC00000u;
  return RoundFtz(s);
}

struct Frame {
  uint32_t sp;  // guest r1 of the body after its stwu
};

// ---- sub_82CB83F0 (22966-23934) ----
struct WorkerLocals {
  uint32_t dev;    // r31
  uint64_t flags;  // r27 (64-bit: 82CB7E50 gets the whole register in r4)
  uint32_t rect;   // r5
  uint64_t colour; // r23
  uint32_t opaque; // r22 (only stw'd)
  uint64_t f31;    // f1 bits
  uint64_t l, t, r, b;  // r24 r25 r26 r28 (r26/r28 may hold a 33-bit add result: 23085/23097)
};

// 23023-23157: clip the rect against the fctiwz'd viewport D+13024..13036 and, when D+12288 != 0, the clamps
// D+13052..13064. All compares are cmpw (signed low words). false = empty (-> epilogue).
template <class M>
bool ClipRect(M& m, WorkerLocals& L) {
  const uint32_t D = L.dev;
  const uint64_t x0 = Fctiwz(m.L32(D + 13024u)), y0 = Fctiwz(m.L32(D + 13028u));
  const uint64_t w = Fctiwz(m.L32(D + 13032u)), h = Fctiwz(m.L32(D + 13036u));
  auto gt = [](uint64_t a, uint64_t b) { return int32_t(uint32_t(a)) > int32_t(uint32_t(b)); };
  auto lt = [](uint64_t a, uint64_t b) { return int32_t(uint32_t(a)) < int32_t(uint32_t(b)); };
  uint64_t v = m.L32(L.rect + 0u);
  L.l = gt(v, x0) ? v : x0;
  v = m.L32(L.rect + 4u);
  L.t = gt(v, y0) ? v : y0;
  v = m.L32(L.rect + 8u);
  const uint64_t xr = w + x0;  // add r10,r8,r10 (64-bit add of two zero-extended words)
  L.r = lt(v, xr) ? v : xr;
  v = m.L32(L.rect + 12u);
  const uint64_t yb = h + y0;  // add r10,r7,r9
  L.b = lt(v, yb) ? v : yb;
  if (m.L32(D + 12288u) != 0) {
    v = m.L32(D + 13052u);
    if (!gt(L.l, v)) L.l = v;
    v = m.L32(D + 13056u);
    if (!gt(L.t, v)) L.t = v;
    v = m.L32(D + 13060u);
    if (!lt(L.r, v)) L.r = v;
    v = m.L32(D + 13064u);
    if (!lt(L.b, v)) L.b = v;
  }
  return gt(L.r, L.l) && gt(L.b, L.t);
}

// 23158-23382: optional 82CC0988 flush of D+16, then six register runs through 82CC0D18. Every row re-reads its
// dirty word before the test and after the call; the clear masks are the generated constants (not complements).
template <class M>
void DirtyRegisterRuns(M& m, WorkerLocals& L) {
  const uint32_t D = L.dev;
  const uint64_t d16 = m.L64(D + 16u);
  if (d16 != 0) {
    if ((m.L64(D + 40u) & d16) != 0)
      m.S64(D + 16u, m.Call(kStateEncoder, 0x82CB8564u, D, d16, m.L32(D + 10560u), 0, 0, 0, 0, 0));
    struct Row { uint64_t test; unsigned rot; uint64_t vmask; uint32_t reg, off, lr; uint64_t clear; };
    static constexpr Row rows[4] = {
        {0xFFFull, 52, 0xFFF0000000000000ull, 8704u, 10548u, 0x82CB858Cu, 0xFFFFFFFFFFFFF000ull},  // 23182-23206
        {0x1F000ull, 47, 0xF800000000000000ull, 8576u, 10528u, 0x82CB85BCu, 0xFFFFFFFFFFFE0FFFull},  // lis -2|4095
        {0x03FFFC0000000000ull, 6, 0xFFFF000000000000ull, 8192u, 10368u, 0x82CB8600u,
         0xFC0003FFFFFFFFFFull},  // 23238-23274: 0xFFFF rotl 42; (lis -1) rotl 42
        {0x000003FFFFE00000ull, 22, 0xFFFFF80000000000ull, 8448u, 10444u, 0x82CB8644u,
         0xFFFFFC00001FFFFFull},  // 23276-23310: (lis -32) clrldi 22; (lis -32) rotl 21
    };
    for (const Row& w : rows) {
      const uint64_t v = m.L64(D + 16u);
      if ((v & w.test) == 0) continue;
      m.Call(kRegEmit, w.lr, D, Rotl64(v, w.rot) & w.vmask, w.reg, D + w.off, 0, 0, 0, 0);
      m.S64(D + 16u, m.L64(D + 16u) & w.clear);
    }
  }
  uint64_t v = m.L64(D + 24u);  // 23312-23352
  if (v != 0 && (v & 0x007FFFFC00000000ull) != 0) {  // (lis 31|65535) rotl 34
    m.Call(kRegEmit, 0x82CB8694u, D, Rotl64(v, 9) & 0xFFFFF80000000000ull, 8832u, D + 10596u, 0, 0, 0, 0);
    m.S64(D + 24u, m.L64(D + 24u) & 0xFF800003FFFFFFFFull);  // (lis -32) rotl 34
  }
  v = m.L64(D + 32u);  // 23354-23382
  if (v != 0 && (v & 0x3FFFFFFFFFull) != 0) {
    m.Call(kRegEmit, 0x82CB86D8u, D, Rotl64(v, 26) & 0xFFFFFFFFFC000000ull, 8960u, D + 10680u, 0, 0, 0, 0);
    m.S64(D + 32u, m.L64(D + 32u) & 0xFFFFFFC000000000ull);
  }
}

// Ring stores are stwu-form: the cursor addresses the last word written; each word goes to cursor + 4.
template <class M>
inline void Put(M& m, uint32_t& cur, uint32_t v) {
  cur += 4u;
  m.S32(cur, v);
}

// 23383-23831: the three fixed packets (+ optional 0xC0006000 pair), the dirty ORs and, for a plane mask, packet 4.
// Returns true when the plane loop runs (cr0 of clrlwi. r19,r27,28 at 23614, consumed at 23729).
template <class M>
bool ClearPackets(M& m, WorkerLocals& L) {
  const uint32_t D = L.dev;
  uint32_t cur = m.L32(D + 48u);
  if (cur > m.L32(D + 56u)) cur = uint32_t(m.Call(kRingReserve, 0x82CB86FCu, D, 0, 0, 0, 0, 0, 0, 0));
  if (m.L8(D + 11068u) & 0x20u) {  // 23400-23420
    Put(m, cur, 0xC0006000u);
    Put(m, cur, m.L32(D + 13084u));
  }
  Put(m, cur, 0xC0003B00u);  // 23422-23465
  Put(m, cur, 768u);
  Put(m, cur, 0xC01C2B00u);
  Put(m, cur, 0u);
  Put(m, cur, 27u);
  m.Call(kMemcpy, 0x82CB876Cu, cur + 4u, Sext32(kTemplate108), 108u, 0, 0, 0, 0, 0);
  uint32_t limit = m.L32(D + 56u);  // loaded before the D+48 store (23472)
  cur += 108u;
  m.S32(D + 48u, cur);
  if (cur > limit) cur = uint32_t(m.Call(kRingReserve, 0x82CB8788u, D, 0, 0, 0, 0, 0, 0, 0));
  Put(m, cur, 0xC00A2B00u);  // 23487-23517 (lis -16374 | 11008)
  Put(m, cur, 1u);
  Put(m, cur, 9u);
  m.Call(kMemcpy, 0x82CB87B8u, cur + 4u, Sext32(kTemplate36), 36u, 0, 0, 0, 0, 0);
  cur += 36u;
  Put(m, cur, 0x00012180u);  // 23518-23541
  Put(m, cur, 0x10010001u);
  Put(m, cur, 0u);
  limit = m.L32(D + 56u);
  m.S32(D + 48u, cur);  // stored BEFORE the reserve (23546)
  if (cur > limit) cur = uint32_t(m.Call(kRingReserve, 0x82CB87F8u, D, 0, 0, 0, 0, 0, 0, 0));
  static constexpr uint32_t body[14] = {0x00022100u, 0xFFFFu, 0u, 0u, 8851u, 0u, 0x00022204u,
                                        0x00010000u, 0x00010000u, 768u, 8978u, 0xFFFFu, 8205u, 0u};
  for (uint32_t x : body) Put(m, cur, x);  // 23566-23656
  const bool planes = (uint32_t(L.flags) & 0xFu) != 0;  // 23614
  uint64_t d = m.L64(D + 16u);  // 23657-23726: eleven OR-stores, D+16 re-read twice
  d |= 1ull << 41, m.S64(D + 16u, d);
  d |= 1ull << 40, m.S64(D + 16u, d);
  d |= 1ull << 39, m.S64(D + 16u, d);
  m.S64(D + 24u, m.L64(D + 24u) | (1ull << 35));
  d = m.L64(D + 16u);
  d |= 128u, m.S64(D + 16u, d);
  d |= 64u, m.S64(D + 16u, d);
  d |= 32u, m.S64(D + 16u, d);
  m.S64(D + 32u, m.L64(D + 32u) | 0x80000u);
  d = m.L64(D + 16u);
  d |= 1ull << 44, m.S64(D + 16u, d);
  d |= 0x80000u, m.S64(D + 16u, d);
  d |= 0x100000u, m.S64(D + 16u, d);
  m.S32(D + 48u, cur);  // 23727
  if (!planes) return false;
  if (cur > m.L32(D + 56u)) cur = uint32_t(m.Call(kRingReserve, 0x82CB895Cu, D, 0, 0, 0, 0, 0, 0, 0));
  Put(m, cur, 8452u);  // 23770-23815; word 2 is r30 = 0xFFFF from 23564
  Put(m, cur, 0xFFFFu);
  Put(m, cur, 8706u);
  Put(m, cur, 0u);
  Put(m, cur, 8705u);
  Put(m, cur, 0x00010001u);
  m.S32(D + 48u, cur);
  d = m.L64(D + 16u) | (1ull << 37);
  m.S64(D + 16u, d);
  d |= 512u, m.S64(D + 16u, d);
  d |= 1024u, m.S64(D + 16u, d);
  return true;
}

template <class M>
void PlaneEmit(M& m, const Frame& f, const WorkerLocals& L, uint64_t plane, uint32_t lr) {
  m.S32(f.sp + 92u, L.opaque);  // stw r22,92(r1): the 9th argument of 82CB7E50
  m.CallF(kPlaneEmit, lr, L.dev, L.flags, plane, L.l, L.t, L.r, L.b, L.colour, L.f31);
}

// 23883-23924: scissor packets from the two packed words (four sign-extended 15-bit fields), then two state bits.
template <class M>
void TailScissor(M& m, WorkerLocals& L) {
  const uint32_t D = L.dev;
  const uint32_t w1 = m.L32(D + 10440u), w0 = m.L32(D + 10436u);
  auto sx = [](uint32_t v) { return uint64_t(int64_t(int32_t(v) >> 17)); };
  m.Call(kScissorPackets, 0x82CB8A4Cu, D, sx(w0 << 17), sx(w0 << 1), sx(w1 << 17), sx(w1 << 1), 0, 0, 0);
  m.S8(D + 11069u, uint8_t(m.L8(D + 11069u) | 4u));
  m.S64(D + 16u, m.L64(D + 16u) | 0x20000u);
}

// sub_82CB83F0. r3 D, r4 flags, r5 rect, r6 colour pointer (0 -> default), r8 opaque, f1 bits; frame = its r1.
template <class M>
void ClearWorker(M& m, const Frame& f, uint32_t D, uint64_t flags, uint32_t rect, uint64_t colour, uint32_t opaque,
                 uint64_t f1) {
  WorkerLocals L{};
  L.dev = D, L.flags = flags, L.rect = rect, L.colour = colour, L.opaque = opaque, L.f31 = f1;
  if (uint32_t(colour) == 0) {  // 22992-23009: aligned lvx/stvx copy
    const uint32_t dst = (f.sp + 112u) & ~0xFu;
    for (uint32_t k = 0; k < 16u; k += 4) m.S32(dst + k, m.L32((kDefaultColour & ~0xFu) + k));
    L.colour = f.sp + 112u;
  }
  if (m.L32(D + 12832u) == 0) {  // 23011-23021
    L.flags = ClearNibble1(L.flags);
    if (uint32_t(L.flags) == 0) return;
  }
  if (!ClipRect(m, L)) return;
  DirtyRegisterRuns(m, L);
  if (!ClearPackets(m, L)) {
    PlaneEmit(m, f, L, ~0ull, 0x82CB8944u);  // li r5,-1
  } else {
    for (uint32_t i = 0; i < 4u; ++i) {  // 23832-23882
      if (((1u << i) & uint32_t(L.flags)) == 0) continue;
      if (m.L32(D + 12816u + 4u * i) == 0) continue;
      PlaneEmit(m, f, L, i, 0x82CB8A08u);
      L.flags = ClearNibble1(L.flags);
    }
  }
  TailScissor(m, L);
}

// sub_82CB8A70 (23937-24118). r5 == 0 -> RECT {0, 0, right, bottom} at its stk80 from the bound target's size word
// (D+12816, else D+12832; a NULL pointer reads *(36) like the guest), optionally overridden by D+13556/13560.
template <class M>
void ClearOne(M& m, const Frame& f, uint32_t D, uint64_t flags, uint32_t rect, uint64_t colour, uint32_t opaque,
              uint64_t f1) {
  if (rect == 0) {
    const uint32_t r7 = m.L32(D + 12816u);
    const uint32_t obj = r7 != 0 ? r7 : m.L32(D + 12832u);
    const uint32_t s = m.L32(obj + 36u);
    const uint32_t b = m.L8(D + 11068u);
    m.S32(f.sp + 80u, 0u);
    m.S32(f.sp + 84u, 0u);
    m.S32(f.sp + 88u, ((s >> 18) & 0x3FFFu) + 1u);
    m.S32(f.sp + 92u, ((s >> 3) & 0x7FFFu) + 1u);
    bool flag = true;
    if (!(b & 0x10u)) {
      if (!(b & 0x20u)) {
        flag = false;
      } else {
        for (uint32_t k = 0; k < 5u && flag; ++k) {  // 24003-24066
          const uint32_t cur = k == 0 ? r7 : m.L32(D + 12816u + 4u * k);
          if (m.L32(D + 13104u + 4u * k) != cur && cur != 0) flag = false;
        }
      }
    }
    if (flag) {
      m.S32(f.sp + 88u, m.L32(D + 13556u));
      m.S32(f.sp + 92u, m.L32(D + 13560u));
    }
    rect = f.sp + 80u;
  }
  if (!(uint32_t(flags) & 0x10u)) f1 = F32ToF64Bits(m.L32(kDefaultF1));
  ClearWorker(m, Frame{f.sp - 256u}, D, flags, rect, colour, opaque, f1);
}

// 24139-24185: packed colour -> float4 at stk80 = scale * vupkd3d(vsldoi(lvlx(stk212), 4)) + bias, written as the
// generated simde code computes it (host lanes: lvx/stvx reverse the 16 bytes; alignr(v, v, 12) = guest vsldoi 4).
template <class M>
void ColourToF4(M& m, const Frame& f, uint32_t colour) {
  const uint32_t ea = f.sp + 212u;
  m.S32(ea, colour);
  const uint32_t al = ea & ~0xFu, k = ea & 0xFu;
  uint8_t v[16], s[16];
  for (uint32_t i = 0; i < 16u; ++i) v[i] = i < k ? 0 : m.L8(al + 15u - i + k);  // pshufb VectorMaskL[k]
  for (uint32_t i = 0; i < 16u; ++i) s[i] = v[(i + 12u) & 15u];                 // alignr_epi8(v, v, 12)
  const uint32_t u[4] = {0x3F800000u | s[3], 0x3F800000u | s[0], 0x3F800000u | s[1], 0x3F800000u | s[2]};
  const uint32_t dst = (f.sp + 80u) & ~0xFu;
  for (uint32_t i = 0; i < 4u; ++i) {  // host lane i = guest word 3 - i
    const uint32_t sc = m.L32(kUnpackScale + 12u - 4u * i), bi = m.L32(kUnpackBias + 12u - 4u * i);
    m.S32(dst + 12u - 4u * i, X86AddFtz(X86MulFtz(sc, u[i]), bi));
  }
}

// ===== sub_82CB8BA8: the Clear entry (24120-24244) =====
// Guest entry registers: r3 D, r4 rect count, r5 RECT array (16-byte stride) or 0, r6 flags (whole 64-bit register),
// r7 packed colour, r9 opaque word (r8 is unused), f1 (double bits); caller_sp = r1 at entry. Writes the guest stack
// the same way the guest does: caller_sp+52 (the colour word), the float4 at its frame (caller_sp-160)+80, the default
// RECT at (caller_sp-272)+80 and the 82CB7E50 argument at (caller_sp-528)+92. Return value (r3) is not modelled.
template <class M>
void Clear(M& m, uint32_t caller_sp, uint32_t D, uint32_t count, uint32_t rects, uint64_t flags, uint32_t colour,
           uint32_t opaque, uint64_t f1) {
  const Frame f{caller_sp - 160u};
  ColourToF4(m, f, colour);
  const Frame one{f.sp - 112u};
  const uint64_t f4 = f.sp + 80u;
  if (rects == 0) {
    ClearOne(m, one, D, flags, 0u, f4, opaque, f1);
    return;
  }
  if (count == 0) return;
  uint32_t r = rects;
  do {
    ClearOne(m, one, D, flags, r, f4, opaque, f1);
    r += 16u;
  } while (--count != 0);
}

}  // namespace wwe2k14::d3d::native_clear
