#pragma once

#include "wwe13_pipeline.h"
#include <map>
#include <utility>

namespace wwe2k14::launcher {

// The base-game files a WWE '13 wrestler import copies out of a WWE '13 disc image for these recipes (disc paths).
std::vector<std::string> WWE13DiscImportFiles(const std::vector<DiscImageFile>& disc, const std::vector<WWE13ImportPort>& ports);

struct WWE13ImportSummary {
  size_t imported_wrestlers = 0;
  std::vector<std::string> lines;
};

// Converts the selected recipe rows, then applies the staged files transactionally. The three
// game/source trees are read-only inputs; only the WWE 2K14 game and userdata outputs, launcher
// Mods metadata, and an undo set under backups/wwe13-import are changed.
Result RunWWE13Import(const Paths& launcher_paths, const std::filesystem::path& game_folder,
                      const WWE13ImportScan& scan, const std::vector<uint32_t>& selected_source_ids,
                      const ProgressFn& progress, CancelFlag& cancel, WWE13ImportSummary* summary);

// Applies a staged files/ tree. Exposed so the host-side test can exercise hard-link-safe apply,
// re-import, hash-backed undo, cancellation, and rollback without any retail game files.
Result ApplyWWE13StagedFiles(const Paths& launcher_paths, const std::filesystem::path& game_folder,
                             const std::filesystem::path& staged_files,
                             const ProgressFn& progress, CancelFlag& cancel,
                             WWE13ImportSummary* summary,
                             const std::vector<std::string>& removal_keys = {});
Result UndoWWE13Import(const Paths& launcher_paths, const std::filesystem::path& game_folder,
                       const ProgressFn& progress, CancelFlag& cancel);
bool HasWWE13Import(const Paths& launcher_paths);
// SHA-256 of a file as the undo records store it (empty when unreadable).
std::string ImportFileHash(const std::filesystem::path& path);
// key ("game/...") -> {old hash, new hash}: every import record whose applied hash is the old one takes the new one.
bool RebaseImportRecords(const Paths& launcher_paths,
                         const std::map<std::string, std::pair<std::string, std::string>>& changes, std::string* error);
// files: key -> {path, current hash}. Fails unless every file is held by a record at its current hash; menu PACs no
// record holds get one (current bytes saved as the original) in the import whose record holds anchor_key at
// anchor_hash. *adopted lists them for DropAdoptedImportRecords (rollback).
bool AdoptImportRecords(const Paths& launcher_paths,
                        const std::map<std::string, std::pair<std::filesystem::path, std::string>>& files,
                        const std::string& anchor_key, const std::string& anchor_hash, std::vector<std::string>* adopted,
                        std::string* error);
bool DropAdoptedImportRecords(const Paths& launcher_paths, const std::vector<std::string>& adopted, std::string* error);
// GitHub #141: which of target_ids already have a row in a DLC.pac registry (ENEX/LSD leaf 0). Stock files have none,
// so a hit means an earlier import changed this game folder (for example from another copy of the launcher, whose
// undo record this copy does not have). Empty on success with no hits; false + error when the file cannot be read.
// GitHub #141: an earlier WWE '13 import by ANOTHER launcher copy (same game folder) is undone with that copy's undo
// record, so this copy can import. A candidate folder is used only when every game/ file of its current record is
// exactly in the imported state (or already original) and userdata/ files here are imported or original; otherwise it is
// skipped. All-or-nothing (rolled back on failure). True + *used = the folder whose record was used; true + empty *used =
// no matching folder; false = a restore failed (rolled back, *error says why).
bool RestoreWWE13ImportFromOtherLauncher(const Paths& launcher_paths, const std::filesystem::path& game_folder,
                                         const std::vector<std::filesystem::path>& candidates,
                                         const ProgressFn& progress, CancelFlag& cancel,
                                         std::filesystem::path* used, std::string* error);
// GitHub #160: when no other copy is exactly in its imported state, put back the original game/ files from its backups
// (every original verified against its saved hash first; oldest layer wins; all-or-nothing). Same return contract.
bool RestoreWWE13OriginalsFromOtherBackup(const Paths& launcher_paths, const std::filesystem::path& game_folder,
                                          const std::vector<std::filesystem::path>& candidates,
                                          const ProgressFn& progress, CancelFlag& cancel,
                                          std::filesystem::path* used, std::string* error);
bool WWE13TargetsInDlcRegistry(const std::vector<uint8_t>& dlc_pac, const std::vector<uint32_t>& target_ids,
                               std::vector<uint32_t>* present, std::string* error);

// WWE '13 arenas (wwe13_arena_install.cpp): same transaction, own undo set (backups/wwe13-arenas). Wrestler import and
// wrestler undo refuse while arenas are installed (both change string.pac and the pac lists); a second arena import
// needs an undo first.
Result ApplyWWE13ArenaStagedFiles(const Paths& launcher_paths, const std::filesystem::path& game_folder,
                                  const std::filesystem::path& staged_files, const ProgressFn& progress,
                                  CancelFlag& cancel);
Result UndoWWE13ArenaImport(const Paths& launcher_paths, const std::filesystem::path& game_folder,
                            const ProgressFn& progress, CancelFlag& cancel);
bool HasWWE13ArenaImport(const Paths& launcher_paths);

// Owner 11-18 Mods on/off switch (mods_switch.cpp): every file an import layer changed (WWE '13 wrestlers, WWE '13 arenas,
// WWE 2K15), keyed "game/..." / "userdata/..." / "launcher/...", with the ORIGINAL of the oldest layer that changed it
// (exists = false: the import added the file). artifact = the saved original's path in that layer's undo set.
struct ImportOriginal {
  bool exists = false;
  char kind = 'F';  // F = bytes, L = symbolic link text
  std::string hash;  // SHA-256 of the original bytes (exists only)
  std::filesystem::path artifact;
  std::string set_id;
};
bool CollectImportOriginals(const Paths& launcher_paths, std::map<std::string, ImportOriginal>* out, std::string* error);

}  // namespace wwe2k14::launcher
