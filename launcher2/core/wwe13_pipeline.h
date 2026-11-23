#pragma once

#include "launcher_core.h"
#include "wwe13_convert.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

// Per-wrestler conversion choices. This is an in-memory recipe; callers choose which rows to
// include. It is deliberately separate from the Import page and never points at an install.
struct WWE13ImportPort {
  uint32_t source_id = 0;
  uint32_t target_id = 0;
  uint32_t donor_id = 0;
  std::string name;
  std::string handle;
  std::string pack;
  std::string tron;  // numeric WWE 2K14 stand-in ID only
  bool own_pro = true;
  std::string attrs = "13";
  std::string index = "free";
  std::string win = "13";
  std::string mv = "13";
  std::string pce = "donor";
  std::string rank0 = "auto";
  std::string rank2 = "auto";
  std::string ent = "13";
  std::string ent_keep = "all";
  std::string ent_host;
  std::string ent_video = "keep";
  std::string leaf_fix;
  std::optional<uint32_t> leaf10000;
  std::string ra_name;
  std::string gender = "M";
  std::optional<uint32_t> stock_model_source_id;
  std::optional<uint32_t> stock_body_source_id;
  bool own_announcer_name = false;  // add this source wrestler's WWE '13 voice line when available
};

// Loads the supported subset of data/wwe13-ports.tsv. The file contains only per-port choices
// and labels; it never contains wrestler assets or copied game tables.
bool ReadWWE13ImportRecipes(const std::filesystem::path& file,
                            std::vector<WWE13ImportPort>* ports, std::string* error);

struct WWE13ImportPaths {
  std::filesystem::path wwe13_root;
  std::vector<std::filesystem::path> wwe13_pack_roots;
  std::filesystem::path wwe14_root;
  std::filesystem::path userdata_root;
  std::filesystem::path bink_converter_executable;  // optional standalone child process
  std::filesystem::path titantron_cache;  // optional: converted movies kept here survive a cancelled run
  std::filesystem::path staged_game_root;  // optional: the audio step adds to arcs already rebuilt here
};

struct WWE13ImportOptions {
  bool own_attire_codes = false;  // all106v reference mode
};

struct WWE13RegistryChar {
  uint32_t target_id = 0;
  uint16_t rank_lsd0 = 0;
  uint16_t rank_lsd2 = 0;
};

// Folder name (under userdata/0000000000000000/545408B2/00000002) of the player's WWE 2K14 Add-On 2 offer pack, which
// hosts the imported wrestlers' offers. Found by its catalog (pack 2 with the wrestler 338 offer), so a pack added under
// another file name still works (player reports #103, 2026-11-08). The standard name wins when present; empty if none.
std::string FindWWE2K14HostPack(const std::filesystem::path& userdata_root);
// Folder the import writes: the host renamed with '0' first (045BEE31.. for the standard Add-On 2, 067E5576.. for the
// 4-offer version); hosts under other names (#103) use 045BEE31... Pipeline and installer must agree.
std::string WWE13HostOutputPack(const std::string& host);

// Stage 6 registry transformation, exposed for synthetic host tests.
bool ConvertWWE13DlcRegistry(const WWE13ConvertBytes& source_pac,
                             const std::vector<WWE13RegistryChar>& chars,
                             const std::string& file_name,
                             WWE13ConvertBytes* output, std::string* error);

// Reindexes a rewritten base-game PAC in the two retail catalog arcs. Inputs are never changed.
// Sync the record of a rewritten PAC in one pac-list arc; *synced = false (no error) when that list does not carry it.
bool SyncWWE13PacArchiveIfListed(WWE13ConvertBytes* arc, const WWE13ConvertBytes& original_pac,
                                 const WWE13ConvertBytes& rewritten_pac, bool* synced,
                                 std::string* error);
// Insert the block of a new PAC for pac-list line `index` (later lines renumbered, sentinel block kept).
bool InsertWWE13ArcBlock(WWE13ConvertBytes* arc, uint16_t index, const WWE13ConvertBytes& pac,
                         std::string* error);
bool SyncWWE13PacArchives(const WWE13ConvertBytes& original_pac,
                          const WWE13ConvertBytes& rewritten_pac,
                          std::vector<WWE13ConvertBytes>* arcs, std::string* error,
                          bool loose = false);

struct WWE13ImportRunResult {
  std::filesystem::path staging_directory;
  std::vector<std::string> files;
  std::vector<std::string> notices;  // per-wrestler stand-in fallback messages
};

// Writes changed stage 2–9 files under <fresh_staging_directory>/files. The requested staging
// directory must not already exist. All supplied game trees are read-only inputs.
bool RunWWE13ImportStages2To9(const WWE13ImportPaths& paths,
                             const std::vector<WWE13ImportPort>& ports,
                             const std::filesystem::path& fresh_staging_directory,
                             const WWE13ImportOptions& options,
                             WWE13ImportRunResult* result, std::string* error,
                             const ProgressFn& progress = {}, const CancelFlag* cancel = nullptr);

}  // namespace wwe2k14::launcher
