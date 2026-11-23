#ifdef NDEBUG
#undef NDEBUG  // the checks below must run in every build type (they also do the work under test)
#endif
#include "grid_faces.h"
#include "wwe13_convert.h"
#include "wwe15_portraits.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace wwe2k14::launcher;
using Bytes = std::vector<uint8_t>;

namespace {

// A 64x64 DXT5 face filled with one colour (header from the launcher's own encoder, cut to one level like the stock faces).
Bytes Face(uint8_t r, uint8_t g, uint8_t b) {
  Bytes rgba(64 * 64 * 4);
  for (size_t i = 0; i < rgba.size(); i += 4) { rgba[i] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = 255; }
  Bytes dds = EncodeWWE15Dxt5Dds(rgba, 64, 64);
  dds.resize(128 + 4096);
  return dds;
}

Bytes MenuPac(const std::map<uint32_t, Bytes>& faces) {
  std::vector<WWE13PachLeafData> inner;
  for (const auto& [id, dds] : faces) inner.push_back({id, dds});
  Bytes inner_plain, packed, outer, pac;
  std::string error;
  Bytes header(16, 0);
  std::memcpy(header.data(), "ZLIB", 4);
  header[4] = 1;
  assert(BuildWWE13PachLeaves(inner, &inner_plain, false, &error));
  assert(EncodeWWE13ZlibLeaf(header, inner_plain, &packed, &error));
  assert(BuildWWE13PachLeaves({{0, packed}}, &outer, false, &error));
  std::vector<WWE13PacGroupData> groups(2);
  groups[0].name = "MENU";
  groups[0].entries.push_back({"SSFD", outer});
  groups[1].name = "SSFA";
  groups[1].entries.push_back({"0100", Bytes(300, 7)});
  assert(BuildWWE13PacGroups(groups, &pac, &error));
  return pac;
}

}  // namespace

int main() {
  std::string error;
  const Bytes stock = Face(200, 150, 120), other = Face(10, 200, 10), fresh = Face(40, 40, 220);
  assert(IsGridFace(stock) && !IsGridFace(Bytes(4224, 0)));

  // Add: a new id lands; a stock id is never replaced; the rest of the PAC is untouched.
  const Bytes pac = MenuPac({{100, stock}, {10000, stock}});
  Bytes out;
  size_t added = 0;
  assert(AddGridFaces(pac, {{100, other}, {437, fresh}}, &out, &added, &error));
  assert(added == 1);
  std::map<uint32_t, Bytes> faces;
  assert(ReadGridFaces(out, &faces, &error));
  assert(faces.size() == 3 && faces.at(100) == stock && faces.at(437) == fresh && faces.at(10000) == stock);
  std::vector<WWE13PacGroupData> groups, before;
  assert(ReadWWE13PacGroups(out, &groups, &error) && ReadWWE13PacGroups(pac, &before, &error));
  // The other group reads back exactly as from the input PAC (entries are stored in 0x100 units, so 300 B reads as 512 B).
  assert(groups.size() == 2 && groups[1].entries.size() == 1 && groups[1].entries[0].data == before[1].entries[0].data);
  assert(std::equal(groups[1].entries[0].data.begin(), groups[1].entries[0].data.begin() + 300, Bytes(300, 7).begin()));
  // Nothing new: output is the input as is.
  Bytes same;
  assert(AddGridFaces(pac, {{100, other}}, &same, &added, &error) && added == 0 && same == pac);
  // A face of the wrong size is refused.
  assert(!AddGridFaces(pac, {{438, Bytes(100, 1)}}, &out, &added, &error));
  std::cout << "PASS add/read select-grid faces\n";

  // Fallback: a 256x256 portrait with an opaque figure becomes a stock-format 64x64 face.
  Bytes portrait(256 * 256 * 4, 0);
  for (int y = 30; y < 256; ++y)
    for (int x = 90; x < 170; ++x) {
      uint8_t* p = &portrait[(size_t(y) * 256 + x) * 4];
      p[0] = 180; p[1] = 120; p[2] = 90; p[3] = 255;
    }
  const Bytes portrait_dds = EncodeWWE15Dxt5Dds(portrait, 256, 256);
  Bytes face;
  assert(MakeGridFaceFromPortrait(portrait_dds, stock, &face, &error));
  assert(IsGridFace(face) && std::equal(face.begin(), face.begin() + 128, stock.begin()));
  int w = 0, h = 0, mips = 0;
  Bytes rgba;
  assert(DecodeWWE15Dxt5Dds(face, &w, &h, &mips, &rgba, &error) && w == 64 && h == 64);
  assert(rgba[(32 * 64 + 32) * 4 + 3] > 200);  // the head box sits on the figure, not on empty background
  std::cout << "PASS portrait fallback face\n";

  // Real game data (optional): WWE2K14_GRID_REAL=<2K14 menuHD.pac>|<WWE '13 or 2K15 menuHD.pac>
  if (const char* real = std::getenv("WWE2K14_GRID_REAL")) {
    const std::string spec = real;
    const size_t bar = spec.find('|');
    auto read = [](const std::string& path) {
      std::ifstream f(path, std::ios::binary);
      return Bytes(std::istreambuf_iterator<char>(f), {});
    };
    const Bytes k14 = read(spec.substr(0, bar)), src = read(spec.substr(bar + 1));
    std::map<uint32_t, Bytes> f14, fsrc;
    assert(ReadGridFaces(k14, &f14, &error) && ReadGridFaces(src, &fsrc, &error));
    std::map<uint32_t, Bytes> add;
    for (const auto& [id, dds] : fsrc) if (!f14.count(id) && id < 10000) add[id] = dds;
    Bytes rebuilt;
    assert(AddGridFaces(k14, add, &rebuilt, &added, &error));
    std::map<uint32_t, Bytes> back;
    assert(ReadGridFaces(rebuilt, &back, &error));
    for (const auto& [id, dds] : f14) assert(back.at(id) == dds);
    for (const auto& [id, dds] : add) assert(back.at(id) == dds);
    std::cout << "PASS real menus: 2K14 " << f14.size() << " faces + " << added << " from the other game = " << back.size() << "\n";
  }
  return 0;
}
