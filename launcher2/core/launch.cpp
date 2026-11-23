#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <fstream>
#include <cwchar>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace wwe2k14::launcher {
namespace {

// The 2K14 runtime's --resolution preset sets the guest video mode and startup window; the independent
// --resolution_scale cvar scales renderer drawing. There is no WWE '13-style scene_resolution override.
const char* ResolutionPreset(Resolution resolution) {
  switch (resolution) {
    case Resolution::k1080p: return "1080p";
    case Resolution::k1440p: return "1440p";
    case Resolution::k720p: default: return "720p";
  }
}

Settings EffectiveSettings(const Settings& source, const std::vector<GpuInfo>& gpus) {
  const Settings recommended = RecommendedSettingsFor(gpus, source.gpu_id).settings;
  Settings effective = ApplyRecommendedSettings(source, recommended);
  if (!effective.mods_enabled) {  // owner 11-18: Mods off = no Mods-page option reaches the game
    effective.hiac_weapon_wheel = false;
    effective.weapon_wheel_more = false;
    effective.head_edit = false;
    effective.unlock_all = false;
    effective.imports_in_grid = false;
  }
  return effective;
}

#ifdef _WIN32

std::wstring Wide(std::string_view text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring output(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      output.data(), length);
  return output;
}

std::wstring LowerWide(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
    return ch >= L'A' && ch <= L'Z' ? wchar_t(ch + (L'a' - L'A')) : ch;
  });
  return text;
}

std::wstring QuoteArgument(std::wstring_view argument) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (const wchar_t ch : argument) {
    if (ch == L'\\') {
      ++slashes;
    } else if (ch == L'"') {
      result.append(slashes * 2 + 1, L'\\');
      result.push_back(L'"');
      slashes = 0;
    } else {
      result.append(slashes, L'\\');
      slashes = 0;
      result.push_back(ch);
    }
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

std::vector<wchar_t> WindowsEnvironmentBlock(
    const std::vector<std::pair<std::string, std::string>>& overrides,
    const std::vector<std::string>& removals) {
  std::map<std::wstring, std::wstring> environment;
  LPWCH block = GetEnvironmentStringsW();
  if (block) {
    for (const wchar_t* item = block; *item; item += std::wcslen(item) + 1) {
      const std::wstring entry(item);
      const size_t separator = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
      if (separator != std::wstring::npos) {
        environment[LowerWide(entry.substr(0, separator))] = entry;
      }
    }
    FreeEnvironmentStringsW(block);
  }
  for (const std::string& name : removals) environment.erase(LowerWide(Wide(name)));
  for (const auto& [name_utf8, value_utf8] : overrides) {
    const std::wstring name = Wide(name_utf8);
    const std::wstring value = Wide(value_utf8);
    const std::wstring folded = LowerWide(name);
    environment[folded] = name + L"=" + value;
  }
  std::vector<wchar_t> output;
  for (const auto& [folded, entry] : environment) {
    (void)folded;
    output.insert(output.end(), entry.begin(), entry.end());
    output.push_back(L'\0');
  }
  output.push_back(L'\0');
  if (environment.empty()) output.push_back(L'\0');
  return output;
}

#else

std::vector<std::string> PosixEnvironment(
    const std::vector<std::pair<std::string, std::string>>& overrides,
    const std::vector<std::string>& removals) {
  std::map<std::string, std::string> environment;
  for (char** entry = environ; entry && *entry; ++entry) {
    const std::string value(*entry);
    const size_t separator = value.find('=');
    if (separator != std::string::npos) environment[value.substr(0, separator)] = value.substr(separator + 1);
  }
  for (const std::string& name : removals) environment.erase(name);
  for (const auto& [name, value] : overrides) {
    environment[name] = value;
  }
  std::vector<std::string> output;
  output.reserve(environment.size());
  for (const auto& [name, value] : environment) output.push_back(name + "=" + value);
  return output;
}

#endif

bool IsCancelled(const CancelFlag* cancel) {
  return cancel && cancel->load(std::memory_order_acquire);
}

}  // namespace

fs::path TestBuildMarkerRoot(const Paths& paths) {
  for (const fs::path& candidate : {paths.exe_dir, paths.exe_dir.parent_path()}) {
    std::error_code error;
    if (fs::exists(candidate / kTestUnlockAllMarker, error) && !error) return candidate;
  }
  return {};
}

