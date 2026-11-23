#include "internal/paint_tool.h"

#include "internal/util.h"

#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
#undef MINIZ_HEADER_FILE_ONLY

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace wwe2k14::launcher::internal {
namespace {

// Offsets inside a slot (slot start = 8 + k*0x50468).
constexpr size_t kSlotFlag = 0;
constexpr size_t kSlotVersion = 8;
constexpr size_t kSlotGameStamp = 12;
constexpr size_t kSlotGuid = 24;
constexpr size_t kSlotFlagBytes = 48;   // 8 bytes, summed as signed bytes
constexpr size_t kSlotWidth = 64;
constexpr size_t kSlotHeight = 68;
constexpr size_t kSlotCanvas = 72;
constexpr size_t kCanvasWords = 256 * 256;
constexpr size_t kCanvasBytes = kCanvasWords * 4;
constexpr size_t kSlotTga = kSlotCanvas + kCanvasBytes;  // 0x40048
constexpr size_t kSlotPalette = kSlotTga + 20;           // 0x4005C
constexpr size_t kSlotIndices = kSlotPalette + 1024;     // 0x4045C
constexpr size_t kSlotStamp = kSlotIndices + 65536;      // 0x5045C
constexpr size_t kSlotSum = kSlotStamp + 8;              // 0x50464
static_assert(kSlotSum + 4 == kPtSlotSize, "slot layout must fill the slot");

size_t SlotBase(size_t slot) { return 8 + slot * kPtSlotSize; }

uint32_t Be32(const std::vector<uint8_t>& b, size_t o) {
  return (uint32_t(b[o]) << 24) | (uint32_t(b[o + 1]) << 16) | (uint32_t(b[o + 2]) << 8) | b[o + 3];
}
uint32_t Be16(const std::vector<uint8_t>& b, size_t o) { return (uint32_t(b[o]) << 8) | b[o + 1]; }
void PutBe32(std::vector<uint8_t>* b, size_t o, uint32_t v) {
  (*b)[o] = uint8_t(v >> 24); (*b)[o + 1] = uint8_t(v >> 16); (*b)[o + 2] = uint8_t(v >> 8); (*b)[o + 3] = uint8_t(v);
}
void PutBe16(std::vector<uint8_t>* b, size_t o, uint32_t v) { (*b)[o] = uint8_t(v >> 8); (*b)[o + 1] = uint8_t(v); }
void PutLe32(std::vector<uint8_t>* b, size_t o, uint32_t v) {
  (*b)[o] = uint8_t(v); (*b)[o + 1] = uint8_t(v >> 8); (*b)[o + 2] = uint8_t(v >> 16); (*b)[o + 3] = uint8_t(v >> 24);
}
uint32_t Le32(const std::vector<uint8_t>& b, size_t o) {
  return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
}

void SetStamp(std::vector<uint8_t>* f, size_t b, const PtStamp& stamp) {
  PutBe16(f, b + kSlotStamp, stamp.year);
  (*f)[b + kSlotStamp + 2] = stamp.month;
  (*f)[b + kSlotStamp + 3] = stamp.day;
  (*f)[b + kSlotStamp + 4] = stamp.hour;
  (*f)[b + kSlotStamp + 5] = stamp.minute;
  (*f)[b + kSlotStamp + 6] = stamp.second;
  (*f)[b + kSlotStamp + 7] = stamp.weekday;
}

}  // namespace

// The sum mirrors the game's sub_833D4590: 12 header words, 8 signed flag bytes, 4 words (w,h + 2 reserved),
// 65536 canvas words, the TGA header fields, 256 palette words, 65536 index bytes, the 8 stamp bytes.
uint32_t PtComputeSlotSum(const std::vector<uint8_t>& f, size_t slot) {
  const size_t b = SlotBase(slot);
  uint32_t s = 0;
  for (size_t o = 0; o < 48; o += 4) s += Be32(f, b + o);
  for (size_t o = 0; o < 8; ++o) s += static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(f[b + kSlotFlagBytes + o])));
  for (size_t o = 56; o < 72; o += 4) s += Be32(f, b + o);
  for (size_t i = 0; i < kCanvasWords; ++i) s += Be32(f, b + kSlotCanvas + i * 4);
  const size_t t = b + kSlotTga;
  for (size_t o : {size_t{0}, size_t{1}, size_t{2}, size_t{7}, size_t{16}, size_t{17}}) s += f[t + o];
  for (size_t o : {size_t{3}, size_t{5}, size_t{8}, size_t{10}, size_t{12}, size_t{14}, size_t{18}}) s += Be16(f, t + o);
  for (size_t i = 0; i < 256; ++i) s += Be32(f, b + kSlotPalette + i * 4);
  for (size_t i = 0; i < 65536; ++i) s += f[b + kSlotIndices + i];
  s += Be16(f, b + kSlotStamp);
  for (size_t o = 2; o < 8; ++o) s += f[b + kSlotStamp + o];
  return s;
}

