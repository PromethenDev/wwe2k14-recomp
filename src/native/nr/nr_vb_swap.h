// Guest vertex bytes -> host byte order. Kept standalone so the upload-ring path can be host-tested.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nr::vbswap {

inline void CopySwapDwords(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t endian) {
  const size_t n = bytes / 4;
  switch (endian) {
    case 1:
      for (size_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = (v & 0x00FF00FFu) << 8 | (v & 0xFF00FF00u) >> 8;
        std::memcpy(dst + i * 4, &v, 4);
      }
      break;
    case 2:
      for (size_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = __builtin_bswap32(v);
        std::memcpy(dst + i * 4, &v, 4);
      }
      break;
    case 3:
      for (size_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = v << 16 | v >> 16;
        std::memcpy(dst + i * 4, &v, 4);
      }
      break;
    default:
      std::memcpy(dst, src, n * 4);
      break;
  }
  if (bytes > n * 4) std::memcpy(dst + n * 4, src + n * 4, bytes - n * 4);
}

inline void CopySwapDwordsToSpan(uint8_t* span, const uint8_t* src, size_t bytes, uint32_t endian) {
  const size_t n = bytes / 4;
  switch (endian) {
    case 1:
      for (size_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = (v & 0x00FF00FFu) << 8 | (v & 0xFF00FF00u) >> 8;
        std::memcpy(span + i * 4, &v, 4);
      }
      break;
    case 2:
      for (size_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = __builtin_bswap32(v);
        std::memcpy(span + i * 4, &v, 4);
      }
      break;
    case 3:
      for (size_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = v << 16 | v >> 16;
        std::memcpy(span + i * 4, &v, 4);
      }
      break;
    default:
      std::memcpy(span, src, n * 4);
      break;
  }
  if (bytes > n * 4) std::memcpy(span + n * 4, src + n * 4, bytes - n * 4);
}

}  // namespace nr::vbswap
