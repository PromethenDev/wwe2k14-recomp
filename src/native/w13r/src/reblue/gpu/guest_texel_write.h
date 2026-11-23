// Per-texel helpers for writing a read-back render target into the game's (tiled, big-endian)
// texture memory without two calls into the runtime DLL per texel (texture_util::GetTiledOffset2D
// and texture_conversion::CopySwapBlock). WWE '13: a 1024x512 skin composite read back every 301
// frames spent ~10 ms of a 25 ms hitch in those calls (PIX nx1, 2026-11-20).
//
// Both functions give the same bytes as the SDK functions they replace (host test
// tests/guest_texel_write_test.cpp compares them with verbatim copies). TiledOffset2D is the SDK's
// formula (rexglue-sdk src/graphics/pipeline/texture/util.cpp, after UModel UnTexture.cpp).
// CopySwapSmall covers what CopySwapBlock does for a 2- or 4-byte block, except Endian::k16in32:
// CopySwapBlock passes the byte length there as a count of 32-bit words and so writes 4x the
// block; callers keep the SDK call for that case.
#pragma once

#include <cstdint>
#include <cstring>

namespace bd::gpu::texel_write {

inline int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31u) & ~31u;  // rex::align(pitch, xenos::kTextureTileWidthHeight = 32)
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// endian: xenos::Endian value (0 none, 1 8in16, 2 8in32, 3 16in32). length: 2 or 4.
// Returns false when the caller must use CopySwapBlock (k16in32, other lengths).
inline bool CopySwapSmall(uint32_t endian, uint8_t *out, const uint8_t *in, uint32_t length) {
  if (length != 2 && length != 4)
    return false;
  switch (endian) {
    case 0:
      std::memcpy(out, in, length);
      return true;
    case 1:  // copy_and_swap_16_unaligned(out, in, length / 2)
      out[0] = in[1], out[1] = in[0];
      if (length == 4)
        out[2] = in[3], out[3] = in[2];
      return true;
    case 2:  // copy_and_swap_32_unaligned(out, in, length / 4): nothing for a 2-byte block
      if (length == 4)
        out[0] = in[3], out[1] = in[2], out[2] = in[1], out[3] = in[0];
      return true;
    default:  // k16in32 (see above); CopySwapBlock's default case is memcpy, values > 3 too
      if (endian > 3) {
        std::memcpy(out, in, length);
        return true;
      }
      return false;
  }
}

}  // namespace bd::gpu::texel_write
