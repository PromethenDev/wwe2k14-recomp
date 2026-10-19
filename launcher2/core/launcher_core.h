// WWE 2K14 launcher core: everything the launcher does that is not drawing. No UI, no SDL video.
// Shared contract between the core implementation and the SDL3 + Dear ImGui front end (launcher2/ui).
// Windows and Linux, UTF-8 strings everywhere (std::filesystem::path for paths).
//
// Long operations (scan, ISO extract, package import, backup) run on a worker thread owned by the caller;
// they report through a ProgressFn and honour cancellation. They never throw: errors come back as
// Result::error with a player-readable message (no emulator/debug wording).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wwe2k14::launcher {

namespace fs = std::filesystem;

struct Result {
  bool ok = true;
  std::string error;  // player-readable, empty when ok
  bool unknown = false;  // outcome could not be classified; callers should not report a false failure
  static Result Ok() { return {}; }
  static Result Fail(std::string message) { return {false, std::move(message), false}; }
  static Result Unknown() { return {false, {}, true}; }
};

// done/total in bytes (or items when bytes are unknown); label = what is happening now ("Extracting pac/ch/ch82.pac").
using ProgressFn = std::function<void(uint64_t done, uint64_t total, const std::string& label)>;
using CancelFlag = std::atomic<bool>;

// ---------------------------------------------------------------------------------------------- paths
// Where things live. Portable layout: everything next to the launcher (exe_dir). Same rules on both OSes.
struct Paths {
  fs::path exe_dir;        // folder of the launcher executable
  fs::path game_exe;       // wwe2k14.exe (Windows) / wwe2k14 (Linux) next to the launcher
  fs::path settings_file;  // wwe2k14-enhanced.ini (existing schema-7 file is read and kept compatible)
  fs::path userdata_dir;   // exe_dir/userdata (saves under B13EBABEBABEBABE/545408B2, DLC under 0000000000000000/545408B2/00000002)
  fs::path music_dir;      // exe_dir/music (the game reads custom songs from here)
  fs::path logs_dir;       // exe_dir/logs
  fs::path backups_dir;    // exe_dir/backups
  fs::path default_game_folder;  // exe_dir/"WWE 2K14"
};
Paths ResolvePaths(const fs::path& launcher_executable);
// True when any path component is a personal or business OneDrive root.
bool IsUnderOneDrive(const fs::path& path);
// Testable path matcher. allow_name_fallback is true only when no OneDrive environment variables exist.
bool IsOneDrivePath(const fs::path& path, const std::vector<fs::path>& roots,
                    bool allow_name_fallback);

// ---------------------------------------------------------------------------------------------- settings
// WWE 2K14 does not have the WWE '13 runtime's custom frame-rate or scene-AA hooks. The launcher exposes
// the output size/display mode, host frame sync, optional wide-screen stretching, and the runtime's renderer scale.
enum class Resolution { k720p, k1080p, k1440p };
enum class DisplayMode { kFullscreen, kWindowed };

struct Settings {
  Resolution resolution = Resolution::k720p;
  DisplayMode display = DisplayMode::kFullscreen;
  bool sync_frames_to_monitor = true;
  bool stretch_to_fill_wide_screens = false;
  int picture_scale = 1;  // internal draw scale: 1 = the game's 720p, 2 = 1440p, 3 = 4K (experimental)
  std::string gpu_id;    // stable id of the chosen Vulkan device ("" = recommended)
  fs::path game_folder;  // folder holding default.xex + default.xexp
  bool auto_start = false;          // "Start straight away next time" (hold Shift at start to come back)
  bool auto_backup = true;          // back up saves before each launch, keep last 5
  bool check_updates = true;        // look for a newer release when the launcher starts
  std::string skipped_update_version;  // "Skip this version" choice ("" = none)
  bool explicit_choice = false;     // false = follow RecommendedSettings()
};
// Reads the existing wwe2k14-enhanced.ini (UTF-16LE with BOM written by the old launcher, or UTF-8) including
// its schema-7 keys and legacy migrations; unknown keys are preserved on save.
Settings LoadSettings(const Paths& paths);
Result SaveSettings(const Paths& paths, const Settings& settings);

