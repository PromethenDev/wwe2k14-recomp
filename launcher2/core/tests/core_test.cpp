#ifdef NDEBUG
#undef NDEBUG
#endif

#include "launcher_core.h"
#include "../../ui/display_scale.h"

#include "internal/archive.h"
#include "internal/util.h"

#include <algorithm>
#include <cassert>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
  {  // the v1.5 keys survive changing them twice: old lines must not be written back behind new ones
    Paths toggle_paths;
    toggle_paths.settings_file = root / "toggle-settings.ini";
    Settings t;
    assert(!t.weapon_wheel_more);
    t.performance_mode = false; t.hiac_weapon_wheel = true; t.weapon_wheel_more = true; t.multi_shadows = 1;
    assert(SaveSettings(toggle_paths, t).ok);
    assert(LoadSettings(toggle_paths).weapon_wheel_more);
    assert(!LoadSettings(toggle_paths).head_edit);
    t.performance_mode = true; t.hiac_weapon_wheel = false; t.weapon_wheel_more = false; t.multi_shadows = 0;
    assert(SaveSettings(toggle_paths, t).ok);
    const Settings r = LoadSettings(toggle_paths);
    assert(r.performance_mode && !r.hiac_weapon_wheel && !r.weapon_wheel_more && r.multi_shadows == 0);
    std::string text;
    assert(wwe2k14::launcher::internal::ReadTextFile(toggle_paths.settings_file, text));
    assert(text.find("hiac_weapon_wheel") == text.rfind("hiac_weapon_wheel"));
    assert(text.find("weapon_wheel_more") == text.rfind("weapon_wheel_more"));
    assert(!LoadSettings(toggle_paths).unlock_all);  // Mods "Unlock everything": off unless ticked
    t.unlock_all = true;
    assert(SaveSettings(toggle_paths, t).ok);
    assert(LoadSettings(toggle_paths).unlock_all);
    t.unlock_all = false;
    assert(SaveSettings(toggle_paths, t).ok);
    assert(!LoadSettings(toggle_paths).unlock_all);
    t.imports_in_grid = false;  // saved off -> loads off; a file without the key -> on (default)
    assert(SaveSettings(toggle_paths, t).ok);
    assert(!LoadSettings(toggle_paths).imports_in_grid);
    t.imports_in_grid = true;
    assert(SaveSettings(toggle_paths, t).ok);
    assert(LoadSettings(toggle_paths).imports_in_grid);
    t.head_edit = true;
    assert(SaveSettings(toggle_paths, t).ok);
    assert(LoadSettings(toggle_paths).head_edit);
    t.head_edit = false;
    assert(SaveSettings(toggle_paths, t).ok);
    assert(!LoadSettings(toggle_paths).head_edit);
    assert(wwe2k14::launcher::internal::ReadTextFile(toggle_paths.settings_file, text));
    assert(text.find("head_edit") == text.rfind("head_edit"));
  }
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
  assert(loaded.renderer == Renderer::kNative);  // Existing INI without renderer migrates to Native.
  assert(loaded.resolution == Resolution::k1440p);
  assert(loaded.display == DisplayMode::kWindowed);
  assert(loaded.gpu_id == "10DE:2684");
  assert(wwe2k14::launcher::internal::PathToUtf8(loaded.game_folder) == "C:/Games/Roster-\xC3\xA9");
  assert(loaded.auto_start && !loaded.auto_backup && loaded.explicit_choice);
  assert(loaded.recommendation_fields_tracked && loaded.manual_resolution && loaded.manual_picture_size &&
         loaded.manual_frame_rate && loaded.manual_xbox_frame_rate);
  assert(loaded.sync_frames_to_monitor && !loaded.stretch_to_fill_wide_screens && loaded.picture_scale == 1);
  assert(!loaded.glitch_capture_mode);
  assert(SaveSettings(paths, loaded).ok);
  std::vector<uint8_t> saved_bytes;
  assert(wwe2k14::launcher::internal::ReadBinaryFile(paths.settings_file, saved_bytes));
  assert(saved_bytes.size() >= 2 && saved_bytes[0] == 0xFF && saved_bytes[1] == 0xFE);
  std::string saved_text;
  assert(wwe2k14::launcher::internal::ReadTextFile(paths.settings_file, saved_text));
  assert(saved_text.find("schema=7") != std::string::npos);
  assert(saved_text.find("renderer=native") != std::string::npos);
  assert(saved_text.find("unknown_key=keep-this") != std::string::npos);
  assert(saved_text.find("gpu_name=Old GPU") != std::string::npos);
  assert(saved_text.find("[Other]") != std::string::npos && saved_text.find("keep=also-this") != std::string::npos);
  assert(saved_text.find("sync_frames_to_monitor=1") != std::string::npos);
  assert(saved_text.find("stretch_to_fill_wide_screens=0") != std::string::npos);
  assert(saved_text.find("picture_scale=1") != std::string::npos);
  assert(saved_text.find("recommendation_fields_version=1") != std::string::npos);
  assert(saved_text.find("manual_picture_size=1") != std::string::npos);
  assert(saved_text.find("glitch_capture_mode=0") != std::string::npos);

  Settings custom_screen = loaded;
  custom_screen.sync_frames_to_monitor = false;
  custom_screen.stretch_to_fill_wide_screens = true;
  custom_screen.picture_scale = 3;
  assert(SaveSettings(paths, custom_screen).ok);
  const Settings custom_screen_loaded = LoadSettings(paths);
  assert(!custom_screen_loaded.sync_frames_to_monitor && custom_screen_loaded.stretch_to_fill_wide_screens &&
         custom_screen_loaded.picture_scale == 3);

  // Existing schema-7 settings store PictureSize as an ordinal; preserve all original saved meanings.
  const std::array<std::pair<const char*, PictureSize>, 5> existing_picture_sizes{{
      {"0", PictureSize::k480p}, {"1", PictureSize::k720p}, {"2", PictureSize::k1080p},
      {"3", PictureSize::k1440p}, {"4", PictureSize::k4k},
  }};
  for (const auto& [serialized, expected] : existing_picture_sizes) {
    const std::string old_picture_size_ini = std::string("[Settings]\r\nschema=7\r\nnative_picture_size=") +
                                             serialized + "\r\n";
    assert(wwe2k14::launcher::internal::WriteBinaryFile(
        paths.settings_file, wwe2k14::launcher::internal::Utf8ToUtf16LeBytes(old_picture_size_ini)));
    assert(LoadSettings(paths).picture_size == expected);
  }

  Paths legacy_paths = MakePaths(directory / "legacy");
  fs::create_directories(legacy_paths.exe_dir);
  const auto legacy_bytes = wwe2k14::launcher::internal::Utf8ToUtf16LeBytes(
      "[Settings]\r\nschema=2\r\ninternal_resolution=576p\r\nframe_rate=classic\r\n"
      "display=fullscreen\r\ngpu=1\r\n");
  assert(wwe2k14::launcher::internal::WriteBinaryFile(legacy_paths.settings_file, legacy_bytes));
  const Settings legacy = LoadSettings(legacy_paths);
  assert(legacy.resolution == Resolution::k720p && legacy.explicit_choice);
  assert(legacy.recommendation_fields_tracked && legacy.manual_resolution && legacy.manual_picture_size &&
         legacy.manual_frame_rate && legacy.manual_xbox_frame_rate);
  assert(legacy.gpu_id.empty());
  // Missing music keys (settings from 1.4.8) keep the music defaults instead of reading as 0.
  {
    const MusicPreset defaults;
    assert(legacy.music_preset.target_rms_dbfs == defaults.target_rms_dbfs &&
           legacy.music_preset.fade_in_seconds == defaults.fade_in_seconds &&
           legacy.music_preset.bass_frequency_hz == defaults.bass_frequency_hz &&
           legacy.music_preset.treble_frequency_hz == defaults.treble_frequency_hz);
    // The all-zero preset that 1.4.9 test launchers saved is repaired on load.
    Paths zero_paths = MakePaths(directory / "music-zero");
    fs::create_directories(zero_paths.exe_dir);
    assert(wwe2k14::launcher::internal::WriteBinaryFile(
        zero_paths.settings_file,
        wwe2k14::launcher::internal::Utf8ToUtf16LeBytes(
            "[Settings]\r\nschema=7\r\nmusic_target_rms_dbfs=-6\r\nmusic_fade_in_seconds=0\r\n"
            "music_fade_out_seconds=0\r\nmusic_bass_gain_db=0\r\nmusic_bass_frequency_hz=20\r\n"
            "music_treble_gain_db=0\r\nmusic_treble_frequency_hz=1000\r\n")));
    const Settings zero = LoadSettings(zero_paths);
    assert(zero.music_preset.target_rms_dbfs == defaults.target_rms_dbfs &&
           zero.music_preset.fade_out_seconds == defaults.fade_out_seconds &&
           zero.music_preset.bass_frequency_hz == defaults.bass_frequency_hz);
  }
  assert(legacy.sync_frames_to_monitor && !legacy.stretch_to_fill_wide_screens && legacy.picture_scale == 1);

  Paths granular_paths = MakePaths(directory / "granular");
  fs::create_directories(granular_paths.exe_dir);
  Settings granular;
  granular.recommendation_fields_tracked = true;
  granular.manual_picture_size = true;
  granular.picture_size = PictureSize::k540p;
  granular.frame_rate = FrameRate::kLocked30;
  assert(SaveSettings(granular_paths, granular).ok);
  const Settings granular_loaded = LoadSettings(granular_paths);
  assert(granular_loaded.recommendation_fields_tracked && granular_loaded.manual_picture_size &&
         !granular_loaded.manual_frame_rate && granular_loaded.picture_size == PictureSize::k540p);

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
  Settings xbox_settings;
  xbox_settings.renderer = Renderer::kXbox;
  xbox_settings.resolution = Resolution::k1440p;
  xbox_settings.explicit_choice = true;
  assert(SaveSettings(resolution_paths, xbox_settings).ok);
  const Settings xbox_loaded = LoadSettings(resolution_paths);
  assert(xbox_loaded.renderer == Renderer::kXbox && xbox_loaded.resolution == Resolution::k1440p);
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
  {
    Paths arena_paths = MakePaths(root / "arena-import-env");
    fs::create_directories(arena_paths.exe_dir);
    Settings arena_settings;
    arena_settings.gpu_id = "10DE:2684";
    arena_settings.explicit_choice = true;
    const auto absent = BuildLaunchPlan(arena_paths, arena_settings, gpus);
    assert(EnvironmentValue(absent, "WWE2K14_ARENA_LIST") &&
           EnvironmentValue(absent, "WWE2K14_ARENA_LIST")->empty());
    assert(!EnvironmentValue(absent, "WWE2K14_ARENA_RELOCATE"));

    const fs::path arena_list = arena_paths.exe_dir / "mods" / "wwe13" / "arenas.txt";
    fs::create_directories(arena_list.parent_path());
    std::ofstream(arena_list, std::ios::binary) << "57 W13_57\n";
    const std::string expected_list = wwe2k14::launcher::internal::PathToUtf8(arena_list);
    const auto native = BuildLaunchPlan(arena_paths, arena_settings, gpus);
    assert(EnvironmentValue(native, "WWE2K14_ARENA_LIST") &&
           *EnvironmentValue(native, "WWE2K14_ARENA_LIST") == expected_list);
    assert(!EnvironmentValue(native, "WWE2K14_ARENA_RELOCATE"));

    arena_settings.renderer = Renderer::kXbox;
    const auto xbox = BuildLaunchPlan(arena_paths, arena_settings, gpus);
    assert(EnvironmentValue(xbox, "WWE2K14_ARENA_LIST") &&
           *EnvironmentValue(xbox, "WWE2K14_ARENA_LIST") == expected_list);
    assert(!EnvironmentValue(xbox, "WWE2K14_ARENA_RELOCATE"));
    assert(std::find(xbox.environment_to_remove.begin(), xbox.environment_to_remove.end(), "WWE2K14_ARENA_LIST") ==
           xbox.environment_to_remove.end());
    std::error_code arena_cleanup_error;
    fs::remove_all(arena_paths.exe_dir / "mods", arena_cleanup_error);
    const auto xbox_absent = BuildLaunchPlan(arena_paths, arena_settings, gpus);
    assert(EnvironmentValue(xbox_absent, "WWE2K14_ARENA_LIST") &&
           EnvironmentValue(xbox_absent, "WWE2K14_ARENA_LIST")->empty());
    assert(!EnvironmentValue(xbox_absent, "WWE2K14_ARENA_RELOCATE"));
  }
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
    // v1.7.1: the default (native) choice starts the new renderer: wwe2k14-vulkan(.exe) beside the game program.