// Native renderer test package: the environment of the package's "1 - Integrated GPU" .bat (2026-10-20) plus the
// picture-size / frame-rate / anti-aliasing choices, exactly as the .bat launchers 1-9 set them.
void AddNativeRendererEnvironment(LaunchPlan& plan, const Paths& paths, const Settings& s,
                                 bool create_output_directory = true) {
  static const char* const kBase[][2] = {
      {"WWE13_RATE_CENSUS", "1"}, {"WWE2K14_FRAME_TIME", "1"}, {"WWE2K14_NR", "replace"},
      {"WWE2K14_NR_DIRECT_INVALIDATE", "1"}, {"WWE2K14_NR_EARLY_SUBMIT", "256"}, {"WWE2K14_NR_UPLOAD_RING", "1"},
      {"WWE2K14_NR_STATE_ELIDE", "1"}, {"WWE2K14_NR_SHADER_KEY_MEMO", "1"}, {"WWE2K14_NR_TEX_DIRTY_SKIP", "1"},
      {"WWE2K14_NR_TEX_MEMO", "1"}, {"WWE2K14_NR_DRAW_FAST", "1"}, {"WWE2K14_NR_GPU_DEPTH24", "1"},
      {"WWE2K14_NR_RECORD_THREAD", "0"}, {"WWE2K14_PRESENT_REPEAT", "1"}, {"WWE2K14_NR_CONST_FENCE", "1"},
      {"WWE2K14_PUMP_GUARD", "1"}, {"WWE2K14_NR_PREWARM", "1"}, {"WWE2K14_NR_FAST_FETCH", "1"},
      {"WWE2K14_NR_DIRECT_PRESENT", "1"}, {"WWE13_NR_UPLOAD_HOST", "1"}, {"WWE2K14_NR_STENCIL_EXPORT", "1"},
      {"WWE2K14_NR_RESOLVE_DEFER", "1"}, {"WWE2K14_NR_SCENE_ONCE", "1"}, {"WWE2K14_NR_EDRAM_SKIP", "1"},
      {"WWE2K14_NR_EDRAM_FUSED", "1"}, {"WWE2K14_NR_F24_CONSERVATIVE", "1"}, {"WWE2K14_NR_CLEAR_REPLAY", "1"},
      {"WWE2K14_NR_TEX_PATCH", "1"}, {"WWE2K14_NR_COPYOUT_NOHASH", "1"}, {"WWE2K14_NR_DEPTH_DIRECT", "1"},
      {"WWE2K14_NR_CLEAR_ZONLY", "1"}, {"WWE2K14_NR_LAZY_STENCIL", "1"}, {"WWE2K14_NR_K8_REUSE", "1"},
      {"WWE2K14_NR_FOLD4X", "1"}, {"WWE2K14_NR_F16_REUSE", "1"}, {"WWE2K14_NR_HOLD_CLEARS", "1"},
      {"WWE2K14_NR_VB_CACHE", "1"}, {"WWE2K14_NR_SHORT_FETCH", "1"}, {"WWE2K14_NR_PRECISE_BARRIERS", "0"}  /* S3-51: P22 barriers caused black flashes on AMD/Adreno */,
      {"WWE2K14_NR_SCENE_CLEAR_ROWS", "1"}, {"WWE2K14_D3D_OBSERVE", "1"}, {"WWE2K14_D3D_OBSERVE_EVERY", "60"},
      {"WWE2K14_D3D_SHADOW", "2"}, {"WWE2K14_D3D_SHADOW_RT_MB", "512"}, {"WWE2K14_NR_PRESENT_D3D", "1"},
      {"WWE2K14_NR_DRAW_OFF", "1"}, {"WWE2K14_D3D_SHADOW_COMPARE_EVERY", "600"},
      {"WWE2K14_D3D_SHADOW_DIGEST_EVERY", "1000000"}, {"WWE2K14_D3D_IRQ_SYNC", "0"}, {"WWE2K14_D5_SERIALIZE", "1"},
      {"WWE2K14_DIAG_TILING", "1"}, {"WWE2K14_D3D_NATIVE_DEPTH", "1"}, {"WWE2K14_D3D_TILE_DEDUPE", "1"},
      {"WWE2K14_D3D_CLEAR_IMAGE", "1"}, {"WWE2K14_D3D_F16_FUSED", "1"}, {"WWE2K14_D3D_IRQ_WAIT_MS", "100"},
      {"WWE2K14_D3D_STALE_SAMPLE", "1"}, {"WWE2K14_D3D_OVERLAP_BYPASS", "1"}, {"WWE2K14_D3D_TSTAGE_FAST", "1"}};
  for (const auto& kv : kBase) plan.environment.emplace_back(kv[0], kv[1]);
  const char* scale = "1.0";
  switch (s.picture_size) {
    case PictureSize::k360p: scale = "0.5"; break;
    case PictureSize::k480p: scale = "0.6"; break;
    case PictureSize::k540p: scale = "0.75"; break;
    case PictureSize::k720p: scale = "1.0"; break;
    case PictureSize::k1080p: scale = "1.5"; break;
    case PictureSize::k1440p: scale = "2.0"; break;
    case PictureSize::k4k: scale = "3.0"; break;
  }
  const bool dedupe_columns = s.picture_size == PictureSize::k360p || s.picture_size == PictureSize::k480p;
  plan.environment.emplace_back("WWE2K14_D3D_SCENE_SCALE", scale);
  plan.environment.emplace_back("WWE2K14_D3D_SCALE_DYNAMIC", "0");  // owner 2026-10-20: fixed 480p, no dynamic scaling
  plan.environment.emplace_back("WWE2K14_D3D_TILE_DEDUPE_COLS", dedupe_columns ? "1" : "0");
  plan.environment.emplace_back("WWE2K14_D3D_AA_CAP", (s.anti_aliasing && !s.performance_mode) ? "2" : "1");
  plan.environment.emplace_back("WWE2K14_LOW_POWER", s.low_power ? "1" : "");
  // v1.5 render work (owner 2026-11-06), all picture-identical: lighter memory use, colour targets declare their view
  // formats, flat crowd cards drawn in groups (variant shaders in crowd-cards, 64-bit shader set only), timer thread sleeps.
  plan.environment.emplace_back("WWE2K14_NR_MEM_LEAN", "15");
  plan.environment.emplace_back("WWE2K14_NR_RT_VIEW_FORMATS", "1");
  plan.environment.emplace_back("WWE2K14_D3D_CROWD_CARDS", "1");
  plan.environment.emplace_back("WWE2K14_D3D_CROWD_CARDS_DIR", internal::PathToUtf8(paths.exe_dir / "crowd-cards"));
  plan.environment.emplace_back("WWE2K14_NR_PREWARM_SEED", internal::PathToUtf8(paths.exe_dir / "pipelines-seed.keys"));
  plan.environment.emplace_back("WWE2K14_TIMER_SLEEP", "1");
  // Crowd: unset = full crowd (the renderer's default); 2 = half, 0 = none.
  plan.environment.emplace_back("WWE2K14_D3D_CROWD", s.crowd == 2 ? "2" : s.crowd == 0 ? "0" : "");
  // Shadows: unset = on (the renderer's default); 0 = off.
  plan.environment.emplace_back("WWE2K14_D3D_SHADOWS", s.shadows == 0 ? "0" : "");
  // Texture quality: unset = normal (the renderer's default); 1 = low.
  plan.environment.emplace_back("WWE2K14_D3D_TEX_QUALITY", s.textures == 0 ? "1" : "");
  switch (s.frame_rate) {
    case FrameRate::kLocked30:
      // The batched recorder hurts locked-30 pacing (UHD 630 720p30 and Radeon 780M, 2026-10-20): not set here.
      plan.environment.emplace_back("WWE2K14_LOCK_30", s.fast_loading ? "4" : "1");  // 4 = Faster loading (card N4e)
      plan.environment.emplace_back("WWE2K14_NR_RECORD_BATCH", "");
      plan.environment.emplace_back("WWE2K14_NR_DRAW_SWAP", "");  // card U11 only acts on the batched recorder
      plan.environment.emplace_back("WWE2K14_D3D_NATIVE_DOF", "");
      break;
    case FrameRate::kStandard30_60:
      plan.environment.emplace_back("WWE2K14_LOCK_30", "");
      plan.environment.emplace_back("WWE2K14_NR_RECORD_BATCH", "64");
      plan.environment.emplace_back("WWE2K14_NR_DRAW_SWAP", "1");  // card U11: batch handoff by swap (~3 % less worker time/draw)
      plan.environment.emplace_back("WWE2K14_D3D_NATIVE_DOF", "");
      break;
    case FrameRate::k60Everywhere:
      plan.environment.emplace_back("WWE2K14_LOCK_30", "3");
      plan.environment.emplace_back("WWE2K14_NR_RECORD_BATCH", "64");
      plan.environment.emplace_back("WWE2K14_NR_DRAW_SWAP", "1");  // card U11
      plan.environment.emplace_back("WWE2K14_D3D_NATIVE_DOF", "1");
      break;
  }
  // The release zip ships one shaders/ folder at its root; the Linux launcher lives in Linux-x86_64/ below it.
  fs::path shader_dir = paths.exe_dir / "shaders";
  std::error_code shader_error;
  if (!fs::is_directory(shader_dir, shader_error) &&
      fs::is_directory(paths.exe_dir.parent_path() / "shaders", shader_error)) {
    shader_dir = paths.exe_dir.parent_path() / "shaders";
  }
  plan.environment.emplace_back("WWE2K14_NR_SHADERS", internal::PathToUtf8(shader_dir));
  // Intel Lake compatibility shaders (native only): unset = the renderer default.
  plan.environment.emplace_back("WWE2K14_NR_CONSTANTS32", s.intel_lake_shaders ? "1" : "");
  const fs::path nr_out = paths.logs_dir /
      ("nr-out-" + internal::LocalTimestamp(std::chrono::system_clock::now(), "%Y%m%d-%H%M%S"));
  if (create_output_directory) {
    std::error_code ignored;
    fs::create_directories(nr_out, ignored);
  }
  plan.environment.emplace_back("WWE2K14_NR_OUT", internal::PathToUtf8(nr_out));
}

