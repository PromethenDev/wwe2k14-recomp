#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "launcher_core.h"
#include "wwe13_arena.h"

namespace wwe2k14::launcher {

// The WWE '13 arenas WWE 2K14 does not have (G4 report): 5 Capitol Punishment, 10 Vengeance, 15 WrestleMania XXVIII,
// 28-49 the Attitude Era arenas. Each item says whether the player's WWE '13 folder has its file.
struct WWE13ArenaItem {
  uint32_t source_id = 0;
  std::string name;      // the '13 English name (string 40002 + id)
  std::string picture;   // its arena-select picture in the '13 MatchHD MUAS bank
  bool available = false;
  std::string missing_reason;
};
std::vector<WWE13ArenaItem> ListWWE13Arenas(const std::filesystem::path& wwe13_base, std::string* error);

// Proven free target numbers for the 25-arena import. Targets without a stock arena record use the game's guarded record
// relocation path, enabled automatically while the imported arena list exists.
std::vector<uint32_t> FreeWWE13ArenaNumbers(const std::filesystem::path& game_folder);
const std::vector<uint32_t>& WWE13ArenaImportTargets();

// The '13 arena number -> its picture in MatchHD MLYE/MUAS 7001 (from WWE '13's own arena table, update image 0x82040110).
std::string WWE13ArenaPicture(uint32_t source_id);

// The list the game hook reads (WWE2K14_ARENA_LIST): exe_dir/mods/wwe13/arenas.txt.
std::filesystem::path WWE13ArenaListFile(const Paths& paths);

struct WWE13ArenaImportSummary {
  std::vector<std::string> lines;   // "VENGEANCE -> arena 10" ..., plus notes (stage movies not converted ...)
};
// Converts the chosen '13 arenas onto the free numbers in order and applies them in one transaction with its own undo
// set (backups/wwe13-arenas). The '13 folder is read only. bink_converter may be empty: the arenas then get no MOV/MVAB
// stage movies (their Bink 1 MOVT is copied either way).
Result RunWWE13ArenaImport(const Paths& paths, const std::filesystem::path& game_folder,
                           const std::filesystem::path& wwe13_base, const std::vector<uint32_t>& source_ids,
                           const std::filesystem::path& bink_converter, const ProgressFn& progress, CancelFlag& cancel,
                           WWE13ArenaImportSummary* summary);

}  // namespace wwe2k14::launcher
