#pragma once

// WWE 2K15 import, step H: scan, apply and Undo. The staged files (wwe15_pipeline) go through the same
// hash-backed, hard-link-safe transaction as the WWE '13 import, with their own undo set (backups/wwe15-import).
// Layers: [WWE '13 import] -> WWE 2K15 import -> WWE '13 arenas. With a WWE '13 import the 2K15 wrestlers join its
// add-on folder; without one the 2K15 import makes that folder from Add-On 2 itself (Undo restores Add-On 2). Each
// layer is undone before the one below it changes: the 2K15 import and its Undo refuse while arenas are installed;
// the WWE '13 import and its Undo refuse while the 2K15 import is installed (undo 2K15, import '13, import 2K15 again).

#include "launcher_core.h"
#include "wwe15_pipeline.h"

#include <filesystem>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

struct WWE15ImportWrestler {
  uint32_t source_id = 0;
  uint32_t target_id = 0;
  std::string name;
  bool available = false;
  std::string missing_reason;  // which WWE 2K15 file is missing for this wrestler
};
struct WWE15ImportScan {
  std::filesystem::path pac_folder;  // the WWE 2K15 pac folder found by contents (empty: none)
  std::filesystem::path disc_image;  // or: the player's WWE 2K15 disc image (.iso), read without extracting it
  std::string error;
  std::vector<WWE15ImportWrestler> wrestlers;
  bool searched_pc = false;           // no folder was chosen: this PC was searched
  bool cancelled = false;             // the player stopped the search
  bool search_limit_reached = false;  // the search hit its time/folder cap before finding WWE 2K15
  uint64_t directories_visited = 0;
};
// Finds the player's WWE 2K15 files under `selected` and lists the recipe's wrestlers with what is missing.
// An empty `selected` searches this PC instead (SearchWWE15PacFolder).
WWE15ImportScan ScanWWE15Import(const Paths& launcher_paths, const std::filesystem::path& selected,
                                const ProgressFn& progress = {}, const CancelFlag* cancel = nullptr);

// WWE 2K15 disc images (owner 11-13: "it should get the pac files without an extracted disc"). A disc image counts when
// its default.xex is WWE 2K15 (title ID 545408B6) and its pac folder passes the same contents check as a folder.
constexpr uint32_t kWwe2k15TitleId = 0x545408B6;
bool IsWWE15DiscImage(const std::filesystem::path& iso);
// A WWE 2K15 disc image under `selected` (the file itself, or a .iso up to 4 folders down); empty if none.
std::filesystem::path FindWWE15DiscImage(const std::filesystem::path& selected);
// Copies the files the import reads out of the disc image into dest/pac: the pac folder's own files, evt/Entrance*,
// evt/entrance*, evt/Winning*, evt/evtobj.pac, menu/menuHD2.pac + menuSD2.pac, ch/chEtc.pac and the models of
// `model_ids` (plus ch424/ch398, which the contents check needs). Checks the free space first.
bool ExtractWWE15DiscFiles(const std::filesystem::path& iso, const std::filesystem::path& dest,
                           const std::vector<uint32_t>& model_ids, const ProgressFn& progress,
                           const CancelFlag* cancel, std::string* error);

// The whole-PC search behind the WWE 2K15 picker: every drive (game_search.h), each folder listed once and judged by
// names; returns the first WWE 2K15 pac folder (IsWWE15PacFolder) or WWE 2K15 disc image (IsWWE15DiscImage), or empty.
std::filesystem::path SearchWWE15PacFolder(const Paths& launcher_paths, const ProgressFn& progress,
                                           const CancelFlag* cancel, bool* limit_reached,
                                           uint64_t* directories_visited);

struct WWE15ImportSummary {
  size_t imported_wrestlers = 0;
  std::vector<std::string> lines;
  // Wrestlers left out because only they failed (the rest were imported), each "NAME: plain reason".
  std::vector<std::string> skipped;
};
Result RunWWE15Import(const Paths& launcher_paths, const std::filesystem::path& game_folder,
                     const WWE15ImportScan& scan, const std::vector<uint32_t>& selected_source_ids,
                     const ProgressFn& progress, CancelFlag& cancel, WWE15ImportSummary* summary,
                     const WWE15ImportOptions& options = {});

// Transaction pieces (in wwe13_install.cpp, which owns the installer).
Result ApplyWWE15StagedFiles(const Paths& launcher_paths, const std::filesystem::path& game_folder,
                             const std::filesystem::path& staged_files, const ProgressFn& progress, CancelFlag& cancel,
                             const std::vector<std::string>& removal_keys = {});
Result UndoWWE15Import(const Paths& launcher_paths, const std::filesystem::path& game_folder,
                       const ProgressFn& progress, CancelFlag& cancel);
bool HasWWE15Import(const Paths& launcher_paths);

// What the current WWE 2K15 import was made from (mods/wwe15/import.txt, written with the import and removed by its Undo), so
// a WWE '13 import can take the WWE 2K15 wrestlers out and put the same ones back (owner 11-16: "import in any order").
struct WWE15ImportRecord {
  std::filesystem::path source;  // the WWE 2K15 pac folder or disc image the import read
  bool existing_wrestler_looks = false;
  std::vector<uint32_t> source_ids;
  uint32_t version = 1;  // what the import brought (kWWE15ImportVersion); records without the line are 1
};
bool ReadWWE15ImportRecord(const Paths& launcher_paths, WWE15ImportRecord* record);

// 2 = own WWE 2K15 titantrons (1.6.2). Imports made with an older launcher keep a stand-in film and, before 1.6.1, a
// donor theme; the Mods page offers to bring them up to date (GitHub #155).
constexpr uint32_t kWWE15ImportVersion = 3;
bool WWE15ImportOutdated(const Paths& launcher_paths);
// Takes the WWE 2K15 wrestlers out (their own Undo) and imports the same ones again with this launcher. Without an import
// record (imports before 1.6.1) the wrestlers are own-slots.txt's WWE 2K15 slots and WWE 2K15 is searched for on this PC.
// If the new import fails after the old one was taken out, the error says so (the player imports again by hand).
Result RefreshWWE15Import(const Paths& launcher_paths, const std::filesystem::path& game_folder, const ProgressFn& progress,
                          CancelFlag& cancel, WWE15ImportSummary* summary);

}  // namespace wwe2k14::launcher
