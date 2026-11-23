// RR2 host test: rr_png.h writes a PNG that a standard decoder reads back to the same pixels (checked by
// tools/rr/rr-selftest.sh with Pillow). Writes <out>/a.png (odd size, > 64 KB of rows: several stored blocks) + raw.
#include <cstdio>
#include <string>
#include <vector>

#include "native/d3d/rr_png.h"

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const std::string out = argv[1];
  const uint32_t w = 301, h = 257;
  std::vector<uint8_t> rgb(size_t(w) * h * 3);
  uint32_t x = 12345;
  for (auto& b : rgb) b = uint8_t((x = x * 1103515245u + 12345u) >> 16);
  if (!wwe2k14::d3d::rr::png::WriteRgb(out + "/a.png", rgb.data(), w, h)) return 1;
  FILE* f = std::fopen((out + "/a.raw").c_str(), "wb");
  if (!f) return 1;
  std::fwrite(rgb.data(), 1, rgb.size(), f);
  std::fclose(f);
  return 0;
}
