#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <cwchar>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

namespace wwe2k14::launcher {
namespace {

// The 2K14 runtime exposes only the SDL preset (--resolution) and the draw scale (--resolution_scale);
// there is no internal-resolution override like the WWE '13 runtime's scene_resolution hook.
const char* ResolutionPreset(Resolution resolution) {
  switch (resolution) {
    case Resolution::k1080p: return "1080p";
    case Resolution::k1440p: return "1440p";
    case Resolution::k720p: default: return "720p";
  }
}

Settings EffectiveSettings(const Settings& source, const std::vector<GpuInfo>& gpus) {
  Settings effective = source;
  if (effective.explicit_choice) return effective;
  std::vector<GpuInfo> recommendation_gpus;
  if (!source.gpu_id.empty()) {
    const auto selected = std::find_if(gpus.begin(), gpus.end(), [&](const GpuInfo& gpu) {
      return gpu.id == source.gpu_id;
    });
    if (selected != gpus.end()) recommendation_gpus.push_back(*selected);
  } else {
    recommendation_gpus = gpus;
  }
  const Settings recommended = RecommendedSettings(recommendation_gpus).settings;
  effective.resolution = recommended.resolution;
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
    const std::vector<std::pair<std::string, std::string>>& overrides) {
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
    const std::vector<std::pair<std::string, std::string>>& overrides) {
  std::map<std::string, std::string> environment;
  for (char** entry = environ; entry && *entry; ++entry) {
    const std::string value(*entry);
    const size_t separator = value.find('=');
    if (separator != std::string::npos) environment[value.substr(0, separator)] = value.substr(separator + 1);
  }
  for (const auto& [name, value] : overrides) {
    environment[name] = value;
  }
  std::vector<std::string> output;
  output.reserve(environment.size());
  for (const auto& [name, value] : environment) output.push_back(name + "=" + value);
  return output;
}

#endif

}  // namespace

LaunchPlan BuildLaunchPlan(const Paths& paths, const Settings& settings,
                           const std::vector<GpuInfo>& gpus) {
  const Settings effective = EffectiveSettings(settings, gpus);
  LaunchPlan plan;
  plan.executable = paths.game_exe;
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

  plan.arguments.emplace_back(std::string("--resolution=") + ResolutionPreset(effective.resolution));
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

  // Only the diagnostics the 2K14 executable actually reads: its thread profiler still uses the
  // WWE13_THREAD_PROFILE* names (src/thread_profiler.cpp). Empty values clear inherited developer
  // switches. The WWE '13-only renderer overrides (internal resolution, scene AA, frame-rate modes,
  // F24 pixel rate, rate census, perf overlay, lock-owner sample) are deliberately not set.
  plan.environment = {
      {"REX_DEBUG_UI", "false"},
      {"WWE13_THREAD_PROFILE", ""},
      {"WWE13_THREAD_PROFILE_HZ", ""},
      {"WWE13_THREAD_PROFILE_OUT", ""}};
  return plan;
}

Result StartGame(const LaunchPlan& plan) {
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
      return Result::Fail("The game program could not be found next to the launcher.");
    }
#ifdef _WIN32
    std::wstring command = QuoteArgument(plan.executable.wstring());
    for (const std::string& argument : plan.arguments) {
      command.push_back(L' ');
      command += QuoteArgument(Wide(argument));
    }
    command.push_back(L'\0');
    std::vector<wchar_t> environment = WindowsEnvironmentBlock(plan.environment);
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
    CloseHandle(process.hProcess);
    return Result::Ok();
#else
    std::vector<std::string> arguments;
    arguments.reserve(plan.arguments.size() + 1);
    arguments.push_back(internal::PathToUtf8(plan.executable));
    arguments.insert(arguments.end(), plan.arguments.begin(), plan.arguments.end());
    std::vector<char*> argv;
    for (std::string& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment = PosixEnvironment(plan.environment);
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
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (status != 0) {
      return Result::Fail("WWE 2K14 could not be started. Check the game folder and try again.");
    }
    return Result::Ok();
#endif
  } catch (...) {
    return Result::Fail("WWE 2K14 could not be started. Check the game folder and try again.");
  }
}

}  // namespace wwe2k14::launcher
