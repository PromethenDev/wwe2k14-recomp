#ifdef NDEBUG
#undef NDEBUG
#endif

#include "launcher_core.h"
#include "../../ui/display_scale.h"

#include "internal/archive.h"
#include "internal/util.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {

void Put16(std::ostream& output, uint16_t value) {
  output.put(static_cast<char>(value & 0xFF));
  output.put(static_cast<char>(value >> 8));
}

void Put32(std::ostream& output, uint32_t value) {
  Put16(output, static_cast<uint16_t>(value));
  Put16(output, static_cast<uint16_t>(value >> 16));
}

void WriteShortWav(const fs::path& file) {
  constexpr uint32_t rate = 8000;
  constexpr uint32_t frames = 2000;
  constexpr uint32_t data_bytes = frames * 2;
  std::ofstream output(file, std::ios::binary | std::ios::trunc);
  output.write("RIFF", 4);
  Put32(output, 36 + data_bytes);
  output.write("WAVEfmt ", 8);
  Put32(output, 16);
  Put16(output, 1);
  Put16(output, 1);
  Put32(output, rate);
  Put32(output, rate * 2);
  Put16(output, 2);
  Put16(output, 16);
  output.write("data", 4);
  Put32(output, data_bytes);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    const int16_t sample = static_cast<int16_t>(std::sin(frame * 2.0 * 3.141592653589793 * 440.0 / rate) * 10000.0);
    Put16(output, static_cast<uint16_t>(sample));
  }
  output.flush();
  assert(output.good());
}

Paths MakePaths(const fs::path& root) {
  Paths paths = ResolvePaths(root / "launcher");
  paths.exe_dir = root;
  paths.game_exe = root / "wwe2k14";
  paths.settings_file = root / "wwe2k14-enhanced.ini";
  paths.userdata_dir = root / "userdata";
  paths.music_dir = root / "music";
  paths.logs_dir = root / "logs";
  paths.backups_dir = root / "backups";
  paths.default_game_folder = root / "WWE 2K14";
  return paths;
}

