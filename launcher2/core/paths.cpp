#include "launcher_core.h"

#include <system_error>

namespace wwe2k14::launcher {

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

}  // namespace wwe2k14::launcher
