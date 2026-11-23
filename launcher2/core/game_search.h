#pragma once

// The search behind "Import from WWE '13" / "Import from WWE 2K15" (owner 11-13: "it should know exactly which files to
// look for" and "find them anywhere on the PC, drives included"). Every folder is listed once and judged by NAMES only;
// a visitor opens a file only when its name says it can be the game (default.xex beside pac, a large .iso/.img, a fixed
// add-on package name). Never follows links or junctions, never opens cloud-only (OneDrive-style) files.

#include "launcher_core.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

struct GameSearchEntry {
  std::filesystem::path path;
  std::string lower_name;  // file or folder name, lower case
  bool directory = false;
  uint64_t size = 0;  // files only
};

struct GameSearchStats {
  uint64_t folders = 0;
  bool limit_reached = false;  // stopped by the time or folder limit before the end
  bool cancelled = false;
  bool stopped_early = false;  // the visitor found what it wanted
};

// Every place the PC search walks, most likely first: the launcher's folder, the player's Downloads, Desktop,
// Documents, Videos and home folder, then every drive (Windows: fixed and removable/USB drives; network and optical
// drives are skipped. Linux: every mounted file system). WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS replaces the list in tests.
std::vector<std::filesystem::path> GameSearchRoots(const Paths& paths);

// Walks `roots` (each folder once, even where roots overlap). `visit` gets each folder's listing and returns true to
// stop the search. `time_limited`: stop after kGameSearchTimeLimitSeconds (the whole-PC search; a chosen folder is
// always walked to the end). Cancel is checked between every entry.
inline constexpr int kGameSearchTimeLimitSeconds = 300;
GameSearchStats WalkGameFiles(const std::vector<std::filesystem::path>& roots,
                              const std::function<bool(const std::filesystem::path& folder,
                                                       const std::vector<GameSearchEntry>& entries)>& visit,
                              bool time_limited, const ProgressFn& progress, const CancelFlag* cancel);

// A disc image worth opening: .iso/.img of at least 1 GiB (Xbox 360 game discs are 7-9 GB; tests lift the size
// floor) that is not a cloud-only placeholder.
bool IsGameDiscImageCandidate(const GameSearchEntry& entry);
// True for a cloud-only placeholder (OneDrive Files On-Demand and similar): opening it would start a download.
bool IsCloudOnlyFile(const std::filesystem::path& file);

}  // namespace wwe2k14::launcher
