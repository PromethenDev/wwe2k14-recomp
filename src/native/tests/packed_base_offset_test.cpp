// Card S4-2 host test: level 0 of a packed-mips texture is read from its place in the mip tail (sh::PackedLevelOffset),
// checked against the SDK's own layout and offset code, plus a round trip of a tiled 256x16 DXT1 tail through the
// detile arithmetic. Build command: docs/tasks/S4-2-cage-report.md.
#include <cstdio>
#include <cstring>
#include <vector>

#include "nr_shared.h"

using namespace rex::graphics;
namespace sh = nr::shared;
static int g_fail = 0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void Off(uint32_t w, uint32_t h, xenos::TextureFormat f, bool packed, uint32_t level, uint32_t* x, uint32_t* y) {
  const auto l = texture_util::GetGuestTextureLayout(xenos::DataDimension::k2DOrStacked, (w + 31) / 32, w, h, 1, true, f,
                                                     packed, true, level);
  sh::PackedLevelOffset(l, w, h, f, level, x, y);
}

int main() {
  const auto dxt1 = xenos::TextureFormat::k_DXT1;
  const auto rgba = xenos::TextureFormat::k_8_8_8_8;
  uint32_t x, y;
  Off(256, 16, dxt1, true, 0, &x, &y); EXPECT(x == 0 && y == 4);    // the cage strip: base 16 texels = 4 blocks down
  Off(256, 16, dxt1, false, 0, &x, &y); EXPECT(x == 0 && y == 0);   // same size, no packed mips
  Off(16, 128, dxt1, true, 0, &x, &y); EXPECT(x == 4 && y == 0);    // tall strip: along x
  Off(16, 16, rgba, true, 0, &x, &y); EXPECT(x == 16 && y == 0);    // uncompressed: in texels
  Off(256, 256, dxt1, true, 0, &x, &y); EXPECT(x == 0 && y == 0);   // big texture: base not in the tail
  Off(256, 256, dxt1, true, 1, &x, &y); EXPECT(x == 0 && y == 0);   // and level 1 neither
  {  // levels > 0 keep the SDK offset ComputeLevelGeom always used
    uint32_t sx, sy, sz;
    texture_util::GetPackedMipOffset(256, 16, 1, dxt1, 2, sx, sy, sz);
    Off(256, 16, dxt1, true, 2, &x, &y); EXPECT(x == sx && y == sy);
  }
  // Round trip: tiled base at its packed place, read back with the offset as DetileLevelCpu does. The old read
  // (no offset) must see only padding here.
  std::vector<uint8_t> mem(1 << 16, 0);
  auto val = [](uint32_t bx, uint32_t by) { return uint8_t(1 + (bx + by * 64) % 251); };
  for (uint32_t by = 0; by < 4; ++by)
    for (uint32_t bx = 0; bx < 64; ++bx) std::memset(&mem[sh::TiledOffset2D(bx, by + 4, 64, 3)], val(bx, by), 8);
  Off(256, 16, dxt1, true, 0, &x, &y);
  uint32_t bad = 0, old_zero = 0;
  for (uint32_t by = 0; by < 4; ++by)
    for (uint32_t bx = 0; bx < 64; ++bx) {
      bad += mem[sh::TiledOffset2D(bx + x, by + y, 64, 3)] != val(bx, by);
      old_zero += mem[sh::TiledOffset2D(bx, by, 64, 3)] == 0;
    }
  EXPECT(bad == 0);
  EXPECT(old_zero == 256);
  std::printf(g_fail ? "FAILED %d\n" : "PASS\n", g_fail);
  return g_fail != 0;
}