void TestSettings(const fs::path& root) {
  const fs::path directory = root / "settings";
  fs::create_directories(directory);
  Paths paths = MakePaths(directory);
  const std::string ini =
      "[Settings]\r\nschema=7\r\nanti_aliasing=2x\r\nresolution=1440p\r\n"
      "frame_rate=lock-30\r\ndisplay=windowed\r\ngpu=10de:2684\r\n"
      "game_folder=C:/Games/Roster-\xC3\xA9\r\nauto_start=1\r\n"
      "auto_backup=0\r\nunknown_key=keep-this\r\ngpu_name=Old GPU\r\n\r\n[Other]\r\nkeep=also-this\r\n";
  const auto ini_bytes = wwe2k14::launcher::internal::Utf8ToUtf16LeBytes(ini);
  assert(wwe2k14::launcher::internal::WriteBinaryFile(paths.settings_file, ini_bytes));
  Settings loaded = LoadSettings(paths);
  assert(loaded.resolution == Resolution::k1440p);
  assert(loaded.display == DisplayMode::kWindowed);
  assert(loaded.gpu_id == "10DE:2684");
  assert(wwe2k14::launcher::internal::PathToUtf8(loaded.game_folder) == "C:/Games/Roster-\xC3\xA9");
  assert(loaded.auto_start && !loaded.auto_backup && loaded.explicit_choice);
  assert(loaded.sync_frames_to_monitor && !loaded.stretch_to_fill_wide_screens && loaded.picture_scale == 1);
  assert(SaveSettings(paths, loaded).ok);
  std::vector<uint8_t> saved_bytes;
  assert(wwe2k14::launcher::internal::ReadBinaryFile(paths.settings_file, saved_bytes));
  assert(saved_bytes.size() >= 2 && saved_bytes[0] == 0xFF && saved_bytes[1] == 0xFE);
  std::string saved_text;
  assert(wwe2k14::launcher::internal::ReadTextFile(paths.settings_file, saved_text));
  assert(saved_text.find("schema=7") != std::string::npos);
  assert(saved_text.find("unknown_key=keep-this") != std::string::npos);
  assert(saved_text.find("gpu_name=Old GPU") != std::string::npos);
  assert(saved_text.find("[Other]") != std::string::npos && saved_text.find("keep=also-this") != std::string::npos);
  assert(saved_text.find("sync_frames_to_monitor=1") != std::string::npos);
  assert(saved_text.find("stretch_to_fill_wide_screens=0") != std::string::npos);
  assert(saved_text.find("picture_scale=1") != std::string::npos);

  Settings custom_screen = loaded;
  custom_screen.sync_frames_to_monitor = false;
  custom_screen.stretch_to_fill_wide_screens = true;
  custom_screen.picture_scale = 3;
  assert(SaveSettings(paths, custom_screen).ok);
  const Settings custom_screen_loaded = LoadSettings(paths);
  assert(!custom_screen_loaded.sync_frames_to_monitor && custom_screen_loaded.stretch_to_fill_wide_screens &&
         custom_screen_loaded.picture_scale == 3);

  Paths legacy_paths = MakePaths(directory / "legacy");
  fs::create_directories(legacy_paths.exe_dir);
  const auto legacy_bytes = wwe2k14::launcher::internal::Utf8ToUtf16LeBytes(
      "[Settings]\r\nschema=2\r\ninternal_resolution=576p\r\nframe_rate=classic\r\n"
      "display=fullscreen\r\ngpu=1\r\n");
  assert(wwe2k14::launcher::internal::WriteBinaryFile(legacy_paths.settings_file, legacy_bytes));
  const Settings legacy = LoadSettings(legacy_paths);
  assert(legacy.resolution == Resolution::k720p && legacy.explicit_choice);
  assert(legacy.gpu_id.empty());
  assert(legacy.sync_frames_to_monitor && !legacy.stretch_to_fill_wide_screens && legacy.picture_scale == 1);

  // A 1.0.1 test build's sharper_picture=1 becomes the 1440p picture scale.
  Paths sharper_paths = MakePaths(directory / "sharper");
  fs::create_directories(sharper_paths.exe_dir);
  assert(wwe2k14::launcher::internal::WriteBinaryFile(
      sharper_paths.settings_file,
      wwe2k14::launcher::internal::Utf8ToUtf16LeBytes("[Settings]\r\nschema=7\r\nsharper_picture=1\r\n")));
  assert(LoadSettings(sharper_paths).picture_scale == 2);
  // Out-of-range or garbage picture_scale values fall back to the normal picture (no --resolution_scale argument).
  for (const char* bad : {"0", "4", "-1", "2x", "high", ""}) {
    assert(wwe2k14::launcher::internal::WriteBinaryFile(
        sharper_paths.settings_file, wwe2k14::launcher::internal::Utf8ToUtf16LeBytes(
                                         std::string("[Settings]\r\nschema=7\r\npicture_scale=") + bad + "\r\n")));
    const Settings bad_loaded = LoadSettings(sharper_paths);
    assert(bad_loaded.picture_scale == 1);
    for (const std::string& argument : BuildLaunchPlan(sharper_paths, bad_loaded, {}).arguments) {
      assert(argument.rfind("--resolution_scale", 0) != 0);
    }
  }

  Paths invalid_utf8_paths = MakePaths(directory / "invalid-utf8");
  fs::create_directories(invalid_utf8_paths.exe_dir);
  const std::string invalid_ini = "[Settings]\nschema=7\ngame_folder=bad-\xC3\nauto_backup=0\n";
  assert(wwe2k14::launcher::internal::WriteBinaryFile(invalid_utf8_paths.settings_file,
                                                     invalid_ini.data(), invalid_ini.size()));
  const Settings invalid_utf8 = LoadSettings(invalid_utf8_paths);
  assert(invalid_utf8.game_folder == invalid_utf8_paths.default_game_folder);
  assert(!invalid_utf8.auto_backup);

  Paths resolution_paths = MakePaths(directory / "1080");
  fs::create_directories(resolution_paths.exe_dir);
  Settings resolution_settings;
  resolution_settings.resolution = Resolution::k1080p;
  resolution_settings.explicit_choice = true;
  assert(SaveSettings(resolution_paths, resolution_settings).ok);
  assert(LoadSettings(resolution_paths).resolution == Resolution::k1080p);
  std::cout << "PASS settings UTF-16LE schema-7 round-trip, unknown keys, schema-2 migration, and 1080p persistence\n";
}

