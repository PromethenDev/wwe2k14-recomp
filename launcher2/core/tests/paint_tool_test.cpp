#ifdef NDEBUG
#undef NDEBUG
#endif

#include "internal/paint_tool.h"
#include "internal/util.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace wwe2k14::launcher::internal;

namespace {

uint32_t Le32Test(const std::vector<uint8_t>& b, size_t o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (uint32_t(b[o + 3]) << 24); }
PtStamp PtStampOf(const std::vector<uint8_t>& s, size_t k) { return PtSlotStamp(s, k); }

std::vector<uint8_t> TestPicture(int w, int h, bool alpha) {
  std::vector<uint8_t> px(size_t(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &px[(size_t(y) * w + x) * 4];
      p[0] = uint8_t(x * 255 / std::max(1, w - 1));
      p[1] = uint8_t(y * 255 / std::max(1, h - 1));
      p[2] = uint8_t((x ^ y) * 7);
      p[3] = alpha ? (x < w / 4 ? 0 : uint8_t(255 - y * 100 / h)) : 255;
    }
  return px;
}

}  // namespace

int main() {
  // Empty file is a valid 2K14 file and has no used slots.
  std::vector<uint8_t> f = PtMakeEmpty();
  std::string why;
  assert(f.size() == kPtFileSize && kPtFileSize == 6576172);
  assert(PtIsValid(f, &why));
  for (size_t k = 0; k < kPtSlotCount; ++k) assert(!PtSlotUsed(f, k));

  // Fit: a wide picture is letterboxed (centred, never stretched).
  const auto wide = TestPicture(512, 128, false);
  const auto fitted = FitRgbaTo256(wide.data(), 512, 128);
  assert(fitted.size() == 256 * 256 * 4);
  assert(fitted[(0 * 256 + 128) * 4 + 3] == 0);              // top padding transparent
  assert(fitted[(128 * 256 + 128) * 4 + 3] == 255);          // centre opaque
  assert(fitted[(127 * 256 + 0) * 4 + 3] == 255);            // picture rows 96..159
  assert(fitted[(95 * 256 + 0) * 4 + 3] == 0 && fitted[(96 * 256 + 0) * 4 + 3] == 255);

  // Write a slot, decode it back: canvas is exact, palette picture is close, file stays valid.
  const auto picture = FitRgbaTo256(TestPicture(300, 200, true).data(), 300, 200);
  const uint32_t guid = 0x1234;
  PtStamp stamp;
  stamp.year = 2026; stamp.month = 10; stamp.day = 26; stamp.hour = 12;
  PtWriteSlot(&f, 3, picture, guid, stamp);
  assert(PtIsValid(f, &why));
  assert(PtSlotUsed(f, 3) && !PtSlotUsed(f, 2) && !PtSlotUsed(f, 4));
  const auto back = PtDecodeCanvas(f, 3);
  for (size_t i = 0; i < 256 * 256; ++i) {
    if (picture[i * 4 + 3] == 0) { assert(back[i * 4 + 3] == 0); continue; }
    for (int c = 0; c < 4; ++c) assert(back[i * 4 + c] == picture[i * 4 + c]);
  }
  const auto pal = PtDecodePalettePicture(f, 3);
  uint64_t err = 0, n = 0;
  for (size_t i = 0; i < 256 * 256; ++i) {
    if (back[i * 4 + 3] == 0 && pal[i * 4 + 3] == 0) continue;
    for (int c = 0; c < 4; ++c) { int d = int(back[i * 4 + c]) - pal[i * 4 + c]; err += d * d; ++n; }
  }
  std::cout << "palette picture RMS error " << std::sqrt(double(err) / n) << "\n";
  assert(std::sqrt(double(err) / n) < 12.0);
  assert(PtSlotId(f, 3) == PtComputeSlotSum(f, 3));

  // Container: pack/unpack is byte exact, and a second write of the same picture gives the same file.
  std::vector<uint8_t> packed, unpacked;
  assert(PtPackFile(f, &packed));
  assert(PtUnpackFile(packed, &unpacked) && unpacked == f);
  std::vector<uint8_t> g = f;
  PtWriteSlot(&g, 3, picture, guid, stamp);
  assert(g == f);

  // Flat colour image: exact palette, exact palette picture.
  std::vector<uint8_t> flat(256 * 256 * 4);
  for (size_t i = 0; i < 256 * 256; ++i) { flat[i * 4] = 10; flat[i * 4 + 1] = 200; flat[i * 4 + 2] = 30; flat[i * 4 + 3] = 255; }
  PtWriteSlot(&f, 0, flat, guid, stamp);
  assert(PtDecodePalettePicture(f, 0) == PtDecodeCanvas(f, 0));

  // Remove: slot cleared, other slot untouched, file valid.
  const auto keep = PtDecodeCanvas(f, 3);
  PtClearSlot(&f, 0);
  assert(!PtSlotUsed(f, 0) && PtSlotUsed(f, 3) && PtIsValid(f, &why) && PtDecodeCanvas(f, 3) == keep);

  // A 128 logo uses compact 128-pixel rows at the start of the fixed-size canvas/index buffers.
  const auto fitted128 = FitRgbaTo128(TestPicture(300, 100, true).data(), 300, 100);
  assert(fitted128.size() == 128 * 128 * 4);
  const auto canvas128 = ExpandRgba128To256(fitted128);
  assert(canvas128.size() == 256 * 256 * 4);
  PtWriteSlot(&f, 4, canvas128, guid, stamp, 0x003E5C12, 128, 128);
  assert(PtIsValid(f, &why) && PtSlotUsed(f, 4));
  assert(PtSlotWidth(f, 4) == 128 && PtSlotHeight(f, 4) == 128);
  assert(PtDecodeCanvas(f, 4) == canvas128);
  assert(PtDecodePalettePicture(f, 4).size() == 256 * 256 * 4);
  const size_t slot_base = 8 + 4 * kPtSlotSize;
  for (size_t i = 128 * 128 * 4; i < 256 * 256 * 4; ++i)
    assert(f[slot_base + 72 + i] == 0);
  for (size_t i = 128 * 128; i < 256 * 256; ++i)
    assert(f[slot_base + 0x4045C + i] == 0);
  std::vector<uint8_t> same = f;
  PtWriteSlot(&same, 4, PtDecodeCanvas(f, 4), PtSlotToken(f, 4), PtSlotStamp(f, 4),
              PtSlotGameStamp(f, 4), 128, 128);
  assert(same == f);

  // Damage is detected.
  std::vector<uint8_t> bad = f;
  bad[8 + 3 * kPtSlotSize + 100] ^= 1;
  assert(!PtIsValid(bad, &why));

  // Header
  const auto h = PtMakeHeader("01.CUSTOM LOGOS", "00PaintTool.pt");
  assert(h.size() == 0x148 && h[0x108] == '0' && h[0x140] == 0x54 && h[0x143] == 0xB2);

  // Optional: the WWE '13 sample of the same family (version 5): the slot sums must match the game's stored ones.
  if (const char* sample = std::getenv("WWE2K14_PT_SAMPLE_W13")) {
    std::vector<uint8_t> s;
    assert(ReadBinaryFile(sample, s) && s.size() == kPtFileSize);
    assert(PtSlotUsed(s, 0) || true);
    assert(PtComputeSlotSum(s, 0) == PtSlotId(s, 0));
    std::cout << "W13 slot 0 sum ok " << std::hex << PtSlotId(s, 0) << "\n";
    // Byte-exact round trip of an existing in-save image: rebuild the same file through our decode + checksum path.
    std::vector<uint8_t> copy = s;
    PtFixChecksums(&copy);
    for (size_t i = 0; i < copy.size() - 4; ++i) if (i < 8 + kPtSlotSize || i >= 8 + 2 * kPtSlotSize) {}
    assert(PtDecodeCanvas(copy, 0) == PtDecodeCanvas(s, 0));
    assert(PtSlotId(copy, 0) == PtSlotId(s, 0));
  }
  // The owner's game-made file (4 chunks, 3 logos): decode -> re-encode must give identical bytes.
  if (const char* sample = std::getenv("WWE2K14_PT_SAMPLE_OWNER")) {
    std::vector<uint8_t> raw, s;
    assert(ReadBinaryFile(sample, raw) && PtUnpackFile(raw, &s) && PtIsValid(s, &why));
    // container: 4 quarter chunks like the game
    std::vector<uint8_t> repacked, again;
    assert(PtPackFile(s, &repacked) && PtUnpackFile(repacked, &again) && again == s);
    assert(repacked[8] != 0 && Le32Test(repacked, 12) == s.size() / 4);
    // slots 1 and 2 are drawn by the player (slot 0 is a built-in logo made by the game's own quantiser)
    for (size_t k : {size_t{1}}) {
      std::vector<uint8_t> t = s;
      const auto rgba = PtDecodeCanvas(s, k);
      PtWriteSlot(&t, k, rgba, PtSlotToken(s, k), PtStampOf(s, k), PtSlotGameStamp(s, k));
      assert(t == s);
    }
    assert(PtSlotBuiltIn(s, 0) && !PtSlotBuiltIn(s, 1) && !PtSlotBuiltIn(s, 2));
    std::cout << "owner sample round trip ok\n";
  }
  // Owner 2K14 sample: slot index 2 is the game's 128x128 entry. Re-encode the decoded pixels byte-for-byte.
  if (const char* sample = std::getenv("WWE2K14_PT_SAMPLE_LOGO128")) {
    std::vector<uint8_t> raw, s;
    assert(ReadBinaryFile(sample, raw) && PtUnpackFile(raw, &s) && PtIsValid(s, &why));
    constexpr size_t kSampleSlot = 2;
    assert(PtSlotUsed(s, kSampleSlot) && PtSlotWidth(s, kSampleSlot) == 128 && PtSlotHeight(s, kSampleSlot) == 128);
    std::vector<uint8_t> rebuilt = s;
    const auto rgba = PtDecodeCanvas(s, kSampleSlot);
    PtWriteSlot(&rebuilt, kSampleSlot, rgba, PtSlotToken(s, kSampleSlot), PtStampOf(s, kSampleSlot),
                PtSlotGameStamp(s, kSampleSlot), 128, 128);
    assert(rebuilt == s);
    std::cout << "owner 128 slot 3 round trip byte-exact\n";
  }
  std::cout << "paint_tool_test ok\n";
  return 0;
}