uint32_t PtComputeTotal(const std::vector<uint8_t>& f) {
  uint32_t total = Be32(f, 0) + Be32(f, 4);
  for (size_t k = 0; k < kPtSlotCount; ++k) total += PtComputeSlotSum(f, k);
  return total;
}

bool PtIsValid(const std::vector<uint8_t>& f, std::string* why) {
  auto fail = [&](const char* m) { if (why) *why = m; return false; };
  if (f.size() != kPtFileSize) return fail("wrong file size");
  if (Be32(f, 0) != kPtMagic) return fail("wrong magic");
  if (Be32(f, 4) != kPtVersion) return fail("wrong version");
  if (Be32(f, kPtFileSize - 4) != PtComputeTotal(f)) return fail("checksum mismatch");
  return true;
}

void PtFixChecksums(std::vector<uint8_t>* f) {
  uint32_t total = Be32(*f, 0) + Be32(*f, 4);
  for (size_t k = 0; k < kPtSlotCount; ++k) {
    const uint32_t s = PtComputeSlotSum(*f, k);
    PutBe32(f, SlotBase(k) + kSlotSum, s);
    total += s;
  }
  PutBe32(f, kPtFileSize - 4, total);
}

std::vector<uint8_t> PtMakeEmpty() {
  std::vector<uint8_t> f(kPtFileSize, 0);
  PutBe32(&f, 0, kPtMagic);
  PutBe32(&f, 4, kPtVersion);
  for (size_t k = 0; k < kPtSlotCount; ++k) {
    PutBe32(&f, SlotBase(k) + kSlotVersion, kPtVersion);  // as the game's empty-file initialiser
    f[SlotBase(k) + kSlotFlagBytes] = 1;                  // header-object default byte (sub_827DBBF8)
  }
  PtFixChecksums(&f);
  return f;
}

bool PtSlotUsed(const std::vector<uint8_t>& f, size_t slot) {
  if (slot >= kPtSlotCount || f.size() != kPtFileSize) return false;
  return Be32(f, SlotBase(slot) + kSlotFlag) != 0 && Be32(f, SlotBase(slot) + kSlotWidth) != 0;
}

uint32_t PtSlotId(const std::vector<uint8_t>& f, size_t slot) { return Be32(f, SlotBase(slot) + kSlotSum); }

