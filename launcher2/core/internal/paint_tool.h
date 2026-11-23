#pragma once

// WWE 2K14 Paint Tool save (00PaintTool.pt): the custom-logo slots used by the CAW editor
// ("TATTOOS -> PAINT TOOL DATA"). Layout (all big-endian), proven from the game's own code
// (generated/wwe2k14_recomp.59.cpp sub_829A0B58, .161.cpp sub_833D41F8/sub_833D4590/sub_833D4A20)
// and the WWE '13 sample of the same format family:
//   0x000000 u32 magic 0x02A78E9A, u32 version 7
//   slot k at 8 + k*0x50468 (20 slots), then u32 total checksum at 0x645828; file = 0x64582C bytes.
//   slot: +0 used flag, +8 version, +12 game stamp, +24..39 GUID, +48..55 flag bytes, +64/+68 width/height,
//         +72 canvas capacity 256x256 ARGB words, +0x40048 TGA header (18 B + 2 pad), +0x4005C 256 palette ARGB words,
//         +0x4045C index-plane capacity 65536 bytes, +0x5045C 8-byte stamp, +0x50464 u32 slot sum (the content id).
// A 128x128 logo occupies the first 128x128 words/bytes of those buffers with a 128-pixel row stride; the unused
// remainder is zero. 256x256 logos use the full buffers with a 256-pixel row stride.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wwe2k14::launcher::internal {

constexpr size_t kPtSlotCount = 20;
constexpr size_t kPtSlotSize = 0x50468;
constexpr size_t kPtFileSize = 8 + kPtSlotCount * kPtSlotSize + 4;  // 0x64582C
constexpr uint32_t kPtMagic = 0x02A78E9A;
constexpr uint32_t kPtVersion = 7;
constexpr int kPtImageSize = 256;

struct PtStamp {
  uint16_t year = 2026;
  uint8_t month = 1, day = 1, hour = 0, minute = 0, second = 0, weekday = 0;
};

// 2K14 plain file: validates size, magic, version and the total checksum.
bool PtIsValid(const std::vector<uint8_t>& plain, std::string* why = nullptr);
std::vector<uint8_t> PtMakeEmpty();
bool PtSlotUsed(const std::vector<uint8_t>& plain, size_t slot);
uint32_t PtSlotId(const std::vector<uint8_t>& plain, size_t slot);   // stored content id
uint32_t PtComputeSlotSum(const std::vector<uint8_t>& plain, size_t slot);
uint32_t PtComputeTotal(const std::vector<uint8_t>& plain);          // from freshly computed slot sums
// Canvas decoded as 256x256 RGBA8 (R,G,B,A byte order), straight alpha; 128x128 stored canvases are upscaled 2x.
std::vector<uint8_t> PtDecodeCanvas(const std::vector<uint8_t>& plain, size_t slot);
// Palette picture as RGBA8; must match the canvas closely for a consistent slot.
std::vector<uint8_t> PtDecodePalettePicture(const std::vector<uint8_t>& plain, size_t slot);
// Writes a full slot (canvas, 8-bit palette picture, header, stamp) from canonical 256x256 RGBA8; refreshes checksums.
// width/height accepts 128 or 256 square. For 128, input pixels are sampled from the upscaled canvas and packed at stride 128.
// game_stamp (+12) and token (+44) are the game's per-logo fields (token differs per logo; any 32-bit value).
void PtWriteSlot(std::vector<uint8_t>* plain, size_t slot, const std::vector<uint8_t>& rgba, uint32_t token,
                 const PtStamp& stamp, uint32_t game_stamp = 0x003E5C12, int width = 256, int height = 256);
uint32_t PtSlotWidth(const std::vector<uint8_t>& plain, size_t slot);
uint32_t PtSlotHeight(const std::vector<uint8_t>& plain, size_t slot);
// Slot header fields, to copy a slot exactly.
bool PtSlotBuiltIn(const std::vector<uint8_t>& plain, size_t slot);  // the game's own logo (+4 != 0)
uint32_t PtSlotToken(const std::vector<uint8_t>& plain, size_t slot);
uint32_t PtSlotGameStamp(const std::vector<uint8_t>& plain, size_t slot);
PtStamp PtSlotStamp(const std::vector<uint8_t>& plain, size_t slot);
void PtStampAll(std::vector<uint8_t>* plain, const PtStamp& stamp);
void PtClearSlot(std::vector<uint8_t>* plain, size_t slot);
void PtFixChecksums(std::vector<uint8_t>* plain);  // all slot sums + the total

// Container as the game writes it: FOUR 'ZLIB' chunks (u32le 0x3012, compressed size, plain size), each a quarter
// of the file (last takes the remainder). Verified on the owner sample; a single chunk is rejected as "damaged".
bool PtPackFile(const std::vector<uint8_t>& plain, std::vector<uint8_t>* file);
bool PtUnpackFile(const std::vector<uint8_t>& file, std::vector<uint8_t>* plain);
// 0x148-byte sidecar header for "00PaintTool.pt" (display "01.CUSTOM LOGOS").
std::vector<uint8_t> PtMakeHeader(const std::string& display_name, const std::string& package_name);

// Image import: any PNG/JPG/BMP/TGA/GIF -> 256x256 RGBA8, fitted (never stretched) and centred on a
// transparent canvas. Fails with a player-readable message.
bool LoadImageFitted256(const std::filesystem::path& file, std::vector<uint8_t>* rgba, std::string* error);
// Fits into 128x128 and expands nearest-neighbour to the 256x256 RGBA canvas used by LogoImage and PtWriteSlot.
bool LoadImageFitted128(const std::filesystem::path& file, std::vector<uint8_t>* rgba, std::string* error);
// Same fitting for already decoded pixels (testing).
std::vector<uint8_t> FitRgbaTo256(const uint8_t* rgba, int width, int height);
std::vector<uint8_t> FitRgbaTo128(const uint8_t* rgba, int width, int height);
std::vector<uint8_t> ExpandRgba128To256(const std::vector<uint8_t>& rgba);
// Inverse of the palette step, exposed for tests: quantise RGBA8 256x256 to <=256 colours.
void QuantizeTo256(const std::vector<uint8_t>& rgba, std::vector<uint32_t>* palette_argb,
                   std::vector<uint8_t>* indices);
// Writes a PNG (RGBA8) with stored zlib blocks; used for thumbnails/tests.
bool WritePngRgba(const std::filesystem::path& file, const uint8_t* rgba, int width, int height);

}  // namespace wwe2k14::launcher::internal