// ---------------------------------------------------------------------------------------------- PC info
struct GpuInfo {
  std::string id;          // stable across runs (vendor:device[:index])
  std::string name;        // "AMD Radeon 780M Graphics"
  bool integrated = false;
  int vulkan_index = -1;   // what the game's --vulkan_device expects
};
std::vector<GpuInfo> ListGpus();  // Vulkan enumeration (volk), CPU/llvmpipe devices excluded
// The current default shown as "Recommended for this PC" is 720p on all detected GPUs. Returns the
// reason line too (for example, "Radeon 780M - integrated graphics"). The 2K14 runtime has no verified
// 480p internal-render option or launcher frame-rate override.
struct Recommendation {
  Settings settings;
  std::string reason;
};
Recommendation RecommendedSettings(const std::vector<GpuInfo>& gpus);

// ---------------------------------------------------------------------------------------------- game files (F1)
enum class FileState { kMissing, kFound, kWrongVersion, kCorrupt };
struct GameFilesStatus {
  FileState base_game = FileState::kMissing;     // default.xex, title ID 545408B2
  FileState title_update = FileState::kMissing;  // default.xexp, TU 1.0.3 matching the base
  int dlc_installed = 0;                         // packs found under userdata (0..3)
  int dlc_known = 3;
  fs::path game_folder;
  std::string detail;  // one line per item for the UI
  bool ready() const { return base_game == FileState::kFound && title_update == FileState::kFound; }
};
// Checks one folder (same title-ID / TU checks as the game's src/game_version_check.cpp).
GameFilesStatus CheckGameFolder(const fs::path& folder, const Paths& paths);
// Searches: next to the launcher, previous choice, all drives / home folders (bounded depth), Xenia and
// Xenia Canary content folders. Returns every candidate found, best first.
struct FoundGame {
  fs::path folder;
  GameFilesStatus status;
};
std::vector<FoundGame> FindGameFiles(const Paths& paths, const ProgressFn& progress, const CancelFlag& cancel);

// ---------------------------------------------------------------------------------------------- disc image (F2)
// Player's own Xbox 360 disc image (XDVDFS / XISO, including Redump images with a video partition offset).
// Verifies it is WWE 2K14 (default.xex title ID) before extracting into dest_folder.
Result ExtractDiscImage(const fs::path& iso, const fs::path& dest_folder, const ProgressFn& progress,
                        const CancelFlag& cancel);

// ---------------------------------------------------------------------------------------------- TU / DLC (F3)
// STFS CON/LIVE/PIRS packages (the title update package and the DLC packs) or Xenia content folders.
// Title update: installs default.xexp (+ its data files) into the game folder. DLC: installs into
// userdata/0000000000000000/545408B2/00000002/<name> with its Headers entry, like the game expects.
enum class PackageKind { kTitleUpdate, kDlc, kUnknown };
struct PackageInfo {
  PackageKind kind = PackageKind::kUnknown;
  std::string display_name;  // from the package header
  uint32_t title_id = 0;
};
std::optional<PackageInfo> InspectPackage(const fs::path& package_or_folder);
// Installable files a player dropped into a game folder (top level only): disc images (.iso/.img) and WWE 2K14
// title-update / DLC package files. Never throws; unreadable entries are skipped.
struct FolderInstallables {
  std::vector<fs::path> disc_images;
  std::vector<fs::path> packages;
  bool has_title_update = false;
};
FolderInstallables FindInstallableFiles(const fs::path& folder);
Result ImportPackage(const fs::path& package_or_folder, const Paths& paths, const fs::path& game_folder,
                     const ProgressFn& progress, const CancelFlag& cancel);

// ---------------------------------------------------------------------------------------------- saves (F5)
struct BackupInfo {
  fs::path file;        // backups/wwe2k14-saves-YYYYMMDD-HHMMSS[-auto].zip
  std::string when;     // local time, "Today, 9:41 AM" style is the UI's job; this is ISO 8601
  uint64_t bytes = 0;
  bool automatic = false;
};
std::vector<BackupInfo> ListBackups(const Paths& paths);
Result BackupSaves(const Paths& paths, bool automatic, BackupInfo* created);  // automatic: keep the newest 5
Result RestoreBackup(const Paths& paths, const fs::path& backup_zip);        // backs up the current saves first
Result ImportBackup(const Paths& paths, const fs::path& external_zip);       // copies into backups/ after checking it