std::vector<uint8_t> PtDecodeCanvas(const std::vector<uint8_t>& f, size_t slot) {
  std::vector<uint8_t> rgba(kCanvasBytes);
  const size_t b = SlotBase(slot) + kSlotCanvas;
  const size_t size = PtSlotWidth(f, slot) == 128 && PtSlotHeight(f, slot) == 128 ? 128 : 256;
  const size_t scale = 256 / size;
  for (size_t y = 0; y < size; ++y) {
    for (size_t x = 0; x < size; ++x) {
      const uint32_t v = Be32(f, b + (y * size + x) * 4);  // 0xAARRGGBB, compact row stride for 128 slots
      const uint8_t pixel[4] = {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), uint8_t(v >> 24)};
      for (size_t dy = 0; dy < scale; ++dy)
        for (size_t dx = 0; dx < scale; ++dx)
          std::copy(pixel, pixel + 4, rgba.begin() + static_cast<std::ptrdiff_t>(((y * scale + dy) * 256 + x * scale + dx) * 4));
    }
  }
  return rgba;
}

std::vector<uint8_t> PtDecodePalettePicture(const std::vector<uint8_t>& f, size_t slot) {
  std::vector<uint8_t> rgba(kCanvasBytes);
  const size_t b = SlotBase(slot);
  std::array<uint32_t, 256> pal{};
  for (size_t i = 0; i < 256; ++i) pal[i] = Be32(f, b + kSlotPalette + i * 4);
  const size_t size = PtSlotWidth(f, slot) == 128 && PtSlotHeight(f, slot) == 128 ? 128 : 256;
  const size_t scale = 256 / size;
  for (size_t y = 0; y < size; ++y) {
    for (size_t x = 0; x < size; ++x) {
      const uint32_t v = pal[f[b + kSlotIndices + y * size + x]];
      const uint8_t pixel[4] = {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), uint8_t(v >> 24)};
      for (size_t dy = 0; dy < scale; ++dy)
        for (size_t dx = 0; dx < scale; ++dx)
          std::copy(pixel, pixel + 4, rgba.begin() + static_cast<std::ptrdiff_t>(((y * scale + dy) * 256 + x * scale + dx) * 4));
    }
  }
  return rgba;
}

void PtClearSlot(std::vector<uint8_t>* f, size_t slot) {
  const size_t b = SlotBase(slot);
  std::fill(f->begin() + static_cast<std::ptrdiff_t>(b), f->begin() + static_cast<std::ptrdiff_t>(b + kPtSlotSize), 0);
  PutBe32(f, b + kSlotVersion, kPtVersion);
  (*f)[b + kSlotFlagBytes] = 1;
  PtFixChecksums(f);
}

namespace {

struct Box {
  std::vector<std::pair<uint32_t, uint32_t>> colors;  // packed ARGB, pixel count
  uint64_t total = 0;
  int lo[4], hi[4];
  void Measure() {
    for (int c = 0; c < 4; ++c) { lo[c] = 255; hi[c] = 0; }
    total = 0;
    for (const auto& [v, n] : colors) {
      const int ch[4] = {int(v >> 24), int((v >> 16) & 255), int((v >> 8) & 255), int(v & 255)};
      for (int c = 0; c < 4; ++c) { lo[c] = std::min(lo[c], ch[c]); hi[c] = std::max(hi[c], ch[c]); }
      total += n;
    }
  }
  int WidestChannel() const {
    int best = 0, range = -1;
    for (int c = 0; c < 4; ++c) if (hi[c] - lo[c] > range) { range = hi[c] - lo[c]; best = c; }
    return best;
  }
};

}  // namespace

