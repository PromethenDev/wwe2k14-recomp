#include "grid_faces.h"

#include "wwe13_convert.h"
#include "wwe15_portraits.h"

#include <algorithm>
#include <cstring>

namespace wwe2k14::launcher {
namespace {

using Bytes = GridFaceBytes;
constexpr size_t kFaceBytes = 128 + 64 * 64;  // DDS header + one 64x64 DXT5 level

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}
uint32_t Le32(const Bytes& b, size_t at) {
  return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
}
std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  return s;
}
bool HasMagic(const Bytes& b, const char* magic) { return b.size() >= 4 && std::memcmp(b.data(), magic, 4) == 0; }

// Locates MENU/SSFD in parsed groups.
WWE13PacEntryData* FindSsfd(std::vector<WWE13PacGroupData>* groups) {
  for (auto& g : *groups)
    if (Trim(g.name) == "MENU")
      for (auto& e : g.entries)
        if (Trim(e.name) == "SSFD") return &e;
  return nullptr;
}

// SSFD entry -> outer leaf id, the packed inner leaf, and the inner table.
bool OpenSsfd(const Bytes& entry, uint32_t* outer_id, Bytes* packed, std::map<uint32_t, Bytes>* faces, std::string* error) {
  std::vector<WWE13PachLeafData> outer;
  if (!ReadWWE13PachLeaves(entry, &outer, error)) return Fail(error, "MENU/SSFD is not a PACH: " + *error);
  if (outer.size() != 1) return Fail(error, "MENU/SSFD has " + std::to_string(outer.size()) + " outer leaves; 1 expected.");
  *outer_id = outer[0].id;
  *packed = outer[0].data;
  Bytes plain;
  if (HasMagic(*packed, "ZLIB")) {
    if (!DecodeWWE13ZlibLeaf(*packed, &plain, error)) return Fail(error, "MENU/SSFD inner table: " + *error);
  } else if (!DecodeWWE13Bpe(*packed, &plain, error)) {
    return Fail(error, "MENU/SSFD inner table: " + *error);
  }
  std::vector<WWE13PachLeafData> inner;
  if (!ReadWWE13PachLeaves(plain, &inner, error)) return Fail(error, "MENU/SSFD inner table: " + *error);
  faces->clear();
  for (auto& leaf : inner) (*faces)[leaf.id] = std::move(leaf.data);
  return true;
}

}  // namespace

bool IsGridFace(const Bytes& dds) {
  return dds.size() == kFaceBytes && HasMagic(dds, "DDS ") && Le32(dds, 12) == 64 && Le32(dds, 16) == 64 &&
         std::memcmp(dds.data() + 84, "DXT5", 4) == 0;
}

bool ReadGridFaces(const Bytes& menu_pac, std::map<uint32_t, Bytes>* faces, std::string* error) {
  if (error) error->clear();
  if (!faces) return Fail(error, "The face output is missing.");
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(menu_pac, &groups, error)) return false;
  WWE13PacEntryData* ssfd = FindSsfd(&groups);
  if (!ssfd) return Fail(error, "The menu PAC has no MENU/SSFD select-grid faces.");
  uint32_t outer_id = 0;
  Bytes packed;
  return OpenSsfd(ssfd->data, &outer_id, &packed, faces, error);
}

bool AddGridFaces(const Bytes& menu_pac, const std::map<uint32_t, Bytes>& add, Bytes* output, size_t* added,
                  std::string* error, const std::set<uint32_t>* replace) {
  if (error) error->clear();
  if (!output) return Fail(error, "The menu PAC output is missing.");
  if (added) *added = 0;
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(menu_pac, &groups, error)) return false;
  WWE13PacEntryData* ssfd = FindSsfd(&groups);
  if (!ssfd) return Fail(error, "The menu PAC has no MENU/SSFD select-grid faces.");
  uint32_t outer_id = 0;
  Bytes packed;
  std::map<uint32_t, Bytes> faces;
  if (!OpenSsfd(ssfd->data, &outer_id, &packed, &faces, error)) return false;
  if (!HasMagic(packed, "ZLIB")) return Fail(error, "The WWE 2K14 MENU/SSFD table is not ZLIB-packed as expected.");
  size_t count = 0;
  for (const auto& [id, dds] : add) {
    if (!IsGridFace(dds)) return Fail(error, "The select-grid face for wrestler " + std::to_string(id) + " is not a 64x64 DXT5 image.");
    if (faces.emplace(id, dds).second) ++count;  // stock faces are never replaced...
    else if (replace && replace->count(id) && faces[id] != dds) { faces[id] = dds; ++count; }  // ...unless asked (repair)
  }
  if (added) *added = count;
  if (count == 0) {
    *output = menu_pac;
    return true;
  }
  std::vector<WWE13PachLeafData> inner;  // std::map: ascending ids, as the stock table
  for (const auto& [id, dds] : faces) inner.push_back({id, dds});
  Bytes inner_plain, inner_packed, outer;
  if (!BuildWWE13PachLeaves(inner, &inner_plain, false, error) ||
      !EncodeWWE13ZlibLeaf(packed, inner_plain, &inner_packed, error) ||
      !BuildWWE13PachLeaves({{outer_id, inner_packed}}, &outer, false, error))
    return Fail(error, "The select-grid face table could not be rebuilt: " + *error);
  ssfd->data = std::move(outer);
  return RebuildWWE13PacGroups(menu_pac, groups, output, error);
}