void TestUiDisplayScale() {
  using wwe2k14::launcher::ui::ComputeUiScale;
  assert(ComputeUiScale(2.0f, 2.0f) == 1.0f);  // pixel density must not enlarge logical layout.
  assert(ComputeUiScale(2.0f, 1.0f) == 2.0f);  // content scaling remains in layout/fonts.
  assert(ComputeUiScale(1.5f, 2.0f) == 0.75f);
  assert(ComputeUiScale(0.0f, 1.0f) == 1.0f);
  assert(ComputeUiScale(2.0f, 0.0f) == 2.0f);
  assert(ComputeUiScale(1.0f, 4.0f) == 1.0f);  // reject implausibly small resulting scales.
  std::cout << "PASS HiDPI UI scale separates logical content scale from framebuffer density\n";
}

void TestOneDrivePathDetection(const fs::path& root) {
  const fs::path configured_root = root / "OneDriveConsumerRoot";
  fs::create_directories(configured_root / "Nested");
  const fs::path configured_child = configured_root / "Nested" / ".." / "WWE 2K14";
  assert(IsOneDrivePath(configured_child, {configured_root}, false));
  const fs::path case_variant_child = configured_root.parent_path() /
      "onedriveconsumerroot" / "WWE 2K14";
  assert(IsOneDrivePath(case_variant_child, {configured_root}, false));

  // Configured roots take precedence over misleading folder names: do not use the fallback if any
  // OneDrive environment variable exists, even if this particular path is not beneath its root.
  const fs::path local_named_onedrive = root / "Games" / "OneDrive" / "WWE 2K14";
  assert(!IsOneDrivePath(local_named_onedrive, {configured_root}, false));
  const fs::path prefix_lookalike = configured_root.parent_path() /
      (configured_root.filename().string() + "Backup") / "WWE 2K14";
  assert(!IsOneDrivePath(prefix_lookalike, {configured_root}, false));

  // When no configured roots exist, retain the historical folder-name fallback.
  const fs::path business_named = root / "OneDrive - Example Org" / "WWE2K14-Recomp";
  const fs::path unrelated = root / "Games" / "OneDriveBackup" / "WWE 2K14";
  assert(IsOneDrivePath(business_named, {}, true));
  assert(IsOneDrivePath(local_named_onedrive, {}, true));
  assert(!IsOneDrivePath(unrelated, {}, true));
  std::cout << "PASS OneDrive path matching uses canonical configured roots and falls back to folder names only without roots\n";
}

const std::string* EnvironmentValue(const LaunchPlan& plan, const std::string& name) {
  for (const auto& [key, value] : plan.environment) if (key == name) return &value;
  return nullptr;
}