#ifdef _WIN32
    assert(plan.executable == paths.game_exe.parent_path() / "wwe2k14-vulkan.exe");
#else
    assert(plan.executable == paths.game_exe.parent_path() / "wwe2k14-vulkan");
#endif
    {
      Settings xbox = settings;
      xbox.renderer = Renderer::kXbox;
      assert(BuildLaunchPlan(paths, xbox, gpus).executable == paths.game_exe);  // Xbox keeps wwe2k14(.exe)
    }
    assert(plan.working_directory == paths.exe_dir);
    const std::vector<std::string> fixed = {
        wwe2k14::launcher::internal::PathToUtf8(settings.game_folder),
        "--async_shader_compilation=false", "--vulkan_async_skip_incomplete_frames=false",
        "--fullscreen=true", "--vulkan_allow_present_mode_immediate=false",
        "--ignore_offset_for_ranged_allocations=true", "--log_verbose=false", "--mnk_mode=true"};
    assert(plan.arguments.size() == fixed.size() + 6);
    assert(std::find(plan.arguments.begin(), plan.arguments.end(), "--user_language=1") != plan.arguments.end());
    {  // #116 borderless: fullscreen window plus --display_refresh_match=false; fullscreen keeps the refresh match
      const auto has = [](const LaunchPlan& p, const char* arg) {
        return std::find(p.arguments.begin(), p.arguments.end(), arg) != p.arguments.end();
      };
      assert(!has(plan, "--display_refresh_match=false"));
      Settings borderless = settings;
      borderless.display = DisplayMode::kBorderless;
      const LaunchPlan bp = BuildLaunchPlan(paths, borderless, gpus);
#if defined(_WIN32)
      assert(has(bp, "--fullscreen=true") && has(bp, "--display_refresh_match=false"));
#else
      assert(has(bp, "--fullscreen=true") && !has(bp, "--display_refresh_match=false"));
#endif
      Settings windowed = settings;
      windowed.display = DisplayMode::kWindowed;
      const LaunchPlan wp = BuildLaunchPlan(paths, windowed, gpus);
      assert(has(wp, "--fullscreen=false") && !has(wp, "--display_refresh_match=false"));
    }
    {
      Settings french = settings;
      french.language = 4;
      const LaunchPlan french_plan = BuildLaunchPlan(paths, french, gpus);
      assert(std::find(french_plan.arguments.begin(), french_plan.arguments.end(), "--user_language=4") !=
             french_plan.arguments.end());
      Settings unsupported = settings;
      unsupported.language = 2;  // Japanese: not offered, falls back to English
      const LaunchPlan unsupported_plan = BuildLaunchPlan(paths, unsupported, gpus);
      assert(std::find(unsupported_plan.arguments.begin(), unsupported_plan.arguments.end(), "--user_language=1") !=
             unsupported_plan.arguments.end());
    }
    for (size_t index = 0; index < fixed.size(); ++index) assert(plan.arguments[index] == fixed[index]);
    assert(plan.arguments[0].find("Masa\xC3\xBCst\xC3\xBC") != std::string::npos);
    assert(plan.arguments[8].starts_with("--log_file="));
    const fs::path log_path = wwe2k14::launcher::internal::PathFromUtf8(plan.arguments[8].substr(11));
    assert(log_path.parent_path().lexically_normal() == paths.logs_dir.lexically_normal());
    assert(wwe2k14::launcher::internal::PathToUtf8(log_path.filename()).starts_with("wwe2k14-"));
    assert(plan.arguments[9] == "--user_data_root=" + wwe2k14::launcher::internal::PathToUtf8(paths.userdata_dir));
    assert(plan.arguments[10] == "--debug_ui=false");
    (void)resolution;
    const std::string expected_resolution = "--resolution=720p";  // native test package: always 720p
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
    // Native test package: the first five are the public launcher's; the native renderer environment follows.
    assert(plan.environment.size() > expected_environment.size());
    assert(std::equal(expected_environment.begin(), expected_environment.end(), plan.environment.begin()));
    for (const char* dropped : {"WWE2K14_F24_PIXEL_RATE", "WWE2K14_INTERNAL_RES", "WWE2K14_SCENE_AA",
                                "WWE2K14_KEEP_60", "WWE2K14_RATE_CENSUS", "WWE2K14_PERF_OVERLAY",
                                "WWE2K14_LOCK_OWNER_SAMPLE"}) {
      assert(EnvironmentValue(plan, dropped) == nullptr);
    }
  }
  // Native renderer options (2026-10-20): every picture size x frame rate x anti-aliasing combination.
  {
    const std::array<std::pair<PictureSize, const char*>, 7> sizes{{{PictureSize::k360p, "0.5"},
        {PictureSize::k480p, "0.6"}, {PictureSize::k540p, "0.75"}, {PictureSize::k720p, "1.0"},
        {PictureSize::k1080p, "1.5"}, {PictureSize::k1440p, "2.0"}, {PictureSize::k4k, "3.0"}}};
    for (const auto& [size, scale] : sizes) {
      for (FrameRate rate : {FrameRate::kLocked30, FrameRate::kStandard30_60, FrameRate::k60Everywhere}) {
        for (bool aa : {false, true}) {
          Settings native;
          native.gpu_id = "10DE:2684";
          native.picture_size = size;
          native.frame_rate = rate;
          native.anti_aliasing = aa;
          native.explicit_choice = true;
          const LaunchPlan plan = BuildLaunchPlan(paths, native, gpus);
          auto value = [&](const char* name) { return EnvironmentValue(plan, name); };
          assert(value("WWE2K14_NR") && *value("WWE2K14_NR") == "replace");
          assert(*value("WWE2K14_D3D_SCENE_SCALE") == scale);
          const bool dedupe_columns = size == PictureSize::k360p || size == PictureSize::k480p;
          assert(*value("WWE2K14_D3D_SCALE_DYNAMIC") == "0");
          assert(*value("WWE2K14_D3D_TILE_DEDUPE_COLS") == (dedupe_columns ? "1" : "0"));
          assert(*value("WWE2K14_D3D_AA_CAP") == (aa ? "2" : "1"));
          {  // Owner 2026-11-06: picture-lowering speed settings are opt-in; Performance mode keeps the saved choices.
            const auto has_filter = [](const LaunchPlan& p) {
              return std::find(p.arguments.begin(), p.arguments.end(), "--anisotropic_override=-1") != p.arguments.end();
            };
            const auto env_of = [](const LaunchPlan& p, std::string_view key) {
              std::string out;
              for (const auto& [k, v] : p.environment) if (k == key) out = v;
              return out;
            };
            assert(native.texture_filtering == 1 && !native.performance_mode && !has_filter(plan));
            Settings perf = native;
            perf.performance_mode = true;
            const LaunchPlan perf_plan = BuildLaunchPlan(paths, perf, gpus);
            assert(has_filter(perf_plan) && env_of(perf_plan, "WWE2K14_D3D_AA_CAP") == "1");
            assert(perf.anti_aliasing == aa);
            Settings full = native;
            full.multi_shadows = 1;
            assert(env_of(BuildLaunchPlan(paths, full, gpus), "WWE2K14_MULTI_SHADOWS") == "1");
            full.shadows = 0;
            assert(env_of(BuildLaunchPlan(paths, full, gpus), "WWE2K14_MULTI_SHADOWS").empty());
            full.shadows = 1;
            full.multi_shadows = 0;
            assert(env_of(BuildLaunchPlan(paths, full, gpus), "WWE2K14_MULTI_SHADOWS").empty());
            Settings wheel = native;
            const char* wheel_env = "WWE2K14_HIAC_WHEEL";
            const LaunchPlan wheel_off = BuildLaunchPlan(paths, wheel, gpus);
            assert(EnvironmentValue(wheel_off, wheel_env) == nullptr);  // Both options off.
            wheel.hiac_weapon_wheel = true;
            const LaunchPlan wheel_hiac = BuildLaunchPlan(paths, wheel, gpus);
            assert(EnvironmentValue(wheel_hiac, wheel_env) && *EnvironmentValue(wheel_hiac, wheel_env) == "1");
            wheel.weapon_wheel_more = true;
            const LaunchPlan wheel_all = BuildLaunchPlan(paths, wheel, gpus);
            assert(EnvironmentValue(wheel_all, wheel_env) && *EnvironmentValue(wheel_all, wheel_env) == "2");
            wheel.hiac_weapon_wheel = false;
            const LaunchPlan wheel_more_only = BuildLaunchPlan(paths, wheel, gpus);
            assert(EnvironmentValue(wheel_more_only, wheel_env) == nullptr);  // More matches is ignored when HiAC is off.
            Settings heads = native;
            { const LaunchPlan off_plan = BuildLaunchPlan(paths, heads, gpus);
              const std::string* off = EnvironmentValue(off_plan, "WWE2K14_HEAD_EDIT");
              assert(off && off->empty()); }  // Off by default: set empty so an inherited value cannot switch it on.
            heads.head_edit = true;
            const LaunchPlan heads_on = BuildLaunchPlan(paths, heads, gpus);
            assert(EnvironmentValue(heads_on, "WWE2K14_HEAD_EDIT") && EnvironmentValue(heads_on, "WWE2K14_HEAD_EDIT")->empty());  // hidden in v1.6.0
            Settings soft = native;
            soft.texture_filtering = 0;
            const LaunchPlan soft_plan = BuildLaunchPlan(paths, soft, gpus);
            assert(has_filter(soft_plan) && env_of(soft_plan, "WWE2K14_D3D_AA_CAP") == (aa ? "2" : "1"));
          }
          assert(*value("WWE2K14_D3D_TSTAGE_FAST") == "1");
          assert(value("WWE2K14_LOW_POWER") && value("WWE2K14_LOW_POWER")->empty());  // Lower CPU use: off by default since v1.4
          assert(value("WWE2K14_D3D_SHADOWS") && value("WWE2K14_D3D_SHADOWS")->empty());  // Shadows on by default
          assert(value("WWE2K14_D3D_TEX_QUALITY") && value("WWE2K14_D3D_TEX_QUALITY")->empty());  // Normal textures
          const char* lock = rate == FrameRate::kLocked30 ? "1" : rate == FrameRate::k60Everywhere ? "3" : "";
          assert(*value("WWE2K14_LOCK_30") == lock);
          assert(*value("WWE2K14_NR_RECORD_BATCH") == (rate == FrameRate::kLocked30 ? "" : "64"));
          assert(*value("WWE2K14_D3D_NATIVE_DOF") == (rate == FrameRate::k60Everywhere ? "1" : ""));
          assert(value("WWE2K14_NR_SHADERS") && value("WWE2K14_NR_OUT"));
          assert(value("WWE2K14_NR_CONSTANTS32") && value("WWE2K14_NR_CONSTANTS32")->empty());  // off by default
        }
      }
    }
    GpuInfo measured_uhd{"8086:3E92", "Intel(R) UHD Graphics 630", true, 0};
    Settings automatic_settings;
    const LaunchPlan automatic_uhd_plan = BuildLaunchPlan(paths, automatic_settings, {measured_uhd});
    assert(EnvironmentValue(automatic_uhd_plan, "WWE2K14_D3D_SCENE_SCALE") &&
           *EnvironmentValue(automatic_uhd_plan, "WWE2K14_D3D_SCENE_SCALE") == "0.6");
    assert(EnvironmentValue(automatic_uhd_plan, "WWE2K14_LOCK_30") &&
           *EnvironmentValue(automatic_uhd_plan, "WWE2K14_LOCK_30") == "3");  // UHD profile = 60 fps everywhere
    Settings manual_settings = automatic_settings;
    manual_settings.recommendation_fields_tracked = true;
    manual_settings.manual_picture_size = true;
    manual_settings.manual_frame_rate = true;
    manual_settings.picture_size = PictureSize::k540p;
    manual_settings.frame_rate = FrameRate::kLocked30;
    const LaunchPlan manual_uhd_plan = BuildLaunchPlan(paths, manual_settings, {measured_uhd});
    assert(EnvironmentValue(manual_uhd_plan, "WWE2K14_D3D_SCENE_SCALE") &&
           *EnvironmentValue(manual_uhd_plan, "WWE2K14_D3D_SCENE_SCALE") == "0.75");
    assert(EnvironmentValue(manual_uhd_plan, "WWE2K14_LOCK_30") &&
           *EnvironmentValue(manual_uhd_plan, "WWE2K14_LOCK_30") == "1");
    GpuInfo measured_780m{"1002:15BF", "AMD Radeon 780M Graphics", true, 0};
    Settings amd_settings;
    amd_settings.gpu_id = measured_780m.id;
    const LaunchPlan amd_780m_plan = BuildLaunchPlan(paths, amd_settings, {measured_780m});
    assert(EnvironmentValue(amd_780m_plan, "WWE2K14_D3D_SCENE_SCALE") &&
           *EnvironmentValue(amd_780m_plan, "WWE2K14_D3D_SCENE_SCALE") == "1.0");
    assert(EnvironmentValue(amd_780m_plan, "WWE2K14_NR_MEM_BUDGET_PCT") &&
           *EnvironmentValue(amd_780m_plan, "WWE2K14_NR_MEM_BUDGET_PCT") == "85");
    assert(EnvironmentValue(amd_780m_plan, "WWE2K14_NR_MEM_BUDGET_REUSE") &&
           *EnvironmentValue(amd_780m_plan, "WWE2K14_NR_MEM_BUDGET_REUSE") == "1");
    // Recommended: integrated graphics -> 480p + locked 30; discrete -> 720p + 30/60.
    const Recommendation integrated = RecommendedSettings({gpus[1]});
    assert(integrated.settings.picture_size == PictureSize::k480p &&
           integrated.settings.frame_rate == FrameRate::kLocked30);
    const Recommendation discrete = RecommendedSettings(gpus);
    assert(discrete.settings.picture_size == PictureSize::k720p &&
           discrete.settings.frame_rate == FrameRate::kStandard30_60);
    // Settings round trip.
    Paths settings_paths = MakePaths(root / "native-settings");
    fs::create_directories(settings_paths.exe_dir);
    Settings saved;
    saved.picture_size = PictureSize::k1440p;
    saved.frame_rate = FrameRate::k60Everywhere;
    saved.anti_aliasing = false;
    saved.check_updates = true;
    assert(SaveSettings(settings_paths, saved).ok);
    const Settings loaded = LoadSettings(settings_paths);
    assert(loaded.picture_size == PictureSize::k1440p && loaded.frame_rate == FrameRate::k60Everywhere &&
           !loaded.anti_aliasing && loaded.check_updates);
    for (PictureSize size : {PictureSize::k360p, PictureSize::k540p}) {
      saved.picture_size = size;
      assert(SaveSettings(settings_paths, saved).ok);
      assert(LoadSettings(settings_paths).picture_size == size);
    }
    assert(!Settings{}.low_power && !loaded.low_power);
    saved.low_power = true;
    assert(SaveSettings(settings_paths, saved).ok);
    assert(LoadSettings(settings_paths).low_power);
    saved.low_power = false;
    assert(SaveSettings(settings_paths, saved).ok);
    const Settings loaded_off = LoadSettings(settings_paths);
    assert(!loaded_off.low_power);
    {
      // A low_power=1 written by v1.2.0-v1.3.0 was their default, not a choice: it loads as off, and the next save
      // drops the old key.
      std::string ini;
      assert(wwe2k14::launcher::internal::ReadTextFile(settings_paths.settings_file, ini));
      const size_t at = ini.find("lower_cpu=0");
      assert(at != std::string::npos);
      ini.replace(at, std::string("lower_cpu=0").size(), "low_power=1");
      assert(wwe2k14::launcher::internal::WriteTextFileAtomic(settings_paths.settings_file, ini));
      Settings legacy = LoadSettings(settings_paths);
      assert(!legacy.low_power);
      assert(SaveSettings(settings_paths, legacy).ok);
      assert(wwe2k14::launcher::internal::ReadTextFile(settings_paths.settings_file, ini));
      assert(ini.find("low_power=") == std::string::npos && ini.find("lower_cpu=0") != std::string::npos);
    }
    saved.glitch_capture_mode = true;
    assert(SaveSettings(settings_paths, saved).ok);
    assert(LoadSettings(settings_paths).glitch_capture_mode);
    Settings native_off;
    native_off.low_power = false;
    native_off.explicit_choice = true;
    const LaunchPlan off_plan = BuildLaunchPlan(settings_paths, native_off, gpus);
    assert(EnvironmentValue(off_plan, "WWE2K14_LOW_POWER") && EnvironmentValue(off_plan, "WWE2K14_LOW_POWER")->empty());
    // Faster loading (card N4e): off by default; saved/loaded; Locked 30 -> LOCK_30=4 only when on. DRAW_SWAP (card U11)
    // only with the batched modes.
    assert(!Settings{}.fast_loading);
    saved.fast_loading = true;
    assert(SaveSettings(settings_paths, saved).ok);
    assert(LoadSettings(settings_paths).fast_loading);
    saved.fast_loading = false;
    assert(SaveSettings(settings_paths, saved).ok);
    assert(!LoadSettings(settings_paths).fast_loading);
    // Controller vibration (#102): on by default and no argument; off is saved/loaded and passes --input_vibration=false.
    {
      const auto vibration_off = [](const LaunchPlan& p) {
        return std::find(p.arguments.begin(), p.arguments.end(), "--input_vibration=false") != p.arguments.end();
      };
      assert(Settings{}.controller_vibration);
      assert(!vibration_off(BuildLaunchPlan(settings_paths, saved, gpus)));
      saved.controller_vibration = false;
      assert(SaveSettings(settings_paths, saved).ok);
      assert(!LoadSettings(settings_paths).controller_vibration);
      assert(vibration_off(BuildLaunchPlan(settings_paths, saved, gpus)));
      Settings xbox_off = saved;
      xbox_off.renderer = Renderer::kXbox;
      assert(vibration_off(BuildLaunchPlan(settings_paths, xbox_off, gpus)));
      saved.controller_vibration = true;
      assert(SaveSettings(settings_paths, saved).ok);
      assert(LoadSettings(settings_paths).controller_vibration);
    }
    // #87 keybinds while typing: off by default (no variable); saved/loaded; WWE2K14_KEYBINDS_WHILE_TYPING=1 when on.
    {
      assert(!Settings{}.keybinds_while_typing);
      assert(!EnvironmentValue(BuildLaunchPlan(settings_paths, saved, gpus), "WWE2K14_KEYBINDS_WHILE_TYPING"));
      saved.keybinds_while_typing = true;
      assert(SaveSettings(settings_paths, saved).ok);
      assert(LoadSettings(settings_paths).keybinds_while_typing);
      assert(*EnvironmentValue(BuildLaunchPlan(settings_paths, saved, gpus), "WWE2K14_KEYBINDS_WHILE_TYPING") == "1");
      saved.keybinds_while_typing = false;
      assert(SaveSettings(settings_paths, saved).ok);
      assert(!LoadSettings(settings_paths).keybinds_while_typing);
    }
    // Intel Lake compatibility shaders: off by default; saved/loaded; WWE2K14_NR_CONSTANTS32=1 only when on (native).
    assert(!Settings{}.intel_lake_shaders);
    saved.intel_lake_shaders = true;
    assert(SaveSettings(settings_paths, saved).ok);
    assert(LoadSettings(settings_paths).intel_lake_shaders);
    assert(*EnvironmentValue(BuildLaunchPlan(settings_paths, saved, gpus), "WWE2K14_NR_CONSTANTS32") == "1");
    saved.intel_lake_shaders = false;
    assert(SaveSettings(settings_paths, saved).ok);
    assert(!LoadSettings(settings_paths).intel_lake_shaders);
    assert(EnvironmentValue(BuildLaunchPlan(settings_paths, saved, gpus), "WWE2K14_NR_CONSTANTS32")->empty());
    {
      Settings rate = native_off;
      rate.frame_rate = FrameRate::kLocked30;
      assert(*EnvironmentValue(BuildLaunchPlan(settings_paths, rate, gpus), "WWE2K14_LOCK_30") == "1");
      assert(EnvironmentValue(BuildLaunchPlan(settings_paths, rate, gpus), "WWE2K14_NR_DRAW_SWAP")->empty());
      rate.fast_loading = true;
      assert(*EnvironmentValue(BuildLaunchPlan(settings_paths, rate, gpus), "WWE2K14_LOCK_30") == "4");
      rate.frame_rate = FrameRate::kStandard30_60;
      assert(EnvironmentValue(BuildLaunchPlan(settings_paths, rate, gpus), "WWE2K14_LOCK_30")->empty());
      assert(*EnvironmentValue(BuildLaunchPlan(settings_paths, rate, gpus), "WWE2K14_NR_DRAW_SWAP") == "1");
      rate.frame_rate = FrameRate::k60Everywhere;
      assert(*EnvironmentValue(BuildLaunchPlan(settings_paths, rate, gpus), "WWE2K14_LOCK_30") == "3");
      assert(*EnvironmentValue(BuildLaunchPlan(settings_paths, rate, gpus), "WWE2K14_NR_DRAW_SWAP") == "1");
    }
    // Shadows Off: saved, loaded back and passed to the game as WWE2K14_D3D_SHADOWS=0.
    saved.shadows = 0;
    assert(SaveSettings(settings_paths, saved).ok);
    assert(LoadSettings(settings_paths).shadows == 0);
    native_off.shadows = 0;
    assert(*EnvironmentValue(BuildLaunchPlan(settings_paths, native_off, gpus), "WWE2K14_D3D_SHADOWS") == "0");
    saved.shadows = 1;
    assert(SaveSettings(settings_paths, saved).ok);
    assert(LoadSettings(settings_paths).shadows == 1);
    // Textures Low: saved, loaded back and passed as WWE2K14_D3D_TEX_QUALITY=1.
    saved.textures = 0;
    assert(SaveSettings(settings_paths, saved).ok);
    assert(LoadSettings(settings_paths).textures == 0);
    native_off.textures = 0;
    assert(*EnvironmentValue(BuildLaunchPlan(settings_paths, native_off, gpus), "WWE2K14_D3D_TEX_QUALITY") == "1");
    saved.textures = 1;
    assert(SaveSettings(settings_paths, saved).ok);
    std::cout << "PASS native renderer options: 30 combinations, recommendations, settings round trip\n";
  }
  // Xbox renderer options (GitHub #34): the game-side frame-rate hook and the SDK's FXAA post effect.
  {
    auto has_argument = [](const LaunchPlan& plan, const std::string& argument) {
      return std::find(plan.arguments.begin(), plan.arguments.end(), argument) != plan.arguments.end();
    };
    for (FrameRate rate : {FrameRate::kLocked30, FrameRate::kStandard30_60, FrameRate::k60Everywhere}) {
      for (bool aa : {false, true}) {
        Settings xbox;
        xbox.renderer = Renderer::kXbox;
        xbox.gpu_id = "10DE:2684";
        xbox.xbox_frame_rate = rate;
        xbox.xbox_anti_aliasing = aa;
        xbox.frame_rate = FrameRate::kLocked30;  // the native choice must not leak into Xbox mode
        xbox.explicit_choice = true;
        const LaunchPlan plan = BuildLaunchPlan(paths, xbox, gpus);
        const char* lock = rate == FrameRate::kLocked30 ? "1" : rate == FrameRate::k60Everywhere ? "3" : "";
        assert(EnvironmentValue(plan, "WWE2K14_LOCK_30") && *EnvironmentValue(plan, "WWE2K14_LOCK_30") == lock);
        assert(!EnvironmentValue(plan, "WWE2K14_NR") && !EnvironmentValue(plan, "WWE2K14_D3D_NATIVE_DOF"));
        assert(std::find(plan.environment_to_remove.begin(), plan.environment_to_remove.end(), "WWE2K14_LOCK_30") ==
               plan.environment_to_remove.end());
        assert(std::find(plan.environment_to_remove.begin(), plan.environment_to_remove.end(), "WWE2K14_NR") !=
               plan.environment_to_remove.end());
        assert(has_argument(plan, aa ? "--swap_post_effect=fxaa" : "--swap_post_effect=none"));
      }
    }
    // Defaults keep the Xbox renderer as v1.1.0 shipped it; Recommended keeps 30/60.
    Settings defaults;
    defaults.renderer = Renderer::kXbox;
    const LaunchPlan default_plan = BuildLaunchPlan(paths, defaults, gpus);
    assert(*EnvironmentValue(default_plan, "WWE2K14_LOCK_30") == "");
    assert(has_argument(default_plan, "--swap_post_effect=none"));
    Paths xbox_paths = MakePaths(root / "xbox-settings");
    fs::create_directories(xbox_paths.exe_dir);
    Settings saved;
    saved.renderer = Renderer::kXbox;
    saved.xbox_frame_rate = FrameRate::k60Everywhere;
    saved.xbox_anti_aliasing = true;
    saved.frame_rate = FrameRate::kLocked30;
    assert(SaveSettings(xbox_paths, saved).ok);
    const Settings loaded = LoadSettings(xbox_paths);
    assert(loaded.xbox_frame_rate == FrameRate::k60Everywhere && loaded.xbox_anti_aliasing &&
           loaded.frame_rate == FrameRate::kLocked30);
    std::cout << "PASS xbox renderer options: 6 combinations, defaults, settings round trip\n";
  }
  // An explicit 1440p selection must survive recommendation resolution and reach the runtime as
  // the selected preset. This verifies launcher-side intent only; the SDK's draw scale is separate.
  Settings selected_1440p;
  selected_1440p.resolution = Resolution::k1440p;
  selected_1440p.gpu_id = "1002:164E";
  selected_1440p.explicit_choice = true;
  const LaunchPlan selected_plan = BuildLaunchPlan(paths, selected_1440p, gpus);
  assert(selected_plan.arguments[11] == "--resolution=720p");

  Settings xbox;
  xbox.renderer = Renderer::kXbox;
  xbox.resolution = Resolution::k1440p;
  xbox.explicit_choice = true;
  const LaunchPlan xbox_plan = BuildLaunchPlan(paths, xbox, gpus);
  assert(xbox_plan.arguments[11] == "--resolution=1440p");
  assert(EnvironmentValue(xbox_plan, "WWE2K14_NR") == nullptr);
  assert(EnvironmentValue(xbox_plan, "WWE2K14_D3D_SCENE_SCALE") == nullptr);
  assert(std::find(xbox_plan.environment_to_remove.begin(), xbox_plan.environment_to_remove.end(), "WWE2K14_NR") !=
         xbox_plan.environment_to_remove.end());
  assert(std::find(xbox_plan.environment_to_remove.begin(), xbox_plan.environment_to_remove.end(),
                   "WWE2K14_D3D_AA_CAP") != xbox_plan.environment_to_remove.end());

  Settings output_options;
  output_options.explicit_choice = true;
  const LaunchPlan default_options_plan = BuildLaunchPlan(paths, output_options, gpus);
  assert(default_options_plan.arguments.size() == 13);  // + --user_language
  assert(EnvironmentValue(default_options_plan, "WWE2K14_HOST_VSYNC") &&
         *EnvironmentValue(default_options_plan, "WWE2K14_HOST_VSYNC") == "1");
  assert(TestBuildMarkerRoot(paths).empty());
