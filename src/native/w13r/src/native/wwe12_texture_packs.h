// WWE '12 texture dumps and texture packs, for modders (docs/mod_maker.md).
//
// Adapted from the texture packs of Kaiko's SvR 2011 port (port/src/native/texture_packs.*, used with
// his permission and credit; the PNG writer and the PNG/DDS readers are his), plugged into re:Blue's
// texture mirror (gpu/native_texture_mirror.cpp, Build2DMirror).
//
// A texture is known by its content hash: XXH3 of its top level's blocks, untiled and tightly packed,
// as the GPU reads them (DXT1/3/5 and 8:8:8:8), so the same texture hashes the same on every run.
//
// Dumps (cvar wwe12_texture_dump): every texture shown is written once to "texture_dumps" beside the
// exe, as <width>x<height>_<format>_<hash16>.png, with the fetch constant's swizzle applied (as it
// looks in the game).
// Packs (cvar wwe12_texture_packs, on by default): any file under "textures" beside the exe (any
// subfolder, mods included) whose name ends with _<hash16> (or is <hash16>) .png or .dds replaces that
// texture, at any resolution. PNGs get a box-filtered mip chain; a DDS (DXT1/3/5 or 32-bit) keeps its
// own mips. Files are read the first time the texture is shown. Of two files with one hash, the first
// one by path wins.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace wwe12::texture_packs {

enum class Format : uint8_t { kRGBA8, kBC1, kBC2, kBC3 };

struct Replacement {
  uint32_t width = 0, height = 0;
  Format format = Format::kRGBA8;
  // Each level tightly packed: rows of pixels (RGBA8) or of 4x4 blocks (BC).
  std::vector<std::vector<uint8_t>> levels;
};

// Content hashes are needed (dumping, or at least one pack file). Starts everything on first call.
bool Active();

// XXH3 of the tightly packed blocks of a top level held in rows of row_bytes.
uint64_t HashBlocks(const uint8_t *rows, uint32_t row_bytes, uint32_t blocks_w, uint32_t blocks_h,
                    uint32_t bytes_per_block);

// Queues a texture for the dump (once per hash; does nothing when not dumping). `rows` as for
// HashBlocks; `format` is the texture's; `swizzle` the fetch constant's (0x688 = RGBA).
void Dump(uint64_t hash, uint32_t width, uint32_t height, Format format, const uint8_t *rows,
          uint32_t row_bytes, uint32_t swizzle);

// The replacement for a hash, read on first use and kept; nullptr when no pack has it.
std::shared_ptr<const Replacement> Find(uint64_t hash);

}  // namespace wwe12::texture_packs