void TestLaunchPlan(const fs::path& root) {
  Paths paths = MakePaths(root / "launch");
  fs::create_directories(paths.exe_dir);
  const std::vector<GpuInfo> gpus = {{"10DE:2684", "Discrete", false, 0},
                                     {"1002:164E", "Integrated", true, 1}};
  const std::vector<Resolution> resolutions = {Resolution::k720p, Resolution::k1080p,
                                               Resolution::k1440p};
  for (Resolution resolution : resolutions) {
    Settings settings;
    settings.resolution = resolution;
    settings.display = DisplayMode::kFullscreen;
    settings.gpu_id = "10DE:2684";
    settings.game_folder = root / "launch" / "Masaüstü" / "WWE 2K14";
    settings.explicit_choice = true;
    const LaunchPlan plan = BuildLaunchPlan(paths, settings, gpus);
    assert(plan.executable == paths.game_exe);
    assert(plan.working_directory == paths.exe_dir);
    const std::vector<std::string> fixed = {
        wwe2k14::launcher::internal::PathToUtf8(settings.game_folder),
        "--async_shader_compilation=false", "--vulkan_async_skip_incomplete_frames=false",
        "--fullscreen=true", "--vulkan_allow_present_mode_immediate=false",
        "--ignore_offset_for_ranged_allocations=true", "--log_verbose=false", "--mnk_mode=true"};
    assert(plan.arguments.size() == fixed.size() + 5);
    for (size_t index = 0; index < fixed.size(); ++index) assert(plan.arguments[index] == fixed[index]);
    assert(plan.arguments[0].find("Masa\xC3\xBCst\xC3\xBC") != std::string::npos);
    assert(plan.arguments[8].starts_with("--log_file="));
    const fs::path log_path = wwe2k14::launcher::internal::PathFromUtf8(plan.arguments[8].substr(11));
    assert(log_path.parent_path().lexically_normal() == paths.logs_dir.lexically_normal());
    assert(wwe2k14::launcher::internal::PathToUtf8(log_path.filename()).starts_with("wwe2k14-"));
    assert(plan.arguments[9] == "--user_data_root=" + wwe2k14::launcher::internal::PathToUtf8(paths.userdata_dir));
    assert(plan.arguments[10] == "--debug_ui=false");
    const std::string expected_resolution =
        resolution == Resolution::k1080p ? "--resolution=1080p"
        : resolution == Resolution::k1440p ? "--resolution=1440p" : "--resolution=720p";
    assert(plan.arguments[11] == expected_resolution);
    assert(plan.arguments[12] == "--vulkan_device_id=10DE:2684");
    // Only the diagnostics the 2K14 runtime reads are passed; the WWE '13-only renderer overrides are not.
    assert(EnvironmentValue(plan, "REX_DEBUG_UI") && *EnvironmentValue(plan, "REX_DEBUG_UI") == "false");
    assert(EnvironmentValue(plan, "WWE13_THREAD_PROFILE") && EnvironmentValue(plan, "WWE13_THREAD_PROFILE")->empty());
    assert(EnvironmentValue(plan, "WWE13_THREAD_PROFILE_HZ") && EnvironmentValue(plan, "WWE13_THREAD_PROFILE_HZ")->empty());
    assert(EnvironmentValue(plan, "WWE13_THREAD_PROFILE_OUT") && EnvironmentValue(plan, "WWE13_THREAD_PROFILE_OUT")->empty());
    assert(EnvironmentValue(plan, "WWE2K14_HOST_VSYNC") && *EnvironmentValue(plan, "WWE2K14_HOST_VSYNC") == "1");
    const std::vector<std::pair<std::string, std::string>> expected_environment = {
        {"REX_DEBUG_UI", "false"}, {"WWE13_THREAD_PROFILE", ""},
        {"WWE13_THREAD_PROFILE_HZ", ""}, {"WWE13_THREAD_PROFILE_OUT", ""},
        {"WWE2K14_HOST_VSYNC", "1"}};
    assert(plan.environment == expected_environment);
    for (const char* dropped : {"WWE2K14_F24_PIXEL_RATE", "WWE2K14_INTERNAL_RES", "WWE2K14_SCENE_AA",
                                "WWE2K14_KEEP_60", "WWE2K14_LOCK_30", "WWE2K14_RATE_CENSUS",
                                "WWE2K14_FRAME_TIME", "WWE2K14_PERF_OVERLAY",
                                "WWE2K14_LOCK_OWNER_SAMPLE"}) {
      assert(EnvironmentValue(plan, dropped) == nullptr);
    }
  }
  // An explicit 1440p selection must survive recommendation resolution and reach the runtime as
  // the selected preset. This verifies launcher-side intent only; the SDK's draw scale is separate.
  Settings selected_1440p;
  selected_1440p.resolution = Resolution::k1440p;
  selected_1440p.gpu_id = "1002:164E";
  selected_1440p.explicit_choice = true;
  const LaunchPlan selected_plan = BuildLaunchPlan(paths, selected_1440p, gpus);
  assert(selected_plan.arguments[11] == "--resolution=1440p");

  Settings output_options;
  output_options.explicit_choice = true;
  const LaunchPlan default_options_plan = BuildLaunchPlan(paths, output_options, gpus);
  assert(default_options_plan.arguments.size() == 12);
  assert(EnvironmentValue(default_options_plan, "WWE2K14_HOST_VSYNC") &&
         *EnvironmentValue(default_options_plan, "WWE2K14_HOST_VSYNC") == "1");

  output_options.sync_frames_to_monitor = false;
  output_options.stretch_to_fill_wide_screens = true;
  output_options.picture_scale = 2;
  const LaunchPlan enabled_options_plan = BuildLaunchPlan(paths, output_options, gpus);
  assert(enabled_options_plan.arguments.size() == 14);
  assert(enabled_options_plan.arguments[12] == "--present_letterbox=false");
  assert(enabled_options_plan.arguments[13] == "--resolution_scale=2");
  assert(EnvironmentValue(enabled_options_plan, "WWE2K14_HOST_VSYNC") &&
         *EnvironmentValue(enabled_options_plan, "WWE2K14_HOST_VSYNC") == "0");
  output_options.picture_scale = 3;
  assert(BuildLaunchPlan(paths, output_options, gpus).arguments[13] == "--resolution_scale=3");

  Settings automatic;
  automatic.gpu_id = "1002:164E";
  const LaunchPlan integrated = BuildLaunchPlan(paths, automatic, gpus);
  // Integrated graphics now share the original 720p preset; 2K14 has no 480p / 30-locked override.
  assert(integrated.arguments[1] == "--async_shader_compilation=false");
  std::cout << "PASS launch plan output sizes, optional widescreen/sharp-picture arguments, monitor-sync env, and defaults\n";
}