#ifdef WWE2K14_LAUNCHER_NO_TEST_TOOLS
  assert(EnvironmentValue(default_options_plan, "WWE2K14_GLITCH_CAPTURE") == nullptr);
  assert(EnvironmentValue(default_options_plan, "WWE2K14_GLITCH_CAPTURE_DIR") == nullptr);
#else
  assert(EnvironmentValue(default_options_plan, "WWE2K14_GLITCH_CAPTURE") &&
         EnvironmentValue(default_options_plan, "WWE2K14_GLITCH_CAPTURE")->empty());
  assert(EnvironmentValue(default_options_plan, "WWE2K14_GLITCH_CAPTURE_DIR") &&
         EnvironmentValue(default_options_plan, "WWE2K14_GLITCH_CAPTURE_DIR")->empty());
#endif

  output_options.sync_frames_to_monitor = false;
  output_options.stretch_to_fill_wide_screens = true;
  output_options.picture_scale = 2;
  const LaunchPlan enabled_options_plan = BuildLaunchPlan(paths, output_options, gpus);
  assert(enabled_options_plan.arguments.size() == 15);
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

  // Game-fix switches must reach the game with both renderers (v1.2.0 lost them to the environment reset).
  for (const LaunchPlan* plan : {&default_options_plan, &xbox_plan}) {
    for (const char* name : {"REX_WWE2K14_THREAD_FREE_STACK_ON_EXIT", "WWE2K14_XMP_FIX",
                             "REX_WWE2K14_ZERO_PHYSICAL_ON_ALLOC", "WWE2K14_KEYBOARD_TEXT"}) {
      assert(EnvironmentValue(*plan, name) && *EnvironmentValue(*plan, name) == "1");
    }
  }
  for (const LaunchPlan* plan : {&default_options_plan, &xbox_plan}) {
    // CAW matches hang with the cache: partition mounted (#72 #84): explicitly off on every platform.
    assert(EnvironmentValue(*plan, "REX_MOUNT_CACHE") && *EnvironmentValue(*plan, "REX_MOUNT_CACHE") == "false");
  }
  {
    // Card AMD780M: AMD integrated GPU (chosen) gets the memory headroom env; the NVIDIA discrete one does not.
    Settings amd;
    amd.explicit_choice = true;
    amd.gpu_id = "1002:164E";
    const LaunchPlan amd_plan = BuildLaunchPlan(paths, amd, gpus);
    assert(EnvironmentValue(amd_plan, "WWE2K14_NR_MEM_BUDGET_PCT") && *EnvironmentValue(amd_plan, "WWE2K14_NR_MEM_BUDGET_PCT") == "85");
    assert(EnvironmentValue(amd_plan, "WWE2K14_NR_MEM_BUDGET_REUSE") && *EnvironmentValue(amd_plan, "WWE2K14_NR_MEM_BUDGET_REUSE") == "1");
    Settings nvidia = amd;
    nvidia.gpu_id = "10DE:2684";
    assert(EnvironmentValue(BuildLaunchPlan(paths, nvidia, gpus), "WWE2K14_NR_MEM_BUDGET_PCT") == nullptr);
    Settings automatic = amd;
    automatic.gpu_id.clear();  // two GPUs, automatic choice -> not applied
    assert(EnvironmentValue(BuildLaunchPlan(paths, automatic, gpus), "WWE2K14_NR_MEM_BUDGET_PCT") == nullptr);
  }
  std::cout << "PASS game-fix env (free stack on exit, XMP fix, zero physical on alloc) for Native and Xbox\n";

  // Owner test builds: the existing marker also gates the opt-in RenderDoc capture mode.
  // Off by default; set empty (not missing) so an inherited value cannot switch it on.
  assert(EnvironmentValue(default_options_plan, "WWE2K14_UNLOCK_ALL") &&
         EnvironmentValue(default_options_plan, "WWE2K14_UNLOCK_ALL")->empty());
  // Mods page "Imported wrestlers in the main select grid" (owner 11-16): on by default, "" when the player turns it off.
  assert(EnvironmentValue(default_options_plan, "WWE2K14_OWN_GRID") &&
         *EnvironmentValue(default_options_plan, "WWE2K14_OWN_GRID") == "1");
  {
    Settings off = output_options;
    off.imports_in_grid = false;
    const LaunchPlan plan = BuildLaunchPlan(paths, off, gpus);
    assert(EnvironmentValue(plan, "WWE2K14_OWN_GRID") && EnvironmentValue(plan, "WWE2K14_OWN_GRID")->empty());
  }
  {  // Mods page "Unlock everything" (owner 11-15): a player option in public packages too, both renderers
    for (const Renderer renderer : {Renderer::kNative, Renderer::kXbox}) {
      Settings mod = output_options;
      mod.renderer = renderer;
      mod.unlock_all = true;
      const LaunchPlan plan = BuildLaunchPlan(paths, mod, gpus);
      assert(EnvironmentValue(plan, "WWE2K14_UNLOCK_ALL") && *EnvironmentValue(plan, "WWE2K14_UNLOCK_ALL") == "1");
    }
  }
  {
    const fs::path marker = paths.exe_dir.parent_path() / kTestUnlockAllMarker;
    std::ofstream(marker) << "test build\n";
    assert(TestBuildMarkerRoot(paths) == paths.exe_dir.parent_path());
    for (const Renderer renderer : {Renderer::kNative, Renderer::kXbox}) {
      Settings unlocked = output_options;
      unlocked.renderer = renderer;
      const LaunchPlan plan = BuildLaunchPlan(paths, unlocked, gpus);
      assert(EnvironmentValue(plan, "WWE2K14_UNLOCK_ALL") && *EnvironmentValue(plan, "WWE2K14_UNLOCK_ALL") == "1");
#ifdef WWE2K14_LAUNCHER_NO_TEST_TOOLS
      unlocked.glitch_capture_mode = true;
      assert(EnvironmentValue(BuildLaunchPlan(paths, unlocked, gpus), "WWE2K14_GLITCH_CAPTURE") == nullptr);
#else
      assert(EnvironmentValue(plan, "WWE2K14_GLITCH_CAPTURE") &&
             EnvironmentValue(plan, "WWE2K14_GLITCH_CAPTURE")->empty());
      assert(EnvironmentValue(plan, "WWE2K14_GLITCH_CAPTURE_DIR") &&
             EnvironmentValue(plan, "WWE2K14_GLITCH_CAPTURE_DIR")->empty());
      unlocked.glitch_capture_mode = true;
      const LaunchPlan capture_plan = BuildLaunchPlan(paths, unlocked, gpus);
      assert(EnvironmentValue(capture_plan, "WWE2K14_GLITCH_CAPTURE") &&
             *EnvironmentValue(capture_plan, "WWE2K14_GLITCH_CAPTURE") == "1");
      assert(EnvironmentValue(capture_plan, "WWE2K14_GLITCH_CAPTURE_DIR") &&
             *EnvironmentValue(capture_plan, "WWE2K14_GLITCH_CAPTURE_DIR") ==
                 wwe2k14::launcher::internal::PathToUtf8(paths.exe_dir.parent_path() / "logs" / "captures"));
#endif
    }
    std::error_code ignored;
    fs::remove(marker, ignored);
    assert(TestBuildMarkerRoot(paths).empty());
    Settings stale_capture_setting = output_options;
    stale_capture_setting.glitch_capture_mode = true;
    const LaunchPlan public_plan = BuildLaunchPlan(paths, stale_capture_setting, gpus);
    assert(EnvironmentValue(public_plan, "WWE2K14_UNLOCK_ALL") && EnvironmentValue(public_plan, "WWE2K14_UNLOCK_ALL")->empty());
#ifdef WWE2K14_LAUNCHER_NO_TEST_TOOLS
    assert(EnvironmentValue(public_plan, "WWE2K14_GLITCH_CAPTURE") == nullptr);
#else
    assert(EnvironmentValue(public_plan, "WWE2K14_GLITCH_CAPTURE") &&
           EnvironmentValue(public_plan, "WWE2K14_GLITCH_CAPTURE")->empty());
    assert(EnvironmentValue(public_plan, "WWE2K14_GLITCH_CAPTURE_DIR") &&
           EnvironmentValue(public_plan, "WWE2K14_GLITCH_CAPTURE_DIR")->empty());
#endif
  }
  std::cout << "PASS test marker visibility/root and glitch-capture env (off without marker or setting, on only with both)\n";
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
  assert(SaveOperationsAllowed(false));
  assert(!SaveOperationsAllowed(true));

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
  const fs::path last_play_marker = paths.userdata_dir / ".wwe2k14-last-play";
  BackupInfo legacy;
  assert(BackupSaves(paths, false, &legacy).ok && !legacy.tag_known);
  assert(MarkLastPlayModded(paths, true).ok && fs::is_regular_file(last_play_marker));
  BackupInfo tagged;
  assert(BackupSaves(paths, false, &tagged).ok && tagged.tag_known && tagged.modded && !tagged.automatic);
  fs::path tagged_marker = tagged.file;
  tagged_marker += ".tag";
  assert(fs::is_regular_file(tagged_marker));
  internal::ZipReader tagged_zip;
  assert(tagged_zip.Open(tagged.file));
  for (size_t index = 0; index < tagged_zip.Count(); ++index) {
    mz_zip_archive_file_stat stat{};
    assert(tagged_zip.Stat(index, stat));
    assert(std::string(stat.m_filename).find(".wwe2k14-last-play") == std::string::npos);
  }
  assert(MarkLastPlayModded(paths, false).ok && fs::is_regular_file(last_play_marker));
  std::string last_play_text;
  assert(wwe2k14::launcher::internal::ReadTextFile(last_play_marker, last_play_text) && last_play_text == "NORMAL\n");
  BackupInfo manual;
  assert(BackupSaves(paths, false, &manual).ok && !manual.automatic);
  assert(manual.tag_known && !manual.modded && fs::is_regular_file(fs::path(manual.file.string() + ".tag")));
  const auto listed_tagged = ListBackups(paths);
  const auto found_tagged = std::find_if(listed_tagged.begin(), listed_tagged.end(), [&tagged](const BackupInfo& info) {
    return info.file == tagged.file;
  });
  assert(found_tagged != listed_tagged.end() && found_tagged->modded);
  const auto listed_legacy = std::find_if(listed_tagged.begin(), listed_tagged.end(), [&legacy](const BackupInfo& info) {
    return info.file == legacy.file;
  });
  assert(listed_legacy != listed_tagged.end() && !listed_legacy->tag_known);
  const std::string changed = "changed-save-data";
  assert(wwe2k14::launcher::internal::WriteBinaryFile(save, changed.data(), changed.size()));
  assert(RestoreBackup(paths, manual.file).ok);
  std::string restored;
  assert(wwe2k14::launcher::internal::ReadTextFile(save, restored));
  assert(restored == original);
  // An import named like an automatic backup stays out of the rotation (imports keep their file name).
  {
    const fs::path named = paths.backups_dir / "wwe2k14-saves-import-1-x-auto.zip";
    fs::copy_file(manual.file, named);
    for (int index = 0; index < 6; ++index) assert(BackupSaves(paths, true, nullptr).ok);
    assert(fs::is_regular_file(named));
    fs::remove(named);
  }
  // Restoring a tagged backup makes the next backup carry that tag, whatever was played last.
  assert(MarkLastPlayModded(paths, true).ok);
  assert(RestoreBackup(paths, manual.file).ok);
  BackupInfo after_restore;
  assert(BackupSaves(paths, false, &after_restore).ok && after_restore.tag_known && !after_restore.modded);

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
    if (index == 0) output << "\n[glitch-capture] frame 9 time 123 scene unknown\n";
    output.close();
    fs::last_write_time(log, fs::file_time_type::clock::now() - std::chrono::seconds(20 - index));
  }
  fs::create_directories(paths.logs_dir / "captures");
  {
    std::ofstream rdc(paths.logs_dir / "captures" / "frame.rdc", std::ios::binary);
    rdc << "RenderDoc data must not be attached to the bug report";
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
  for (const char* account : {"0000000000000000", "B13EBABEBABEBABE"}) {
    const fs::path pack = paths.userdata_dir / account / "545408B2" / "00000002" / "DupAddOn";
    fs::create_directories(pack / "sub");
    std::ofstream(pack / "a.bin", std::ios::binary) << "12345";
    std::ofstream(pack / "sub" / "b.bin", std::ios::binary) << "678";
  }
  fs::path report;
  assert(SaveBugReport(paths, &report).ok);
  assert(report.parent_path() == paths.exe_dir && fs::is_regular_file(report));
  wwe2k14::launcher::internal::ZipReader zip;
  assert(zip.Open(report));
  bool newest_seen = false, old_seen = false, ini_seen = false, gpu_seen = false, system_seen = false;
  bool freeze_seen = false, content_seen = false;
  bool capture_marker_seen = false, rdc_seen = false;
  for (size_t index = 0; index < zip.Count(); ++index) {
    mz_zip_archive_file_stat stat{};
    assert(zip.Stat(index, stat));
    const std::string name = stat.m_filename;
    if (name == "logs/game-3.log") newest_seen = true;
    if (name == "logs/glitch-capture-markers.txt") {
      std::vector<uint8_t> log_bytes;
      assert(zip.Extract(index, log_bytes));
      const std::string log_contents(log_bytes.begin(), log_bytes.end());
      capture_marker_seen = log_contents.find("[glitch-capture] frame 9 time 123 scene unknown") != std::string::npos;
    }
    if (name == "logs/game-0.log") old_seen = true;
    if (name == "logs/wwe2k14-freeze-20260101-000000.txt") freeze_seen = true;
    if (name == "wwe2k14-enhanced.ini") ini_seen = true;
    if (name == "gpu-list.txt") gpu_seen = true;
    if (name == "system.txt") system_seen = true;
    if (name == "content-list.txt") {
      std::vector<uint8_t> bytes;
      assert(zip.Extract(index, bytes));
      const std::string list(bytes.begin(), bytes.end());
      content_seen = list.starts_with("Launcher version: ") &&
                     list.find("0000000000000000 | add-on | DupAddOn | 2 | 8") != std::string::npos &&
                     list.find("B13EBABEBABEBABE | add-on | DupAddOn | 2 | 8") != std::string::npos &&
                     list.find(paths.exe_dir.string()) == std::string::npos &&
                     list.find(paths.userdata_dir.string()) == std::string::npos;
    }
    if (name.ends_with(".rdc")) rdc_seen = true;
  }
  assert(newest_seen && !old_seen && freeze_seen && ini_seen && gpu_seen && system_seen && content_seen && capture_marker_seen && !rdc_seen);
  std::cout << "PASS bug report ZIP includes newest three logs and glitch-capture marker lines, but not .rdc files, "
               "plus freeze report, INI, GPU list, and system summary\n";
}

