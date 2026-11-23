// Card DE-2M (piece 2, draw-packet removal): the ordering marker that replaces the DRAW packet of an indexed draw.
// DrawIndexedVertices 82CB52D0 ends its non-predicated path with a 7-dword suffix written at the reserved cursor c:
// c+4 0x00002102 (VGT_INDX_OFFSET), c+8 base vertex, c+12 DRAW_INDX header 0xC0032201 (type 3, 4 payload words,
// predicate bit 1), c+16..c+28 its payload; dev+48 = c+28 on return (generated/wwe2k14_recomp.85.cpp:15843-15858,
// 15956-15983, 16113-16118). The marker keeps the footprint and the predicate bit (a bin-predicated-out marker is skipped
// exactly like the DRAW was) and carries the capture event's seq, so the CP gate needs no address table.
// Pure, header-only: encoded by capture on the game thread, decoded by the SDK NOP hook on the CP thread.
#pragma once

#include <cstdint>
#include <cstring>

namespace wwe2k14::d3d::marker {

inline constexpr uint32_t kDrawIndxHeader = 0xC0032201u;  // PM4_DRAW_INDX, count field 3, predicate 1
inline constexpr uint32_t kNopHeader = 0xC0031001u;       // PM4_NOP (0x10), count field 3, predicate 1
inline constexpr uint32_t kMagicA = 0x4E52444Du;          // 'NRDM'
inline constexpr uint32_t kMagicB = 0xA5C3F00Fu;          // seq check
inline constexpr uint32_t kMagicC = 0x4D4B5232u;          // 'MKR2'

inline uint32_t LoadBe(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline void StoreBe(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

// Writes header + 4 payload words at p (guest memory, big-endian).
inline void Encode(uint8_t* p, uint32_t seq) {
  StoreBe(p, kNopHeader);
  StoreBe(p + 4, kMagicA);
  StoreBe(p + 8, seq);
  StoreBe(p + 12, seq ^ kMagicB);
  StoreBe(p + 16, kMagicC);
}
// UP draws (BeginVertices 82CB4E10) write a 3-dword DRAW_INDX (count field 1: viz token + initiator); their marker is a
// 3-dword NOP: magic + seq.
inline constexpr uint32_t kDrawIndxUpHeader = 0xC0012201u;  // PM4_DRAW_INDX, count field 1, predicate 1
inline constexpr uint32_t kNop2Header = 0xC0011001u;        // PM4_NOP, count field 1, predicate 1
inline constexpr uint32_t kMagicU = 0x4E524455u;            // 'NRDU'
inline void EncodeUp(uint8_t* p, uint32_t seq) {
  StoreBe(p, kNop2Header);
  StoreBe(p + 4, kMagicU);
  StoreBe(p + 8, seq);
}
inline bool DecodeUp(const uint8_t* payload, uint32_t* seq) {
  if (LoadBe(payload) != kMagicU) return false;
  *seq = LoadBe(payload + 4);
  return true;
}
// Internal clear-helper draws (82CB7930) write a 2-dword DRAW_INDX_2 (count field 0: initiator only); their marker is a
// 2-dword NOP whose payload is tag 0xD2 + the low 24 bits of the seq (the CP side extends it from the newest seq).
inline constexpr uint32_t kDrawIndx2Header1 = 0xC0003601u;  // PM4_DRAW_INDX_2, count field 0, predicate 1
inline constexpr uint32_t kNop1Header = 0xC0001001u;        // PM4_NOP, count field 0, predicate 1
inline constexpr uint32_t kTag1 = 0xD2u;
inline void EncodeOne(uint8_t* p, uint32_t seq) {
  StoreBe(p, kNop1Header);
  StoreBe(p + 4, (kTag1 << 24) | (seq & 0xFFFFFFu));
}
// latest = a seq at or after the marker's own (the newest the producer allocated).
inline bool DecodeOne(const uint8_t* payload, uint32_t latest, uint32_t* seq) {
  const uint32_t w = LoadBe(payload);
  if ((w >> 24) != kTag1) return false;
  *seq = latest - ((latest - w) & 0xFFFFFFu);
  return true;
}
// payload = the 4 payload words (guest byte order). True and *seq when they form a marker.
inline bool Decode(const uint8_t* payload, uint32_t* seq) {
  if (LoadBe(payload) != kMagicA || LoadBe(payload + 12) != kMagicC) return false;
  const uint32_t s = LoadBe(payload + 4);
  if ((s ^ kMagicB) != LoadBe(payload + 8)) return false;
  *seq = s;
  return true;
}

}  // namespace wwe2k14::d3d::marker