// ---------------------------------------------------------------------------------------------- music
struct Song {
  fs::path file;
  std::string title;     // file name without extension
  double seconds = 0;    // 0 when unknown
  std::string format;    // "MP3" / "FLAC" / "WAV" / "OGG"
};
std::vector<Song> ListSongs(const Paths& paths);
// Copies dropped files into music_dir (skips unsupported types with a message; Unicode names are fine).
Result AddSongs(const Paths& paths, const std::vector<fs::path>& files, std::vector<std::string>* skipped);
Result RemoveSong(const Paths& paths, const fs::path& song_file);

// ---------------------------------------------------------------------------------------------- launch
// The exact command line and environment the launcher uses from Settings: output size/display, optional
// letterbox and renderer scale, and host frame sync. Only the diagnostics the 2K14 runtime reads
// (WWE13_THREAD_PROFILE*) are passed through; the WWE '13-only override variables are not.
struct LaunchPlan {
  fs::path executable;
  std::vector<std::string> arguments;
  std::vector<std::pair<std::string, std::string>> environment;  // overrides on top of the launcher's env
  fs::path working_directory;
};
LaunchPlan BuildLaunchPlan(const Paths& paths, const Settings& settings, const std::vector<GpuInfo>& gpus);
inline constexpr const char* kGameClosedEarlyMessage =
    "The game closed unexpectedly while starting. Please use 'Save a Bug Report' and attach it to your "
    "GitHub report. On Intel graphics, installing the newest Intel graphics driver may help.";
// Detached start. process_started is set immediately after creation; with watch_seconds > 0 an
// unsuccessful early exit is reported so the launcher can offer diagnostics instead of closing silently.
// cancel is observed during the watch; an unclassifiable wait result returns Result::Unknown().
Result StartGame(const LaunchPlan& plan, double watch_seconds = 0.0,
                 std::atomic<bool>* process_started = nullptr, const CancelFlag* cancel = nullptr);

// ---------------------------------------------------------------------------------------------- keyboard controls
// The game's keyboard keys: keybind_* in wwe2k14.toml beside the game executable (read by the game at start).
// Key names are ReXGlue's (src/ui/keybinds.cpp): "A", "Space", "Shift", "Up", "Numpad5", ...
struct KeyBinding {
  const char* label;        // controller button it plays as ("Right trigger (RT)")
  const char* cvar;         // keybind_right_trigger
  const char* default_key;  // the game's own default ("Control")
};
struct KeyboardControls {
  std::vector<std::string> keys;  // one per KeyBindings() row; "" = no key
  std::vector<bool> present;      // the row was set in wwe2k14.toml
  std::vector<bool> modified;     // changed in the launcher since loading
};
const std::vector<KeyBinding>& KeyBindings();  // 24 rows, same order as the previous launcher
bool IsSupportedKeyName(std::string_view name);
KeyboardControls DefaultKeyboardControls();
KeyboardControls LoadKeyboardControls(const Paths& paths);  // never fails: unreadable file -> defaults
std::vector<bool> DuplicateKeyBindings(const KeyboardControls& controls);  // true = shares its key with another row
// Adds the launcher's default key for every keybind_* row the file is missing (first start, older files); keeps rows
// that are present. Does nothing when a merge would create a duplicate key.
Result EnsureKeyboardDefaults(const Paths& paths);
// Rewrites only the keybind_* lines (other settings in the file are kept byte for byte); refuses duplicates.
Result SaveKeyboardControls(const Paths& paths, const KeyboardControls& controls);

// ---------------------------------------------------------------------------------------------- bug report
// Zips the newest game logs, wwe2k14-enhanced.ini, GPU list and a short system summary into exe_dir.
Result SaveBugReport(const Paths& paths, fs::path* created_zip);

}  // namespace wwe2k14::launcher
