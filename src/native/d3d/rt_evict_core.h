// Card V1 (render-target eviction of provably dead images): pure EDRAM-footprint and trigger arithmetic, host-testable
// (src/native/tests/rt_evict_test.cpp). Written for this project. Tile geometry from the SDK's xenos constants
// (rex/graphics/xenos.h kEdramTileWidthSamples 80, kEdramTileHeightSamples 16, 11-bit periodic tile addressing
// kEdramBaseTilesBits, GetSurfacePitchTiles); the caller passes plain numbers so this header has no SDK dependency.
//
// Model: on the console a render target's pixels live only in its EDRAM tile range; once other targets have written
// those pixels' EDRAM, the target's former pixels no longer exist anywhere the game could read them (a resolve reads
// EDRAM). Our host keeps one private image per target key instead, so such an image holds pixels the console no longer
// has. Only writes of KNOWN coverage count as overwriting: clears (rect known), never draws (coverage unknown).
#pragma once

#include <algorithm>
#include <cstdint>

namespace wwe2k14::d3d::rt_evict {

constexpr uint32_t kTiles = 2048;             // 11-bit periodic EDRAM tile addressing (xenos kEdramBaseTilesBits)
constexpr uint32_t kTileW = 80, kTileH = 16;  // samples (32-bit units) per tile (xenos kEdramTile{Width,Height}Samples)

struct Geometry {
  uint32_t base = 0;         // first tile (color_base / depth_base)
  uint32_t pitch_tiles = 0;  // tiles per tile row (xenos::GetSurfacePitchTiles)
  uint32_t x_mul = 1;        // pixel column -> 32-bit sample column: x2 at 4x MSAA, x2 for 64bpp
  uint32_t y_mul = 1;        // pixel row -> sample row: x2 at 2x/4x MSAA
  uint32_t used_sx = 0;      // sample columns the surface's pixels use (<= pitch; the rest of a row is pitch padding)
  uint32_t used_sy = 0;      // sample rows the surface's pixels use
  uint32_t cols = 0, rows = 0;  // tiles touched by the used area
};

// Per EDRAM tile: the latest known write that covers the tile from its origin, and how far ([0,hx) x [0,hy) samples).
struct TileMark {
  uint64_t seq = 0;
  uint8_t hx = 0, hy = 0;
};

// msaa_log2: RB_SURFACE_INFO bits 16-17 (0 = 1x, 1 = 2x, 2 = 4x). width_px / height_px: the surface's pixels (declared
// extent, or a larger value = a larger, more conservative footprint); width is clamped to the pitch.
inline Geometry MakeGeometry(uint32_t base_tiles, uint32_t pitch_px, uint32_t msaa_log2, bool is_64bpp, uint32_t width_px,
                             uint32_t height_px) {
  Geometry g;
  g.base = base_tiles & (kTiles - 1);
  g.x_mul = (msaa_log2 >= 2 ? 2u : 1u) * (is_64bpp ? 2u : 1u);
  g.y_mul = msaa_log2 >= 1 ? 2u : 1u;
  // Same as xenos::GetSurfacePitchTiles: samples rounded up to whole tiles, doubled for 64bpp.
  const uint32_t pitch_samples = pitch_px << (msaa_log2 >= 2 ? 1u : 0u);
  g.pitch_tiles = ((pitch_samples + kTileW - 1) / kTileW) * (is_64bpp ? 2u : 1u);
  g.used_sx = std::min(width_px, pitch_px) * g.x_mul;
  g.used_sy = height_px * g.y_mul;
  g.cols = std::min(g.pitch_tiles, (g.used_sx + kTileW - 1) / kTileW);
  g.rows = (g.used_sy + kTileH - 1) / kTileH;
  return g;
}

inline uint64_t FootprintTiles(const Geometry& g) { return uint64_t(g.cols) * g.rows; }
inline uint32_t TileAt(const Geometry& g, uint64_t r, uint64_t c) {
  return uint32_t((g.base + r * g.pitch_tiles + c) & (kTiles - 1));
}

// A write of pixel rect [x0,x1) x [y0,y1) of a surface with geometry g (clipped to its used area): marks every tile
// whose origin the rect covers with (seq, covered extent). Tiles the rect starts inside are not marked (conservative).
inline void MarkWrite(const Geometry& g, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint64_t seq, TileMark* marks) {
  if (x1 <= x0 || y1 <= y0 || !g.cols || !g.rows) return;
  const uint64_t sx0 = uint64_t(std::max(x0, 0)) * g.x_mul, sy0 = uint64_t(std::max(y0, 0)) * g.y_mul;
  const uint64_t sx1 = std::min<uint64_t>(uint64_t(std::max(x1, 0)) * g.x_mul, g.used_sx);
  const uint64_t sy1 = std::min<uint64_t>(uint64_t(std::max(y1, 0)) * g.y_mul, g.used_sy);
  for (uint64_t r = (sy0 + kTileH - 1) / kTileH; r < g.rows && r * kTileH < sy1; ++r) {
    const uint8_t hy = uint8_t(std::min<uint64_t>(kTileH, sy1 - r * kTileH));
    for (uint64_t c = (sx0 + kTileW - 1) / kTileW; c < g.cols && c * kTileW < sx1; ++c) {
      TileMark& m = marks[TileAt(g, r, c)];
      m.seq = seq;
      m.hx = uint8_t(std::min<uint64_t>(kTileW, sx1 - c * kTileW));
      m.hy = hy;
    }
  }
}

// True when every sample of the used area was written after `since` (the slot's last use) by marked writes.
inline bool FootprintOverwritten(const Geometry& g, const TileMark* marks, uint64_t since) {
  if (!FootprintTiles(g)) return false;
  for (uint64_t r = 0; r < g.rows; ++r) {
    const uint64_t ny = std::min<uint64_t>(kTileH, g.used_sy - r * kTileH);
    for (uint64_t c = 0; c < g.cols; ++c) {
      const uint64_t nx = std::min<uint64_t>(kTileW, g.used_sx - c * kTileW);
      const TileMark& m = marks[TileAt(g, r, c)];
      if (m.seq <= since || m.hx < nx || m.hy < ny) return false;
    }
  }
  return true;
}

// Round 2: a clear counts as an overwrite only if its own rect covers the writer's whole extent ew x eh.
inline bool ClearCoversExtent(int32_t x, int32_t y, int32_t w, int32_t h, int32_t ew, int32_t eh) {
  return ew > 0 && eh > 0 && x <= 0 && y <= 0 && int64_t(x) + w >= ew && int64_t(y) + h >= eh;
}

// Budget trigger: device-local heap use at or above pct % of its budget, or the render-target bytes at or above
// rt_pct % of the budget. budget 0 (unknown) never triggers.
inline bool Pressure(uint64_t usage, uint64_t budget, uint64_t rt_bytes, uint32_t pct, uint32_t rt_pct) {
  if (!budget) return false;
  return usage * 100 >= budget * pct || rt_bytes * 100 >= budget * rt_pct;
}

// Card V1 round 3 (780M device-lost report): the pressure trigger measures heap 0, so only an image that occupies heap 0
// can relieve it. U2 steering (WWE2K14_NR_MEM_BUDGET=2) places over-budget images in host memory (heap 1/2); releasing
// those leaves heap-0 usage unchanged (780M: 538 images / 408 MB released, heap 0 stayed 243/243 MB). `heap0_bytes` =
// the image's bytes in heap 0; `any_placement` = WWE2K14_D3D_RT_EVICT_HOST=1 (round-2 behaviour: any placement).
inline bool PlacementRelieves(uint64_t heap0_bytes, bool any_placement) { return any_placement || heap0_bytes > 0; }

}  // namespace wwe2k14::d3d::rt_evict
