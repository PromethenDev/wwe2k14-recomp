#pragma once

// WWE 2K15 import, step E: the staged files for one import (port of p15roster build.py). Every
// input is read-only: the player's WWE 2K15 files, WWE 2K14 game folder and userdata. Output is a
// fresh staging folder with files/game/..., files/userdata/... and files/launcher/own-slots.txt,
// applied by the installer (step H). The 2K15 wrestlers join the WWE '13 import's add-on folder when that
// import is installed (as in every python kit); otherwise this import makes that folder from Add-On 2 itself.

#include "launcher_core.h"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

struct WWE15ImportPort {
  uint32_t source_id = 0;   // WWE 2K15 ID
  uint32_t target_id = 0;   // WWE 2K14 slot
  std::string name;
  std::string handle;
  uint32_t donor_id = 0;    // same-class stock WWE 2K14 wrestler
  bool alternate = false;   // kind "alt": another version of a WWE 2K14 wrestler
  uint32_t original14_id = 0;
  uint32_t entrance_pack = 4;
  std::set<std::string> options;  // donor-music, donor-titantron, keep-donor-entrance, allow-no-victory, gate-one
  uint32_t victory15 = 0;         // victory15=ID
  std::string gender = "M";       // M or F, from recipe column 10
};
bool ReadWWE15ImportRecipes(const std::filesystem::path& file, std::vector<WWE15ImportPort>* ports,
                            std::string* error);
// Compute aggregate offer headroom across independent catalog packages. Each catalog has its own cap.
size_t CountWWE15OfferRoom(const std::vector<size_t>& current_offers, size_t per_catalog_cap = 64);

struct WWE15ImportPaths {
  std::filesystem::path wwe15_root;     // folder holding the WWE 2K15 pac/ (and sound/, movies/) folders
  std::filesystem::path wwe14_root;     // WWE 2K14 game folder (pac/, movies/, plist360.arc ...)
  std::filesystem::path userdata_root;  // WWE 2K14 userdata (0000000000000000/545408B2/00000002/...)
  std::filesystem::path own_slots;      // the launcher's current own-slots.txt (may be missing)
  std::filesystem::path moves_data;     // data/wwe2k15-moves.tsv
  std::filesystem::path bink_converter_executable;  // wwe2k14-bink-convert beside the launcher (empty = stand-in films)
  std::filesystem::path titantron_cache;            // converted films by source ID + size, reused by later imports
};

struct WWE15ImportOptions {
  // The wrestlers' own 2K15 entrance themes (step G, owner 11-16): added to the entrance music pack and the PRO music
  // selectors left on the wrestler's own slot. A wrestler without a 2K15 theme keeps the donor's stock theme. Announcer
  // name lines come from the wrestlers' 2K15 RASS keys and name clips. false = donor themes, no names (pre-11-16 behaviour).
  bool audio = true;
  bool titantrons = true;  // films: the wrestler's own 2K15 film (converted), else the 2K14 original's / donor's / stand-in
  bool existing_wrestler_looks = false;  // optional 2K15 model updates for the 47 IDs already in 2K14
};

struct WWE15ImportRunResult {
  std::filesystem::path staging_directory;
  std::vector<std::string> files;
  std::vector<std::string> notices;
  // Per wrestler: every 2K15 move the planner could not keep (slot, move, reason, replacement). One header line per
  // wrestler ("NAME (2K15 id): K of M moves are the wrestler's own 2K15 moves"), then its notes. Owner 11-15.
  std::vector<std::string> move_notes;
  std::vector<std::pair<uint32_t, float>> theme_levels;  // target id, gain of his own 2K15 theme (Mods page theme volume)
  std::vector<std::string> removal_keys;  // a new host folder: the original Add-On 2 files and header it replaces
  // On failure: the one wrestler the failure belongs to (WWE 2K15 id), 0 when it is not about one wrestler. The import
  // can then leave that wrestler out and run again (owner 11-13: one bad wrestler must not stop the others).
  uint32_t failed_source_id = 0;
};

// True when `pac` is a WWE 2K15 pac folder (checked by contents, no folder-name rules; WWE 2K14's own pac fails it).
bool IsWWE15PacFolder(const std::filesystem::path& pac);

// Finds the WWE 2K15 pac folder under `selected` by contents (ch/chEtc.pac with CHAR/PRO rows
// and ch/ch<id>.pac model files); empty if none.
std::filesystem::path FindWWE15PacFolder(const std::filesystem::path& selected);

bool RunWWE15ImportStages(const WWE15ImportPaths& paths, const std::vector<WWE15ImportPort>& ports,
                          const std::filesystem::path& fresh_staging_directory, const WWE15ImportOptions& options,
                          WWE15ImportRunResult* result, std::string* error, const ProgressFn& progress = {},
                          const CancelFlag* cancel = nullptr);

}  // namespace wwe2k14::launcher
