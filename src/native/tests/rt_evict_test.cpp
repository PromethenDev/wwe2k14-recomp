// Card V1 host test: EDRAM footprint / coverage / trigger arithmetic of src/native/d3d/rt_evict_core.h.
// g++ -std=c++20 -Wall -Wextra -Werror -I src/native/d3d src/native/tests/rt_evict_test.cpp -o rt_evict_test
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "rt_evict_core.h"

using namespace wwe2k14::d3d::rt_evict;

static int g_fail = 0;
#define EXPECT(c)                                              \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);  \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

int main() {
  // 1280x720 1x 32bpp at base 0: 16 tiles per row, 45 rows (xenos GetSurfacePitchTiles(1280, 1x) = 16).
  Geometry a = MakeGeometry(0, 1280, 0, false, 1280, 720);
  EXPECT(a.pitch_tiles == 16 && a.cols == 16 && a.rows == 45 && FootprintTiles(a) == 720);
  // 4x MSAA doubles both sample dimensions: 32 tiles per row, 90 rows.
  Geometry a4 = MakeGeometry(0, 1280, 2, false, 1280, 720);
  EXPECT(a4.pitch_tiles == 32 && a4.rows == 90 && a4.x_mul == 2 && a4.y_mul == 2);
  // 2x MSAA doubles rows only.
  Geometry a2 = MakeGeometry(0, 1280, 1, false, 1280, 720);
  EXPECT(a2.pitch_tiles == 16 && a2.rows == 90);
  // 64bpp: twice the tiles per row.
  Geometry w64 = MakeGeometry(100, 640, 0, true, 640, 360);
  EXPECT(w64.pitch_tiles == 16 && w64.cols == 16 && w64.rows == 23);
  // Pitch padding: pitch 560 (7 tiles), declared width 512 -> 7 columns touched, the last one partly (32 samples).
  Geometry p = MakeGeometry(0, 560, 0, false, 512, 512);
  EXPECT(p.pitch_tiles == 7 && p.cols == 7 && p.used_sx == 512 && p.rows == 32);

  std::vector<TileMark> m(kTiles);
  // Nothing written: never overwritten.
  EXPECT(!FootprintOverwritten(a, m.data(), 0));
  // A whole clear of another 1280x720 target at the same base after seq 5 overwrites it; not if since >= the write.
  MarkWrite(a, 0, 0, 1280, 720, 6, m.data());
  EXPECT(FootprintOverwritten(a, m.data(), 5));
  EXPECT(!FootprintOverwritten(a, m.data(), 6));
  // A smaller clear (top half) leaves the bottom rows of a full-size target unproven.
  std::vector<TileMark> h(kTiles);
  MarkWrite(a, 0, 0, 1280, 360, 9, h.data());
  EXPECT(!FootprintOverwritten(a, h.data(), 1));
  Geometry top = MakeGeometry(0, 1280, 0, false, 1280, 360);
  EXPECT(FootprintOverwritten(top, h.data(), 1));
  // Partial last tile column: a 512-wide clear covers the 512 used samples of each row's 7th tile (hx = 32).
  std::vector<TileMark> q(kTiles);
  MarkWrite(p, 0, 0, 512, 512, 3, q.data());
  EXPECT(FootprintOverwritten(p, q.data(), 2));
  // ... but not the same tiles of a 560-wide surface (uses all 80 samples of the 7th tile).
  Geometry p560 = MakeGeometry(0, 560, 0, false, 560, 512);
  EXPECT(!FootprintOverwritten(p560, q.data(), 2));
  // A rect starting inside a tile does not mark that tile.
  std::vector<TileMark> r(kTiles);
  MarkWrite(a, 8, 0, 1280, 720, 4, r.data());
  EXPECT(r[0].seq == 0 && r[1].seq == 4);
  EXPECT(!FootprintOverwritten(a, r.data(), 1));
  // Rows: a 600-row surface's last tile row is 8 rows deep; a 600-row clear covers it, a 592-row one does not.
  Geometry s600 = MakeGeometry(0, 1280, 0, false, 1280, 600);
  std::vector<TileMark> t(kTiles);
  MarkWrite(s600, 0, 0, 1280, 600, 2, t.data());
  EXPECT(FootprintOverwritten(s600, t.data(), 1));
  std::vector<TileMark> t2(kTiles);
  MarkWrite(s600, 0, 0, 1280, 592, 2, t2.data());
  EXPECT(!FootprintOverwritten(s600, t2.data(), 1));
  // Different base: a write of tiles 0..719 does not cover a target at base 1000; periodic wrap at 2048.
  Geometry b = MakeGeometry(1000, 1280, 0, false, 1280, 720);
  EXPECT(!FootprintOverwritten(b, m.data(), 0));
  Geometry wrap = MakeGeometry(2040, 1280, 0, false, 1280, 16);  // tiles 2040..2047, 0..7
  EXPECT(TileAt(wrap, 0, 15) == 7);
  std::vector<TileMark> w(kTiles);
  MarkWrite(wrap, 0, 0, 1280, 16, 7, w.data());
  EXPECT(w[2047].seq == 7 && w[0].seq == 7 && w[7].seq == 7 && w[8].seq == 0);
  // Depth 4x scene tile 320x736 (M2c7): 8 tiles per row, 92 rows.
  Geometry d = MakeGeometry(0, 320, 2, false, 320, 736);
  EXPECT(d.pitch_tiles == 8 && d.rows == 92);
  // Empty geometry (no image height) is never "overwritten".
  EXPECT(!FootprintOverwritten(MakeGeometry(0, 1280, 0, false, 1280, 0), m.data(), 0));

  // Round 2: only clears covering the writer's whole extent count.
  EXPECT(ClearCoversExtent(0, 0, 1280, 720, 1280, 720));
  EXPECT(ClearCoversExtent(-8, -8, 1300, 740, 1280, 720));
  EXPECT(!ClearCoversExtent(0, 0, 1280, 719, 1280, 720));
  EXPECT(!ClearCoversExtent(8, 0, 1272, 720, 1280, 720));
  EXPECT(!ClearCoversExtent(0, 0, 1280, 720, 0, 720));

  // Trigger.
  const uint64_t MB = 1ull << 20;
  EXPECT(!Pressure(1000 * MB, 0, 0, 75, 25));               // unknown budget
  EXPECT(Pressure(1132 * MB, 1509 * MB, 100 * MB, 75, 25));  // 75 % of a 2 GB card's budget
  EXPECT(!Pressure(1100 * MB, 1509 * MB, 100 * MB, 75, 25));
  EXPECT(Pressure(500 * MB, 1509 * MB, 378 * MB, 75, 25));   // RT bytes >= 25 % of the budget
  EXPECT(!Pressure(1601 * MB, 5289 * MB, 431 * MB, 75, 25));  // the Arc A380 run at its end: no trigger
  // Round 3: the 780M owner run (heap 0 243/243 MB, rt_dev 5 MB) is under pressure, but a dead image steered to host
  // memory (0 heap-0 bytes) is not released by default; one in heap 0 is; RT_EVICT_HOST=1 releases either.
  EXPECT(Pressure(243 * MB, 243 * MB, 5 * MB, 75, 25));
  EXPECT(!PlacementRelieves(0, false));
  EXPECT(PlacementRelieves(3 * MB, false));
  EXPECT(PlacementRelieves(0, true));

  if (g_fail) {
    std::printf("%d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("rt_evict_test: all passed\n");
  return 0;
}