bool MultiShadowsOn(const Settings& settings, const std::vector<GpuInfo>& gpus) {
  if (settings.shadows == 0 || settings.multi_shadows == 0) return false;
  if (settings.multi_shadows == 1) return true;
  // Automatic: on, except AMD integrated graphics until the owner's 780M check (docs/tasks/G70-option-report.md).
  const auto target = std::find_if(gpus.begin(), gpus.end(), [&](const GpuInfo& gpu) {
    return settings.gpu_id.empty() ? gpus.size() == 1 : gpu.id == settings.gpu_id;
  });
  return !(target != gpus.end() && target->integrated && target->id.rfind("1002:", 0) == 0);
}

fs::path ShaderToolsPath(const Paths& paths) {
#ifdef _WIN32
  return paths.exe_dir / "shadertools" / "XenosRecomp.exe";
#else
  return paths.exe_dir / "shadertools" / "XenosRecomp";
#endif
}

bool ShaderToolsPresent(const Paths& paths) {
  std::error_code error;
  return fs::is_regular_file(ShaderToolsPath(paths), error);
}

const char* NativeApiName(NativeApi api) {
  switch (api) {
    case NativeApi::kD3D12: return "d3d12";
    case NativeApi::kD3D11: return "d3d11";
    case NativeApi::kVulkan: break;
  }
  return "vulkan";
}

