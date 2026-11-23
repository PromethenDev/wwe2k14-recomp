// Host-only checks for the crowd draw grouping decisions and ring packing (card U2c). No GPU, no game.
//   g++ -std=c++20 -O1 -Wall -Wextra -I src/native/nr src/native/nr/tests/crowd-group-test.cpp -o crowd-group-test
#include "../crowd_group.h"

#include <cstdio>
#include <random>

using namespace nr::backend;
using namespace nr::backend::crowd;

static Draw MakeCrowdDraw(uint32_t seed) {
  Draw d;
  d.color = 3;
  d.depth = 4;
  d.vs_key = 0x9F553BC6F85E1626ull;
  d.ps_key = 0xF5C9EF1CFA228C6Eull;
  static const std::vector<uint8_t> spirv(16, 1);
  d.vs_spirv = d.ps_spirv = &spirv;
  d.count = 1496;
  d.index32 = true;
  d.index_cache_id = 77;
  d.index_cache_bytes = 1496 * 4;
  VertexBuffer vb;
  vb.stride_bytes = 32;
  vb.cache_id = 55;
  vb.cache_size = 40000;
  d.buffers.push_back(vb);
  d.attributes.push_back({0, VtxFormat::kR32G32B32_SFLOAT, 0, 0});
  d.attributes.push_back({1, VtxFormat::kR32G32_SFLOAT, 0, 12});
  TextureBinding tb{};
  tb.slot = 0;
  tb.texture = 9;
  d.textures.push_back(tb);
  d.viewport[2] = 1280;
  d.viewport[3] = 720;
  d.scissor[2] = 1280;
  d.scissor[3] = 720;
  d.depth_test = true;
  d.blend.enabled = true;
  std::memset(d.vs_constants, 0, sizeof(d.vs_constants));
  for (int i = 0; i < 4096; ++i) d.vs_constants[i] = uint8_t(seed * 31 + i * 7);
  std::memset(d.ps_constants, 0x5A, sizeof(d.ps_constants));
  std::memset(d.shared, 0x11, sizeof(d.shared));
  d.ordinal = seed;
  return d;
}

