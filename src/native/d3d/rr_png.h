// RR2: minimal PNG writer (8-bit RGB, deflate "stored" blocks, no compression library). Header-only, no SDK; shared by
// the offline replay (rr_play.cpp) and host tools. Output is deterministic: identical pixels -> identical bytes.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wwe2k14::d3d::rr::png {

inline uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static const auto table = [] {
    std::vector<uint32_t> t(256);
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

inline void Put32(std::vector<uint8_t>* o, uint32_t v) {
  o->push_back(uint8_t(v >> 24)), o->push_back(uint8_t(v >> 16)), o->push_back(uint8_t(v >> 8)), o->push_back(uint8_t(v));
}

inline void Chunk(std::vector<uint8_t>* o, const char type[4], const std::vector<uint8_t>& data) {
  Put32(o, uint32_t(data.size()));
  const size_t start = o->size();
  o->insert(o->end(), type, type + 4);
  o->insert(o->end(), data.begin(), data.end());
  Put32(o, Crc32(o->data() + start, o->size() - start));
}

// Encodes w x h RGB8 pixels (tight rows) as a PNG file image.
inline std::vector<uint8_t> EncodeRgb(const uint8_t* rgb, uint32_t w, uint32_t h) {
  std::vector<uint8_t> raw;
  raw.reserve((size_t(w) * 3 + 1) * h);
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);  // filter: none
    raw.insert(raw.end(), rgb + size_t(y) * w * 3, rgb + size_t(y + 1) * w * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t off = 0; off < raw.size() || off == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - off);
    const bool last = off + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    off += n;
    if (last) break;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
  Put32(&z, (b << 16) | a);
  std::vector<uint8_t> o = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  Put32(&ihdr, w), Put32(&ihdr, h);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, RGB, deflate, adaptive filter set, no interlace
  Chunk(&o, "IHDR", ihdr);
  Chunk(&o, "IDAT", z);
  Chunk(&o, "IEND", {});
  return o;
}

inline bool WriteRgb(const std::string& path, const uint8_t* rgb, uint32_t w, uint32_t h) {
  const std::vector<uint8_t> f = EncodeRgb(rgb, w, h);
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  const bool ok = std::fwrite(f.data(), 1, f.size(), fp) == f.size();
  return std::fclose(fp) == 0 && ok;
}

}  // namespace wwe2k14::d3d::rr::png
