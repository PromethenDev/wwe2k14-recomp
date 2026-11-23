// F1 (docs/PLAN-FULLY-NATIVE.md): pure logic of the native D3D setter bodies, host-testable.
// SetVSConstantF sub_82CB3F48 / SetPSConstantF sub_82CB4020 (generated/wwe2k14_recomp.85.cpp:12489, 12622):
// r3 device, r4 first register, r5 source (may be unaligned), r6 vec4 count, r7 dirty bits.
// Rows go to (r3 + (r4 + 120 | 376) * 16) & ~15 in order; then the big-endian 64-bit word at dev+0 (VS) / dev+8 (PS) |= r7.
// Guest bytes are stored raw, so each row is a plain 16-byte copy (lvlx/lvrx/stvx byte swaps cancel out).
#pragma once
#include <cstdint>
#include <cstring>

namespace wwe2k14::d3d::native_setters {

constexpr uint32_t kMaxRows = 256;
constexpr uint32_t ConstBaseRow(bool ps) { return ps ? 376u : 120u; }
constexpr uint32_t DirtyOffset(bool ps) { return ps ? 8u : 0u; }
constexpr uint32_t ConstDest(uint32_t dev, uint32_t first, bool ps) {
  return (dev + ((first + ConstBaseRow(ps)) << 4)) & ~0xFu;
}
// The guest computes the row offset first and aligns it, then adds dev: same result for the 16-byte aligned device;
// callers keep the native path only for such a device.
constexpr bool ConstArgsOk(uint32_t dev, uint32_t first, uint32_t count) {
  return (dev & 0xFu) == 0 && count <= kMaxRows && first <= kMaxRows && first + count <= kMaxRows;
}
inline uint64_t Bswap64(uint64_t v) { return __builtin_bswap64(v); }
inline void OrDirtyBe(uint8_t* word, uint64_t bits) {
  uint64_t v;
  std::memcpy(&v, word, 8);
  v = Bswap64(Bswap64(v) | bits);
  std::memcpy(word, &v, 8);
}
// In-run A/B rule shared with cost-ab.py: even blocks old path, odd blocks new path. block 0 = always new.
constexpr bool AbNewArm(uint32_t frame, uint32_t block) { return block == 0 || ((frame / block) & 1u) != 0; }

// F1 step 3. SetStreamSource sub_82CAECB8 (generated 85:4): vertex fetch words for a buffer whose word +24 is address and +28
// size, with r6 = offset. Guest order: address word at dev + (17 - stream + 222) * 8, size word at dev + (17 - stream) * 8 + 1780.
constexpr uint32_t StreamFetchAddrOffset(uint32_t stream) { return ((17u - stream + 222u) << 3) & ~7u; }
constexpr uint32_t StreamFetchSizeOffset(uint32_t stream) { return (((17u - stream) << 3) & ~7u) + 1780u; }
constexpr uint32_t StreamFetchAddr(uint32_t buffer_addr, uint32_t offset) {
  const uint32_t a = buffer_addr + offset;
  return (((a >> 20) + 512u) & 0x1000u) + (a & 0x1FFFFFFFu);
}
constexpr uint32_t kDirtyDecl = 0x80000u;  // the bit SetVertexDecl and a stride change OR into dev+16
constexpr uint32_t kOffDecl = 12240, kOffIndices = 12812, kOffStreamSlots = 12836, kOffStride = 12904, kOffDeclStride = 12264;
constexpr uint32_t kOffFenceNow = 11036, kOffFenceMask = 11040;
// F1 step 4. SetTexture sub_82CAC430 (generated 84:67956): r4 stage, r5 texture, r6 dirty bits. Slot = dev + (stage + 48) * 24: six
// big-endian fetch words merged from the texture object (+28..+48) and the slot's current bits. Pure so the host test can
// compare it with a literal model of the generated sequence.
struct TexIn { uint32_t t28, t32, t36, t40, t44, t48, w0, w4, w12, w16, w20; uint8_t min_clamp, max_clamp; };
struct TexOut { uint32_t w0, w4, w8, w12, w16, w20; };
constexpr TexOut MergeTextureFetch(const TexIn& i) {
  const uint32_t mip_addr = (((i.t48 >> 20) + 512u) & 0x1000u) + (i.t48 & 0x1FFFFE00u);
  const uint32_t base_addr = (((i.t32 >> 20) + 512u) & 0x1000u) + (i.t32 & 0x1FFFFFFFu);
  TexOut o{};
  o.w0 = (i.w0 & 0x3FFC00u) | (i.t28 & 0xFFC003FFu);
  o.w4 = (i.w4 & 0x800u) | (base_addr & 0xFFFFF7FFu);
  o.w8 = i.t36;
  o.w12 = (i.w12 & 0x7FF80000u) | (i.t40 & 0x8007FFFFu);
  o.w20 = (i.w20 & 0x1FFu) | (mip_addr & 0xFFFFFE00u);
  uint32_t lo = (i.t44 >> 2) & 0xFu;
  if (lo < i.min_clamp) lo = i.min_clamp;
  uint32_t hi = (i.t44 >> 6) & 0xFu;
  if (hi > i.max_clamp) hi = i.max_clamp;
  uint32_t w16 = (i.w16 & 0xFFFFFC03u) | (i.t44 & 0x3FCu);
  w16 = ((lo << 2) & 0x3Cu) | (w16 & 0xFFFFFFC3u);
  o.w16 = ((hi << 6) & 0x3C0u) | (w16 & 0xFFFFFC3Fu);
  return o;
}
constexpr uint32_t kTextureStages = 26;  // the two per-stage clamp byte arrays are 26 apart (12356, 12382); larger stages run the original
constexpr uint32_t kOffTextureSlots = 12920, kOffMinClamp = 12356, kOffMaxClamp = 12382;

// Device bytes the CHECK compares whole: every field these setters touch lies below this.
constexpr uint32_t kDeviceCheckBytes = 14336;

}  // namespace wwe2k14::d3d::native_setters
