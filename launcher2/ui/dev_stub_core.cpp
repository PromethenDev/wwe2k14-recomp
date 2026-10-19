// UI-only demonstration implementation. This file is selected only with
// -DWWE2K14_LAUNCHER_STUB_CORE=ON and never touches real game files.
#include "launcher_core.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace wwe2k14::launcher {
namespace {

Settings g_settings;
std::vector<Song> g_songs{
    {fs::path("music/Entrance Theme.mp3"), "Entrance Theme", 186.0, "MP3"},
    {fs::path("music/Friday Night.flac"), "Friday Night", 214.0, "FLAC"},
    {fs::path("music/Walkout.wav"), "Walkout", 92.0, "WAV"},
};
std::vector<BackupInfo> g_backups{
    {fs::path("backups/wwe2k14-saves-20261001-094100.zip"), "2026-10-01T09:41:00", 1874321, false},
    {fs::path("backups/wwe2k14-saves-20260930-183500-auto.zip"), "2026-09-30T18:35:00", 1869812, true},
    {fs::path("backups/wwe2k14-saves-20260929-121000.zip"), "2026-09-29T12:10:00", 1857730, false},
};

GameFilesStatus ReadyStatus(const fs::path& folder) {
  GameFilesStatus status;
  status.base_game = FileState::kFound;
  status.title_update = FileState::kFound;
  status.dlc_installed = 3;
  status.game_folder = folder;
  status.detail = "WWE 2K14 is ready to play.";
  return status;
}

void ReportProgress(const ProgressFn& progress, const char* label) {
  if (progress) progress(1, 1, label);
}

std::string UpperExtension(const fs::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
  return extension;
}

}  // namespace

Paths ResolvePaths(const fs::path& launcher_executable) {
  Paths paths;
  paths.exe_dir = launcher_executable.parent_path();
#ifdef _WIN32
  paths.game_exe = paths.exe_dir / "wwe2k14.exe";
#else
  paths.game_exe = paths.exe_dir / "wwe2k14";
#endif
  paths.settings_file = paths.exe_dir / "wwe2k14-enhanced.ini";
  paths.userdata_dir = paths.exe_dir / "userdata";
  paths.music_dir = paths.exe_dir / "music";
  paths.logs_dir = paths.exe_dir / "logs";
  paths.backups_dir = paths.exe_dir / "backups";
  paths.default_game_folder = paths.exe_dir / "WWE 2K14";
  return paths;
}

Settings LoadSettings(const Paths& paths) {
  if (g_settings.game_folder.empty()) g_settings.game_folder = paths.default_game_folder;
  return g_settings;
}

Result SaveSettings(const Paths&, const Settings& settings) {
  g_settings = settings;
  return Result::Ok();
}

std::vector<GpuInfo> ListGpus() {
  return {{"stub:5060", "NVIDIA GeForce RTX 5060 Ti", false, 0},
          {"stub:780m", "AMD Radeon 780M Graphics", true, 1}};
}

Recommendation RecommendedSettings(const std::vector<GpuInfo>& gpus) {
  Recommendation recommendation;
  recommendation.settings = Settings{};
  recommendation.reason = gpus.empty() ? "Settings picked automatically" : gpus.front().name + " · discrete graphics";
  return recommendation;
}

GameFilesStatus CheckGameFolder(const fs::path& folder, const Paths&) { return ReadyStatus(folder); }

std::vector<FoundGame> FindGameFiles(const Paths& paths, const ProgressFn& progress, const CancelFlag& cancel) {
  ReportProgress(progress, "Checking a sample WWE 2K14 folder");
  if (cancel.load()) return {};
  return {{paths.default_game_folder, ReadyStatus(paths.default_game_folder)}};
}

Result ExtractDiscImage(const fs::path&, const fs::path&, const ProgressFn& progress, const CancelFlag& cancel) {
  ReportProgress(progress, "Preparing the sample game folder");
  return cancel.load() ? Result::Fail("The operation was canceled.") : Result::Ok();
}

std::optional<PackageInfo> InspectPackage(const fs::path& package_or_folder) {
  if (package_or_folder.empty()) return std::nullopt;
  const std::string extension = UpperExtension(package_or_folder);
  return PackageInfo{extension == "XEXP" ? PackageKind::kTitleUpdate : PackageKind::kDlc,
                     package_or_folder.stem().string(), 0x545408B2};
}

Result ImportPackage(const fs::path&, const Paths&, const fs::path&, const ProgressFn& progress,
                     const CancelFlag& cancel) {
  ReportProgress(progress, "Adding the sample update or pack");
  return cancel.load() ? Result::Fail("The operation was canceled.") : Result::Ok();
}

std::vector<BackupInfo> ListBackups(const Paths&) { return g_backups; }

Result BackupSaves(const Paths&, bool automatic, BackupInfo* created) {
  BackupInfo backup;
  backup.file = fs::path(automatic ? "backups/wwe2k14-saves-sample-auto.zip" : "backups/wwe2k14-saves-sample.zip");
  backup.when = "2026-10-01T10:00:00";
  backup.bytes = 1880040;
  backup.automatic = automatic;
  g_backups.insert(g_backups.begin(), backup);
  if (automatic && g_backups.size() > 5) g_backups.resize(5);
  if (created) *created = backup;
  return Result::Ok();
}

Result RestoreBackup(const Paths&, const fs::path&) { return Result::Ok(); }
Result ImportBackup(const Paths&, const fs::path&) { return Result::Ok(); }

std::vector<Song> ListSongs(const Paths&) { return g_songs; }

Result AddSongs(const Paths&, const std::vector<fs::path>& files, std::vector<std::string>* skipped) {
  for (const auto& file : files) {
    const std::string format = UpperExtension(file);
    if (format != "MP3" && format != "FLAC" && format != "WAV" && format != "OGG") {
      if (skipped) skipped->push_back(file.filename().string());
      continue;
    }
    const auto duplicate = std::find_if(g_songs.begin(), g_songs.end(), [&](const Song& song) {
      return song.title == file.stem().string();
    });
    if (duplicate == g_songs.end()) g_songs.push_back({file, file.stem().string(), 0, format});
  }
  return Result::Ok();
}

Result RemoveSong(const Paths&, const fs::path& song_file) {
  std::erase_if(g_songs, [&](const Song& song) { return song.file == song_file; });
  return Result::Ok();
}

LaunchPlan BuildLaunchPlan(const Paths& paths, const Settings&, const std::vector<GpuInfo>&) {
  LaunchPlan plan;
  plan.executable = paths.game_exe;
  plan.working_directory = paths.exe_dir;
  return plan;
}

Result StartGame(const LaunchPlan&, double, std::atomic<bool>* process_started, const CancelFlag*) {
  if (process_started) process_started->store(true);
  return Result::Ok();
}

bool IsUnderOneDrive(const fs::path&) { return false; }

Result SaveBugReport(const Paths& paths, fs::path* created_zip) {
  if (created_zip) *created_zip = paths.exe_dir / "wwe2k14-bug-report-sample.zip";
  return Result::Ok();
}

}  // namespace wwe2k14::launcher