void TestBugReportSessionExtras(const fs::path& root) {
  assert(DescribeExitNote("exit_code=0x00000000\n").find("normal close") != std::string::npos);
  assert(DescribeExitNote("exit_code=0x00000001\n").find("Task Manager") != std::string::npos);
  assert(DescribeExitNote("exit_code=0xC0000005\n").find("memory access") != std::string::npos);
  assert(DescribeExitNote("exit_code=0xC00000FD\n").find("stack overflow") != std::string::npos);
  assert(DescribeExitNote("exit_code=0xC0000409\n").find("fast fail") != std::string::npos);
  assert(DescribeExitNote("exit_code=0xC000012D\n").find("out of memory") != std::string::npos);
  assert(DescribeExitNote("exit_code=0xC0000374\n").find("heap corruption") != std::string::npos);
  assert(DescribeExitNote("exit_code=0xE06D7363\n").find("C++ exception") != std::string::npos);
  assert(DescribeExitNote("exit_code=0x40010004\n").find("killed") != std::string::npos);
  assert(DescribeExitNote("exit_code=0x12345678\n").find("unknown code") != std::string::npos);
  assert(DescribeExitNote("exit_code=unknown\n").find("unknown") != std::string::npos);

  Paths paths = MakePaths(root / "bug-report-session");
  fs::create_directories(paths.logs_dir / "crash-watch");
  fs::create_directories(paths.logs_dir / "nr-out-20261108-194320" / "missing-shaders");
  const fs::path log = paths.logs_dir / "wwe2k14-20261108-194320.log";
  std::ofstream(log) << "game log\n";
  std::ofstream(paths.logs_dir / "crash-watch" / "wwe2k14-20261108-194320.log.exit") << "exit_code=0xC0000005\n";
  std::ofstream(paths.logs_dir / "nr-out-20261108-194320" / "missing-shaders" / "AAAA.vs.ucode") << "vs";
  std::ofstream(paths.logs_dir / "nr-out-20261108-194320" / "missing-shaders" / "BBBB.ps.ucode") << "ps";
  fs::create_directories(paths.exe_dir / "mods" / "wwe13");
  std::ofstream(paths.exe_dir / "mods" / "wwe13" / "roster.tsv") << "# c\n1\tA\tM\n2\tB\tF\n";
  Settings settings;
  settings.game_folder = paths.default_game_folder;
  assert(SaveSettings(paths, settings).ok);

  auto read = [&](const fs::path& zip_path, std::set<std::string>& names, std::string& system_text,
                  std::string& end_text) {
    wwe2k14::launcher::internal::ZipReader zip;
    assert(zip.Open(zip_path));
    for (size_t index = 0; index < zip.Count(); ++index) {
      mz_zip_archive_file_stat stat{};
      assert(zip.Stat(index, stat));
      const std::string name = stat.m_filename;
      names.insert(name);
      std::vector<uint8_t> bytes;
      assert(zip.Extract(index, bytes));
      if (name == "system.txt") system_text.assign(bytes.begin(), bytes.end());
      if (name == "session-end.txt") end_text.assign(bytes.begin(), bytes.end());
    }
  };
  fs::path report;
  assert(SaveBugReport(paths, &report, log).ok);
  std::set<std::string> names;
  std::string system_text, end_text;
  read(report, names, system_text, end_text);
  assert(names.count("logs/crash-watch/wwe2k14-20261108-194320.log.exit"));
  assert(names.count("missing-shaders/AAAA.vs.ucode") && names.count("missing-shaders/BBBB.ps.ucode"));
  assert(end_text.find("memory access") != std::string::npos);
  assert(end_text.find("Session: crashed while playing") != std::string::npos);
  {  // A crash code after the game logged its close = the known exit crash: the report says the player closed it.
    std::ofstream(log) << "game log\n[core] Execution complete\n";
    fs::path closed_report;
    assert(SaveBugReport(paths, &closed_report, log).ok);
    std::set<std::string> closed_names;
    std::string closed_system, closed_end;
    read(closed_report, closed_names, closed_system, closed_end);
    assert(closed_end.find("Session: closed by the player") != std::string::npos);
    fs::remove(closed_report);
    std::ofstream(log) << "game log\n";
  }
  assert(system_text.find("own-slots.txt): MISSING") != std::string::npos);
  assert(system_text.find("mods/wwe13/roster.tsv 2 rows") != std::string::npos);
  assert(system_text.find(paths.exe_dir.string()) == std::string::npos);
  std::ofstream(paths.exe_dir / "own-slots.txt") << "100\n101\n102\n";
  fs::remove(report);
  assert(SaveBugReport(paths, &report, log).ok);
  names.clear();
  read(report, names, system_text, end_text);
  assert(system_text.find("own-slots.txt): 3 ids") != std::string::npos);
  std::cout << "PASS bug report carries exit note, decoded exit, missing-shader dumps, own-slots and roster lines\n";
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
  const Recommendation integrated = RecommendedSettings({{"1002:164E", "Integrated", true, 0}}, 8);
  assert(integrated.settings.resolution == Resolution::k720p);
  assert(integrated.settings.picture_size == PictureSize::k480p &&
         integrated.settings.frame_rate == FrameRate::kLocked30);
  const Recommendation discrete = RecommendedSettings({{"10DE:2684", "Discrete", false, 0}}, 8);
  assert(discrete.settings.resolution == Resolution::k720p);
  assert(discrete.settings.picture_size == PictureSize::k720p &&
         discrete.settings.frame_rate == FrameRate::kStandard30_60);

  GpuInfo uhd630{"8086:3E92", "Intel(R) UHD Graphics 630", true, 0};
  uhd630.vram_mb = 2048;
  const Recommendation uhd_4t = RecommendedSettings({uhd630}, 4);
  const Recommendation uhd_24t = RecommendedSettings({uhd630}, 24);
  assert(uhd_4t.settings.picture_size == PictureSize::k480p &&
         uhd_4t.settings.frame_rate == FrameRate::k60Everywhere);
  assert(uhd_24t.settings.picture_size == uhd_4t.settings.picture_size &&
         uhd_24t.settings.frame_rate == uhd_4t.settings.frame_rate);
  assert(uhd_4t.reason.find("UHD Graphics 630") != std::string::npos);

  GpuInfo radeon780m{"1002:15BF", "AMD Radeon 780M Graphics", true, 0};
  radeon780m.vram_mb = 4096;
  const Recommendation amd_780m = RecommendedSettings({radeon780m}, 16);
  assert(amd_780m.settings.picture_size == PictureSize::k720p &&
         amd_780m.settings.frame_rate == FrameRate::kStandard30_60);
  assert(amd_780m.reason.find("Radeon 780M") != std::string::npos);

  const std::vector<GpuInfo> hybrid{uhd630, {"10DE:2684", "NVIDIA Discrete", false, 1}};
  assert(RecommendedSettings(hybrid, 16).settings.picture_size == PictureSize::k720p);
  assert(RecommendedSettingsFor(hybrid, uhd630.id, 16).settings.picture_size == PictureSize::k480p);
  assert(RecommendedSettingsFor(hybrid, "10DE:2684", 16).settings.picture_size == PictureSize::k720p);

  // Owner laptop: 780M + RTX 4060 -> the whole-PC recommendation picks the RTX and its measured profile, even
  // when the Graphics card setting points at the 780M (the button proposes switching the card).
  {
    GpuInfo rtx4060{"10DE:28E0", "NVIDIA GeForce RTX 4060 Laptop GPU", false, 0};
    const std::vector<GpuInfo> laptop{radeon780m, rtx4060};
    const Recommendation whole_pc = RecommendedSettings(laptop, 16);
    assert(whole_pc.gpu_id == rtx4060.id);
    assert(whole_pc.settings.picture_size == PictureSize::k1440p &&
           whole_pc.settings.frame_rate == FrameRate::k60Everywhere);
    assert(RecommendedSettingsFor(laptop, radeon780m.id, 16).gpu_id == radeon780m.id);
  }

  const Recommendation unknown = RecommendedSettings({}, 8);
  assert(unknown.settings.picture_size == PictureSize::k720p &&
         unknown.settings.frame_rate == FrameRate::kStandard30_60);
  const Recommendation unknown_device = RecommendedSettings({{"1234:5678", "Unlisted Graphics", false, 0}}, 8);
  assert(unknown_device.settings.picture_size == PictureSize::k720p &&
         unknown_device.settings.frame_rate == FrameRate::kStandard30_60);

  Settings first_run;
  const Settings first_run_effective = ApplyRecommendedSettings(first_run, uhd_4t.settings);
  assert(first_run_effective.picture_size == PictureSize::k480p &&
         first_run_effective.frame_rate == FrameRate::k60Everywhere);

  Settings manual = first_run;
  manual.recommendation_fields_tracked = true;
  manual.manual_picture_size = true;
  manual.picture_size = PictureSize::k720p;
  manual.frame_rate = FrameRate::kLocked30;
  const Settings auto_refresh = ApplyRecommendedSettings(manual, uhd_4t.settings);
  assert(auto_refresh.picture_size == PictureSize::k720p);  // a hand-picked value survives automatic refresh
  assert(auto_refresh.frame_rate == FrameRate::k60Everywhere);
  const std::vector<RecommendationChange> changes = CompareRecommendedSettings(manual, uhd_4t.settings);
  assert(changes.size() == 2);
  assert(changes[0].label == "3D picture size" && changes[0].manually_changed);
  assert(changes[1].label == "Native frame rate" && !changes[1].manually_changed);
  const Settings confirmed = ApplyRecommendedSettings(manual, uhd_4t.settings, true);
  assert(confirmed.picture_size == PictureSize::k480p && confirmed.frame_rate == FrameRate::k60Everywhere);
  assert(confirmed.recommendation_fields_tracked && !confirmed.manual_picture_size &&
         !confirmed.explicit_choice);

  Settings legacy_manual = manual;
  legacy_manual.recommendation_fields_tracked = false;
  legacy_manual.explicit_choice = true;
  const Settings legacy_kept = ApplyRecommendedSettings(legacy_manual, uhd_4t.settings);
  assert(legacy_kept.picture_size == PictureSize::k720p && legacy_kept.frame_rate == FrameRate::kLocked30);

  std::cout << "PASS Vulkan filtering/list; UHD 630 and Radeon 780M profiles; unknown fallback; manual-setting ownership ("
            << gpus.size() << " listed)\n";
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

void TestExtraSuperstarHeads(const fs::path& root) {
  const char* source_env = std::getenv("WWE2K14_TEST_HEAD_PACK_SOURCE");
  const char* expected_env = std::getenv("WWE2K14_TEST_HEAD_PACK_EXPECTED");
  const char* game_env = std::getenv("WWE2K14_TEST_GAME_FOLDER");
  if (!source_env || !expected_env || !game_env) {
    std::cout << "SKIP extra Superstar Heads: set WWE2K14_TEST_HEAD_PACK_SOURCE, "
                 "WWE2K14_TEST_HEAD_PACK_EXPECTED and WWE2K14_TEST_GAME_FOLDER for the private fixture check\n";
    return;
  }
  const fs::path source_pack = fs::u8path(source_env);
  const fs::path expected_pack = fs::u8path(expected_env);
  const fs::path source_content = source_pack.parent_path();
  const fs::path test_root = root / "extra-superstar-heads-test";
  std::error_code ec;
  fs::remove_all(test_root, ec);
  ec.clear();
  fs::create_directories(test_root, ec);
  assert(!ec);
  const std::string folder = source_pack.filename().string();
  const fs::path test_pack = test_root / "userdata/0000000000000000/545408B2/00000002" / folder;
  fs::create_directories(test_pack.parent_path(), ec);
  assert(!ec);
  fs::copy(source_pack, test_pack, fs::copy_options::recursive, ec);
  assert(!ec);

  // The edit is generated from one copied head pack. String tables from the other pristine
  // installed add-ons are also copied into scratch because existing head names span DLC packs.
  for (fs::directory_iterator pack(source_content, ec), end; !ec && pack != end; pack.increment(ec)) {
    if (!pack->is_directory(ec) || ec) continue;
    const fs::path string_file = pack->path() / "pac";
    std::error_code file_ec;
    for (fs::directory_iterator file(string_file, file_ec), file_end; !file_ec && file != file_end; file.increment(file_ec)) {
      const std::string name = file->path().filename().string();
      if (name.rfind("string_dlc", 0) != 0 || !file->is_regular_file(file_ec) || file_ec) continue;
      const fs::path destination = test_root / "userdata/0000000000000000/545408B2/00000002" /
                                   pack->path().filename() / "pac" / file->path().filename();
      fs::create_directories(destination.parent_path(), ec);
      assert(!ec);
      fs::copy_file(file->path(), destination, fs::copy_options::overwrite_existing, ec);
      assert(!ec);
    }
  }
  assert(!ec);

  Paths paths = MakePaths(test_root);
  const fs::path game_folder = fs::u8path(game_env);
  auto read = [](const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    assert(input);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  const std::array<std::string, 3> names{
      "pac/edit/edit_dlc05.pac", "info/catalog_hd.arc", "info/catalog_sd.arc"};
  const std::array<std::string, 3> pristine{
      read(source_pack / names[0]), read(source_pack / names[1]), read(source_pack / names[2])};
  const std::array<std::string, 3> expected{
      read(expected_pack / names[0]), read(expected_pack / names[1]), read(expected_pack / names[2])};
  assert(pristine[0] != expected[0] && pristine[1] != expected[1] && pristine[2] != expected[2]);
  const ExtraSuperstarHeadsStatus before = InspectExtraSuperstarHeads(paths, game_folder);
  assert(before.available && !before.applied && !before.changed);

  CancelFlag cancel{false};
  const Result applied = ApplyExtraSuperstarHeads(paths, game_folder, {}, cancel);
  assert(applied.ok);
  std::array<std::string, 3> output;
  for (size_t i = 0; i < names.size(); ++i) {
    output[i] = read(test_pack / names[i]);
    assert(output[i] == expected[i]);
  }
  const ExtraSuperstarHeadsStatus after = InspectExtraSuperstarHeads(paths, game_folder);
  assert(after.available && after.applied && !after.changed);
  assert(ApplyExtraSuperstarHeads(paths, game_folder, {}, cancel).ok);  // already applied is a no-op
  for (size_t i = 0; i < names.size(); ++i) assert(read(test_pack / names[i]) == output[i]);

  assert(UndoExtraSuperstarHeads(paths, {}, cancel).ok);
  for (size_t i = 0; i < names.size(); ++i) assert(read(test_pack / names[i]) == pristine[i]);
  const ExtraSuperstarHeadsStatus undone = InspectExtraSuperstarHeads(paths, game_folder);
  assert(undone.available && !undone.applied && !undone.changed);

  // A damaged source pack is rejected before any file is changed.
  {
    std::fstream damaged(test_pack / names[0], std::ios::in | std::ios::out | std::ios::binary);
    assert(damaged);
    char byte = '\0';
    damaged.write(&byte, 1);
    damaged.flush();
    assert(damaged);
  }
  const std::array<std::string, 3> corrupt_before{
      read(test_pack / names[0]), read(test_pack / names[1]), read(test_pack / names[2])};
  const Result refused = ApplyExtraSuperstarHeads(paths, game_folder, {}, cancel);
  assert(!refused.ok);
  for (size_t i = 0; i < names.size(); ++i) assert(read(test_pack / names[i]) == corrupt_before[i]);
  std::cout << "PASS extra Superstar Heads: copy-only apply matches HB7 bytes, repeated apply is a no-op, "
               "Undo is byte-exact, corrupted input is refused\n";
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
  TestBugReportSessionExtras(root);
  TestGpus();
  TestKeyboardControls(root);
  TestExtraSuperstarHeads(root);
  std::cout << "All core tests passed.\n";
  return 0;
}
