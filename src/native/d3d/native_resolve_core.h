// Native body of the guest Resolve worker sub_82CB2320 (overnight 11-18, owner: "as native as possible by morning").
// Plan + maps: out/qwen/DS-RW-SPEC/SPEC.md, out/qwen/DS-RW{1,2,3}/MAP.md (DeepSeek, spot-checked by Claude); functions
// #1-#25 written by DeepSeek (DS-RW-CODE-A/B/C), merged + composed (Resolve) by Claude, verified only by
// src/native/d3d/tests/native-resolve-test.cpp against the generated body. Pure templates on a memory policy M:
//   M::L8/L16/L32/L64, S8/S16/S32/S64 (big-endian guest memory), M::Call(addr, lr, r3..r10) -> r3 (64-bit),
//   M::CallF(.., f1 as double bits) for 82CB1A50, M::CallV(.., v1 from a 16-byte guest address) for 82CB1FE8.
#pragma once
#include <cstdint>
#include <cstring>

namespace wwe2k14::d3d::native_resolve {

// Game-image constants (lis/addi pairs at 8359/8378/8523/8624/10653 of the generated body).
constexpr uint32_t kGateA = 0x83947024u;        // runtime switch behind op-flag bit 14
constexpr uint32_t kGateB = 0x83947028u;        // runtime switch behind op-flag bit 15
constexpr uint32_t kFormatTable = 0x820828A1u;  // u8 per 2*format code: bytes per sample
constexpr uint32_t kDefaultSrc = 0x82082B28u;   // static source descriptor used when r9 == 0
constexpr uint32_t kDev16 = 16, kDev24 = 24, kDev32 = 32;  // dirty-register words of the device record

// 64-bit rotate: the generated code writes rldicr/rldicrl forms as __builtin_rotateleft64(v, n) & mask.
inline uint64_t Rotl64(uint64_t v, unsigned n) {
  n &= 63u;
  return n ? ((v << n) | (v >> (64u - n))) : v;
}
// rlwinm/rlwimi emulated exactly as the generated code writes it:
// __builtin_rotateleft64(x | (x << 32), n) & mask.  (Equal to (rotl32(x, n) & mask) for mask < 2^32.)
inline uint32_t Rotl32And(uint32_t x, unsigned n, uint32_t mask) {
  const uint64_t v = uint64_t(x) | (uint64_t(x) << 32);
  return uint32_t(Rotl64(v, n) & mask);
}

struct Frame {
  uint32_t sp;  // guest stack pointer of the worker frame (r1 after stwu r1,-384(r1))
};
inline uint32_t Stk(const Frame& f, uint32_t off) { return f.sp + off; }  // stk<N> of the body

// Values the body keeps live between the plan's functions. Named by meaning, guest register in the comment.
struct Locals {
  uint32_t dev;         // r31  D3D device (also the r3 return value of the body)
  uint32_t flags;       // r21  op-flag word; ClassBits ORs the class bits in
  uint32_t rect;        // r23  RECT* (guest address); 0 -> the frame's stk192 copy
  uint32_t payload;     // r16  ring payload word of the 0x00002102 packet
  uint32_t callerFlag;  // r7   body spills it to stk436
  uint32_t rec;         // r15  resolve/target record
  uint32_t src;         // r29  source descriptor (defaulted to kDefaultSrc)
  uint32_t mode;        // r10  body spills it to stk460
  uint64_t factorBits;  // f31  scale-hook argument (double bits)
  uint32_t factorPtr;   // r17  stk476; 0 -> 16-byte default float4 in the image
  uint32_t hookArg;     // stk492
  uint32_t srcAlt;      // stk500  raw source-descriptor argument (reloaded into r29 at 8885)
  uint32_t gateA;       // stk144
  uint32_t gateB;       // stk136
  uint32_t target;      // stk148  flags & 7
  uint32_t bias;        // r22  1 or 8; later reused as the "rect changed" flag
  uint32_t height;      // r24
  uint32_t width;       // stk132
  uint32_t addrWord;    // r10 / stk140
  uint32_t rowBytes;    // r28  row bytes, then the 2/3/7 stride factor
  uint32_t rowDivisor;  // r19
  uint32_t byteOffset;  // r18
  uint32_t extent;      // r26
  uint32_t code;        // r30  remapped format code
  uint32_t fmtWord;     // r30  rec[32] before the code remap
  uint32_t resClass;    // r14  (rec[28] >> 2) & 3
  uint32_t scaleFlag;   // r25  (rec[40] & 0xE) != 0
  uint32_t regBits;     // r27
  uint32_t packWord;    // r11  packed register word
  uint32_t extentWord;  // r8
  uint32_t dimSel;      // r6   rec[48] & 0x600
  uint32_t geomArg;     // r20  stk468; becomes -1 at 8887
  uint32_t tiled;       // #9 result (guest r11, as a u32 flag)
  uint32_t rows;        // r30 at 8522 = stk156 before 82CAB1E8
};

// #1  generated 8350-8391.  Gate words stk144/stk136.  Also r22 = 1 (8353-8356).
// The generated stores 1 and then falls into the 0 store when the global is non-zero; only the final
// value survives, which is what this computes.
template <class M>
void Gates(M& m, const Frame&, Locals& L) {
  L.bias = 1;
  L.gateA = ((L.flags & 0x4000u) && m.L32(kGateA) == 0) ? 1u : 0u;
  L.gateB = ((L.flags & 0x8000u) && m.L32(kGateB) == 0) ? 1u : 0u;
}

// #2  generated 8392-8441.  stk148 = flags & 7; the "class present" bits 0x10/0x50/0x70 are OR-ed into
// r21 from the target object's u16 at +24 (bits 0-1).  (target + 3204) << 2 == 12816 + 4 * target, which
// also covers the target == 4 special case at 8404 (D + 12832).
template <class M>
void ClassBits(M& m, const Frame&, Locals& L) {
  L.target = L.flags & 7u;
  if (L.flags & 0x70u) return;
  const uint32_t obj = m.L32(L.dev + 12816u + 4u * L.target);
  const uint32_t v = uint32_t(uint16_t(m.L16(obj + 24u))) & 3u;
  if (v == 0) L.flags |= 0x10u;
  else if (v == 1) L.flags |= 0x50u;
  else L.flags |= 0x70u;
}

// #3  generated 8462-8506.  Sample counts from rec[36/40/48]; r22 (1 or 8), stk132 (width), r24 (height),
// stk128 (= r24, an out slot of 82CAB1E8) and r30 = rec[32].
template <class M>
void SampleDims(M& m, const Frame& f, Locals& L) {
  const uint32_t w36 = m.L32(L.rec + 36u);
  const uint32_t w40 = m.L32(L.rec + 40u);
  const uint32_t w48 = m.L32(L.rec + 48u);
  const uint32_t sel = Rotl32And(w48, 0, 0x600u);
  uint32_t bias = Rotl32And(w40, 2, 0x2u) + 1u;  // rlwinm r11,r11,2,30,30 ; addi r11,r11,1
  uint32_t lo, hi;
  if (sel == 1024u) {
    L.bias = 8;
    lo = Rotl32And(w36, 0, 0x7FFu);
    hi = Rotl32And(w36, 21, 0x7FFu);
  } else {
    lo = Rotl32And(w36, 0, 0x1FFFu);
    hi = Rotl32And(w36, 19, 0x1FFFu);
  }
  L.fmtWord = m.L32(L.rec + 32u);
  L.height = hi + bias;
  L.width = lo + bias;
  m.S32(Stk(f, 128), L.height);
  m.S32(Stk(f, 132), L.width);  // stw r10,132(r1) (8506): 82CAB1E8 gets &stk132
}

// NOT one of #1-#9: the body's 8602-8627 (default RECT written to stk192 with r23 = &stk192, then the
// default source descriptor) has no row in the plan's split, but #6 needs both. Move it to that row.
template <class M>
void DefaultRectAndSrc(M& m, const Frame& f, Locals& L) {
  if (L.rect == 0) {
    m.S32(Stk(f, 204), L.height);  // bottom = r24
    m.S32(Stk(f, 192), 0);         // left
    m.S32(Stk(f, 196), 0);         // top
    m.S32(Stk(f, 200), m.L32(Stk(f, 132)));  // right = stk132 as it is now (8608; 82CAB1E8 may rewrite it)
    L.rect = Stk(f, 192);
  }
  if (L.src == 0) L.src = kDefaultSrc;
}

// #4  generated 8909-9004 (colour words, code == 61 only).  stk500 is reloaded into r29 at 8885.
// Defaults when the descriptor is NULL: 0x1111 / 0x80808080 / 0xFFFFFFFF.
template <class M>
void ColourWords(M& m, const Frame&, Locals& L) {
  if (L.code != 61u) return;
  uint32_t c0, c1, c2;
  const uint32_t s = L.srcAlt;
  if (s != 0) {
    const uint32_t w12 = m.L32(s + 12u), w16 = m.L32(s + 16u);
    const uint32_t w20 = m.L32(s + 20u), w24 = m.L32(s + 24u);
    const uint32_t b28 = uint32_t(uint8_t(m.L8(s + 28u)));
    const uint32_t b29 = uint32_t(uint8_t(m.L8(s + 29u)));
    const uint32_t b30 = uint32_t(uint8_t(m.L8(s + 30u)));
    const uint32_t b31 = uint32_t(uint8_t(m.L8(s + 31u)));
    const uint32_t b32 = uint32_t(uint8_t(m.L8(s + 32u)));
    const uint32_t b33 = uint32_t(uint8_t(m.L8(s + 33u)));
    const uint32_t b34 = uint32_t(uint8_t(m.L8(s + 34u)));
    const uint32_t b35 = uint32_t(uint8_t(m.L8(s + 35u)));
    // rlwimi r8,r9,4,25,27 ; rlwinm r8,r8,4,21,27 ; rlwinm r8,r8,0,25,23 ; or r9,r8,r9 ; rlwinm r9,r9,4,0,27
    uint32_t r8 = Rotl32And(w24, 4, 0x70u) | (w20 & 0xFFFFFF8Fu);
    r8 = Rotl32And(r8, 4, 0x7F0u);
    r8 &= 0xFFFFFF7Fu;
    uint32_t r9v = r8 | (w16 & 7u);
    r9v = Rotl32And(r9v, 4, 0xFFFFFFF0u);
    c0 = r9v | (w12 & 7u);
    // bytes 32,35,34,33 -> D+10812 ; bytes 28,31,30,29 -> D+10816
    uint32_t r10 = Rotl32And(b32 << 8, 0, 0xFFFFFF00u) | b35;
    r10 = Rotl32And(r10, 8, 0xFFFFFF00u) | b34;
    r10 = Rotl32And(r10, 8, 0xFFFFFF00u) | b33;
    uint32_t r11 = Rotl32And(b28 << 8, 0, 0xFFFFFF00u) | b31;
    r11 = Rotl32And(r11, 8, 0xFFFFFF00u) | b30;
    r11 = Rotl32And(r11, 8, 0xFFFFFF00u) | b29;
    c1 = r11;  // host test: the bytes 28..31 word lands at 10812, 32..35 at 10816
    c2 = r10;
  } else {
    c0 = 0x1111u;
    c1 = 0x80808080u;
    c2 = 0xFFFFFFFFu;
  }
  m.S32(L.dev + 10808u, c0);
  m.S32(L.dev + 10812u, c1);
  m.S32(L.dev + 10816u, c2);
  uint64_t d = m.L64(L.dev + kDev32);
  m.S64(L.dev + kDev32, d | 60u);
  d |= 60u;
  m.S64(L.dev + kDev32, d | 30u);
  d |= 30u;
  m.S64(L.dev + kDev32, d | 15u);
}

// #5  generated 8507-8601.  82CAA500 fills stk152/stk156; the format byte table gives the row bytes,
// r8/stk152 the row divisor, then (if r26|r20) 82CAB1E8 replaces r24/stk156 and r19 = stk156 / r28.
template <class M>
void RowsAndDivisor(M& m, const Frame& f, Locals& L) {
  const uint32_t code = L.fmtWord & 0x3Fu;
  m.Call(0x82CAA500u, 0x82CB2478u, code, Stk(f, 152), Stk(f, 156), 0, 0, 0, 0, 0);
  const uint32_t w28 = m.L32(L.rec + 28u);
  const uint32_t r8 = Rotl32And(w28, 15, 0x3FE0u);  // rlwinm r8,r8,15,18,26
  L.addrWord = L.fmtWord & 0xFFFFF000u;            // stk140 / r10
  m.S32(Stk(f, 140), L.addrWord);                  // stw r10,140(r1) (8519): 82CAB1E8 gets &stk140
  const uint32_t s152 = m.L32(Stk(f, 152));
  const uint32_t s156 = m.L32(Stk(f, 156));
  L.rows = s156;  // r30 (8522): kept in a non-volatile register across 82CAB1E8, divides the height at 8650
  const uint32_t byte = uint32_t(uint8_t(m.L8(kFormatTable + Rotl32And(code, 1, 0x7Eu))));
  uint32_t prod = uint32_t(int32_t(byte) * int32_t(s152));
  prod = uint32_t(int32_t(prod) * int32_t(s156));
  L.rowBytes = Rotl32And(prod, 29, 0x1FFFFFFFu);   // rlwinm r28,r9,29,3,31
  L.rowDivisor = s152 ? r8 / s152 : 0;             // divwu (twllei r11,0)
  m.S32(Stk(f, 152), L.rowBytes);
  if (L.mode == 0 && L.geomArg == 0) return;
  m.S32(Stk(f, 84), Stk(f, 128));
  m.S32(Stk(f, 92), Stk(f, 160));
  m.S32(Stk(f, 100), Stk(f, 140));
  m.Call(0x82CAB1E8u, 0x82CB2514u, L.rec, L.geomArg & ~(L.bias - 1u), L.mode, Stk(f, 156),
         Stk(f, 192), Stk(f, 176), Stk(f, 168), Stk(f, 132));  // stk84/92/100 above
  const uint32_t s156b = m.L32(Stk(f, 156));
  L.rowDivisor = L.rowBytes ? s156b / L.rowBytes : 0;  // divwu r19,r11,r28
  L.addrWord = m.L32(Stk(f, 140));                     // lwz r10,140(r1) (8595): the helper may rewrite it
  L.height = m.L32(Stk(f, 128));
}

// #6  generated 8630-8760.  Byte offset r18, extent r26, resource class r14, remapped code r30, packed
// register word r11, extent word r8, stride factor r28, r6 = rec[48] & 0x600 (the #7 test).
template <class M>
void OffsetAndCode(M& m, const Frame& f, Locals& L) {
  const uint32_t srcTop = m.L32(L.src + 4u);
  const uint32_t srcLeft = m.L32(L.src + 0u);
  const uint32_t addrMask = Rotl32And(L.addrWord, 12, 0xFFFu);  // rlwinm r11,r10,12,20,31
  const uint32_t r7src = addrMask + 512u;
  const uint32_t rectTop = m.L32(L.rect + 4u);
  const uint32_t rectLeft = m.L32(L.rect + 0u);
  const uint32_t w40 = m.L32(L.rec + 40u);
  const uint32_t w32 = m.L32(L.rec + 32u);
  const uint32_t w28 = m.L32(L.rec + 28u);
  const uint32_t dTop = srcTop - rectTop;
  const uint32_t dLeft = srcLeft - rectLeft;
  const uint32_t heightSpan = L.height - srcTop;
  uint32_t tmp = uint32_t(int32_t(dTop) * int32_t(L.rowDivisor));
  tmp += Rotl32And(dLeft, 5, 0xFFFFFFE0u);
  const uint32_t tiledBit = Rotl32And(r7src, 0, 0x1000u);
  uint32_t off = uint32_t(int32_t(tmp) * int32_t(L.rowBytes)) + tiledBit;
  off += L.addrWord & 0x1FFFFFFFu;  // clrlwi r9,r10,3
  const uint32_t rows = L.rows;  // r30 = stk156 as read at 8522 (before 82CAB1E8)
  const uint32_t ext = rows ? heightSpan / rows : 0u;
  uint32_t code = w32 & 0x3Fu;
  // addic r7,r6,-1 / subfe r25: ~(r6-1) + r6 + ca  ==  (r6 != 0)
  const uint32_t e = w40 & 0xEu;
  const uint32_t scaleFlag = e ? 1u : 0u;
  switch (code) {
    case 54: code = 7; break;
    case 55: code = 16; break;
    case 56: code = 17; break;
    case 27: code = 30; break;
    case 28: code = 31; break;
    case 29: code = 32; break;
    case 22: case 23: case 50: code = 6; break;
    default: break;
  }
  L.extent = ext + rectTop;
  L.byteOffset = off;
  uint32_t regBits = Rotl32And(L.flags, 6, 0x3Fu);  // (flags >> 26) & 0x3F
  const uint32_t r11bit = w40 & 1u;
  const uint32_t cls = Rotl32And(w28, 30, 0x3u);  // (rec[28] >> 2) & 3
  // addi r11,r14,-1 / cntlzw / rlwinm 27,31,31  ->  1 iff cls == 1
  const uint32_t stride = (cls == 1u) ? 1u : 0u;
  uint32_t r28 = (r11bit == 1u) ? stride + 2u : stride;
  const uint32_t pred = uint32_t(m.Call(0x82CB18F0u, 0x82CB2674u, code, 0, 0, 0, 0, 0, 0, 0));
  if (pred != 0) r28 = 7;
  regBits = (regBits & 0xFFFFFEFFu) | (scaleFlag << 8);  // rlwimi r27,r25,8,23,23
  const uint32_t r28p = Rotl32And(regBits & 0x13Fu, 3, 0xFFFFFFF8u) | (r28 & 7u);
  const uint32_t packed = Rotl32And(r28p << 6, 0, 0xFFFFFFC0u) | (code & 0x3Fu);
  L.code = code;
  L.resClass = cls;
  L.scaleFlag = scaleFlag;
  L.regBits = regBits;
  L.rowBytes = r28;
  L.packWord = Rotl32And(packed, 7, 0xFFFFFF80u) | ((w32 >> 6) & 3u);
  L.extentWord = ((L.extent & 0x3FFFu) << 16) | (L.rowDivisor & 0x3FFFu);  // r8 = rotl(r26,16) (8806): r26 = ext + top
  L.dimSel = m.L32(L.rec + 48u) & 0x600u;
}

// #7  generated 8761-8908 (up to the code == 61 hand-off to ColourWords): 0x400-sample block,
// surface class 1/3/0/2, the 10776/10780/10784/10788 stores and D+32 |= 0x3C00.  r20 := -1 at 8887.
template <class M>
void PackCopyRegs(M& m, const Frame&, Locals& L) {
  uint32_t packWord = L.packWord;
  if (L.dimSel == 1024u) {
    uint32_t r10 = (L.bias - 1u) & L.geomArg;
    const uint32_t prod = uint32_t(int32_t(L.rowDivisor) * int32_t(L.height));
    m.S32(L.dev + 10820u, prod);
    m.S64(L.dev + kDev32, m.L64(L.dev + kDev32) | 4u);
    r10 = Rotl32And(r10, 4, 0x70u);  // rlwinm r10,r10,4,25,27
    packWord = r10 | packWord | 8u;
  }
  uint32_t cls = 1;
  if (L.flags & 0x2000u) cls = 3;
  else if (L.target == 4u) cls = 0;
  else if (L.code == 61u) cls = 2;
  const uint32_t dirty76 = Rotl32And(cls, 20, 0x300000u) | ((L.flags & 887u) - 16u);
  m.S32(L.dev + 10788u, packWord);
  m.S32(L.dev + 10780u, L.byteOffset);
  m.S32(L.dev + 10784u, L.extentWord);
  m.S32(L.dev + 10776u, dirty76);
  m.S64(L.dev + kDev32, m.L64(L.dev + kDev32) | 15360u);
  L.packWord = packWord;
  L.rowBytes = dirty76;  // r28 holds the 10776 word from here (8895); Hooks passes r28 & 7 (9029)
  L.src = L.srcAlt;      // lwz r29,500(r1)
  L.geomArg = 0xFFFFFFFFu;  // li r20,-1
}

// #8  generated 9146-9348.  Six cached-register groups: {D word, test mask, value} -> mask shifted to
// the top of the 64-bit register word, writer 82CC0D18(D, value, base, array), then the bits cleared.
template <class M>
void EmitGroups(M& m, const Frame&, Locals& L) {
  const uint32_t D = L.dev;
  auto emit = [&](uint64_t v, uint32_t base, uint32_t arr, uint32_t lr) {
    m.Call(0x82CC0D18u, lr, D, v, base, arr, 0, 0, 0, 0);
  };
  // bits 0-11 -> 0x2200, values at D+10548
  uint64_t w = m.L64(D + kDev16);
  if (w & 0xFFFu) {
    emit(Rotl64(w, 52) & 0xFFF0000000000000ull, 8704u, D + 10548u, 0x82CB2934u);
    w = m.L64(D + kDev16);
    m.S64(D + kDev16, Rotl64(w, 0) & 0xFFFFFFFFFFFFF000ull);
  }
  // bits 12-16 -> 0x2180, values at D+10528 (rldicr r11,r11,0,25 keeps bits 32-63 of the word)
  w = m.L64(D + kDev16);
  if (w & 0x1F000u) {
    emit(Rotl64(w, 47) & 0xF800000000000000ull, 8576u, D + 10528u, 0x82CB2964u);
    w = m.L64(D + kDev16);
    m.S64(D + kDev16, w & 0xFFFFFFFFFFFE0FFFull);
  }
  // bits 42-57 -> 0x2000, values at D+10368
  w = m.L64(D + kDev16);
  if (w & (Rotl64(0xFFFFull, 42) & 0xFFFFFC0000000000ull)) {
    emit(Rotl64(w, 6) & 0xFFFF000000000000ull, 8192u, D + 10368u, 0x82CB29A8u);
    w = m.L64(D + kDev16);
    m.S64(D + kDev16, w & (Rotl64(0xFFFFFFFFFFFF0000ull, 42)));
  }
  // bits 21-41 -> 0x2100, values at D+10444
  w = m.L64(D + kDev16);
  if (w & (0xFFFFFFFFFFE00000ull & 0x3FFFFFFFFFFull)) {
    emit(Rotl64(w, 22) & 0xFFFFF80000000000ull, 8448u, D + 10444u, 0x82CB29ECu);
    w = m.L64(D + kDev16);
    m.S64(D + kDev16, w & Rotl64(0xFFFFFFFFFFE00000ull, 21));
  }
  // D+24 bits 34-54 -> 0x2280, values at D+10596
  uint64_t w24 = m.L64(D + kDev24);
  if (w24 != 0 && (w24 & (Rotl64(0x1FFFFFull, 34) & 0xFFFFFFFC00000000ull))) {
    emit(Rotl64(w24, 9) & 0xFFFFF80000000000ull, 8832u, D + 10596u, 0x82CB2A3Cu);
    w24 = m.L64(D + kDev24);
    m.S64(D + kDev24, w24 & Rotl64(0xFFFFFFFFFFE00000ull, 34));
  }
  // D+32 bits 0-37 -> 0x2300, values at D+10680
  uint64_t w32 = m.L64(D + kDev32);
  if (w32 != 0 && (w32 & 0x3FFFFFFFFFull)) {
    emit(Rotl64(w32, 26) & 0xFFFFFFFFFC000000ull, 8960u, D + 10680u, 0x82CB2A80u);
    w32 = m.L64(D + kDev32);
    m.S64(D + kDev32, Rotl64(w32, 0) & 0xFFFFFFC000000000ull);
  }
}

// #9  generated 9389-9486.  Same predicate as native_rt::TiledSurfaces(D): D+11068 bit 0x10 -> true,
// bit 0x20 -> the five bound surfaces equal the tiling set (or are NULL).  Result left in L.tiled.
template <class M>
void FlushDecision(M& m, const Frame&, Locals& L) {
  const uint32_t b = uint32_t(uint8_t(m.L8(L.dev + 11068u)));
  if (b & 0x10u) { L.tiled = 1u; return; }
  if (!(b & 0x20u)) { L.tiled = 0u; return; }
  for (uint32_t k = 0; k < 5u; ++k) {
    const uint32_t a = m.L32(L.dev + 12816u + 4u * k);
    const uint32_t t = m.L32(L.dev + 13104u + 4u * k);
    if (t != a && a != 0) { L.tiled = 0u; return; }
  }
  L.tiled = 1u;
}


// Game-image constants used by #18-#25 (lis/addi pairs at 9110/10200/10260/10653 of the generated body).
constexpr uint32_t kGlobalPacket = 0x83947020u;  // gates the 13-word main packet (10653-10661)
constexpr uint32_t kFloatHalfAddr = 0x8202B72Cu;  // == native_rt::kHalf (0.5f), D+10688 bit 0 clear
constexpr uint32_t kFloatAltAddr = 0x82000A9Cu;   // the other rect bias constant (value NOT verified)
// r28 when #20/#21 run: the last write to r28 in the whole body is 9645-9713 (`li r28, 0xC0004600`),
// and r28 is non-volatile, so it survives every guest call up to 10334.
constexpr uint32_t kSetConstWord = 0xC0004600u;

// srawi (arithmetic shift right, as the generated code writes it).
inline int32_t Sar(uint32_t v, unsigned n) { return int32_t(v) >> n; }
// stfs/lfs payload helpers.
inline float FBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint32_t BitsF(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
// extsw + fcfid + frsp: int32 -> double exactly -> single.  Used by the six scratch floats.
inline float I2F(int32_t v) { return float(double(v)); }
// "tiled address": (addr>>20 & 0xFFF + 512) & 0x1000 | addr & 0x1FFFFFFF   (generated 10200-10214,
// 8660-8680; the rlwinm mask 12,20,31 gives bits 20-31 of the word).
inline uint32_t TiledAddr(uint32_t a, uint32_t lowMask) {
  return ((((a >> 20) & 0xFFFu) + 512u) & 0x1000u) + (a & lowMask);
}

// State of #18-#25 that Locals (CODE-A) has no field for.  Guest registers in the comments.
struct LocalsExt {
  uint32_t rgL, rgT, rgR, rgB;  // r27/r25/r24/r23 of 9042-9060: rect left/top, (right|7)&~7, (bottom|7)&~7
  uint32_t packPtr;             // r29 of the 10334 packet: 3 (scratch taken) or the caller flag
  uint32_t cursor;              // r3/r11 ring write cursor of #24/#25
  uint32_t destWord;            // r30 of 10652: stk136 (mode != 0) or stk160
  uint32_t tiledAddr;           // r29 of 10652: the tiled address chosen with destWord
  uint32_t globalOn;            // M[kGlobalPacket] as read by #24 (0 -> #25's 11072 fallback)
  uint32_t ring = 0;           // CODE-B: r11 ring cursor handed from #12/#13 to #14 (D+48 is stale there)
  bool fastPath = false;        // #18: caller flag set -> the body skips #8 EmitGroups
  bool abort = false;           // #20: scratch alloc returned 0 -> the body goes straight to the epilogue
};

// ===== CODE-B (#10-#17) =====


// Device-record slots used by #10-#17 (offsets of the D3D device record, guest r31).
constexpr uint32_t kRingCur = 48u;     // D+48  ring write cursor (the body's r3/r11)
constexpr uint32_t kRingLimit = 56u;   // D+56  ring limit; only D+52 (segment end) is not read here
constexpr uint32_t kRingReserve = 0x82CBA798u;  // RingMakeSpace; returns the new cursor, does NOT set D+48
constexpr uint32_t kMicrocode = 0x82082A60u;    // 60-byte shader blob copied by #13 (see NOTES)


// Ring stores use the body's stwu form: the cursor points at the LAST word written; each store goes to cursor + 4.
// The body's inline ring check, written exactly as it appears before each packet:
//   lwz r3,48(D) ; lwz r11,56(D) ; cmplw ; ble ok ; mr r3,D ; bl 0x82CBA798
template <class M>
inline uint32_t RingMakeSpace(M& m, uint32_t dev, uint32_t lr) {
  const uint32_t cur = m.L32(dev + kRingCur);
  if (cur > m.L32(dev + kRingLimit))  // cmplw = unsigned
    return uint32_t(m.Call(kRingReserve, lr, dev, 0, 0, 0, 0, 0, 0, 0));
  return cur;
}

// #10  generated 9349-9388 (entry loc_82CB2A90, reached only when stk436 != 0).
// Flush dispatcher with r4 = 0, then the 2-word packet 0x00002102 / r16; D+48 += 8.  No ring words and no
// cursor store at all when r16 == 0 (the cursor keeps whatever D+48 holds).
template <class M>
void FastPath(M& m, const Frame&, Locals& L, LocalsExt& X) {
  if (L.callerFlag == 0) return;
  m.Call(0x82CB20C0u, 0x82CB2A9Cu, L.dev, 0, 0, 0, 0, 0, 0, 0);
  if (L.payload == 0) return;
  uint32_t cur = RingMakeSpace(m, L.dev, 0x82CB2ABCu);
  m.S32(cur + 4u, 0x00002102u);
  m.S32(cur + 8u, L.payload);
  cur += 8u;
  m.S32(L.dev + kRingCur, cur);
  X.ring = cur;
}

// #11  generated 9487-9644 (entry loc_82CB2B70, guarded by the #9 result r11 != 0).
// 0xC0006100,0 then per entry of D+13124 / table D+13368 (8 bytes): 0xC0006000, 3<<2i, 0xC0015501, 8985,
// address; tail 0xC0006000, D+13084, 0xC0006100, D+13088.  The cursor is stored to D+48 after every entry
// and the reserve is called between entries when it passed the limit.
//   address = (entry[1] * r19 + ((entry[0] << 5) & ~31)) * stk152 + r18   (mullw/add, 32-bit wrap)
// r19 is still the row divisor here (CODE-A #5/#6); stk152 is the row-bytes value #5 stored.
template <class M>
void StateBlock(M& m, const Frame& f, Locals& L, LocalsExt& X) {
  if (L.tiled == 0) return;
  const uint32_t D = L.dev;
  uint32_t cur = RingMakeSpace(m, D, 0x82CB2B90u);
  m.S32(cur + 4u, 0xC0006100u);
  m.S32(cur + 8u, 0u);
  cur += 8u;
  if (int32_t(m.L32(D + 13124u)) > 0) {
    uint32_t i = 0, shift = 0, e = D + 13368u;
    do {
      const uint32_t w0 = m.L32(e);
      const uint32_t w1 = m.L32(e + 4u);
      const uint32_t idx = (shift & 0x20u) ? 0u : (3u << (shift & 0x3Fu));  // slw r7,r7,r29
      const uint32_t rowBytes = m.L32(Stk(f, 152u));
      const uint32_t addr = (w1 * L.rowDivisor + ((w0 << 5) & 0xFFFFFFE0u)) * rowBytes + L.byteOffset;
      m.S32(cur + 4u, 0xC0006000u);
      m.S32(cur + 8u, idx);
      m.S32(cur + 12u, 0xC0015501u);
      m.S32(cur + 16u, 8985u);
      m.S32(cur + 20u, addr);
      cur += 20u;
      m.S32(D + kRingCur, cur);
      if (cur > m.L32(D + kRingLimit))
        cur = uint32_t(m.Call(kRingReserve, 0x82CB2C34u, D, 0, 0, 0, 0, 0, 0, 0));
      ++i;
      e += 8u;
      shift += 2u;
    } while (i < m.L32(D + 13124u));  // the body reloads the count each pass; cmp is unsigned
  }
  m.S32(cur + 4u, 0xC0006000u);
  m.S32(cur + 8u, m.L32(D + 13084u));
  m.S32(cur + 12u, 0xC0006100u);
  m.S32(cur + 16u, m.L32(D + 13088u));
  cur += 16u;
  m.S32(D + kRingCur, cur);
  X.ring = cur;
}

// #12  generated 9645-9713 (entry loc_82CB2C7C).  The ring check is unconditional here, and the block
// does NOT store D+48: the cursor leaves in r11, so it is exported through X.ring.
// D+11070 bit 3 -> three 0xC0004600,6 pairs, then D+11070 &= 247.
template <class M>
void TailScissor(M& m, const Frame&, Locals& L, LocalsExt& X) {
  const uint32_t D = L.dev;
  uint32_t cur = RingMakeSpace(m, D, 0x82CB2C94u);
  if (uint32_t(m.L8(D + 11070u)) & 8u) {
    m.S32(cur + 4u, 0xC0004600u);
    m.S32(cur + 8u, 6u);
    m.S32(cur + 12u, 0xC0004600u);
    m.S32(cur + 16u, 6u);
    m.S32(cur + 20u, 0xC0004600u);
    m.S32(cur + 24u, 6u);
    cur += 24u;
    m.S8(D + 11070u, uint8_t(m.L8(D + 11070u) & 247u));
  }
  X.ring = cur;
}

// #13  generated 9714-9797 (entry loc_82CB2CE4).  Only when stk436 == 0 && stk136 == 0.
// 9 words (0xC0003B00,256,0xC0102B00,0,15 + the 60-byte copy from 0x82082A60) then 0x00012180,0x00010002,0.
// No D+48 store; the cursor comes from and returns to X.ring.
template <class M>
void ShaderUpload(M& m, const Frame&, Locals& L, LocalsExt& X) {
  if (L.callerFlag != 0 || L.gateB != 0) return;
  uint32_t cur = X.ring;
  m.S32(cur + 4u, 0xC0003B00u);
  m.S32(cur + 8u, 256u);
  m.S32(cur + 12u, 0xC0102B00u);
  m.S32(cur + 16u, 0u);
  m.S32(cur + 20u, 15u);
  cur += 20u;
  m.Call(0x82C7C8F0u, 0x82CB2D44u, cur + 4u, kMicrocode, 60u, 0, 0, 0, 0, 0);  // memcpy(dest, src, 60)
  cur += 60u;
  m.S32(cur + 4u, 0x00012180u);
  m.S32(cur + 8u, 0x00010002u);
  m.S32(cur + 12u, 0u);
  cur += 12u;
  X.ring = cur;
}

// #14  generated 9798-9888 (entry loc_82CB2D68).  0x00002208,6,0x00002200,0; then, only when r26 != 0,
// D+11069 |= 4, the state code r30 = (rotl(rotl(w70,30)&8 | w70&~8, 29) & 7) & ~2 | (D+10560 & ~15) and
// D+16 |= 0x20000.  Then D+48 = cursor, the optional 82CB5AE8 flush and the state encoder 82CC0988(D,256,r30).
template <class M>
void CommitState(M& m, const Frame&, Locals& L, LocalsExt& X) {
  const uint32_t D = L.dev;
  uint32_t cur = X.ring;
  m.S32(cur + 4u, 0x00002208u);
  m.S32(cur + 8u, 6u);
  m.S32(cur + 12u, 0x00002200u);
  m.S32(cur + 16u, 0u);
  cur += 16u;
  uint32_t stateCode = 0;  // r30; r19 (the row divisor) dies here
  if (L.flags & 0x200u) {  // r26 = flags & 0x200 since 9007 (the scale-hook test)
    m.S8(D + 11069u, uint8_t(m.L8(D + 11069u) | 4u));
    const uint32_t w70 = uint32_t(m.L8(D + 11070u));
    const uint32_t w60 = m.L32(D + 10560u);
    uint32_t r8 = Rotl32And(w70, 30, 0x8u) | (w70 & 0xFFFFFFF7u);  // rlwimi r8,r10,30,28,28
    r8 = Rotl32And(r8, 29, 0x7u);                                 // rlwinm r8,r8,29,29,31
    r8 &= 0xFFFFFFFDu;                                            // rlwinm r8,r8,0,31,29
    stateCode = r8 | (w60 & 0xFFFFFFF0u);                         // rlwinm r10,r7,0,0,27
    m.S64(D + kDev16, m.L64(D + kDev16) | 0x20000u);              // oris r9,r9,2
  }
  m.S32(D + kRingCur, cur);
  X.ring = cur;
  if (L.gateB == 0 || (uint32_t(m.L8(D + 11072u)) & 0xFFFFFF80u) != 0)
    m.Call(0x82CB5AE8u, 0x82CB2DF0u, D, 0, 0, 0, 0, 0, 0, 0);
  m.Call(0x82CC0988u, 0x82CB2E00u, D, 256u, stateCode, 0, 0, 0, 0, 0);
}

// #15  generated 9889-10000 (entry after the encoder).  Skipped entirely when stk136 != 0.
// 14 ring words, then D+48 = cursor.
template <class M>
void RegRun14(M& m, const Frame&, Locals& L, LocalsExt& X) {
  if (L.gateB != 0) return;
  const uint32_t D = L.dev;
  uint32_t cur = RingMakeSpace(m, D, 0x82CB2E20u);
  static constexpr uint32_t kWords[14] = {0x00022100u, 0x0000FFFFu, 0u, 0u, 8851u, 0u, 0x00022204u,
                                          0x00010000u, 0x00010000u, 768u, 8978u, 0x0000FFFFu, 8205u, 0u};
  for (uint32_t k = 0; k < 14u; ++k) m.S32(cur + 4u * (k + 1u), kWords[k]);
  cur += 56u;
  m.S32(D + kRingCur, cur);
  X.ring = cur;
}

// #16  generated 10001-10073 (entry loc_82CB2EA4).  Only when stk144 == 0.  Device stores only, written in
// the body's interleaved order: D+16 bits 41/40/39, D+24 bit 35, D+16 |= 128, |= 64, |= 32, D+32 |= 0x80000,
// then D+16 bit 44 (the rldicr for bit 44 is computed early but stored last).
template <class M>
void BitmapStamps(M& m, const Frame&, Locals& L, LocalsExt&) {
  if (L.gateA != 0) return;
  const uint32_t D = L.dev;
  uint64_t w = m.L64(D + kDev16);
  w |= Rotl64(1u, 41);
  m.S64(D + kDev16, w);
  w |= Rotl64(1u, 40);
  m.S64(D + kDev16, w);
  w |= Rotl64(1u, 39);
  m.S64(D + kDev16, w);
  m.S64(D + kDev24, m.L64(D + kDev24) | Rotl64(1u, 35));
  w = m.L64(D + kDev16);
  w |= 128u;
  m.S64(D + kDev16, w);
  w |= 64u;
  m.S64(D + kDev16, w);
  w |= 32u;
  m.S64(D + kDev16, w);
  m.S64(D + kDev32, m.L64(D + kDev32) | 0x80000u);
  w = m.L64(D + kDev16);
  m.S64(D + kDev16, w | Rotl64(1u, 44));
}

// #17  generated 10074-10160 (entry loc_82CB2F34).  Only when r14 == 3 and stk148 != 4.
// slot = (stk148 != 0) ? stk148 + 1 : 0; word = D[10372 + 4*slot]; if (word & 0xF0000) == 0x10000:
// reserve, write 0x2001+slot and ((word & 0x3FFFFFF) & 0xFFF00FFF), set one bit of the D+16 word selected by
// ((0x20107+slot) >> 16) * 8 (bit 56-slot), store the cursor.
template <class M>
void SlotAddress(M& m, const Frame&, Locals& L, LocalsExt& X) {
  if (L.resClass != 3u || L.target == 4u) return;
  const uint32_t D = L.dev;
  const uint32_t slot = (L.target != 0) ? L.target + 1u : 0u;
  const uint32_t w = m.L32(D + (((slot + 2593u) << 2) & 0xFFFFFFFCu));  // lwzx r29,r11,r31
  if ((w & 0xF0000u) != 0x10000u) return;
  uint32_t cur = RingMakeSpace(m, D, 0x82CB2F8Cu);
  m.S32(cur + 4u, slot + 8193u);                    // addi r11,r30,8193
  m.S32(cur + 8u, (w & 0x3FFFFFFu) & 0xFFF00FFFu);  // clrlwi r8,r29,6 ; rlwinm 0,20,11
  cur += 8u;
  const uint32_t t = slot + 0x20107u;          // addis r10,r30,2 ; addi r10,r10,263
  const uint32_t wordOff = Rotl32And(uint32_t(int32_t(t) >> 16), 3, 0xFFFFFFF8u);
  const uint32_t bitIdx = t & 0xFFu;           // clrldi r10,r10,56
  const uint64_t bit = (bitIdx & 0x40u) ? 0u : (0x8000000000000000ull >> (bitIdx & 0x7Fu));  // srd r10,r9,r10
  const uint32_t a = D + wordOff;
  m.S64(a, m.L64(a) | bit);                    // stdx r10,r11,r31
  m.S32(D + kRingCur, cur);
  X.ring = cur;
}


// ===== CODE-C (Hooks, #18-#25) =====



// Hooks  generated 9005-9041.  The plan's split has no row for these two.
// flags & 0x200 -> 82CB1A50(D, src, stk492, f1 = the double argument f31)
// flags & 0x100 -> 82CB1FE8(D, src, rowBytes & 7, v1 = the 16 bytes at factorPtr)
// Both callees are KEEP-GUEST, so this needs the float/double and vector ABI seam of the policy:
//   M::CallF(addr, lr, r3..r10, f1bits)  and  M::CallV(addr, lr, r3..r10, v1 quad).  See NOTES.md.
template <class M>
void Hooks(M& m, const Frame&, Locals& L) {
  if (L.flags & 0x200u)
    m.CallF(0x82CB1A50u, 0x82CB2834u, L.dev, L.src, 0u, L.hookArg, 0u, 0u, 0u, 0u, L.factorBits);  // r6 = stk492 (9010)
  if (L.flags & 0x100u)
    m.CallV(0x82CB1FE8u, 0x82CB2850u, L.dev, L.src, L.rowBytes & 7u, 0u, 0u, 0u, 0u, 0u, L.factorPtr);
}

// #19  generated 9042-9116.  Round the RECT to 8, compare with the packed scissor at D+10436/D+10440
// (two 15-bit signed fields each) and emit the scissor packets through the guest writer when the rect
// leaves that box.  r22 (= Locals.bias) becomes 0/1: "rect changed", which #22 then tests.
template <class M>
void ScissorGate(M& m, const Frame&, Locals& L, LocalsExt& E) {
  const uint32_t D = L.dev;
  L.bias = 0;  // li r22,0
  const uint32_t w0 = m.L32(D + 10436u), w1 = m.L32(D + 10440u);
  E.rgL = m.L32(L.rect + 0u);                                   // r27
  E.rgT = m.L32(L.rect + 4u);                                  // r25
  E.rgR = (m.L32(L.rect + 8u) + 7u) & 0xFFFFFFF8u;              // r24 = (right + 7) & ~7
  E.rgB = (m.L32(L.rect + 12u) + 7u) & 0xFFFFFFF8u;             // r23 = (bottom + 7) & ~7
  const int32_t x0 = Sar((w0 << 17) & 0xFFFE0000u, 17);  // rlwinm 17,0,14 ; srawi 17
  const int32_t y0 = Sar((w0 << 1) & 0xFFFFFFFEu, 17);   // rlwinm 1,0,30 ; srawi 17
  const int32_t x1 = Sar((w1 << 17) & 0xFFFE0000u, 17);
  const int32_t y1 = Sar((w1 << 1) & 0xFFFFFFFEu, 17);
  if (!(int32_t(E.rgL) < x0) && !(int32_t(E.rgT) < y0) && !(int32_t(E.rgR) > x1) &&
      !(int32_t(E.rgB) > y1))
    return;  // inside: 0x82cb28d8
  m.Call(0x82CAC5C0u, 0x82CB28D4u, D, E.rgL, E.rgT, E.rgR, E.rgB, 0u, 0u, 0u);
  L.bias = 1;  // li r22,1
}

// #18  generated 9117-9129.  Returns true when the body takes the caller-flag fast path (jump to
// 0x82cb2a90), i.e. #8 EmitGroups must be skipped; otherwise flushes the pending dirty word through
// the state encoder 82CC0988 and falls into #8.  (9130-9145 is that jump target's own code, #10.)
template <class M>
bool PreFlush(M& m, const Frame&, Locals& L) {
  if (L.callerFlag != 0) return true;
  const uint64_t w = m.L64(L.dev + kDev16);
  if (w == 0) return false;  // beq 0x82cb2a04: the groups' own zero tests reproduce the skip
  if ((m.L64(L.dev + 40u) & w) != 0) {
    const uint64_t r = m.Call(0x82CC0988u, 0x82CB290Cu, L.dev, w, m.L32(L.dev + 10560u), 0u, 0u, 0u, 0u, 0u);
    m.S64(L.dev + kDev16, r);
  }
  return false;
}

// #20  generated 10161-10333.  Six-float scratch rect: reserve, 82CB9588(D, 6, 4) takes 6 dwords of ring
// space for a scratch buffer, the six floats go there, then the 11-word copy packet goes to the ring.
// Returns with E.abort set when the allocator returns 0: the guest then rolls D+48 back and jumps to
// the epilogue (0x82cb3468), so #21-#25 must not run.
template <class M>
void ScratchRect(M& m, const Frame&, Locals& L, LocalsExt& E) {
  const uint32_t D = L.dev;
  if (L.callerFlag != 0) {  // 0x82cb2fdc: mr r29,r21 -> the copy packet's source is the caller flag
    E.packPtr = L.callerFlag;
    return;
  }
  uint32_t cur = m.L32(D + 48u);
  if (cur > m.L32(D + 56u)) cur = uint32_t(m.Call(0x82CBA798u, 0x82CB2FF0u, D, 0u, 0u, 0u, 0u, 0u, 0u, 0u));
  const uint32_t p = uint32_t(m.Call(0x82CB9588u, 0x82CB3004u, D, 6u, 4u, 0u, 0u, 0u, 0u, 0u));
  if (p == 0) {              // 0x82cb3008
    m.S32(D + 48u, cur);     // stw r30,48(r31)
    E.abort = true;
    return;
  }
  const uint32_t tiled = TiledAddr(p, 0x1FFFFFFFu) | 3u;  // 10200-10214, then ori 3
  // Bias constant selected by D+10688 bit 0 (clrlwi. r9,r9,31 ; bne 0x82cb3040).
  const float k = FBits(m.L32((m.L32(D + 10688u) & 1u) ? kFloatAltAddr : kFloatHalfAddr));
  const float x0 = I2F(int32_t(E.rgL)) - k;                 // f13 = frsp(extsw r27) - f0
  const float y0 = I2F(int32_t(E.rgT)) - k;                 // f0  = frsp(extsw r25) - f0
  const float x1 = I2F(int32_t(E.rgR - E.rgL)) + x0;         // f11 = frsp(extsw(r24-r27)) ; fadds
  const float y1 = I2F(int32_t(E.rgB - E.rgT)) + y0;         // f10 = frsp(extsw(r23-r25)) ; fadds
  m.S32(p + 0u, BitsF(x0));   // the six stfs, in this order
  m.S32(p + 4u, BitsF(y0));
  m.S32(p + 8u, BitsF(x1));
  m.S32(p + 12u, BitsF(y0));
  m.S32(p + 16u, BitsF(x1));  // f13 already holds x1 at 10314
  m.S32(p + 20u, BitsF(y1));
  const uint32_t words[11] = {0x00054800u, tiled, 0x1000001Au, 0u, 0u, 0u, 0u,
                               0x00025000u, 0u, 0u, 0u};
  for (uint32_t i = 0; i < 11u; ++i) { cur += 4u; m.S32(cur, words[i]); }
  m.S32(D + 48u, cur);  // stw r30,48(r31)
  E.cursor = cur;
  E.packPtr = 3u;       // li r29,3
}

// #21  generated 10334-10442 (the second half of the copy packet).  D[13092] | 0xC0000000 | 0x3600,
// 0x0088 | (r29 << 16), r28, 6, 0x2007, D[10396]; D+48 = cursor.
template <class M>
void CopyPacket(M& m, const Frame&, Locals& L, LocalsExt& E) {
  if (E.abort) return;
  uint32_t p = m.L32(L.dev + 48u);
  if (p > m.L32(L.dev + 56u)) p = uint32_t(m.Call(0x82CBA798u, 0x82CB3154u, L.dev, 0u, 0u, 0u, 0u, 0u, 0u, 0u));
  const uint32_t words[6] = {m.L32(L.dev + 13092u) | 0xC0000000u | 0x3600u,
                              Rotl32And(E.packPtr, 16, 0xFFFF0000u) | 0x0088u,  // rlwimi r10,r29,16,0,15
                              kSetConstWord, 6u, 0x00002007u, m.L32(L.dev + 10396u)};
  for (uint32_t i = 0; i < 6u; ++i) { p += 4u; m.S32(p, words[i]); }
  m.S32(L.dev + 48u, p);
}

// #22  generated 10443-10525.  Scissor packets for the rect that #19 found outside the box
// (r22 != 0), the D+11036 stamp into rec+8, and three dirty stamps when the caller flag is clear.
template <class M>
void ScissorAndStamps(M& m, const Frame&, Locals& L, LocalsExt& E) {
  const uint32_t D = L.dev;
  if (L.bias != 0) {  // cmpwi cr6,r22,0
    const uint32_t w0 = m.L32(D + 10436u), w1 = m.L32(D + 10440u);
    m.Call(0x82CAC5C0u, 0x82CB31C8u, D,
           uint32_t(Sar((w0 << 17) & 0xFFFE0000u, 17)), uint32_t(Sar((w0 << 1) & 0xFFFFFFFEu, 17)),
           uint32_t(Sar((w1 << 17) & 0xFFFE0000u, 17)), uint32_t(Sar((w1 << 1) & 0xFFFFFFFEu, 17)),
           0u, 0u, 0u);
  }
  m.S32(L.rec + 8u, m.L32(D + 11036u));  // stw r11,8(r15)
  if (L.callerFlag == 0) {
    m.S64(D + kDev16, m.L64(D + kDev16) | 0x80000u);   // oris 8
    m.S64(D + kDev24, m.L64(D + kDev24) | 0x80000000u);  // oris 32768 on the upper word
    m.S64(D + kDev16, m.L64(D + kDev16) | 0x18000u);   // oris 1 | ori 32768
  }
  (void)E;
}

// #23  generated 10526-10651.  Descriptor: 82CAA430 fills stk144/148/156 from rec+28, then 82CAA740
// formats them with eight register and six stack args (the only reason this phase needs the guest
// frame).  Afterwards the dest-rect word and the tiled address are picked from stk136/stk160 by mode.
template <class M>
void Descriptor(M& m, const Frame& f, Locals& L, LocalsExt& E) {
  const uint32_t D = L.dev;
  m.S64(D + kDev16, m.L64(D + kDev16) | 8u);    // three separate cumulative stores
  m.S64(D + kDev16, m.L64(D + kDev16) | 0x800u);
  m.S64(D + kDev16, m.L64(D + kDev16) | 0x100u);
  m.Call(0x82CAA430u, 0x82CB3230u, L.rec + 28u, Stk(f, 144), Stk(f, 148), Stk(f, 156), 0u, 0u, 0u, 0u);
  m.S32(Stk(f, 124), Stk(f, 136));  // stack args of 82CAA740, in the body's store order
  m.S32(Stk(f, 116), Stk(f, 160));
  m.S32(Stk(f, 108), Stk(f, 192));
  const uint32_t w28 = m.L32(L.rec + 28u), w32 = m.L32(L.rec + 32u);
  const uint32_t w40 = m.L32(L.rec + 40u), w44 = m.L32(L.rec + 44u), w48 = m.L32(L.rec + 48u);
  const uint32_t r11 = (w28 >> 22) & 0x1FFu;                  // rlwinm r11,r8,10,23,31
  const uint32_t r6v = (w40 << 13) & 0xFFFFE000u;             // rlwinm r6,r10,13,0,18
  m.S32(Stk(f, 92), uint32_t(Sar(r6v, 26)));                  // srawi r7,r6,26
  m.S32(Stk(f, 100), (r11 * m.L32(Stk(f, 152)) << 5) & 0xFFFFFFE0u);  // mullw + rlwinm 5,0,26
  m.S32(Stk(f, 84), (w40 >> 31) & 1u);                        // rlwinm r6,r10,1,31,31
  // r6 = ((rec[44] >> 6) & 0xF) + 1: rlwinm r11,r7,26,28,31 then addi r6,r11,1 (r11 was reused).
  m.Call(0x82CAA740u, 0x82CB32ACu, m.L32(Stk(f, 144)), m.L32(Stk(f, 148)), m.L32(Stk(f, 156)),
         ((w44 >> 6) & 0xFu) + 1u, w32 & 0x3Fu, (w48 >> 9) & 3u, (w28 >> 31) & 1u, (w48 >> 11) & 1u);
  if (L.mode != 0 && (w48 & 0xFFFFF000u) != 0) {
    E.destWord = m.L32(Stk(f, 136));
    E.tiledAddr = TiledAddr(w48, 0x1FFFF000u);  // rlwinm r11,r11,0,3,19 as generated (see NOTES)
  } else {
    const uint32_t y = w32 & 0xFFFFF000u;
    E.destWord = m.L32(Stk(f, 160));
    E.tiledAddr = TiledAddr(y, 0x1FFFFFFFu);
  }
}

// #24  generated 10652-10754.  The 13-word main packet: only when M[0x83947020] != 0 and stk144
// (Locals.gateA, reloaded into r26 at 9988) == 0.  Both paths share the reserve at 0x82cb3300.
template <class M>
void MainPacket(M& m, const Frame&, Locals& L, LocalsExt& E) {
  const uint32_t D = L.dev;
  uint32_t p = m.L32(D + 48u);
  if (p > m.L32(D + 56u)) p = uint32_t(m.Call(0x82CBA798u, 0x82CB3318u, D, 0u, 0u, 0u, 0u, 0u, 0u, 0u));
  E.cursor = p;
  E.globalOn = m.L32(kGlobalPacket);
  if (E.globalOn == 0 || L.gateA != 0) return;
  const uint32_t words[13] = {2609u, 0x03000000u, 0x00010A2Fu,  /* 65536 | 2607 (decimal) */
                               (E.destWord + 8191u) & 0xFFFFF000u, E.tiledAddr & 0xFFFFF000u,
                               0xC0043C00u, 3u, 2609u, 0u, 0x80000000u, 8u, 1480u, 0x00020000u};
  for (uint32_t i = 0; i < 13u; ++i) { p += 4u; m.S32(p, words[i]); }
  E.cursor = p;
}

// #25  generated 10755-10868.  0x2318/0x231B packet, the two rec+8/+12 stamps, the 11072/32 fallback
// stamps (only when the main packet was suppressed), and the optional deferred-list entry.
template <class M>
void FallbackAndList(M& m, const Frame& f, Locals& L, LocalsExt& E) {
  const uint32_t D = L.dev;
  if (E.globalOn == 0 && L.gateA == 0) {  // loc_82CB33B0
    m.S8(D + 11072u, uint8_t(m.L8(D + 11072u) | 0xC0u));
    m.S64(D + kDev32, m.L64(D + kDev32) | 0xC000000000000000ull);  // rldicr r12,3,62,1
  }
  m.S32(D + 10776u, 0u);  // stw r19,10776 / stw r19,10788
  m.S32(D + 10788u, 0u);
  uint32_t p = E.cursor;
  const uint32_t words[4] = {8984u, 0u, 8987u, 0u};
  for (uint32_t i = 0; i < 4u; ++i) { p += 4u; m.S32(p, words[i]); }
  m.S32(D + 48u, p);
  const uint32_t a11036 = m.L32(D + 11036u);
  if (a11036 != 0) {  // stw r11,8(r15) / stw r11,12(r15)
    m.S32(L.rec + 8u, a11036);
    m.S32(L.rec + 12u, a11036);
    return;
  }
  if ((m.L32(D + 11040u) & m.L32(L.rec + 0u)) == 0) return;  // and. r11,r11,r10
  uint32_t r3 = m.L32(D + 13928u);
  if (!(r3 < m.L32(D + 13932u)))  // cmplw/blt else 82CBBD68
    r3 = uint32_t(m.Call(0x82CBBD68u, 0x82CB3444u, D, 0u, 0u, 0u, 0u, 0u, 0u, 0u));
  m.S32(Stk(f, 196), L.geomArg);  // stw r20,196(r1); r20 is -1 since 8887
  uint32_t hdr = Rotl32And(L.rec, 30, 0x3FFFFFFFu) | (m.L32(Stk(f, 192)) & 0xC0000000u);
  hdr |= 0x40000000u;
  m.S32(Stk(f, 192), hdr);
  m.S64(r3, m.L64(Stk(f, 192)));  // ld r10,192(r1) ; std r10,0(r3)
  m.S32(D + 13928u, r3 + 8u);
}


// ===== Composition (Claude): the generated body's control flow, generated/wwe2k14_recomp.85.cpp 8313-10872 =====
// Entry: r3 D, r4 flags, r5 rect, r6 payload, r7 caller flag, r8 record, r9 source, r10 mode, f1, and the caller's
// stack arguments at caller_sp + 84 / 92 / 108 / 116 (the body's stk468 / 476 / 492 / 500 after its 384-byte frame).
template <class M>
void Resolve(M& m, uint32_t caller_sp, uint32_t D, uint32_t flags, uint32_t rect, uint32_t payload, uint32_t caller_flag,
             uint32_t rec, uint32_t src, uint32_t mode, double f1) {
  const Frame f{caller_sp - 384u};
  Locals L{};
  LocalsExt E{};
  L.dev = D, L.flags = flags, L.rect = rect, L.payload = payload, L.callerFlag = caller_flag, L.rec = rec, L.src = src;
  L.mode = mode;
  uint64_t fb;
  std::memcpy(&fb, &f1, 8);
  L.factorBits = fb;  // CallF takes the double bits (see Hooks)
  L.geomArg = m.L32(caller_sp + 84u);
  L.factorPtr = m.L32(caller_sp + 92u);
  L.hookArg = m.L32(caller_sp + 108u);
  L.srcAlt = m.L32(caller_sp + 116u);
  Gates(m, f, L);
  ClassBits(m, f, L);
  if (L.factorPtr == 0) {  // 8442-8461: 16-byte default factor at 0x82020BC0 copied to stk208
    for (uint32_t k = 0; k < 16u; k += 4) m.S32(Stk(f, 208) + k, m.L32(0x82020BC0u + k));
    L.factorPtr = Stk(f, 208);
  }
  SampleDims(m, f, L);
  RowsAndDivisor(m, f, L);
  DefaultRectAndSrc(m, f, L);
  OffsetAndCode(m, f, L);
  PackCopyRegs(m, f, L);
  ColourWords(m, f, L);
  Hooks(m, f, L);
  ScissorGate(m, f, L, E);
  E.fastPath = PreFlush(m, f, L);
  if (E.fastPath) FastPath(m, f, L, E);  // loc_82CB2A90, falls into loc_82CB2ACC
  else EmitGroups(m, f, L);             // ends with goto loc_82CB2ACC
  FlushDecision(m, f, L);
  StateBlock(m, f, L, E);
  TailScissor(m, f, L, E);
  ShaderUpload(m, f, L, E);
  CommitState(m, f, L, E);
  RegRun14(m, f, L, E);
  BitmapStamps(m, f, L, E);
  SlotAddress(m, f, L, E);
  ScratchRect(m, f, L, E);
  if (E.abort) return;  // loc_82CB3468 epilogue
  CopyPacket(m, f, L, E);
  ScissorAndStamps(m, f, L, E);
  Descriptor(m, f, L, E);
  MainPacket(m, f, L, E);
  FallbackAndList(m, f, L, E);
}

}  // namespace wwe2k14::d3d::native_resolve