void TestGameChecksAndSearch(const fs::path& root, const fs::path& data) {
  Paths paths = MakePaths(root / "game-checks");
  fs::create_directories(paths.exe_dir);
  const fs::path ready_folder = data / "tu-folder-min";
  const fs::path base_only = data / "base-only-min";
  const GameFilesStatus ready = CheckGameFolder(ready_folder, paths);
  assert(ready.base_game == FileState::kFound && ready.title_update == FileState::kFound && ready.ready());
  const GameFilesStatus no_tu = CheckGameFolder(base_only, paths);
  assert(no_tu.base_game == FileState::kFound && no_tu.title_update == FileState::kMissing && !no_tu.ready());
  const fs::path empty = root / "empty-game-folder";
  fs::create_directories(empty);
  const GameFilesStatus missing = CheckGameFolder(empty, paths);
  assert(missing.base_game == FileState::kMissing && missing.title_update == FileState::kMissing);

#ifndef _WIN32
  const fs::path search_root = root / "search-root";
  fs::create_directories(search_root);
  const fs::path symlink = search_root / "linked-ready-game";
  std::error_code error;
  fs::create_directory_symlink(ready_folder, symlink, error);
  assert(!error);
  Paths search_paths = MakePaths(search_root);
  CancelFlag cancel{false};
  const auto found = FindGameFiles(search_paths,
      [&](uint64_t, uint64_t, const std::string& label) {
        if (fs::path(label) == symlink) cancel.store(true);
      }, cancel);
  assert(!found.empty());
  assert(found.front().folder == symlink && found.front().status.ready());
#endif
  std::cout << "PASS game folder title/TU validation (ready, missing TU, empty) and symlink search\n";
}

