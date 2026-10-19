#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
#include <array>
#include <system_error>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace wwe2k14::launcher {
namespace {

fs::path CanonicalForMatch(const fs::path& path) {
  std::error_code error;
  fs::path canonical = fs::weakly_canonical(path, error);
  if (error) {
    error.clear();
    canonical = fs::absolute(path, error);
    if (error) canonical = path;
  }
  return canonical.lexically_normal();
}

bool EqualPathComponent(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
  const std::wstring& left_native = left.native();
  const std::wstring& right_native = right.native();
  if (left_native.size() != right_native.size()) return false;
  return CompareStringOrdinal(left_native.data(), static_cast<int>(left_native.size()),
                              right_native.data(), static_cast<int>(right_native.size()), TRUE) ==
         CSTR_EQUAL;
#else
  return internal::LowerAscii(internal::PathToUtf8(left)) ==
         internal::LowerAscii(internal::PathToUtf8(right));
#endif
}

bool IsPathWithin(const fs::path& path, const fs::path& root) {
  if (root.empty()) return false;
  auto path_component = path.begin();
  for (auto root_component = root.begin(); root_component != root.end();
       ++root_component, ++path_component) {
    if (path_component == path.end() || !EqualPathComponent(*path_component, *root_component)) return false;
  }
  return true;
}

bool IsFallbackOneDriveComponent(const fs::path& component) {
  const std::string name = internal::LowerAscii(internal::PathToUtf8(component));
  return name == "onedrive" || name.starts_with("onedrive - ");
}

#ifdef _WIN32
struct OneDriveRoots {
  std::vector<fs::path> paths;
  bool any_environment_variable_exists = false;
};

OneDriveRoots ReadOneDriveRoots() {
  OneDriveRoots result;
  constexpr std::array<const wchar_t*, 3> names{
      L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"};
  for (const wchar_t* name : names) {
    SetLastError(ERROR_SUCCESS);
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
      if (GetLastError() != ERROR_ENVVAR_NOT_FOUND) result.any_environment_variable_exists = true;
      continue;
    }
    result.any_environment_variable_exists = true;
    std::wstring value(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), required);
    if (length != 0 && length < required) {
      value.resize(length);
      result.paths.emplace_back(std::move(value));
    }
  }
  return result;
}
#endif

}  // namespace

Paths ResolvePaths(const fs::path& launcher_executable) {
  Paths result;
  std::error_code error;
  fs::path executable = fs::absolute(launcher_executable, error);
  if (error) executable = launcher_executable;
  result.exe_dir = executable.parent_path();
#ifdef _WIN32
  result.game_exe = result.exe_dir / "wwe2k14.exe";
#else
  result.game_exe = result.exe_dir / "wwe2k14";
#endif
  result.settings_file = result.exe_dir / "wwe2k14-enhanced.ini";
  result.userdata_dir = result.exe_dir / "userdata";
  result.music_dir = result.exe_dir / "music";
  result.logs_dir = result.exe_dir / "logs";
  result.backups_dir = result.exe_dir / "backups";
  result.default_game_folder = result.exe_dir / "WWE 2K14";
  return result;
}

bool IsUnderOneDrive(const fs::path& path) {
#ifdef _WIN32
  const OneDriveRoots roots = ReadOneDriveRoots();
  return IsOneDrivePath(path, roots.paths, !roots.any_environment_variable_exists);
#else
  // OneDrive environment roots are Windows-defined; retain the folder-name fallback elsewhere.
  return IsOneDrivePath(path, {}, true);
#endif
}

bool IsOneDrivePath(const fs::path& path, const std::vector<fs::path>& roots,
                    bool allow_name_fallback) {
  const fs::path canonical_path = CanonicalForMatch(path);
  for (const fs::path& root : roots) {
    if (IsPathWithin(canonical_path, CanonicalForMatch(root))) return true;
  }
  if (!allow_name_fallback) return false;
  for (const fs::path& component : canonical_path) {
    if (IsFallbackOneDriveComponent(component)) return true;
  }
  return false;
}

}  // namespace wwe2k14::launcher