namespace {

void QuantizeToSize(const std::vector<uint8_t>& rgba, int size, std::vector<uint32_t>* palette,
                    std::vector<uint8_t>* indices) {
  const size_t pixel_count = size_t(size) * size;
  std::unordered_map<uint32_t, uint32_t> histogram;
  for (size_t i = 0; i < pixel_count; ++i) {
    uint32_t a = rgba[i * 4 + 3];
    uint32_t v = (a << 24) | (uint32_t(rgba[i * 4]) << 16) | (uint32_t(rgba[i * 4 + 1]) << 8) | rgba[i * 4 + 2];
    if (a == 0) v = 0;
    ++histogram[v];
  }
  palette->assign(256, 0);
  std::unordered_map<uint32_t, uint8_t> lookup;
  if (histogram.size() <= 256) {
    // Exact. Fully transparent black first, as the game's own palettes do.
    std::vector<uint32_t> keys;
    for (const auto& kv : histogram) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (size_t i = 0; i < keys.size(); ++i) { (*palette)[i] = keys[i]; lookup[keys[i]] = uint8_t(i); }
  } else {
    // Median cut over the distinct colours, weighted by pixel count. Entry 0 stays transparent when used.
    const bool has_clear = histogram.count(0) != 0;
    Box first;
    for (const auto& kv : histogram) if (kv.first != 0) first.colors.emplace_back(kv.first, kv.second);
    std::vector<Box> boxes;
    first.Measure();
    boxes.push_back(std::move(first));
    const size_t limit = has_clear ? 255 : 256;
    while (boxes.size() < limit) {
      size_t pick = boxes.size();
      uint64_t best = 0;
      for (size_t i = 0; i < boxes.size(); ++i) {
        if (boxes[i].colors.size() < 2) continue;
        const int c = boxes[i].WidestChannel();
        const uint64_t score = uint64_t(boxes[i].hi[c] - boxes[i].lo[c] + 1) * boxes[i].total;
        if (score > best) { best = score; pick = i; }
      }
      if (pick == boxes.size()) break;
      Box& box = boxes[pick];
      const int c = box.WidestChannel();
      const int shift = 24 - 8 * c;
      std::sort(box.colors.begin(), box.colors.end(), [shift](const auto& a, const auto& b) {
        return ((a.first >> shift) & 255) < ((b.first >> shift) & 255);
      });
      uint64_t acc = 0;
      size_t cut = 1;
      for (size_t i = 0; i + 1 < box.colors.size(); ++i) {
        acc += box.colors[i].second;
        cut = i + 1;
        if (acc * 2 >= box.total) break;
      }
      Box other;
      other.colors.assign(box.colors.begin() + static_cast<std::ptrdiff_t>(cut), box.colors.end());
      box.colors.resize(cut);
      box.Measure();
      other.Measure();
      boxes.push_back(std::move(other));
    }
    size_t at = 0;
    if (has_clear) at = 1;  // palette[0] = 0 (transparent)
    for (const Box& box : boxes) {
      uint64_t sum[4] = {0, 0, 0, 0};
      for (const auto& [v, n] : box.colors) {
        sum[0] += uint64_t(v >> 24) * n; sum[1] += uint64_t((v >> 16) & 255) * n;
        sum[2] += uint64_t((v >> 8) & 255) * n; sum[3] += uint64_t(v & 255) * n;
      }
      const uint64_t n = std::max<uint64_t>(box.total, 1);
      uint32_t a = uint32_t((sum[0] + n / 2) / n);
      if (a == 0) a = 1;  // alpha 0 is reserved for the clear entry
      (*palette)[at++] = (a << 24) | (uint32_t((sum[1] + n / 2) / n) << 16) | (uint32_t((sum[2] + n / 2) / n) << 8) |
                         uint32_t((sum[3] + n / 2) / n);
    }
    const size_t used = at;
    for (const auto& kv : histogram) {
      if (kv.first == 0) { lookup[0] = 0; continue; }
      const int ch[4] = {int(kv.first >> 24), int((kv.first >> 16) & 255), int((kv.first >> 8) & 255), int(kv.first & 255)};
      int64_t best = INT64_MAX;
      uint8_t pick = 0;
      for (size_t p = has_clear ? 1 : 0; p < used; ++p) {
        const uint32_t pv = (*palette)[p];
        const int pc[4] = {int(pv >> 24), int((pv >> 16) & 255), int((pv >> 8) & 255), int(pv & 255)};
        int64_t d = 0;
        for (int c = 0; c < 4; ++c) d += int64_t(ch[c] - pc[c]) * (ch[c] - pc[c]) * (c == 0 ? 2 : 1);
        if (d < best) { best = d; pick = uint8_t(p); }
      }
      lookup[kv.first] = pick;
    }
  }
  indices->resize(pixel_count);
  for (size_t i = 0; i < pixel_count; ++i) {
    uint32_t a = rgba[i * 4 + 3];
    uint32_t v = (a << 24) | (uint32_t(rgba[i * 4]) << 16) | (uint32_t(rgba[i * 4 + 1]) << 8) | rgba[i * 4 + 2];
    if (a == 0) v = 0;
    (*indices)[i] = lookup[v];
  }
}

}  // namespace