void TestBackups(const fs::path& root) {
  Paths fresh_paths = MakePaths(root / "fresh-install");
  fs::create_directories(fresh_paths.exe_dir);
  const Result empty_automatic = BackupSaves(fresh_paths, true, nullptr);
  assert(empty_automatic.ok && !fs::exists(fresh_paths.backups_dir));
  const Result empty_manual = BackupSaves(fresh_paths, false, nullptr);
  assert(!empty_manual.ok && empty_manual.error == "No saved game data was found to back up.");

  Paths paths = MakePaths(root / "saves");
  const fs::path save = paths.userdata_dir / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat";
  fs::create_directories(save.parent_path());
  fs::create_directories(paths.exe_dir);
  const std::string original = "original-save-data";
  assert(wwe2k14::launcher::internal::WriteBinaryFile(save, original.data(), original.size()));
  std::vector<uint8_t> read_back;
  assert(fs::is_regular_file(save));
  assert(wwe2k14::launcher::internal::ReadBinaryFile(save, read_back) && read_back.size() == original.size());
  const fs::path zip_probe = root / "zip-probe.zip";
  internal::ZipEntry probe_entry{"userdata/B13EBABEBABEBABE/545408B2/SaveData.Dat", read_back};
  assert(internal::CreateZip(zip_probe, {probe_entry}));
  internal::ZipReader probe_zip;
  assert(probe_zip.Open(zip_probe) && probe_zip.ContainsSaveData());
  Settings settings;
  settings.game_folder = paths.default_game_folder;
  assert(SaveSettings(paths, settings).ok);
  BackupInfo manual;
  assert(BackupSaves(paths, false, &manual).ok && !manual.automatic);
  const std::string changed = "changed-save-data";
  assert(wwe2k14::launcher::internal::WriteBinaryFile(save, changed.data(), changed.size()));
  assert(RestoreBackup(paths, manual.file).ok);
  std::string restored;
  assert(wwe2k14::launcher::internal::ReadTextFile(save, restored));
  assert(restored == original);

  for (int index = 0; index < 6; ++index) assert(BackupSaves(paths, true, nullptr).ok);
  const auto backups = ListBackups(paths);
  size_t automatic_count = 0;
  for (const auto& backup : backups) if (backup.automatic) ++automatic_count;
  assert(automatic_count == 5);

  const fs::path external = root / "external-save.zip";
  const std::string imported_data = "imported-save-data";
  internal::ZipEntry entry{"SaveData.Dat", std::vector<uint8_t>(imported_data.begin(), imported_data.end())};
  assert(internal::CreateZip(external, {entry}));
  assert(ImportBackup(paths, external).ok);
  fs::path imported;
  for (const auto& backup : ListBackups(paths)) {
    if (wwe2k14::launcher::internal::PathToUtf8(backup.file.filename()).find("wwe2k14-saves-import-") == 0) imported = backup.file;
  }
  assert(!imported.empty());
  assert(RestoreBackup(paths, imported).ok);
  assert(wwe2k14::launcher::internal::ReadTextFile(save, restored));
  assert(restored == imported_data);
  std::cout << "PASS save backup/restore, imported SaveData.Dat, and automatic retention (newest 5)\n";
}

void TestMusic(const fs::path& root) {
  Paths paths = MakePaths(root / "music");
  const fs::path input = root / "short-test.wav";
  WriteShortWav(input);
  const fs::path unsupported = root / "notes.txt";
  {
    std::ofstream out(unsupported);
    out << "not audio";
  }
  std::vector<std::string> skipped;
  assert(AddSongs(paths, {unsupported, input}, &skipped).ok);
  assert(skipped.size() == 1 && skipped.front().find("Only MP3, OGG") != std::string::npos);
  const auto songs = ListSongs(paths);
  assert(songs.size() == 1 && songs.front().format == "WAV" && songs.front().title == "short-test");
  assert(std::abs(songs.front().seconds - 0.25) < 0.01);
  assert(RemoveSong(paths, songs.front().file).ok);
  assert(ListSongs(paths).empty());
  std::cout << "PASS music add/list/remove and WAV duration (0.25 s)\n";
}