bool GridFaceSeeThrough(const Bytes& dds) {
  if (!IsGridFace(dds)) return false;
  int w = 0, h = 0, mips = 0;
  Bytes rgba;
  std::string why;
  if (!DecodeWWE15Dxt5Dds(dds, &w, &h, &mips, &rgba, &why) || w != 64 || h != 64 || rgba.size() < 64 * 64 * 4) return false;
  // Measured 11-16: stock 2K14 and WWE '13 faces have at most 14 % pixels with alpha < 128 (most under 5 %); portrait crops
  // without a backdrop have 15-54 %.
  size_t clear = 0;
  for (size_t p = 0; p < 64 * 64; ++p) clear += rgba[p * 4 + 3] < 128;
  return clear * 100 >= 15 * 64 * 64;
}

Bytes GridFaceBackdrop(const std::map<uint32_t, Bytes>& stock) {
  std::vector<std::vector<uint8_t>> rows[3];  // per channel: rows[c][y] = samples
  for (auto& r : rows) r.resize(64);
  size_t used = 0;
  for (const auto& [id, dds] : stock) {
    if (!IsGridFace(dds)) continue;
    int w = 0, h = 0, mips = 0;
    Bytes rgba;
    std::string why;
    if (!DecodeWWE15Dxt5Dds(dds, &w, &h, &mips, &rgba, &why) || w != 64 || h != 64 || rgba.size() < 64 * 64 * 4) continue;
    ++used;
    for (int y = 0; y < 64; ++y)
      for (int x : {0, 1, 2, 61, 62, 63}) {
        const size_t at = (size_t(y) * 64 + x) * 4;
        if (rgba[at + 3] < 200) continue;  // only opaque backdrop pixels
        for (int c = 0; c < 3; ++c) rows[c][y].push_back(rgba[at + c]);
      }
  }
  if (!used) return {};
  // Per-row median, then a wide vertical smooth (per-row medians alone show stripes).
  float med[64][3];
  float last[3] = {60, 40, 32};
  for (int y = 0; y < 64; ++y)
    for (int c = 0; c < 3; ++c) {
      auto& v = rows[c][y];
      if (!v.empty()) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        last[c] = v[v.size() / 2];
      }
      med[y][c] = last[c];
    }
  Bytes out(64 * 64 * 4);
  for (int y = 0; y < 64; ++y) {
    float sum[3] = {0, 0, 0};
    int n = 0;
    for (int k = std::max(0, y - 12); k <= std::min(63, y + 12); ++k, ++n)
      for (int c = 0; c < 3; ++c) sum[c] += med[k][c];
    for (int x = 0; x < 64; ++x) {
      const size_t at = (size_t(y) * 64 + x) * 4;
      for (int c = 0; c < 3; ++c) out[at + c] = uint8_t(sum[c] / n + 0.5f);
      out[at + 3] = 255;
    }
  }
  return out;
}

bool MakeGridFaceFromPortrait(const Bytes& portrait_dds, const Bytes& header_like, Bytes* face, std::string* error,
                              const Bytes* backdrop) {
  if (error) error->clear();
  if (!face) return Fail(error, "The face output is missing.");
  if (!IsGridFace(header_like)) return Fail(error, "No stock select-grid face to copy the format from.");
  int w = 0, h = 0, mips = 0;
  Bytes rgba;
  if (!DecodeWWE15Dxt5Dds(portrait_dds, &w, &h, &mips, &rgba, error)) return Fail(error, "Select portrait: " + *error);
  if (w < 16 || h < 16 || rgba.size() < size_t(w) * size_t(h) * 4)
    return Fail(error, "Select portrait has an unexpected size.");
  // Head box (same rule as tools/port/grid/ssfd_add.py): top of the opaque figure, centred on it 1/10 of the height lower.
  auto alpha = [&](int x, int y) { return rgba[(size_t(y) * w + x) * 4 + 3]; };
  int top = 0;
  bool found = false;
  for (int y = 0; y < h && !found; ++y)
    for (int x = 0; x < w; ++x)
      if (alpha(x, y) > 32) { top = y; found = true; break; }
  const int probe = std::min(h - 1, top + h / 10);
  int left = -1, right = -1;
  for (int x = 0; x < w; ++x)
    if (alpha(x, probe) > 32) { if (left < 0) left = x; right = x; }
  const int cx = left >= 0 ? (left + right) / 2 : w / 2;
  const int side = std::min(std::min(w, h), std::max(8, int(h * (backdrop && !backdrop->empty() ? 0.28 : 0.34))));  // tighter: heads fill the tile like stock faces
  const int x0 = std::max(0, std::min(w - side, cx - side / 2));
  const int y0 = std::max(0, std::min(h - side, top - side / 12));
  Bytes crop(size_t(side) * side * 4);
  for (int y = 0; y < side; ++y)
    std::memcpy(crop.data() + size_t(y) * side * 4, rgba.data() + (size_t(y0 + y) * w + x0) * 4, size_t(side) * 4);
  Bytes small = ResizeWWE15Box(crop, side, side, 64, 64);
  if (backdrop && backdrop->size() == small.size())
    for (size_t p = 0; p + 3 < small.size(); p += 4) {
      const uint32_t a = small[p + 3];
      for (int c = 0; c < 3; ++c) small[p + c] = uint8_t((small[p + c] * a + (*backdrop)[p + c] * (255 - a) + 127) / 255);
      small[p + 3] = 255;
    }
  const Bytes dds = EncodeWWE15Dxt5Dds(small, 64, 64);
  if (dds.size() < kFaceBytes) return Fail(error, "The face could not be encoded.");
  Bytes out(header_like.begin(), header_like.begin() + 128);
  out.insert(out.end(), dds.begin() + 128, dds.begin() + kFaceBytes);
  *face = std::move(out);
  return true;
}

}  // namespace wwe2k14::launcher
