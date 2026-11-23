#pragma once
// Imported wrestlers in the MAIN select grid (owner 2026-11-15/16; game-side notes docs/tasks/SELGRID-layout-research.md and
// SELGRID3-launch.md in the native workspace). The grid draws each tile's face from menu/menu{HD,SD}.pac, group MENU, entry
// SSFD: an outer PACH {0: packed inner PACH}; the inner PACH maps wrestler id -> 64x64 DXT5 DDS (no mips, 4224 bytes).
// WWE 2K14, WWE '13 and WWE 2K15 all carry this table in the same format, so an import copies the wrestler's own face from
// his game (byte copy) and falls back to a head crop of his 256x256 select portrait (SSFC) when his game has none.
// Proven in game 2026-11-16 (selgrid-3: faces shown; selgrid-5: an import picked from the grid).

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

using GridFaceBytes = std::vector<uint8_t>;

// Every face of a menu PAC's MENU/SSFD table (id -> DDS). The inner table may be ZLIB (2K14, 2K15) or BPE (WWE '13).
bool ReadGridFaces(const GridFaceBytes& menu_pac, std::map<uint32_t, GridFaceBytes>* faces, std::string* error);

// Rebuilds a WWE 2K14 menu PAC with `add` merged into MENU/SSFD (ids ascending; ids already in the table keep their face,
// stock ones are never replaced). `added` = how many were new. Output is the plain EPAC; nothing else in the PAC changes.
// `replace` (optional): ids whose existing face is replaced by the one in `add` (used by the launcher's face repair for
// imported wrestlers' old see-through faces); `added` then counts new + replaced.
bool AddGridFaces(const GridFaceBytes& menu_pac, const std::map<uint32_t, GridFaceBytes>& add, GridFaceBytes* output,
                  size_t* added, std::string* error, const std::set<uint32_t>* replace = nullptr);

// True for a face with 15 % or more see-through pixels: a portrait crop made before the opaque backdrop (11-16). The grid
// draws see-through pixels black, so such a tile looks dark. Stock faces are opaque.
bool GridFaceSeeThrough(const GridFaceBytes& dds);

// Fallback face: head crop of a 256x256 DXT5 select portrait (SSFC), resized to 64x64, encoded like the stock faces
// (`header_like` = any stock face; its 128-byte header is reused so format and flags match exactly).
// `backdrop` (64x64 RGBA from GridFaceBackdrop) fills the portrait's see-through background: the grid draws see-through
// pixels black, so without it the tile looks dark next to the stock faces (owner 11-16: dark WWE '13 tiles).
bool MakeGridFaceFromPortrait(const GridFaceBytes& portrait_dds, const GridFaceBytes& header_like, GridFaceBytes* face,
                              std::string* error, const GridFaceBytes* backdrop = nullptr);

// Opaque 64x64 RGBA backdrop like the stock tiles': per row, the median colour of the stock faces' outer columns.
// Empty when no stock face decodes.
GridFaceBytes GridFaceBackdrop(const std::map<uint32_t, GridFaceBytes>& stock);

// True for a face the grid can use as is (DDS, 64x64, DXT5, 4224 bytes).
bool IsGridFace(const GridFaceBytes& dds);

}  // namespace wwe2k14::launcher