void TestBugReport(const fs::path& root) {
  Paths paths = MakePaths(root / "bug-report");
  fs::create_directories(paths.logs_dir);
  fs::create_directories(paths.exe_dir);
  for (int index = 0; index < 4; ++index) {
    const fs::path log = paths.logs_dir / ("game-" + std::to_string(index) + ".log");
    std::ofstream output(log);
    output << "log " << index;
    output.close();
    fs::last_write_time(log, fs::file_time_type::clock::now() - std::chrono::seconds(20 - index));
  }
  // An always-on freeze report is written once at freeze time and must survive
  // being pushed out of the newest-three window by later log writes.
  {
    const fs::path freeze = paths.logs_dir / "wwe2k14-freeze-20260101-000000.txt";
    std::ofstream output(freeze);
    output << "freeze report";
    output.close();
    fs::last_write_time(freeze, fs::file_time_type::clock::now() - std::chrono::seconds(60));
  }
  Settings settings;
  settings.game_folder = paths.default_game_folder;
  assert(SaveSettings(paths, settings).ok);
  fs::path report;
  assert(SaveBugReport(paths, &report).ok);
  assert(report.parent_path() == paths.exe_dir && fs::is_regular_file(report));
  wwe2k14::launcher::internal::ZipReader zip;
  assert(zip.Open(report));
  bool newest_seen = false, old_seen = false, ini_seen = false, gpu_seen = false, system_seen = false;
  bool freeze_seen = false;
  for (size_t index = 0; index < zip.Count(); ++index) {
    mz_zip_archive_file_stat stat{};
    assert(zip.Stat(index, stat));
    const std::string name = stat.m_filename;
    if (name == "logs/game-3.log") newest_seen = true;
    if (name == "logs/game-0.log") old_seen = true;
    if (name == "logs/wwe2k14-freeze-20260101-000000.txt") freeze_seen = true;
    if (name == "wwe2k14-enhanced.ini") ini_seen = true;
    if (name == "gpu-list.txt") gpu_seen = true;
    if (name == "system.txt") system_seen = true;
  }
  assert(newest_seen && !old_seen && freeze_seen && ini_seen && gpu_seen && system_seen);
  std::cout << "PASS bug report ZIP includes newest three logs, always-on freeze report, INI, GPU list, "
               "and system summary\n";
}

void TestGpus() {
  const auto gpus = ListGpus();
#ifndef _WIN32
  assert(gpus.size() == 3);
#endif
  for (const auto& gpu : gpus) {
    assert(!gpu.id.empty() && !gpu.name.empty() && gpu.vulkan_index >= 0);
    std::cout << "GPU " << gpu.vulkan_index << ": " << gpu.name << " (" << gpu.id << ")\n";
  }
  const Recommendation integrated = RecommendedSettings({{"1002:164E", "Integrated", true, 0}});
  assert(integrated.settings.resolution == Resolution::k720p);
  const Recommendation discrete = RecommendedSettings({{"10DE:2684", "Discrete", false, 0}});
  assert(discrete.settings.resolution == Resolution::k720p);
  std::cout << "PASS Vulkan filtering/list and integrated/discrete recommendations (" << gpus.size() << " listed)\n";
}

}  // namespace

std::string ReadAll(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void WriteAll(const fs::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary);
  output << text;
}