void QuantizeTo256(const std::vector<uint8_t>& rgba, std::vector<uint32_t>* palette,
                   std::vector<uint8_t>* indices) {
  QuantizeToSize(rgba, 256, palette, indices);
}

void PtWriteSlot(std::vector<uint8_t>* f, size_t slot, const std::vector<uint8_t>& rgba, uint32_t token,
                 const PtStamp& stamp, uint32_t game_stamp, int width, int height) {
  const size_t b = SlotBase(slot);
  const int image_size = width == 128 && height == 128 ? 128 : 256;
  std::vector<uint8_t> pixels(size_t(image_size) * image_size * 4);
  const size_t source_scale = 256 / image_size;
  for (int y = 0; y < image_size; ++y) {
    for (int x = 0; x < image_size; ++x) {
      const size_t from = (size_t(y * source_scale) * 256 + size_t(x * source_scale)) * 4;
      const size_t to = (size_t(y) * image_size + x) * 4;
      std::copy(rgba.begin() + static_cast<std::ptrdiff_t>(from),
                rgba.begin() + static_cast<std::ptrdiff_t>(from + 4),
                pixels.begin() + static_cast<std::ptrdiff_t>(to));
    }
  }
  std::fill(f->begin() + static_cast<std::ptrdiff_t>(b), f->begin() + static_cast<std::ptrdiff_t>(b + kPtSlotSize), 0);
  PutBe32(f, b + kSlotFlag, 1);
  PutBe32(f, b + kSlotVersion, kPtVersion);
  PutBe32(f, b + kSlotGameStamp, game_stamp);
  PutBe32(f, b + 44, token);
  (*f)[b + kSlotFlagBytes] = 1;
  (*f)[b + kSlotFlagBytes + 4] = 1;
  PutBe32(f, b + kSlotWidth, static_cast<uint32_t>(width));
  PutBe32(f, b + kSlotHeight, static_cast<uint32_t>(height));
  for (size_t i = 0; i < pixels.size() / 4; ++i) {
    uint32_t a = pixels[i * 4 + 3];
    uint32_t v = a == 0 ? 0 : (a << 24) | (uint32_t(pixels[i * 4]) << 16) | (uint32_t(pixels[i * 4 + 1]) << 8) | pixels[i * 4 + 2];
    PutBe32(f, b + kSlotCanvas + i * 4, v);
  }
  const size_t t = b + kSlotTga;
  (*f)[t + 1] = 1;
  (*f)[t + 2] = 1;
  PutBe16(f, t + 5, 256);
  (*f)[t + 7] = 32;
  PutBe16(f, t + 12, static_cast<uint32_t>(width));
  PutBe16(f, t + 14, static_cast<uint32_t>(height));
  (*f)[t + 16] = 8;
  std::vector<uint32_t> palette;
  std::vector<uint8_t> indices;
  QuantizeToSize(pixels, image_size, &palette, &indices);
  for (size_t i = 0; i < 256; ++i) PutBe32(f, b + kSlotPalette + i * 4, palette[i]);
  std::copy(indices.begin(), indices.end(), f->begin() + static_cast<std::ptrdiff_t>(b + kSlotIndices));
  SetStamp(f, b, stamp);
  PtFixChecksums(f);
}