fs::path GameExecutableFor(const Paths& paths, Renderer renderer, NativeApi api) {
#ifdef _WIN32
  if (renderer == Renderer::kNative && !paths.game_exe.empty()) {
    return paths.game_exe.parent_path() / ("wwe2k14-" + std::string(NativeApiName(api)) + ".exe");
  }
#else
  (void)renderer; (void)api;
#endif
  return paths.game_exe;
}

std::vector<fs::path> AllGameExecutables(const Paths& paths) {
  std::vector<fs::path> result{paths.game_exe};
#ifdef _WIN32
  for (NativeApi api : {NativeApi::kVulkan, NativeApi::kD3D12, NativeApi::kD3D11}) {
    result.push_back(GameExecutableFor(paths, Renderer::kNative, api));
  }
#endif
  return result;
}

LaunchPlan BuildLaunchPlan(const Paths& paths, const Settings& settings,
                           const std::vector<GpuInfo>& gpus) {
  const Settings effective = EffectiveSettings(settings, gpus);
  LaunchPlan plan;
  plan.executable = GameExecutableFor(paths, effective.renderer, effective.native_api);
  plan.working_directory = paths.exe_dir;

  const fs::path game_folder = effective.game_folder.empty() ? paths.default_game_folder
                                                              : effective.game_folder;
  plan.arguments.push_back(internal::PathToUtf8(game_folder));
  const bool fullscreen = effective.display != DisplayMode::kWindowed;
  const fs::path log_file = paths.logs_dir /
      ("wwe2k14-" + internal::LocalTimestamp(std::chrono::system_clock::now(), "%Y%m%d-%H%M%S") + ".log");
  const std::array<std::string, 10> fixed_arguments = {
      "--async_shader_compilation=false",
      "--vulkan_async_skip_incomplete_frames=false",
      fullscreen ? "--fullscreen=true" : "--fullscreen=false",
      "--vulkan_allow_present_mode_immediate=false",
      "--ignore_offset_for_ranged_allocations=true",
      "--log_verbose=false",
      "--mnk_mode=true",
      "--log_file=" + internal::PathToUtf8(log_file),
      "--user_data_root=" + internal::PathToUtf8(paths.userdata_dir),
      "--debug_ui=false"};
  plan.arguments.insert(plan.arguments.end(), fixed_arguments.begin(), fixed_arguments.end());
  // Borderless window (#116): the game's fullscreen already is a borderless monitor-sized window; it additionally
  // switches 144/165 Hz monitors to a multiple of 60 Hz (display_refresh_match), which is what flickers or goes black
  // when switching windows. Borderless keeps the monitor's own refresh rate.
#if defined(_WIN32)  // the setting exists only in the Windows window code (window_win.cpp); Linux never changes modes
  if (effective.display == DisplayMode::kBorderless) plan.arguments.emplace_back("--display_refresh_match=false");
#endif

  // Native picture size controls its D3D scene scale while retaining the package's 720p window mode.
  // Xbox mode keeps the earlier renderer's selectable --resolution preset.
  const bool native_renderer = effective.renderer == Renderer::kNative;
  plan.arguments.emplace_back("--resolution=" +
                              std::string(ResolutionPreset(native_renderer ? Resolution::k720p
                                                                           : effective.resolution)));
  if (!settings.gpu_id.empty()) {
    // "vendor:device" selects a device by identity. When a PC has two identical GPUs the launcher
    // disambiguates them as "vendor:device:index" (gpus.cpp), but the game's vulkan_device_id parser
    // accepts exactly one colon and rejects the extra one, silently falling back to automatic device
    // selection. Pass the enumerated index the launcher recorded for those instead.
    const auto selected_gpu = std::find_if(gpus.begin(), gpus.end(), [&](const GpuInfo& gpu) {
      return gpu.id == settings.gpu_id;
    });
    const bool disambiguated = selected_gpu != gpus.end() && selected_gpu->vulkan_index >= 0 &&
        settings.gpu_id.find(':', settings.gpu_id.find(':') + 1) != std::string::npos;
    if (disambiguated) {
      plan.arguments.emplace_back("--vulkan_device=" + std::to_string(selected_gpu->vulkan_index));
    } else {
      plan.arguments.emplace_back("--vulkan_device_id=" + settings.gpu_id);
    }
  }

  if (effective.stretch_to_fill_wide_screens) {
    plan.arguments.emplace_back("--present_letterbox=false");
  }
  if (effective.picture_scale == 2 || effective.picture_scale == 3) {
    plan.arguments.emplace_back("--resolution_scale=" + std::to_string(effective.picture_scale));
  }

  // Only the diagnostics the 2K14 executable actually reads: its thread profiler still uses the
  // WWE13_THREAD_PROFILE* names (src/thread_profiler.cpp). Empty values clear inherited developer
  // switches. The WWE '13-only environment overrides (scene AA, frame-rate modes, F24 pixel rate,
  // rate census, perf overlay, lock-owner sample) are deliberately not set.
  plan.environment = {
      {"REX_DEBUG_UI", "false"},
      {"WWE13_THREAD_PROFILE", ""},
      {"WWE13_THREAD_PROFILE_HZ", ""},
      {"WWE13_THREAD_PROFILE_OUT", ""}};
  // The launcher's checkbox is authoritative, also over a WWE2K14_HOST_VSYNC inherited from the player's
  // environment (on = 1 is the game's own default).
  plan.environment.emplace_back("WWE2K14_HOST_VSYNC", effective.sync_frames_to_monitor ? "1" : "0");
  // Game fixes, set after the environment reset above (v1.2.0 set them before it and lost them).
  // CAW / signature / finisher save hang (GitHub #5 #19 #29, card S5-1): free the guest stacks of fully finished
  // threads so repeated saves can still create threads. SDK cvar, default off in the SDK; on for both renderers.
  plan.environment.emplace_back("REX_WWE2K14_THREAD_FREE_STACK_ON_EXIT", "1");
  // Custom Jukebox music repeat/stop (GitHub #10): owner-tested on v1.1.1 TEST2 (songs repeat, stop at match load).
  plan.environment.emplace_back("WWE2K14_XMP_FIX", "1");
  // Rope-shadow hard lines on the mat (GitHub #35 #41 #44 #48, card G35): the game fills some textures sparsely and
  // relies on zeroed new memory; zero newly committed physical pages. SDK cvar, default off; on for both renderers.
  plan.environment.emplace_back("REX_WWE2K14_ZERO_PHYSICAL_ON_ALLOC", "1");
  // PC keyboard typing in the game's text fields (#53, card K53; owner OK 2026-10-26 for test builds): the SDK reports
  // a keyboard to the title's keyboard query and sends typed characters; executables without that SDK change ignore it.
  plan.environment.emplace_back("WWE2K14_KEYBOARD_TEXT", "1");
  // GitHub #87 (player option, Controls page): bound keys stay controller buttons while typing; older executables ignore it.
  if (effective.keybinds_while_typing) plan.environment.emplace_back("WWE2K14_KEYBINDS_WHILE_TYPING", "1");
  // Post-match replays (GitHub #7) need the title's cache: partition (REX_MOUNT_CACHE=true, v1.4.0-v1.4.7 on Windows),
  // but with it mounted every match with a created superstar hangs on the loading screen (GitHub #72 #84; owner laptop
  // bisect 2026-11-02: same launcher without the mount loads CAW matches). Off until that is fixed; explicit "false" so
  // an inherited REX_MOUNT_CACHE cannot turn it back on. Replays are off again, as in v1.3.0.
  plan.environment.emplace_back("REX_MOUNT_CACHE", "false");
  // Owner test builds only (owner 2026-10-25): the shared marker is present in test packages and absent in public
  // packages. It both enables the unlocker and gates optional glitch-capture mode.
  const fs::path test_package_root = TestBuildMarkerRoot(paths);
  // Mods page, Gameplay (owner 11-15): "Unlock everything" is a player option too. Always set (empty when off) so an
  // inherited WWE2K14_UNLOCK_ALL=1 cannot turn it on behind the player's back.
  plan.environment.emplace_back("WWE2K14_UNLOCK_ALL", (!test_package_root.empty() || effective.unlock_all) ? "1" : "");
  // Mods page (owner 11-16): imported wrestlers in the main select grid's free places. Always set so an inherited value
  // cannot override the player's choice. Empty = off.
  plan.environment.emplace_back("WWE2K14_OWN_GRID", effective.imports_in_grid ? "1" : "");
  // Owner 11-18 Mods off: the game reads own-slots.txt next to its exe by itself; =0 keeps imported slots out.
  if (!effective.mods_enabled) plan.environment.emplace_back("WWE2K14_OWN_SLOTS", "0");
  else plan.environment_to_remove.emplace_back("WWE2K14_OWN_SLOTS");
#ifndef WWE2K14_LAUNCHER_NO_TEST_TOOLS
  // One test-only switch: empty is explicit even if the launcher's parent process inherited a developer value.
  // RenderDoc stays unloaded unless the existing test marker and the saved setting both enable it.
  const bool glitch_capture_enabled = !test_package_root.empty() && effective.glitch_capture_mode;
  plan.environment.emplace_back("WWE2K14_GLITCH_CAPTURE", glitch_capture_enabled ? "1" : "");
  plan.environment.emplace_back(
      "WWE2K14_GLITCH_CAPTURE_DIR",
      glitch_capture_enabled ? internal::PathToUtf8(test_package_root / "logs" / "captures") : "");
#endif
  // Imported arena handling is a game-side rule, so both renderers receive the list. Empty values clear inherited
  // developer settings; the game automatically relocates missing records while the imported list is present.
  {
    std::error_code arena_error;
    const fs::path arena_list = paths.exe_dir / "mods" / "wwe13" / "arenas.txt";
    const bool installed = effective.mods_enabled && fs::is_regular_file(arena_list, arena_error) && !arena_error;
    plan.environment.emplace_back("WWE2K14_ARENA_LIST", installed ? internal::PathToUtf8(arena_list) : "");
  }
  // Mods page, Gameplay. A game-side rule change, so it applies to both renderers.
  if (effective.hiac_weapon_wheel) {
    plan.environment.emplace_back("WWE2K14_HIAC_WHEEL", effective.weapon_wheel_more ? "2" : "1");
  }
  // Mods page, Gameplay: Superstar Heads edit like any Custom Superstar (neck, traps, hair, facial hair). Game-side, both renderers.
  // Always set (empty when off) so an inherited WWE2K14_HEAD_EDIT=1 cannot turn it on (V160 review), like the arena list.
  // v1.6.0: the option is hidden (owner 11-11), so it is always off, even when an older test build saved head_edit=1.
  plan.environment.emplace_back("WWE2K14_HEAD_EDIT", "");
  // Game language: the game reads the Xbox language id from user_language (XConfig + XamGetLanguage).
  if (native_renderer && (effective.texture_filtering == 0 || effective.performance_mode)) {
    plan.arguments.emplace_back("--anisotropic_override=-1");  // the game's own filtering
  }
  // Controls page: controller vibration off (#102). The input layer is shared, so both renderers.
  if (!effective.controller_vibration) plan.arguments.emplace_back("--input_vibration=false");
  plan.arguments.emplace_back("--user_language=" + std::to_string(IsOfferedLanguage(effective.language) ? effective.language : 1));
  if (native_renderer) {
    AddNativeRendererEnvironment(plan, paths, effective);
    // AMD integrated graphics (Radeon 780M class, card AMD780M): their small device-local heaps sat pinned at 100 %
    // of the driver budget and the device was lost after a few minutes; leave headroom and reuse free space inside
    // existing memory blocks (plume k14 M1). Owner-verified on the 780M 2026-10-27. Target = the chosen GPU, or the
    // only GPU when the choice is automatic.
    const auto target_gpu = std::find_if(gpus.begin(), gpus.end(), [&](const GpuInfo& gpu) {
      return settings.gpu_id.empty() ? gpus.size() == 1 : gpu.id == settings.gpu_id;
    });
    if (MultiShadowsOn(effective, gpus)) plan.environment.emplace_back("WWE2K14_MULTI_SHADOWS", "1");
    // v1.7.0: the new renderer picks its graphics card by name, not by --vulkan_device_id (owner laptop 11-22: with
    // "AMD Radeon 780M" chosen it ran on the RTX 4060). Empty = its own choice (a discrete card first).
    plan.environment.emplace_back("WWE2K14_W13R_GPU",
                                  settings.gpu_id.empty() || target_gpu == gpus.end() ? "" : target_gpu->name);
    if (target_gpu != gpus.end() && target_gpu->integrated && target_gpu->id.rfind("1002:", 0) == 0) {
      plan.environment.emplace_back("WWE2K14_NR_MEM_BUDGET_PCT", "85");
      plan.environment.emplace_back("WWE2K14_NR_MEM_BUDGET_REUSE", "1");
    }
  } else {
    // Derive the removal set from the same native environment table, so inherited launcher switches cannot
    // reactivate the native renderer when the player selects Xbox mode.
    // WWE2K14_LOCK_30 is the game-side frame-rate hook and is set for the Xbox renderer below.
    LaunchPlan native_environment;
    AddNativeRendererEnvironment(native_environment, paths, effective, false);
    for (const auto& [name, value] : native_environment.environment) {
      (void)value;
      if (name != "WWE2K14_LOCK_30") plan.environment_to_remove.push_back(name);
    }
    switch (effective.xbox_frame_rate) {
      case FrameRate::kLocked30: plan.environment.emplace_back("WWE2K14_LOCK_30", "1"); break;
      case FrameRate::kStandard30_60: plan.environment.emplace_back("WWE2K14_LOCK_30", ""); break;
      case FrameRate::k60Everywhere: plan.environment.emplace_back("WWE2K14_LOCK_30", "3"); break;
    }
    plan.arguments.emplace_back(effective.xbox_anti_aliasing ? "--swap_post_effect=fxaa"
                                                             : "--swap_post_effect=none");
  }
  return plan;
}