void TestKeyboardControls(const fs::path& root) {
  const fs::path directory = root / "controls";
  fs::create_directories(directory);
  Paths paths = MakePaths(directory);
  const fs::path toml = directory / "wwe2k14.toml";
  assert(KeyBindings().size() == 24);
  assert(IsSupportedKeyName("Space") && IsSupportedKeyName("NumpadEnter") && IsSupportedKeyName("RMB"));
  assert(!IsSupportedKeyName("") && !IsSupportedKeyName("space") && !IsSupportedKeyName("Win"));

  // Defaults are the modern layout and contain no duplicate.
  KeyboardControls controls = LoadKeyboardControls(paths);
  assert(controls.keys[0] == "L" && controls.keys[1] == "K" && controls.keys[2] == "J" && controls.keys[3] == "Space");
  assert(controls.keys[5] == "1" && controls.keys[23] == "Escape");
  for (bool duplicate : DuplicateKeyBindings(controls)) assert(!duplicate);

  // No file: EnsureKeyboardDefaults writes all 24 rows (the game's built-in keys differ), and is a no-op afterwards.
  assert(EnsureKeyboardDefaults(paths).ok && fs::exists(toml));
  const std::string first = ReadAll(toml);
  assert(first.find("keybind_a = \"L\"\n") != std::string::npos);
  assert(first.find("keybind_right_trigger = \"1\"\n") != std::string::npos);
  size_t rows = 0;
  for (size_t at = first.find("keybind_"); at != std::string::npos; at = first.find("keybind_", at + 1)) ++rows;
  assert(rows == 24);
  assert(EnsureKeyboardDefaults(paths).ok && ReadAll(toml) == first);
  KeyboardControls loaded = LoadKeyboardControls(paths);
  for (bool present : loaded.present) assert(present);

  // Existing settings: other lines/tables/CRLF are kept, the player's own row is kept, missing rows go to the root table.
  WriteAll(toml, "# my settings\r\nresolution_scale = 2\r\nkeybind_b = \"Z\" # B\r\n\r\n[GPU]\r\nvsync = true\r\n");
  assert(EnsureKeyboardDefaults(paths).ok);
  const std::string merged = ReadAll(toml);
  assert(merged.rfind("# my settings\r\nresolution_scale = 2\r\nkeybind_b = \"Z\" # B\r\n\r\nkeybind_a = \"L\"\r\n", 0) == 0);
  assert(merged.find("[GPU]\r\nvsync = true\r\n") == merged.size() - std::string("[GPU]\r\nvsync = true\r\n").size());
  assert(merged.find("keybind_b = \"K\"") == std::string::npos);
  assert(merged.find("keybind_start = \"Escape\"\r\n[GPU]") != std::string::npos);

  // An older file whose own key clashes with a new default (B on Shift = new run key) is left untouched.
  const std::string clash = "keybind_b = \"Shift\"\n";
  WriteAll(directory / "clash.toml", clash);
  {
    Paths clash_paths = paths;
    fs::create_directories(directory / "clash");
    clash_paths.exe_dir = directory / "clash";
    clash_paths.game_exe = directory / "clash" / "wwe2k14";
    WriteAll(directory / "clash" / "wwe2k14.toml", clash);
    assert(EnsureKeyboardDefaults(clash_paths).ok && ReadAll(directory / "clash" / "wwe2k14.toml") == clash);
  }

  // Changing a key edits that line in place (comment kept).
  KeyboardControls edit = LoadKeyboardControls(paths);
  assert(edit.keys[1] == "Z");
  edit.keys[1] = "NumpadEnter";
  edit.modified[1] = true;
  assert(SaveKeyboardControls(paths, edit).ok);
  assert(ReadAll(toml).find("keybind_b = \"NumpadEnter\" # B\r\n") != std::string::npos);
  assert(LoadKeyboardControls(paths).keys[1] == "NumpadEnter");

  // Duplicates (NumpadEnter and Return are the same key for the game) are refused and the file is unchanged.
  const std::string before = ReadAll(toml);
  KeyboardControls duplicate = LoadKeyboardControls(paths);
  duplicate.keys[2] = "Return";
  duplicate.modified[2] = true;
  const std::vector<bool> flags = DuplicateKeyBindings(duplicate);
  assert(flags[1] && flags[2] && !flags[0]);
  assert(!SaveKeyboardControls(paths, duplicate).ok && ReadAll(toml) == before);

  // Unsupported names are refused; a damaged file loads as defaults instead of failing.
  KeyboardControls bad = LoadKeyboardControls(paths);
  bad.keys[3] = "NotAKey";
  bad.modified[3] = true;
  assert(!SaveKeyboardControls(paths, bad).ok && ReadAll(toml) == before);
  WriteAll(toml, std::string("\xFF\xFE\x00garbage = = \"\n", 16));
  assert(LoadKeyboardControls(paths).keys[0] == "L");
  std::cout << "PASS keyboard controls: modern defaults, first-start file, merge into existing settings, in-place "
               "edit, duplicates, unsupported keys, damaged file\n";
}

int main() {
  const fs::path output = fs::path(WWE2K14_LAUNCHER_TESTDATA).parent_path() / "core";
  std::error_code error;
  fs::create_directories(output, error);
  assert(!error);
  fs::remove_all(output / "run", error);
  error.clear();
  fs::create_directories(output / "run", error);
  assert(!error);
  const fs::path root = output / "run";
  const fs::path data = WWE2K14_LAUNCHER_TESTDATA;
  TestUiDisplayScale();
  TestOneDrivePathDetection(root / "onedrive-test");
  TestSettings(root);
  TestLaunchPlan(root);
  TestGameChecksAndSearch(root, data);
  TestBackups(root);
  TestMusic(root);
  TestBugReport(root);
  TestGpus();
  TestKeyboardControls(root);
  std::cout << "All core tests passed.\n";
  return 0;
}