bool PtSlotBuiltIn(const std::vector<uint8_t>& f, size_t slot) { return Be32(f, SlotBase(slot) + 4) != 0; }
uint32_t PtSlotToken(const std::vector<uint8_t>& f, size_t slot) { return Be32(f, SlotBase(slot) + 44); }
uint32_t PtSlotGameStamp(const std::vector<uint8_t>& f, size_t slot) { return Be32(f, SlotBase(slot) + kSlotGameStamp); }
uint32_t PtSlotWidth(const std::vector<uint8_t>& f, size_t slot) { return Be32(f, SlotBase(slot) + kSlotWidth); }
uint32_t PtSlotHeight(const std::vector<uint8_t>& f, size_t slot) { return Be32(f, SlotBase(slot) + kSlotHeight); }
PtStamp PtSlotStamp(const std::vector<uint8_t>& f, size_t slot) {
  const size_t o = SlotBase(slot) + kSlotStamp;
  PtStamp s;
  s.year = static_cast<uint16_t>(Be16(f, o));
  s.month = f[o + 2]; s.day = f[o + 3]; s.hour = f[o + 4]; s.minute = f[o + 5]; s.second = f[o + 6]; s.weekday = f[o + 7];
  return s;
}
void PtStampAll(std::vector<uint8_t>* f, const PtStamp& stamp) {
  for (size_t k = 0; k < kPtSlotCount; ++k) SetStamp(f, SlotBase(k), stamp);
  PtFixChecksums(f);
}

bool PtPackFile(const std::vector<uint8_t>& plain, std::vector<uint8_t>* file) {
  file->clear();
  const size_t quarter = plain.size() / 4;
  for (int i = 0; i < 4; ++i) {
    const size_t begin = quarter * i, end = i == 3 ? plain.size() : begin + quarter;
    mz_ulong capacity = mz_compressBound(static_cast<mz_ulong>(end - begin));
    std::vector<uint8_t> z(capacity);
    mz_ulong size = capacity;
    if (mz_compress2(z.data(), &size, plain.data() + begin, static_cast<mz_ulong>(end - begin), 6) != MZ_OK) return false;
    z.resize(size);
    std::vector<uint8_t> head(16, 0);
    std::memcpy(head.data(), "ZLIB", 4);
    PutLe32(&head, 4, 0x3012);
    PutLe32(&head, 8, static_cast<uint32_t>(z.size()));
    PutLe32(&head, 12, static_cast<uint32_t>(end - begin));
    file->insert(file->end(), head.begin(), head.end());
    file->insert(file->end(), z.begin(), z.end());
  }
  return true;
}

bool PtUnpackFile(const std::vector<uint8_t>& file, std::vector<uint8_t>* plain) {
  plain->clear();
  size_t at = 0;
  while (at + 16 <= file.size() && std::memcmp(file.data() + at, "ZLIB", 4) == 0) {
    if (Le32(file, at + 4) != 0x3012) return false;
    const uint32_t csize = Le32(file, at + 8), psize = Le32(file, at + 12);
    if (at + 16 + uint64_t{csize} > file.size() || psize > 64u * 1024 * 1024) return false;
    std::vector<uint8_t> out(psize);
    mz_ulong got = psize;
    if (mz_uncompress(out.data(), &got, file.data() + at + 16, csize) != MZ_OK || got != psize) return false;
    plain->insert(plain->end(), out.begin(), out.end());
    at += 16 + csize;
  }
  return !plain->empty();
}

std::vector<uint8_t> PtMakeHeader(const std::string& display_name, const std::string& package_name) {
  std::vector<uint8_t> h(0x148, 0);
  PutBe32(&h, 0, 1);
  PutBe32(&h, 4, 1);
  size_t o = 8;
  for (unsigned char c : display_name) {
    if (o + 2 > 0x108) break;
    h[o++] = 0;
    h[o++] = c;
  }
  for (size_t i = 0; i < package_name.size() && 0x108 + i < 0x138; ++i) h[0x108 + i] = static_cast<uint8_t>(package_name[i]);
  PutBe32(&h, 0x140, 0x545408B2);
  return h;
}