StaleMemoryCleanup RemoveStaleGuestMemoryFiles(const fs::path& shm_dir, double min_age_seconds,
                                               const fs::path& proc_dir) {
  StaleMemoryCleanup result;
#ifdef _WIN32
  (void)shm_dir; (void)min_age_seconds; (void)proc_dir;
#else
  std::error_code ec;
  std::vector<fs::path> candidates;
  for (fs::directory_iterator it(shm_dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
       it.increment(ec)) {
    const std::string name = internal::PathToUtf8(it->path().filename());
    if (!name.starts_with("xenia_memory_")) continue;
    std::error_code type_error;
    if (!it->is_regular_file(type_error) || type_error) continue;
    candidates.push_back(it->path());
  }
  if (candidates.empty()) return result;
  // Files still mapped by a running process (this game, the WWE '13 recomp, another copy) are listed in its maps.
  std::set<std::string> mapped;
  for (fs::directory_iterator it(proc_dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
       it.increment(ec)) {
    const std::string pid = internal::PathToUtf8(it->path().filename());
    if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) continue;
    std::ifstream maps(it->path() / "maps");
    if (!maps) {
      // V152 review (11-09): fail closed. A process of this user whose maps cannot be read might map one of the files;
      // skip the whole cleanup this time. Processes that just ended or belong to other users (their files are theirs and
      // /dev/shm's sticky bit keeps them anyway) do not block it.
      struct stat owner {};
      if (::stat(it->path().c_str(), &owner) == 0 && owner.st_uid == ::getuid()) return result;
      continue;
    }
    std::string line;
    while (std::getline(maps, line)) {
      const size_t at = line.find("/xenia_memory_");
      if (at == std::string::npos) continue;
      size_t end_at = line.find(' ', at);
      mapped.insert(line.substr(at + 1, end_at == std::string::npos ? std::string::npos : end_at - at - 1));
    }
    if (maps.bad()) return result;  // V152 review 2: a read error part-way leaves the list incomplete: remove nothing
  }
  if (ec) return result;  // the process list could not be read completely: remove nothing
  const auto now = fs::file_time_type::clock::now();
  for (const fs::path& file : candidates) {
    const std::string name = internal::PathToUtf8(file.filename());
    if (mapped.count(name)) continue;
    std::error_code file_error;
    const auto written = fs::last_write_time(file, file_error);
    if (file_error) continue;
    if (std::chrono::duration<double>(now - written).count() < min_age_seconds) continue;  // just created, not mapped yet
    struct stat info {};
    const uint64_t bytes = ::stat(file.c_str(), &info) == 0 ? uint64_t(info.st_blocks) * 512u : 0;
    if (fs::remove(file, file_error) && !file_error) { ++result.files; result.bytes += bytes; }
  }
#endif
  return result;
}

Result StartGame(const LaunchPlan& plan, double watch_seconds, std::atomic<bool>* process_started,
                 const CancelFlag* cancel, uint64_t* process_id) {
  if (process_started) process_started->store(false, std::memory_order_relaxed);
  if (process_id) *process_id = 0;
  try {
    std::error_code error;
    for (const std::string& argument : plan.arguments) {
      constexpr std::string_view log_prefix = "--log_file=";
      constexpr std::string_view user_prefix = "--user_data_root=";
      if (argument.starts_with(log_prefix)) {
        if (!internal::EnsureDirectory(internal::PathFromUtf8(argument.substr(log_prefix.size())).parent_path())) {
          return Result::Fail("The launcher could not prepare the game folders.");
        }
      } else if (argument.starts_with(user_prefix)) {
        if (!internal::EnsureDirectory(internal::PathFromUtf8(argument.substr(user_prefix.size())))) {
          return Result::Fail("The launcher could not prepare the game folders.");
        }
      }
    }
    if (!fs::is_regular_file(plan.executable, error) || error) {
      // GitHub/Discord 2026-10-31: players copied only the launcher (hotfix) into a new folder. Say what is missing
      // and where the launcher looked.
      return Result::Fail("The recomp's game program (" + internal::PathToUtf8(plan.executable.filename()) +
                          ") is not in " + internal::PathToUtf8(plan.executable.parent_path()) +
                          ". Put the launcher in the folder that has it, or download the full release package.");
    }
    if (IsCancelled(cancel)) return Result::Ok();
#ifdef _WIN32
    std::wstring command = QuoteArgument(plan.executable.wstring());
    for (const std::string& argument : plan.arguments) {
      command.push_back(L' ');
      command += QuoteArgument(Wide(argument));
    }
    command.push_back(L'\0');
    std::vector<wchar_t> environment = WindowsEnvironmentBlock(plan.environment, plan.environment_to_remove);
    const std::wstring working_directory = plan.working_directory.empty()
                                               ? plan.executable.parent_path().wstring()
                                               : plan.working_directory.wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(plan.executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, environment.data(),
                        working_directory.empty() ? nullptr : working_directory.c_str(),
                        &startup, &process)) {
      return Result::Fail("WWE 2K14 could not be started. Check the game folder and try again.");
    }
    CloseHandle(process.hThread);
    if (process_id) *process_id = process.dwProcessId;
    if (process_started) process_started->store(true, std::memory_order_release);
    bool failed_early = false;
    bool outcome_unknown = false;
    if (watch_seconds > 0.0) {
      const auto deadline = std::chrono::steady_clock::now() +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(watch_seconds));
      while (std::chrono::steady_clock::now() < deadline) {
        if (IsCancelled(cancel)) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const DWORD wait_ms = static_cast<DWORD>(std::clamp<int64_t>(remaining.count(), 1, 100));
        const DWORD wait_result = WaitForSingleObject(process.hProcess, wait_ms);
        if (IsCancelled(cancel)) break;
        if (wait_result == WAIT_TIMEOUT) continue;
        if (wait_result == WAIT_OBJECT_0) {
          DWORD exit_code = 0;
          if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
            const DWORD wait_error = GetLastError();
            std::fprintf(stderr, "[launcher] GetExitCodeProcess failed while watching WWE 2K14: %lu\n",
                         static_cast<unsigned long>(wait_error));
            outcome_unknown = true;
          } else if (exit_code == STILL_ACTIVE) {
            std::fprintf(stderr, "[launcher] WWE 2K14 wait signaled but its exit code is still active\n");
            outcome_unknown = true;
          } else {
            failed_early = exit_code != 0;
          }
          break;
        }
        if (wait_result == WAIT_FAILED) {
          const DWORD wait_error = GetLastError();
          std::fprintf(stderr, "[launcher] WaitForSingleObject failed while watching WWE 2K14: %lu\n",
                       static_cast<unsigned long>(wait_error));
        } else {
          std::fprintf(stderr, "[launcher] WaitForSingleObject returned an unknown result: %lu\n",
                       static_cast<unsigned long>(wait_result));
        }
        outcome_unknown = true;
        break;
      }
    }
    CloseHandle(process.hProcess);
    if (failed_early) return Result::Fail(kGameClosedEarlyMessage);
    if (outcome_unknown) return Result::Unknown();
    return Result::Ok();
