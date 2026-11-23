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
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wwe2k14::launcher {

namespace fs = std::filesystem;

struct Result {
  bool ok = true;
  std::string error;  // player-readable, empty when ok
  bool unknown = false;  // outcome could not be classified; callers should not report a false failure
  std::string stage;
  static Result Ok() { return {}; }
  static Result Fail(std::string message, std::string stage = {}) { return {false, std::move(message), false, std::move(stage)}; }
  static Result Unknown() { return {false, {}, true, {}}; }
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
// GitHub #105: the game translates missing shaders with shadertools/XenosRecomp(.exe) beside the game executable
// (the same folder the launcher runs from). Antivirus sometimes removes it. No throw.
fs::path ShaderToolsPath(const Paths& paths);
bool ShaderToolsPresent(const Paths& paths);
// True when any path component is a personal or business OneDrive root.
bool IsUnderOneDrive(const fs::path& path);
// Testable path matcher. allow_name_fallback is true only when no OneDrive environment variables exist.
bool IsOneDrivePath(const fs::path& path, const std::vector<fs::path>& roots,
                    bool allow_name_fallback);

// ---------------------------------------------------------------------------------------------- mods
struct ModWrestler {
  uint32_t id = 0;
  std::string name;
  bool female = false;
  bool enabled = false;  // has a roster slot in the game
  std::string game;      // the game they came from, for the tag on the Mods page
};
// The imported wrestlers, sorted by name. Empty when no mod is installed.
std::vector<ModWrestler> LoadModWrestlers(const Paths& paths);
// own-slots.txt reader/writer shared with the importer: valid ids 100..499; plain-bytes text with the launcher's header.
std::set<uint32_t> ReadOwnSlotIds(const fs::path& file);
std::string MakeOwnSlotsText(const std::set<uint32_t>& ids);
// Rebuilds a MISSING own-slots.txt from mods/*/roster.tsv (returns the number of ids written, 0 = nothing done).
size_t RepairOwnSlotsFromRosters(const Paths& paths);
// True when own-slots.txt contains at least one valid imported roster id.
bool HasEnabledModWrestlers(const Paths& paths);
// Files stay in place, so this is instant and reversible. Takes effect the next time the game starts.
Result SetModWrestlerEnabled(const Paths& paths, uint32_t id, bool enabled);
// Updates a group of imported wrestler slots with one plain-bytes rewrite of own-slots.txt.
Result SetModWrestlersEnabled(const Paths& paths, const std::vector<uint32_t>& ids, bool enabled);

// Extra Superstar Heads: applies the verified HB7 record inserts to the installed DLC5 head pack.
// Originals and installed hashes are kept in backups/ so Undo can restore the exact three files.
struct ExtraSuperstarHeadsStatus {
  bool available = false;
  bool applied = false;
  bool changed = false;  // an installed file differs from the launcher-recorded output
  std::string message;
};
ExtraSuperstarHeadsStatus InspectExtraSuperstarHeads(const Paths& paths, const fs::path& game_folder);
Result ApplyExtraSuperstarHeads(const Paths& paths, const fs::path& game_folder,
                                const ProgressFn& progress, CancelFlag& cancel);
Result UndoExtraSuperstarHeads(const Paths& paths, const ProgressFn& progress, CancelFlag& cancel);

// WWE '13 source discovery. The scanner reads XEX/PAC/STFS metadata and file names only; it never copies or
// changes source game data. A non-empty selected_folder limits the scan to that user-picked tree.
struct WWE13ImportPack {
  uint32_t pack_number = 0;
  fs::path folder;
  std::string display_name;
  std::vector<uint32_t> wrestler_ids;
  bool package_file = false;
};
struct WWE13ImportWrestler {
  uint32_t source_id = 0;
  uint32_t target_id = 0;
  std::string name;
  bool available = false;
  bool selected = false;  // available source rows are preselected for review
  std::string source_label;
  std::string missing_reason;
};
struct WWE13ImportScan {
  fs::path base_folder;
  bool base_found = false;
  bool base_is_disc_image = false;  // base_folder is the player's WWE '13 disc image (.iso), read without extracting it
  std::string error;
  bool cancelled = false;
  bool search_limit_reached = false;
  uint64_t directories_visited = 0;
  std::vector<WWE13ImportPack> packs;
  std::vector<WWE13ImportWrestler> wrestlers;
};
// A WWE '13 disc image: default.xex is WWE '13 (545108B4) and the disc has pac/ch/chEtc.pac, pac/DLC.pac, pac/string.pac
// (the files the folder check needs).
bool IsWWE13DiscImage(const fs::path& iso);
WWE13ImportScan ScanWWE13ImportSources(const Paths& paths, const fs::path& selected_folder,
                                        const ProgressFn& progress, const CancelFlag& cancel);
// A wrestler's theme volume in the game's music pack (game folder/sound/DLC_01.pck). Stored per song, so wrestlers
// who share a song share a value (same sound id). Range: what has run in the game, capped at the highest proven boost.
inline constexpr float kModThemeMinDb = -12.0f, kModThemeMaxDb = 6.1f;
struct ModTheme {
  uint64_t offset = 0;
  uint32_t sound = 0;
  float db = 0.0f;
};
// Entries only for ids whose pack data matches the known layout: an unfamiliar pack gets no slider.
std::map<uint32_t, ModTheme> ReadModThemes(const fs::path& pack, const std::vector<uint32_t>& ids);
// Per wrestler, the song's setting that matches WWE 2K14's own level (written by the import). Empty for older installs.
std::map<uint32_t, float> ReadModThemeLevels(const Paths& paths);
// Changes 4 bytes in place. The game must be closed.
Result SetModThemeVolume(const fs::path& pack, uint64_t offset, float db);

// ---------------------------------------------------------------------------------------------- settings
// WWE 2K14 does not have the WWE '13 runtime's custom frame-rate or scene-AA hooks. The launcher exposes
// the output size/display mode, host frame sync, optional wide-screen stretching, and the runtime's renderer scale.
enum class Renderer { kNative, kXbox };
// v1.7.0 (owner 2026-11-22): "Native" now means the new renderer (the WWE '13 v1.6 renderer, decision 0008), shipped as
// three game programs beside wwe2k14.exe: wwe2k14-vulkan.exe, wwe2k14-d3d12.exe, wwe2k14-d3d11.exe. wwe2k14.exe stays
// for the Xbox renderer. Linux ships no new-renderer program yet: there the native choice keeps wwe2k14.
enum class NativeApi { kVulkan, kD3D12, kD3D11 };
// v1.7.0: the game program a launch uses. Xbox -> paths.game_exe; native on Windows -> wwe2k14-<api>.exe in the same
// folder; native elsewhere -> paths.game_exe. Every game program name (for "is the game running" checks).
fs::path GameExecutableFor(const Paths& paths, Renderer renderer, NativeApi api);
std::vector<fs::path> AllGameExecutables(const Paths& paths);
const char* NativeApiName(NativeApi api);  // "vulkan" / "d3d12" / "d3d11" (ini value and file-name part)
enum class Resolution { k720p, k1080p, k1440p };
enum class DisplayMode { kFullscreen, kWindowed, kBorderless };  // borderless: fullscreen window, monitor refresh untouched (#116)
// Native renderer test package (2026-10-20): the 3D picture size (WWE2K14_D3D_SCENE_SCALE), the frame-rate mode
// (WWE2K14_LOCK_30) and anti-aliasing (WWE2K14_D3D_AA_CAP), as the package's .bat launchers set them.
// Keep the original 0..4 ordinals: native_picture_size is persisted as this integer in existing settings files.
// New sizes are appended so saved 480p/720p/1080p/1440p/4K choices retain their meaning.
enum class PictureSize { k480p, k720p, k1080p, k1440p, k4k, k360p, k540p };
enum class FrameRate { kLocked30, kStandard30_60, k60Everywhere };

// ReXGlue mixes XMP custom songs at 48 kHz. Processed tracks use stereo IEEE-float WAV,
// which the runtime decodes into that same mix format.
inline constexpr uint32_t kGameMusicSampleRate = 48000;
// "Match the game's music" loudness target. Measured 11-02 on the retail sound/Game_Sound_M.pck: the 90 full-length
// stereo music streams (32 kHz, 3-6 min) have whole-song RMS -13.0..-8.2 dBFS, median -9.6 (same RMS as RenderMusic).
inline constexpr float kGameMusicRmsDbfs = -10.0f;
// Peak ceiling of the limiter that RenderMusic applies instead of hard clipping (-1 dBFS).
inline constexpr float kMusicLimiterCeiling = 0.891f;

struct MusicPreset {
  float target_rms_dbfs = -18.0f;
  float fade_in_seconds = 0.25f;
  float fade_out_seconds = 1.0f;
  float bass_gain_db = 0.0f;
  float bass_frequency_hz = 120.0f;
  float treble_gain_db = 0.0f;
  float treble_frequency_hz = 8000.0f;
};

struct MusicTrackSettings {
  float volume_db = 0.0f;
  bool normalize = true;
  float target_rms_dbfs = -18.0f;
  float trim_start_seconds = 0.0f;
  float trim_end_seconds = 0.0f;  // Zero means the source duration.
  float fade_in_seconds = 0.25f;
  float fade_out_seconds = 1.0f;
  float bass_gain_db = 0.0f;
  float bass_frequency_hz = 120.0f;
  float treble_gain_db = 0.0f;
  float treble_frequency_hz = 8000.0f;
};

struct MusicTrackRecord {
  std::string source_relative_path;  // Kept under music/.sources, outside the game's top-level scan.
  MusicTrackSettings settings;
};

struct DecodedMusic {
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  std::vector<float> samples;  // Interleaved, normalized float PCM.
  uint64_t frame_count() const { return channels ? samples.size() / channels : 0; }
};

struct MusicWaveformPoint {
  float minimum_left = 0.0f;
  float maximum_left = 0.0f;
  float minimum_right = 0.0f;
  float maximum_right = 0.0f;
};

struct Settings {
  Renderer renderer = Renderer::kNative;
  NativeApi native_api = NativeApi::kVulkan;  // which new-renderer program (Windows)
  Resolution resolution = Resolution::k720p;
  DisplayMode display = DisplayMode::kFullscreen;
  bool sync_frames_to_monitor = true;
  bool stretch_to_fill_wide_screens = false;
  int picture_scale = 1;  // internal draw scale: 1 = the game's 720p, 2 = 1440p, 3 = 4K (experimental)
  PictureSize picture_size = PictureSize::k720p;  // native renderer 3D picture size
  FrameRate frame_rate = FrameRate::kStandard30_60;
  bool anti_aliasing = true;
  // Xbox renderer (2026-10-23, GitHub #34): the same game-side frame-rate hook (WWE2K14_LOCK_30 is read by
  // src/present_interval.cpp for both renderers) and the SDK's FXAA swap post effect (--swap_post_effect).
  // Defaults keep the Xbox renderer as v1.1.0 shipped it: 30/60, no FXAA.
  FrameRate xbox_frame_rate = FrameRate::kStandard30_60;
  bool xbox_anti_aliasing = false;
  // Lower CPU use (owner 2026-10-24, GitHub #12, card LP1): WWE2K14_LOW_POWER=1 sleeps instead of busy-polling in
  // the guest's ring-space retry loop once a wait passes 1 ms. Native renderer only (measured there). Default OFF since
  // v1.4 (owner 2026-10-27: steady 60 fps with it off on the laptop; #60/#69 slow-down reports); saved as lower_cpu so
  // the low_power=1 that every earlier launcher wrote is not carried over.
  bool low_power = false;
  // Faster loading (owner 2026-10-29, card N4e): with Locked 30, WWE2K14_LOCK_30=4 lets loading screens run at 60 while
  // matches stay at 30 (UHD 630: post-entrance loads 10.2 / 8.9 s -> 6.3 / 7.5 s, match median unchanged). Native
  // renderer, Locked 30 only (the loading detector lives in the native renderer). Off by default.
  bool fast_loading = false;
  // Controller vibration (player request #102, owner OK 2026-11-08): off passes --input_vibration=false, so the game's
  // rumble requests reach the pad as motors off. Both renderers. On by default (the game's own behaviour).
  bool controller_vibration = true;
  // GitHub #87: keyboard keys bound to controller buttons keep that meaning on name/text screens (they do not type).
  bool keybinds_while_typing = false;
  // Intel Lake compatibility shaders: WWE2K14_NR_CONSTANTS32=1 selects the shader set made for Intel Ice Lake / Gen11
  // (no 64-bit shader math). Native renderer only; off by default (the game already switches to it when needed).
  bool intel_lake_shaders = false;
  // Crowd (owner 2026-10-24, GitHub #13, lane weakpc-opts): WWE2K14_D3D_CROWD drops crowd draws before translation to
  // save CPU on weak PCs. 1 = full (default, unchanged picture), 0 = none (2 = half exists in the renderer but measured
  // no gain on the UHD 630, so the launcher offers Full/None only). Native renderer only.
  int crowd = 1;
  // Shadows (owner 2026-10-29): WWE2K14_D3D_SHADOWS=0 removes the shadow passes (UHD 630 budget: ~3.1 ms/frame).
  // 1 = on (default, unchanged picture), 0 = off. Native renderer only.
  int shadows = 1;
  // Texture quality (owner 2026-10-29): WWE2K14_D3D_TEX_QUALITY=1 samples eligible game textures one mip level smaller
  // (UHD 630 720p: gaps>=70 ms 192 -> 59, gameplay p10 39 -> 45 fps). 1 = normal (default), 0 = low. Native only.
  int textures = 1;
  // Texture filtering (owner 2026-11-06): 1 = sharp, the forced 4x (default, unchanged picture); 0 = the game's own
  // filtering, floors at an angle a little softer (UHD 630 720p, one run: 52.4 -> 53.8 fps). Native renderer only.
  int texture_filtering = 1;
  // Performance mode (owner 2026-11-06: what lowers the picture is opt-in): at launch, anti-aliasing off and the game's
  // own texture filtering, without changing the saved choices, so turning it off restores them. Native renderer only.
  bool performance_mode = false;
  int multi_shadows = -1;  // Full shadows in big matches: -1 automatic (see MultiShadowsOn), 0 off, 1 on
  bool hiac_weapon_wheel = false;  // Mods page, Gameplay: weapon wheel in Hell in a Cell
  bool weapon_wheel_more = false;  // Mods page, Gameplay: also allow the weapon wheel in other match types
  bool head_edit = false;  // Mods page, Gameplay: Superstar Heads can edit neck, traps, hair and facial hair
  bool unlock_all = false;  // Mods page, Gameplay: every Unlockable and all 30 Years of WrestleMania eras/matches open
  // Owner 11-18: Mods on/off. Off = every Mods-page option off and the game starts from a clean view of the original
  // files with its own saves (mods_switch.h); the imports stay installed for when Mods are switched back on.
  bool mods_enabled = true;
  bool imports_in_grid = true;  // Mods page: imported wrestlers fill the main select grid's free places (WWE2K14_OWN_GRID=1)
  // Owner test builds only: allow the RenderDoc in-app capture path when the test marker is present.
  bool glitch_capture_mode = false;
  // Game language (2026-10-23): the Xbox language id passed as --user_language (1 English, 3 German, 4 French,
  // 5 Spanish, 6 Italian; see kGameLanguages). Both renderers.
  int language = 1;
  std::string gpu_id;    // stable id of the chosen Vulkan device ("" = recommended)
  fs::path game_folder;  // folder holding default.xex + default.xexp
  bool auto_start = false;          // "Start straight away next time" (hold Shift at start to come back)
  bool auto_backup = true;          // back up saves before each launch, keep last 5
  bool check_updates = true;        // look for a newer release when the launcher starts
  std::string skipped_update_version;  // "Skip this version" choice ("" = none)
  MusicPreset music_preset;
  std::map<std::string, MusicTrackRecord> music_tracks;  // keyed by processed WAV filename.
  bool explicit_choice = false;     // legacy global override bit, migrated into the per-setting flags below
  // Versioned, per-setting ownership for hardware recommendations. Older INIs used one explicit_choice bit;
  // settings.cpp migrates that bit to all four fields once and writes these flags on the next save.
  bool recommendation_fields_tracked = false;
  bool manual_resolution = false;
  bool manual_picture_size = false;
  bool manual_frame_rate = false;
  bool manual_xbox_frame_rate = false;
};
struct GameLanguage {
  int id;
  const char* name;
};
// Languages offered in the launcher. French verified in-game 2026-10-23 (title + demo banner); the others use the
// same string.pac selector and still need an in-game check.
inline constexpr GameLanguage kGameLanguages[] = {
    {1, "English"}, {4, "Fran\xC3\xA7" "ais"}, {3, "Deutsch"}, {6, "Italiano"}, {5, "Espa\xC3\xB1" "ol"}};
bool IsOfferedLanguage(int id);

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
  std::string driver;      // driver version as the vendor writes it (bug reports only)
  uint64_t vram_mb = 0;    // largest device-local memory heap (bug reports only)
};
std::vector<GpuInfo> ListGpus();  // Vulkan enumeration (volk), CPU/llvmpipe devices excluded
// GPU profiles are backed by the measured UHD 630 and Radeon 780M classes; unmeasured devices retain the prior
// integrated/discrete fallback. logical_processors is injected in tests and detected by default at runtime.
struct Recommendation {
  Settings settings;
  std::string reason;
  std::string gpu_id;  // the card the recommendation is for (RecommendedSettings: the strongest one found)
};
enum class RecommendedField { kResolution, kPictureSize, kFrameRate, kXboxFrameRate };
struct RecommendationChange {
  std::string label;
  std::string current;
  std::string recommended;
  bool manually_changed = false;
};
Recommendation RecommendedSettings(const std::vector<GpuInfo>& gpus, unsigned logical_processors = 0);
// The recommendation for the graphics card the game will actually use: the selected one when it is present,
// otherwise all cards. The Settings page and the launch must both use this, or they can disagree (a laptop with a
// discrete card showed 720p while the selected integrated card launched at 480p / Locked 30).
Recommendation RecommendedSettingsFor(const std::vector<GpuInfo>& gpus, const std::string& gpu_id,
                                       unsigned logical_processors = 0);
// Automatic use preserves manually changed fields. A confirmed "Re-check my PC" may pass replace_manual=true.
Settings ApplyRecommendedSettings(const Settings& current, const Settings& recommended, bool replace_manual = false);
std::vector<RecommendationChange> CompareRecommendedSettings(const Settings& current, const Settings& recommended);

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
// The same home/drive/mount roots used by FindGameFiles, for other bounded discovery tasks.
std::vector<fs::path> InstallationSearchRoots(const Paths& paths);

// ---------------------------------------------------------------------------------------------- disc image (F2)
// Player's own Xbox 360 disc image (XDVDFS / XISO, including Redump images with a video partition offset).
// Verifies it is WWE 2K14 (default.xex title ID) before extracting into dest_folder.
Result ExtractDiscImage(const fs::path& iso, const fs::path& dest_folder, const ProgressFn& progress,
                        const CancelFlag& cancel);
// The title ID in an Xbox 360 disc image's default.xex; 0 when it is not a readable Xbox 360 disc image.
uint32_t ReadDiscImageTitleId(const fs::path& iso);
// Reading other games' disc images in place (WWE '13 / WWE 2K15 imports). Paths are relative to the disc root with '/'
// separators, in the disc's own letter case; lookups ignore case (XDVDFS names are case-insensitive).
struct DiscImageFile {
  std::string path;
  uint64_t size = 0;
};
std::vector<DiscImageFile> ListDiscImageFiles(const fs::path& iso);  // every file; empty when unreadable
bool ReadDiscImageFile(const fs::path& iso, const std::string& relative_path, std::vector<uint8_t>* bytes);
// Copies the named files to dest_folder/<path on the disc>. Fails (naming the file) when one is missing.
Result CopyDiscImageFiles(const fs::path& iso, const std::vector<std::string>& relative_paths, const fs::path& dest_folder,
                          const ProgressFn& progress, const CancelFlag* cancel);

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
inline constexpr const char* kGameRunningBackupMessage = "Close the game first, then back up.";
inline constexpr const char* kGameRunningRestoreMessage = "Close the game first, then restore.";
// Shared permission rule used by the save operations and launcher UI.
bool SaveOperationsAllowed(bool game_process_running) noexcept;
struct BackupInfo {
  fs::path file;        // backups/wwe2k14-saves-YYYYMMDD-HHMMSS[-auto].zip
  std::string when;     // local time, "Today, 9:41 AM" style is the UI's job; this is ISO 8601
  uint64_t bytes = 0;
  bool automatic = false;
  bool tag_known = false;  // unmarked legacy backups stay unclassified until a contents check is available
  bool modded = false;     // sidecar snapshot of the last Play launch
};
std::vector<BackupInfo> ListBackups(const Paths& paths);
Result BackupSaves(const Paths& paths, bool automatic, BackupInfo* created);  // automatic: keep the newest 5
// Stores the last launched game's Normal/Modded state beside userdata, never inside a game save file.
Result MarkLastPlayModded(const Paths& paths, bool modded);
Result RestoreBackup(const Paths& paths, const fs::path& backup_zip);        // backs up the current saves first
Result ImportBackup(const Paths& paths, const fs::path& external_zip);       // copies into backups/ after checking it

// ---------------------------------------------------------------------------------------------- creation save import (P5)
// Creation import is limited to the byte-mapped CAW/Arena record regions and verified creation-folder/header
// naming. Unrelated progression bytes are preserved; the main SaveData.dat.header is copied unchanged because
// the owner samples do not establish a per-single-record update for its opaque bytes at 0x144+.
enum class CreationSaveKind { kSuperstar, kArena };
struct CreationSaveRecord {
  CreationSaveKind kind = CreationSaveKind::kSuperstar;
  uint32_t slot_index = 0;  // zero-based record index; the game's displayed slot number is index + 1
  std::string name;         // creation's own name from the record (may be empty)
};
// Lists a structurally recognized save. A null optional means the save layout is unsupported.
std::optional<std::vector<CreationSaveRecord>> ListCreationSaveRecords(const Paths& paths);
// Names the first failing safety check of the save ("" when it reads safely); used only to log WHY it was refused.
std::string DescribeCreationStateProblem(const Paths& paths);
// "" or the log line "save has N unrecognized CAW records / M arena records / K side-table values; they are kept
// as they are" when the tolerant save gate had to keep data it does not recognize (#115/#75).
std::string DescribeCreationTolerance(const Paths& paths);
std::optional<uint32_t> FindFreeCreationSaveSlot(const Paths& paths, CreationSaveKind kind);
// Import/remove the mapped record and its SaveData.Dat folder/header sidecar. Each write creates and validates a
// full saves backup first. Unsupported save layouts are rejected with no save mutation.
// Import and removal also register payload folders that have no record and repair old-launcher records in the SAME
// operation and with the same pre-change backup. `summary` reports the creation's name and what was done extra.
// Test hook: the automatic register/repair during import and removal is ON by default.
void SetCreationAutoRegisterEnabled(bool enabled);
struct CreationChangeSummary {
  std::string name;
  uint32_t registered = 0;
  uint32_t repaired = 0;
};
Result ImportCreationSave(const Paths& paths, CreationSaveKind kind, const fs::path& creation_folder,
                          uint32_t* selected_slot = nullptr, CreationChangeSummary* summary = nullptr);
// Renames the game-visible name for a registered CAW or arena. A verified full-save backup is made first; the
// creation payload and its main-save record are updated together with their game checksums.
Result RenameCreation(const Paths& paths, CreationSaveKind kind, uint32_t slot_index,
                      std::string_view new_name);
struct CreationExportSummary {
  std::string name;
  uint32_t slot_index = 0;
};
// Exports one validated, registered creation as a self-contained .cas/.car STFS CON package. The save is read-only;
// `output_file` must be outside the player's userdata tree.
Result ExportCreation(const Paths& paths, CreationSaveKind kind, uint32_t slot_index,
                      const fs::path& output_file, CreationExportSummary* summary = nullptr);
// Registers recognized two-digit creation payload folders already under the save's content root when their
// matching main-save record is empty. Payload folders and their existing creation headers are not copied or renamed.
// A full verified save backup is created before the main save is updated; an already-registered save is a no-op.
// Also repairs CAW records that lack the per-slot entry/marker/thumbnail header (made by older launchers).
Result RegisterExistingCreationSaves(const Paths& paths, uint32_t* registered_count = nullptr,
                                     uint32_t* repaired_count = nullptr);
Result RemoveCreationSave(const Paths& paths, CreationSaveKind kind, uint32_t slot_index,
                          CreationChangeSummary* summary = nullptr);
// ---------------------------------------------------------------------------------------------- stories (.sas)
// A story is one saved-game file inside the player's save: <save>/00000001/NNCustomStory.sas/SaveData.Dat with a
// matching Headers/00000001/NNCustomStory.sas.header (NN = slot, shown in the game as NN+1). The file is a self-contained
// 5-chunk ZLIB container (13,517,108 bytes of story data, name = ASCII at +4). No story index in the main save is
// known: list/export read the files; import only adds the two new files (see docs in report N5-stories.md).
struct StoryInfo {
  uint32_t slot_index = 0;   // zero-based file number; the game shows slot_index + 1
  std::string name;          // the story's own name (empty when the file is damaged)
  bool damaged = false;      // the file did not pass the container/structure check
};
// nullopt: the save folder is missing its safe layout. An empty list means no story files yet.
std::optional<std::vector<StoryInfo>> ListStories(const Paths& paths);
std::optional<uint32_t> FindFreeStorySlot(const Paths& paths);
struct StoryChangeSummary {
  std::string name;
  uint32_t slot_index = 0;
};
// Writes the story in `slot_index` to `output_file` as a .sas package (an unsigned Xbox 360 "CON " saved-game package
// holding SaveData.Dat, readable by ImportStory and by the creation importers' package reader). The package is read
// back and compared before the call reports success. Does not change the save.
Result ExportStory(const Paths& paths, uint32_t slot_index, const fs::path& output_file,
                   StoryChangeSummary* summary = nullptr);
// Accepts a .sas package, a .zip holding one, an unpacked NNCustomStory.sas folder (or a folder containing exactly one),
// or the raw SaveData.Dat. Checks the file thoroughly, makes a verified save backup, then adds the story to the first
// free slot (payload + header in one journalled transaction). Wrong/damaged files are rejected before any change.
Result ImportStory(const Paths& paths, const fs::path& source, uint32_t* selected_slot = nullptr,
                   StoryChangeSummary* summary = nullptr);
// Test hook / diagnostic: validates a story file's bytes (same check the import uses). Returns an empty string and
// fills `name` when valid, otherwise a player-readable reason.
// Test hook: make the next creation/story commits throw after N successful renames (-1 = off).
// stage 1 = throw after all renames but before the commit marker is complete; 2 = throw after the commit.
void SetCommitFaultInjectionForTest(int fault_after_renames, int stage = 0);
std::string CheckStoryFileBytes(const std::vector<uint8_t>& file, std::string* name);
std::vector<uint8_t> BuildStoryHeaderBytes(uint32_t slot_index, const std::string& name);

// ---------------------------------------------------------------------------------------------- custom images (logos)
// The Paint Tool save (00PaintTool.pt) holds the game's 20 custom-logo slots (128x128 or 256x256, used under the CAW editor's
// TATTOOS -> PAINT TOOL DATA). Adding a picture fits it into a free slot; every change makes a save backup first.
constexpr uint32_t kLogoSlotCount = 20;
enum class LogoImageSize : uint32_t { k128 = 128, k256 = 256 };
struct LogoImage {
  uint32_t slot_index = 0;       // 0-19 (the game shows 1-20)
  uint32_t id = 0;               // the game's content id for this logo
  bool built_in = false;         // the game's own logo: never removed or replaced by the launcher
  uint32_t width = 256;
  uint32_t height = 256;
  std::vector<uint8_t> rgba;     // 256 x 256 RGBA8, straight alpha
};
struct LogoList {
  bool file_present = false;     // false: no Paint Tool save yet (all slots free)
  std::vector<LogoImage> logos;  // used slots only
};
// nullopt: the existing Paint Tool save is unreadable or not the supported 2K14 format (nothing is changed).
std::optional<LogoList> ListLogoImages(const Paths& paths);
// Fits a PNG/JPG/BMP/TGA/GIF into the first free slot at 256x256. `slot` receives the 0-based slot.
Result AddLogoImage(const Paths& paths, const fs::path& picture, uint32_t* slot = nullptr);
// Per-import size choice. Existing two-argument calls keep the 256x256 default.
Result AddLogoImage(const Paths& paths, const fs::path& picture, LogoImageSize size, uint32_t* slot = nullptr);
// Stores a canonical 256x256 RGBA canvas in the first free slot at the requested game resolution.
Result AddLogoImage(const Paths& paths, const std::vector<uint8_t>& rgba256, LogoImageSize size,
                    uint32_t* slot = nullptr);
// Replaces a player's custom logo while keeping its stored 128x128 or 256x256 resolution.
Result EditLogoImage(const Paths& paths, uint32_t slot_index, const std::vector<uint8_t>& rgba256);
Result RemoveLogoImage(const Paths& paths, uint32_t slot_index);

// Restores or finishes cleanup for interrupted creation transactions before the launcher lists saved creations.
Result RecoverCreationSaveFiles(const Paths& paths);

// ---------------------------------------------------------------------------------------------- installation import
struct InstallationImportInfo {
  fs::path source_root;
  uint64_t save_files = 0;
  uint64_t save_bytes = 0;
  uint64_t dlc_files = 0;
  uint64_t dlc_bytes = 0;
  uint64_t other_userdata_files = 0;
  uint64_t other_userdata_bytes = 0;
  uint64_t music_files = 0;
  uint64_t music_bytes = 0;
  bool has_userdata = false;
  bool has_music = false;
  bool has_ini = false;
  bool has_keyboard_settings = false;
  bool backup_required = false;
  uint64_t total_files = 0;
  uint64_t total_bytes = 0;
};
struct FoundInstallationImport {
  fs::path folder;
  uint64_t saves = 0;
  uint64_t dlc_packs = 0;
  uint64_t bytes = 0;
  std::string newest_save_when;
  int64_t newest_save_time = 0;
};
// Selects the installation root in a release archive, validates it, and reports what can be copied.
Result ScanInstallationImport(const Paths& paths, const fs::path& selected_folder,
                              InstallationImportInfo* info);
// Searches the same home/drive/mount roots as FindGameFiles, with a short time limit. Never throws.
std::vector<FoundInstallationImport> FindInstallationImports(const Paths& paths,
                                                              const ProgressFn& progress,
                                                              const CancelFlag& cancel);
// Copies the source installation's user data/settings/music without modifying its files. The current
// install's userdata/cache is retained; conflicts are covered by a pre-import backup.
Result ImportInstallation(const Paths& paths, const InstallationImportInfo& info,
                          const ProgressFn& progress, const CancelFlag& cancel,
                          fs::path* backup_created = nullptr);

// ---------------------------------------------------------------------------------------------- music
struct Song {
  fs::path file;
  std::string title;     // file name without extension
  double seconds = 0;    // 0 when unknown
  std::string format;    // "MP3" / "FLAC" / "WAV" / "OGG"
  bool processed = false;
};
std::vector<Song> ListSongs(const Paths& paths);
fs::path MusicSourcePath(const Paths& paths, const Settings& settings, const fs::path& song_file);
MusicTrackSettings MusicSettingsForSong(const Settings& settings, const fs::path& song_file);
Result DecodeMusicFile(const fs::path& file, DecodedMusic* decoded);
std::vector<MusicWaveformPoint> BuildMusicWaveform(const DecodedMusic& decoded, size_t point_count = 512);
Result RenderMusic(const DecodedMusic& decoded, const MusicTrackSettings& settings,
                   uint32_t output_rate, std::vector<float>* stereo_output);
Result WriteMusicWavFloat(const fs::path& file, const std::vector<float>& stereo_samples,
                          uint32_t sample_rate = kGameMusicSampleRate);

// Live listening in the launcher: plays a song at its own rate and applies the current settings block by block, so
// slider moves are heard at once. The saved file still comes from RenderMusic; volume, loudness, trim, fades and
// bass/treble match it, and peaks use a no-look-ahead limiter (same -1 dBFS ceiling). Thread-safe: the UI sets
// settings/seeks while the audio thread calls Render.
class MusicLivePlayer {
 public:
  explicit MusicLivePlayer(const DecodedMusic& decoded);
  uint32_t sample_rate() const { return rate_; }
  uint64_t frame_count() const { return frames_; }
  // original = play the file untouched (no trim, level or effects).
  void SetSettings(const MusicTrackSettings& settings, bool original);
  void Seek(double seconds);
  double PositionSeconds() const;
  // Writes up to `frames` stereo frames; returns how many were written (fewer = the song ended).
  size_t Render(float* stereo, size_t frames);

 private:
  struct Shelf { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1[2] = {0, 0}, z2[2] = {0, 0}; };
  double RegionRms(uint64_t start, uint64_t end) const;
  void Configure();  // mutex held

  uint32_t rate_ = 0;
  uint64_t frames_ = 0;
  std::vector<float> stereo_;
  std::vector<double> block_squares_;  // sum of squares per kRmsBlock frames, for quick loudness of a trim range
  mutable std::mutex mutex_;
  MusicTrackSettings settings_;
  bool original_ = false;
  uint64_t position_ = 0, start_ = 0, end_ = 0;
  double gain_ = 1.0, limiter_ = 1.0, release_ = 0.0;
  uint64_t rms_start_ = UINT64_MAX, rms_end_ = UINT64_MAX;
  double rms_ = 0.0;
  Shelf bass_, treble_;
  float bass_db_ = 0, bass_hz_ = 0, treble_db_ = 0, treble_hz_ = 0;
};
Result SaveMusicEdit(const Paths& paths, Settings* settings, const fs::path& song_file,
                     const DecodedMusic& decoded, const MusicTrackSettings& edit,
                     fs::path* saved_song_file);
// Copies dropped files into music_dir (skips unsupported types with a message; Unicode names are fine).
Result AddSongs(const Paths& paths, const std::vector<fs::path>& files, std::vector<std::string>* skipped);
Result RemoveSong(const Paths& paths, const fs::path& song_file, Settings* settings = nullptr);

// ---------------------------------------------------------------------------------------------- launch
// The exact command line and environment the launcher uses from Settings: output size/display, optional
// letterbox and renderer scale, and host frame sync. Only the diagnostics the 2K14 runtime reads
// (WWE13_THREAD_PROFILE*) are passed through; the WWE '13-only override variables are not.
struct LaunchPlan {
  fs::path executable;
  std::vector<std::string> arguments;
  std::vector<std::pair<std::string, std::string>> environment;  // overrides on top of the launcher's env
  std::vector<std::string> environment_to_remove;  // keys that must not be inherited by this renderer
  fs::path working_directory;
};
// Owner test builds: this file next to the launcher enables WWE2K14_UNLOCK_ALL=1 (never in public packages).
inline constexpr const char* kTestUnlockAllMarker = "TEST-UNLOCK-ALL.txt";
// Returns the package root containing the existing test marker, or an empty path for public builds.
fs::path TestBuildMarkerRoot(const Paths& paths);
bool MultiShadowsOn(const Settings& settings, const std::vector<GpuInfo>& gpus);
LaunchPlan BuildLaunchPlan(const Paths& paths, const Settings& settings, const std::vector<GpuInfo>& gpus);
inline constexpr const char* kGameClosedEarlyMessage =
    "The game closed unexpectedly while starting. Please use 'Save a Bug Report' and attach it to your "
    "GitHub report. On Intel graphics, installing the newest Intel graphics driver may help.";
// Detached start. process_started is set immediately after creation; with watch_seconds > 0 an
// unsuccessful early exit is reported so the launcher can offer diagnostics instead of closing silently.
// cancel is observed during the watch; an unclassifiable wait result returns Result::Unknown().
// process_id (optional) receives the game's process id once it is created.
Result StartGame(const LaunchPlan& plan, double watch_seconds = 0.0,
                 std::atomic<bool>* process_started = nullptr, const CancelFlag* cancel = nullptr,
                 uint64_t* process_id = nullptr);
// Linux: the game keeps its guest memory in a named file under shm_dir (/dev/shm/xenia_memory_<n>) that only a clean
// exit removes. A forced close (Alt+F4 hang, the 3 s close backstop, a kill) leaves it holding gigabytes of RAM until
// reboot, and on a small-memory PC the next start dies at once with SIGBUS (GitHub #107). Removes such files that no
// running process maps and that are older than min_age_seconds; returns how many and how many bytes. No-op on Windows.
struct StaleMemoryCleanup { size_t files = 0; uint64_t bytes = 0; };
StaleMemoryCleanup RemoveStaleGuestMemoryFiles(const fs::path& shm_dir = "/dev/shm", double min_age_seconds = 30.0,
                                               const fs::path& proc_dir = "/proc");

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
// With session_log (logs/wwe2k14-YYYYMMDD-HHMMSS.log), the zip holds that game session instead of the newest three
// logs: its log, timeline and freeze reports, plus both launcher logs. It is named after the session
// (wwe2k14-bug-report-game-YYYYMMDD-HHMMSS.zip), so saving it again replaces it.
Result SaveBugReport(const Paths& paths, fs::path* created_zip, const fs::path& session_log = {});
// Plain-words reading of the crash watcher's exit note ("exit_code=0xC0000005"); used in bug reports.
std::string DescribeExitNote(std::string_view note_text);

// ---------------------------------------------------------------------------------------------- game sessions
// Bug Reports page (owner 2026-11-09). One game start = one logs/wwe2k14-YYYYMMDD-HHMMSS.log. The game writes
// "CRASH UNHANDLED_EXCEPTION" from its crash handler; a crash after the window closed ("disarmed at window close",
// "Execution complete") is the known exit crash, which players do not notice, so it does not count as a crash.
enum class SessionEnd {
  kClosed,        // the player closed the game (including the known exit crash)
  kCrashed,       // crashed while playing
  kFroze,         // a freeze report was written and the game did not close normally
  kNoEnding,      // log stops without a close or crash line (still running, forced close, power loss)
};
struct GameSession {
  fs::path log;
  std::string started;      // "2026-11-09 06:15:52", from the file name
  std::string ended;        // ISO 8601 of the log's last write
  uint64_t log_bytes = 0;
  SessionEnd end = SessionEnd::kNoEnding;
  std::string crash_code;   // "C0000005" when the crash line or the crash watcher named one
  fs::path timeline;        // logs/wwe2k14-timeline-*.txt that started with this session (empty if none)
  std::vector<fs::path> freeze_reports;
  fs::path report;          // exe_dir/wwe2k14-bug-report-game-<stamp>.zip if saved
};
// Classifies one game log's text. exit_code_text is the crash watcher's sidecar (<log>.exit), may be empty.
SessionEnd ClassifyGameLogText(std::string_view log_text, std::string_view exit_code_text, bool has_freeze_report,
                               std::string* crash_code);
GameSession ReadGameSession(const Paths& paths, const fs::path& log);
// Newest first, at most max_sessions.
std::vector<GameSession> ListRecentGameSessions(const Paths& paths, size_t max_sessions = 10);
struct BugReportFile {
  fs::path file;
  std::string when;  // ISO 8601
  uint64_t bytes = 0;
};
std::vector<BugReportFile> ListBugReports(const Paths& paths, size_t max_reports = 10);
inline constexpr const char* kBugReportIssueUrl = "https://github.com/PromethenDev/wwe2k14-recomp/issues/new";
// The crash pop-up is shown once per crashed session: logs/crash-notice-seen.txt keeps the newest one shown.
bool CrashNoticeSeen(const Paths& paths, const fs::path& log);
void MarkCrashNoticeSeen(const Paths& paths, const fs::path& log);
// The newest session if it crashed within max_age_hours and its pop-up was not shown yet.
std::optional<GameSession> UnseenRecentCrash(const Paths& paths, double max_age_hours = 72.0);

// Crash watcher: after Play the launcher closes as before, and a small copy of itself without a window
// (launcher --watch-game <pid> <log>) waits for the game to end. Windows records the exit code in <log>.exit. If the
// session crashed it saves that session's bug report and starts the launcher again, which shows the crash pop-up.
Result StartCrashWatcher(const fs::path& launcher_executable, uint64_t game_process_id, const fs::path& game_log);
// Blocks until the process ends. Returns false if it could not be watched. exit_code is set on Windows only.
bool WaitForProcessExit(uint64_t process_id, std::optional<uint32_t>* exit_code);
// Starts the launcher with extra arguments, detached.
Result StartLauncherWithArguments(const fs::path& launcher_executable, const std::vector<std::string>& arguments);
// The --log_file=... argument of a launch plan, or empty.
fs::path LaunchPlanLogFile(const LaunchPlan& plan);
// The watcher's "a crash is waiting to be shown" note; true once (the note is removed).
bool TakeCrashPending(const Paths& paths);
// Windows: opens Explorer with the file selected. Elsewhere returns false (open the folder instead).
bool ShowFileInFolder(const fs::path& file);
// The --watch-game mode's whole job (no window). Returns the process exit code.
int RunCrashWatcher(const fs::path& launcher_executable, uint64_t game_process_id, const fs::path& game_log);

}  // namespace wwe2k14::launcher