int main() {
  int bad = 0;
  auto check = [&](bool ok, const char* what) {
    if (!ok) {
      ++bad;
      std::printf("FAIL %s\n", what);
    }
  };
  constexpr uint32_t kMax = 8;

  // Flat cards: both keys must match; a card run joins on VS constants only and a different strip breaks it.
  {
    check(IsCrowdCard(kCardPsKey, kCardVsKey), "card keys");
    check(!IsCrowdCard(kCardPsKey, 0x23CD907AD5C389BBull), "card ps with another vs");
    check(!IsCrowdCard(0xF5C9EF1CFA228C6Eull, kCardVsKey), "card vs with another ps");
    check(IsEntranceCrowd(kEntranceCardPsKey, kEntranceCardVsKey) && IsEntranceCrowd(kEntranceFigurePsKey, kEntranceFigureVsKey), "entrance keys");
    check(!IsEntranceCrowd(kEntranceCardPsKey, kEntranceFigureVsKey) && !IsEntranceCrowd(kCardPsKey, kCardVsKey), "entrance mixed keys");
    Draw a = MakeCrowdDraw(1), b = MakeCrowdDraw(2), c = MakeCrowdDraw(3);
    for (Draw* d : {&a, &b, &c}) {
      d->ps_key = kCardPsKey;
      d->vs_key = kCardVsKey;
      d->count = 4;
      d->topology = 5;
      d->index_cache_id = 0;
      d->index_cache_bytes = 0;
      d->indices = {0, 0, 1, 0, 2, 0, 3, 0};
      d->buffers[0].cache_id = 0;
      d->buffers[0].cache_size = 0;
      d->buffers[0].data = {1, 2, 3, 4};
    }
    CrowdGroup g;
    g.Start(a);
    check(g.TryJoin(b, true, kMax) == kCwJoined && g.n == 2, "card joins on constants only");
    c.buffers[0].data = {9, 2, 3, 4};
    check(g.TryJoin(c, true, kMax) == kCwGeometry && g.n == 2, "card with other vertices breaks the run");
  }

  // Identical state, different VS constants: all join, blocks kept in order.
  {
    CrowdGroup g;
    g.Start(MakeCrowdDraw(1));
    bool all = true;
    for (uint32_t i = 2; i <= 5; ++i) all &= g.TryJoin(MakeCrowdDraw(i), true, kMax) == kCwJoined;
    check(all && g.n == 5 && g.vs.size() == 5 * 4096, "same-state draws join in order");
    bool order = true;
    for (uint32_t i = 0; i < 5; ++i) order &= std::memcmp(g.vs.data() + i * 4096, MakeCrowdDraw(i + 1).vs_constants, 4096) == 0;
    check(order, "vertex blocks retain draw order");
  }
  {
    Draw a = MakeCrowdDraw(1), b = MakeCrowdDraw(2);
    b.ps_key ^= 1;
    check(CrowdCompare(a, b) == kCwState, "PS key remains a grouping condition");
    check(CrowdCompare(a, b, true) == kCwJoined, "diagnostic comparator omits only the PS key");
    static const std::vector<uint8_t> other_ps(16, 2);
    b.ps_spirv = &other_ps;
    check(CrowdCompare(a, b, true) == kCwState, "diagnostic retains shader object identity");
  }
  struct Case { const char* name; CrowdWhy want; void (*edit)(Draw&); };
  const Case cases[] = {
      {"texture handle", kCwTexture, [](Draw& d) { d.textures[0].texture = 10; }},
      {"sampler", kCwTexture, [](Draw& d) { d.textures[0].sampler.lod_bias = 0.5f; }},
      {"texture count", kCwTexture, [](Draw& d) { d.textures.clear(); }},
      {"ps constants", kCwPsConst, [](Draw& d) { d.ps_constants[100] ^= 1; }},
      {"shared block", kCwShared, [](Draw& d) { d.shared[900] ^= 1; }},
      {"index cache id", kCwGeometry, [](Draw& d) { d.index_cache_id = 78; }},
      {"vertex cache id", kCwGeometry, [](Draw& d) { d.buffers[0].cache_id = 56; }},
      {"index count", kCwGeometry, [](Draw& d) { d.count = 1494; }},
      {"stride", kCwGeometry, [](Draw& d) { d.buffers[0].stride_bytes = 36; }},
      {"attribute offset", kCwGeometry, [](Draw& d) { d.attributes[1].offset_bytes = 16; }},
      {"colour target", kCwPass, [](Draw& d) { d.color = 5; }},
      {"depth target", kCwPass, [](Draw& d) { d.depth = kNone; }},
      {"blend", kCwState, [](Draw& d) { d.blend.dst = 3; }},
      {"depth compare", kCwState, [](Draw& d) { d.depth_compare = 4; }},
      {"viewport", kCwState, [](Draw& d) { d.viewport[0] = 1; }},
      {"scissor", kCwState, [](Draw& d) { d.scissor[1] = 2; }},
      {"cull", kCwState, [](Draw& d) { d.cull = 3; }},
      {"stencil ref", kCwState, [](Draw& d) { d.stencil_ref = 1; }},
      {"vs key", kCwState, [](Draw& d) { d.vs_key ^= 1; }},
      {"spec constants", kCwState, [](Draw& d) { d.spec_constants = 1; }},
      {"blend constants", kCwState, [](Draw& d) { d.blend_constants[0] = 1; }},
  };
  for (const Case& c : cases) {
    CrowdGroup g;
    g.Start(MakeCrowdDraw(1));
    Draw b = MakeCrowdDraw(2);
    c.edit(b);
    const CrowdWhy got = g.TryJoin(b, true, kMax);
    char msg[96];
    std::snprintf(msg, sizeof(msg), "%s breaks the run (got %d want %d)", c.name, int(got), int(c.want));
    check(got == c.want && g.n == 1 && g.vs.size() == 4096, msg);
  }
  {
    CrowdGroup g;
    g.Start(MakeCrowdDraw(1));
    Draw b = MakeCrowdDraw(2);
    std::mt19937 rng(7);
    for (uint8_t& v : b.vs_constants) v = uint8_t(rng());
    check(g.TryJoin(b, true, kMax) == kCwJoined, "any VS constant change still joins");
  }
  {
    CrowdGroup g;
    g.Start(MakeCrowdDraw(1));
    check(g.TryJoin(MakeCrowdDraw(2), false, kMax) == kCwOtherDraw, "non-crowd draw ends the run");
    Draw up = MakeCrowdDraw(3);
    up.buffers[0].cache_upload = true;
    up.buffers[0].data.assign(8, 1);
    check(g.TryJoin(up, true, kMax) == kCwGeometry, "vertex cache upload cannot join");
    Draw iu = MakeCrowdDraw(3);
    iu.index_cache_upload = true;
    check(g.TryJoin(iu, true, kMax) == kCwGeometry, "index cache upload cannot join");
    Draw fl = MakeCrowdDraw(3);
    fl.cache_flush = true;
    check(g.TryJoin(fl, true, kMax) == kCwGeometry, "cache flush cannot join");
    Draw uncached = MakeCrowdDraw(3);
    uncached.buffers[0].cache_id = 0;
    uncached.buffers[0].data.assign(64, 2);
    check(!CrowdGeometryCached(uncached), "uncached geometry is not classified as cached");
    CrowdGroup full;
    full.Start(MakeCrowdDraw(1));
    for (uint32_t i = 2; i <= kMax; ++i) full.TryJoin(MakeCrowdDraw(i), true, kMax);
    check(full.n == kMax && full.TryJoin(MakeCrowdDraw(99), true, kMax) == kCwFull, "run respects instance cap");
  }
  {
    constexpr size_t kStride = 17408;
    constexpr uint32_t n = 7;
    std::vector<uint8_t> blocks(size_t(n) * 4096), ring(size_t(n) * kStride + 8192, 0xEE);
    std::mt19937 rng(3);
    for (uint8_t& v : blocks) v = uint8_t(rng());
    uint8_t* slot0_vs = ring.data() + 4096;
    PackVsBlocks(slot0_vs, kStride, blocks.data(), n);
    check(VerifyVsBlocks(slot0_vs, kStride, blocks.data(), n) == n, "packed blocks verify");
    bool addr = true;
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t reg : {0u, 76u, 77u, 78u, 123u, 185u, 255u})
        addr &= std::memcmp(slot0_vs + size_t(i) * kStride + reg * 16, blocks.data() + size_t(i) * 4096 + reg * 16, 16) == 0;
    check(addr, "instance i reads base + i*17408 + 16*register");
    bool guard = true;
    for (uint32_t i = 0; i + 1 < n; ++i) guard &= slot0_vs[size_t(i) * kStride + 4096] == 0xEE;
    check(guard, "bytes after each 4KB VS block remain untouched");
    blocks[5 * 4096 + 100] ^= 1;
    check(VerifyVsBlocks(slot0_vs, kStride, blocks.data(), n) == n - 1, "verification detects a corrupt block");
  }
  {
    auto mk = [&](uint32_t seed, uint8_t fill) {
      Draw d = MakeCrowdDraw(seed);
      d.buffers[0].cache_id = 0; d.buffers[0].cache_size = 0; d.buffers[0].data.assign(300, fill);
      d.index_cache_id = 0; d.index_cache_bytes = 0; d.indices.assign(64, fill);
      return d;
    };
    CrowdGroup g;
    g.Start(mk(1, 5));
    check(g.TryJoin(mk(2, 5), true, kMax) == kCwJoined, "uncached identical geometry joins");
    check(g.TryJoin(mk(3, 6), true, kMax) == kCwGeometry, "different uncached vertex bytes break the run");
    Draw di = mk(4, 5); di.indices[10] ^= 1;
    check(g.TryJoin(di, true, kMax) == kCwGeometry, "different uncached index bytes break the run");
    Draw dp = mk(5, 5); dp.buffers[0].prefix_bytes = 32;
    check(g.TryJoin(dp, true, kMax) == kCwGeometry, "different prefix breaks the run");
    Draw mixed = MakeCrowdDraw(6);
    check(g.TryJoin(mixed, true, kMax) == kCwGeometry, "cached and uncached members do not mix");
    Draw first = MakeCrowdDraw(1);
    first.cache_flush = true; first.buffers[0].cache_upload = true; first.buffers[0].data.assign(8, 1);
    CrowdGroup g2; g2.Start(first);
    check(g2.TryJoin(MakeCrowdDraw(2), true, kMax) == kCwJoined, "first member may store the cache entry");
    Draw later = MakeCrowdDraw(3); later.cache_flush = true;
    check(g2.TryJoin(later, true, kMax) == kCwGeometry, "later member cannot flush cache");
  }
  {
    const size_t px = 64, bpp = 4;
    std::vector<uint8_t> pre(px * bpp, 7), a = pre, b = pre;
    check(ComparePictures(pre, a, b, px).verdict == PictureResult::kInconclusive, "unpainted picture is inconclusive");
    for (size_t i = 8 * bpp; i < 20 * bpp; ++i) a[i] = b[i] = uint8_t(100 + i);
    PictureResult r = ComparePictures(pre, a, b, px);
    check(r.verdict == PictureResult::kPass && r.painted_px == 12 && r.diff_bytes == 0, "painted equal pictures pass");
    b[9 * bpp + 1] ^= 4;
    r = ComparePictures(pre, a, b, px);
    check(r.verdict == PictureResult::kMismatch && r.diff_bytes == 1 && r.max_diff == 4 && r.first_diff == 9 * bpp + 1,
          "one differing byte mismatches");
    std::vector<uint8_t> only_inst = pre;
    only_inst[3 * bpp] = 99;
    check(ComparePictures(pre, only_inst, pre, px).verdict == PictureResult::kMismatch, "painting only one way mismatches");
    check(ComparePictures(pre, a, std::vector<uint8_t>(3), px).verdict == PictureResult::kInconclusive, "different image sizes are inconclusive");
  }
  std::printf(bad ? "crowd-group-test: %d FAILED\n" : "crowd-group-test: all passed\n", bad);
  return bad ? 1 : 0;
}