#else
    std::vector<std::string> arguments;
    arguments.reserve(plan.arguments.size() + 1);
    arguments.push_back(internal::PathToUtf8(plan.executable));
    arguments.insert(arguments.end(), plan.arguments.begin(), plan.arguments.end());
    std::vector<char*> argv;
    for (std::string& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment = PosixEnvironment(plan.environment, plan.environment_to_remove);
    std::vector<char*> envp;
    for (std::string& entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
      return Result::Fail("WWE 2K14 could not be started. Check the game folder and try again.");
    }
    const fs::path cwd = plan.working_directory.empty() ? plan.executable.parent_path()
                                                        : plan.working_directory;
    int status = 0;
    if (!cwd.empty()) status = posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    if (status != 0) {
      posix_spawn_file_actions_destroy(&actions);
      return Result::Fail("WWE 2K14 could not be started. Check the game folder and try again.");
    }
    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0) {
      posix_spawn_file_actions_destroy(&actions);
      return Result::Fail("WWE 2K14 could not be started. Check the game folder and try again.");
    }
#ifdef POSIX_SPAWN_SETSID
    (void)posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
    pid_t child = 0;
    status = posix_spawn(&child, plan.executable.c_str(), &actions, &attributes,
                         argv.data(), envp.data());
    if (status == EACCES) {
      // GitHub #33: some extractors and drive formats drop the executable bit; restore it once and retry.
      std::error_code chmod_error;
      fs::permissions(plan.executable,
                      fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                      fs::perm_options::add, chmod_error);
      std::fprintf(stderr, "[launcher] game not executable; chmod +x %s\n",
                   chmod_error ? chmod_error.message().c_str() : "ok");
      if (!chmod_error) {
        status = posix_spawn(&child, plan.executable.c_str(), &actions, &attributes,
                             argv.data(), envp.data());
      }
    }
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (status != 0) {
      std::fprintf(stderr, "[launcher] posix_spawn %s failed: %s\n",
                   internal::PathToUtf8(plan.executable).c_str(), std::strerror(status));
      if (status == EACCES) {
        return Result::Fail("WWE 2K14 could not be started: the game file is not allowed to run (permission denied). "
                            "Move the folder to your home drive or run: chmod +x \"" +
                            internal::PathToUtf8(plan.executable) + "\"");
      }
      return Result::Fail("WWE 2K14 could not be started (" + std::string(std::strerror(status)) +
                          "). Check the game folder and try again.");
    }
    if (process_id) *process_id = static_cast<uint64_t>(child);
    if (process_started) process_started->store(true, std::memory_order_release);
    bool failed_early = false;
    bool outcome_unknown = false;
    if (watch_seconds > 0.0) {
      const auto deadline = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(static_cast<int64_t>(watch_seconds * 1000.0));
      while (std::chrono::steady_clock::now() < deadline) {
        if (IsCancelled(cancel)) break;
        int wait_status = 0;
        const pid_t done = waitpid(child, &wait_status, WNOHANG);
        if (done == child) {
          if (IsCancelled(cancel)) break;
          if (WIFSIGNALED(wait_status)) {
            failed_early = true;
          } else if (WIFEXITED(wait_status)) {
            failed_early = WEXITSTATUS(wait_status) != 0;
          } else {
            std::fprintf(stderr, "[launcher] waitpid returned an unclassifiable WWE 2K14 status: %d\n",
                         wait_status);
            outcome_unknown = true;
          }
          break;
        }
        if (done < 0) {
          const int wait_error = errno;
          if (wait_error == EINTR) continue;
          std::fprintf(stderr, "[launcher] waitpid failed while watching WWE 2K14: errno=%d\n", wait_error);
          outcome_unknown = true;
          break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(100)));
      }
    }
    if (failed_early) return Result::Fail(kGameClosedEarlyMessage);
    if (outcome_unknown) return Result::Unknown();
    return Result::Ok();
#endif
  } catch (...) {
    return Result::Fail("WWE 2K14 could not be started. Check the game folder and try again.");
  }
}

}  // namespace wwe2k14::launcher
