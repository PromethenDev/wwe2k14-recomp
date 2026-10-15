// Launcher auto-updater: check GitHub for a newer release, download it, verify it and replace only the
// program files while keeping everything that belongs to the player (saves, game files, settings, music,
// logs, backups). No UI code lives here; the SDL3 + Dear ImGui front end owns the banner and progress.
//
// The check contacts only https://api.github.com and the release download URL that the API returns
// (github.com and its asset hosts). A test-only environment override (WWE2K14_UPDATE_API_URL) lets a local
// HTTP server stand in for GitHub and is the only case where plain http is accepted.
#pragma once

#include "launcher_core.h"

namespace wwe2k14::launcher {

// One newer release as described by the GitHub "latest release" API. A default-constructed value means
// "no update" (also what every failure returns); the check never throws.
struct UpdateInfo {
  bool available = false;
  std::string version;  // release tag without the leading "v" ("1.4.0")
  std::string tag;      // the tag exactly as GitHub reports it ("v1.4")
  std::string notes;    // release body (markdown), shown by "What's new"
  std::string name;     // download asset file name
  std::string url;      // download asset URL
  uint64_t size = 0;    // asset size in bytes (0 when the API did not report one)
  std::string sha256;   // lowercase hex digest from the API "digest" field ("" when absent)
};

// Parses a GitHub "latest release" JSON document. Returns std::nullopt when the document is not usable
// JSON or has no release tag. current_version is compared with semver rules to set `available`; drafts and
// prereleases are never offered, and an update with no sha256 digest is not offered because it cannot be
// verified.
std::optional<UpdateInfo> ParseLatestReleaseJson(std::string_view json, std::string_view current_version);

// Synchronous network check. Never throws and never blocks longer than ~timeout_seconds. Any failure
// (offline, no curl, timeout, bad JSON) returns an unavailable UpdateInfo so the UI stays silent.
UpdateInfo CheckLatestUpdate(std::string_view current_version, double timeout_seconds = 5.0);

// Semver-ish comparison: >0 when left is newer, 0 equal, <0 older. A leading "v", build metadata after
// '+' and a prerelease suffix after '-' are handled (1.3.0-rc1 < 1.3.0).
int CompareVersions(std::string_view left, std::string_view right);

// True for a path (relative to the package top folder, '/'-separated, any case) that an update must never
// overwrite or delete: the player's saves, the game files, the launcher settings, their controls file,
// their music and their backups.
bool IsNeverTouchPath(std::string_view relative_path);

// Downloads info.url to <exe_dir>/backups/update-download/<name>.part, then verifies its size and sha256
// against info and renames it to <name>. On any mismatch the file is deleted and a player-readable error
// is returned. Cancellation is allowed while downloading; a cancelled run leaves nothing behind.
Result DownloadUpdate(const Paths& paths, const UpdateInfo& info, const ProgressFn& progress,
                      const CancelFlag& cancel, fs::path* downloaded);

// Applies an already-downloaded (and verified) release zip: extracts to a staging folder, backs up the
// current program files to <exe_dir>/backups/update-<old_version>/, then replaces program files. Nothing
// on IsNeverTouchPath is overwritten or deleted, an existing wwe2k14.toml and existing shader-cache files are
// kept. On any failure the backed-up files are restored and any newly created file is removed.
Result ApplyUpdate(const Paths& paths, const fs::path& zip_file, const UpdateInfo& info,
                   std::string_view old_version, const ProgressFn& progress, const CancelFlag& cancel);

// Starts the launcher executable again, detached from this process (used after an update so the player
// lands on the new version). On Windows the running launcher renames itself out of the way first.
Result RelaunchLauncher(const fs::path& launcher_executable);

// True when a process started from this install's game executable (paths.game_exe) is running. The game has
// no single-instance lock (unlike WWE '13), so the launcher looks at the process list instead. The launcher
// refuses to update while it is true: the game holds the same program files open. Never throws; false when
// the process list cannot be read.
bool GameProcessRunning(const Paths& paths);

// Removes "*.old" left behind by a Windows self-update and stale "*.part" downloads. Called at start-up;
// safe to call at any time and never throws.
void CleanupUpdateLeftovers(const Paths& paths);

// Test-only knobs (used by update_test.cpp and the end-to-end harness). Not for player-facing code.
namespace update_internal {
// Downloads a URL into memory (used for the API JSON). Allowed by the same URL rules as the check.
bool HttpGetForTest(std::string_view url, double timeout_seconds, std::string* body);
// True when the download URL is one the updater is willing to fetch.
bool IsAllowedDownloadUrl(std::string_view url);
}  // namespace update_internal

}  // namespace wwe2k14::launcher
