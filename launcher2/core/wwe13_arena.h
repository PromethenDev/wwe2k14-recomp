#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "wwe13_convert.h"

namespace wwe2k14::launcher {

// WWE '13 arenas added to WWE 2K14 under new arena numbers (G4 Path B, docs/tasks/G4-arenas-report.md).
// Reference implementation: tools/port/arena13/make_import.py (same order of operations, same bytes except
// the recompressed ZLIB leaves, which come from the launcher's own compressor).
struct WWE13ArenaPort {
  uint32_t source_id = 0;   // WWE '13 arena number (pac/bg/bgSS.pac)
  uint32_t target_id = 0;   // free WWE 2K14 arena number (< 100)
  std::string tag;          // arena-select picture name, e.g. "W13_89"
  std::string picture;      // WWE '13 picture name in MatchHD MLYE/MUAS leaf 7001 (arena00-16, AEM01-22)
  std::string name;         // arena name for string key 40002 + target_id (all 9 language tables)
};

// The files the conversion reads, keyed by game-relative path with forward slashes:
//   2K14: plist360.def, plist360_4x3.def, plist360.arc, plist360_4x3.arc, pac/string.pac,
//         pac/menu/MatchHD.pac, pac/menu/MatchSD.pac, pac/game2dHD.pac, pac/game2dSD.pac,
//         pac/evt/evtgdhd.pac, pac/evt/evtgdsd.pac, pac/evt/evtmov.pac, pac/misc02_ingame.pac
//   '13:  pac/bg/bgSS.pac for every source id, plus the same pac/menu, pac/game2d*, pac/evt files.
// existing_bg lists the 2K14 arena numbers that already have a pac/bg/bgNN.pac (they cannot be targets).
struct WWE13ArenaInput {
  std::map<std::string, WWE13ConvertBytes> game;
  std::map<std::string, WWE13ConvertBytes> source;
  std::vector<uint32_t> existing_bg;
  // Extra evtmov.pac entries (group, entry already named for the target id), e.g. converted MOV / MVAB stage movies.
  std::vector<std::pair<std::string, WWE13PacEntryData>> evtmov_added;
};

struct WWE13ArenaOutput {
  std::map<std::string, WWE13ConvertBytes> files;  // game-relative path -> new bytes (new bg pacs included)
  std::string arena_list;                           // "id tag" lines for WWE2K14_ARENA_LIST
};

std::vector<std::string> WWE13ArenaGameFiles();
std::vector<std::string> WWE13ArenaSourceFiles(const std::vector<WWE13ArenaPort>& ports);

bool ConvertWWE13Arenas(const std::vector<WWE13ArenaPort>& ports, const WWE13ArenaInput& input,
                        WWE13ArenaOutput* output, std::string* error);

// Pieces, exposed for host tests.
bool RenameWWE13ArenaPac(const WWE13ConvertBytes& source_pac, uint32_t source_id, uint32_t target_id,
                         WWE13ConvertBytes* plain, std::string* error);
bool InsertWWE13ArenaPacListLine(WWE13ConvertBytes* def, uint32_t target_id, uint16_t* line,
                                 std::string* error);
bool AddWWE13TextureBankEntries(const WWE13ConvertBytes& bank,
                                const std::vector<std::pair<std::string, WWE13ConvertBytes>>& added,
                                WWE13ConvertBytes* output, std::string* error);
// Fill zero stands-light rows for imported arena numbers. The output is plain EPAC; `changed` is false
// when every requested arena already has a populated row, so callers can omit the file from the stage.
bool FillWWE13ArenaLightRows(const WWE13ConvertBytes& misc02_pac, const std::vector<uint32_t>& arena_ids,
                             WWE13ConvertBytes* plain_output, bool* changed, std::string* error);

}  // namespace wwe2k14::launcher
