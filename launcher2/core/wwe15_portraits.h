#pragma once

// WWE 2K15 import, step F: menu portraits. The player's 2K15 menuHD2/menuSD2 portraits (raw
// 32-bit pixels, 256 or 512 square) become the DXT5 DDS sheets WWE 2K14's add-on DLC_HD/SD
// pacs carry (SSFA/SSFB/SSFC). Own encoder; the python route used Pillow, so acceptance is a
// decoded-pixel and format check, not byte identity.

#include <cstdint>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

using WWE15PortraitBytes = std::vector<uint8_t>;

// Decoded 2K15 portrait entry (after BPE): big-endian u16 side/256 at +12, pixels at +44 in
// R,A,B,G order, bottom row first. Output: top-down RGBA.
bool DecodeWWE15PortraitPixels(const WWE15PortraitBytes& raw, int* side, WWE15PortraitBytes* rgba,
                               std::string* error);
// Area-weighted box resize, colour and alpha each on their own (no premultiplication).
WWE15PortraitBytes ResizeWWE15Box(const WWE15PortraitBytes& rgba, int width, int height, int new_width,
                                  int new_height);
// DXT5 DDS with every mip level (each level resized from the full image), header as Yuke's sheets.
WWE15PortraitBytes EncodeWWE15Dxt5Dds(const WWE15PortraitBytes& rgba, int width, int height);
// Top mip of a DXT5 DDS back to RGBA (checks and tests).
bool DecodeWWE15Dxt5Dds(const WWE15PortraitBytes& dds, int* width, int* height, int* mips,
                        WWE15PortraitBytes* rgba, std::string* error);
// One sheet: decoded 2K15 entry -> DDS of the wanted size (SSFC 256x256; HD 512x512; SD 512x256).
bool ConvertWWE15Portrait(const WWE15PortraitBytes& raw, int want_width, int want_height,
                          WWE15PortraitBytes* dds, std::string* error);

}  // namespace wwe2k14::launcher