namespace {

std::vector<uint8_t> FitRgbaToSize(const uint8_t* src, int w, int h, int size) {
  std::vector<uint8_t> out(size_t(size) * size * 4, 0);
  if (w <= 0 || h <= 0) return out;
  const double scale = std::min(double(size) / w, double(size) / h);
  const int tw = std::max(1, std::min(size, int(std::lround(w * scale))));
  const int th = std::max(1, std::min(size, int(std::lround(h * scale))));
  const int ox = (size - tw) / 2, oy = (size - th) / 2;
  // Area (box) average in premultiplied space: sharp when shrinking, plain nearest/bilinear feel when enlarging.
  for (int y = 0; y < th; ++y) {
    const double y0 = double(y) * h / th, y1 = double(y + 1) * h / th;
    for (int x = 0; x < tw; ++x) {
      const double x0 = double(x) * w / tw, x1 = double(x + 1) * w / tw;
      double r = 0, g = 0, b = 0, a = 0, weight = 0;
      const int iy0 = int(std::floor(y0)), iy1 = std::min(h, int(std::ceil(y1)));
      const int ix0 = int(std::floor(x0)), ix1 = std::min(w, int(std::ceil(x1)));
      for (int sy = iy0; sy < iy1; ++sy) {
        const double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
        for (int sx = ix0; sx < ix1; ++sx) {
          const double wx = std::min<double>(sx + 1, x1) - std::max<double>(sx, x0);
          const double wt = wx * wy;
          const uint8_t* p = src + (size_t(sy) * w + sx) * 4;
          const double pa = p[3] / 255.0;
          r += p[0] * pa * wt; g += p[1] * pa * wt; b += p[2] * pa * wt; a += pa * wt; weight += wt;
        }
      }
      uint8_t* o = &out[(size_t(y + oy) * size + (x + ox)) * 4];
      if (weight > 0 && a > 0) {
        o[0] = uint8_t(std::clamp(std::lround(r / a), 0l, 255l));
        o[1] = uint8_t(std::clamp(std::lround(g / a), 0l, 255l));
        o[2] = uint8_t(std::clamp(std::lround(b / a), 0l, 255l));
        o[3] = uint8_t(std::clamp(std::lround(a / weight * 255.0), 0l, 255l));
      }
    }
  }
  return out;
}

}  // namespace

std::vector<uint8_t> FitRgbaTo256(const uint8_t* src, int w, int h) {
  return FitRgbaToSize(src, w, h, 256);
}

std::vector<uint8_t> FitRgbaTo128(const uint8_t* src, int w, int h) {
  return FitRgbaToSize(src, w, h, 128);
}

std::vector<uint8_t> ExpandRgba128To256(const std::vector<uint8_t>& rgba) {
  constexpr size_t source_bytes = 128 * 128 * 4;
  if (rgba.size() != source_bytes) return {};
  std::vector<uint8_t> out(kCanvasBytes);
  for (size_t y = 0; y < 128; ++y) {
    for (size_t x = 0; x < 128; ++x) {
      const size_t from = (y * 128 + x) * 4;
      for (size_t dy = 0; dy < 2; ++dy)
        for (size_t dx = 0; dx < 2; ++dx) {
          const size_t to = ((y * 2 + dy) * 256 + x * 2 + dx) * 4;
          std::copy(rgba.begin() + static_cast<std::ptrdiff_t>(from),
                    rgba.begin() + static_cast<std::ptrdiff_t>(from + 4),
                    out.begin() + static_cast<std::ptrdiff_t>(to));
        }
    }
  }
  return out;
}

}  // namespace wwe2k14::launcher::internal
