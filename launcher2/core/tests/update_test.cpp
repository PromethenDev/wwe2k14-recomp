#ifdef NDEBUG
#undef NDEBUG
#endif

#include "launcher_core.h"
#include "update.h"

#include "internal/archive.h"
#include "internal/util.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {

std::string Bytes(const std::string& text) { return text; }

std::vector<uint8_t> ToBytes(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

void WriteAll(const fs::path& path, const std::string& text) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  assert(!error);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  assert(output.good());
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  assert(output.good());
}

std::string ReadAll(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return text;
}

void TestVersionCompare() {
  assert(CompareVersions("1.4.0", "1.3.0") > 0);
  assert(CompareVersions("1.3.0", "1.4.0") < 0);
  assert(CompareVersions("1.3.0", "1.3.0") == 0);
  assert(CompareVersions("v1.4", "1.3.0") > 0);
  assert(CompareVersions("1.10.0", "1.9.0") > 0);
  assert(CompareVersions("1.4", "1.4.0") == 0);
  assert(CompareVersions("1.4.0", "1.4.0-rc1") > 0);
  assert(CompareVersions("1.4.0-rc1", "1.4.0") < 0);
  assert(CompareVersions("2.0.0", "1.99.99") > 0);
  assert(CompareVersions("1.3.0+build7", "1.3.0") == 0);
  std::cout << "PASS version compare\n";
}

const char* kValidRelease =
    "{\n"
    "  \"tag_name\": \"v1.4.0\",\n"
    "  \"name\": \"v1.4.0\",\n"
    "  \"body\": \"What's new:\\n- Fixes\\n- A \\\"quoted\\\" line \\u2714\",\n"
    "  \"draft\": false,\n"
    "  \"prerelease\": false,\n"
    "  \"assets\": [\n"
    "    {\"name\": \"ffmpeg-source-abc.zip\", \"size\": 5, "
    "\"browser_download_url\": \"https://github.com/PromethenDev/wwe2k14-recomp/releases/download/"
    "v1.1.0/ffmpeg-source-abc.zip\", \"digest\": \"sha256:0000000000000000000000000000000000000000"
    "000000000000000000000000\"},\n"
    "    {\"name\": \"WWE2K14-Recomp-v1.1.0.zip\", \"size\": 123456, "
    "\"browser_download_url\": \"https://github.com/PromethenDev/wwe2k14-recomp/releases/download/"
    "v1.1.0/WWE2K14-Recomp-v1.1.0.zip\", "
    "\"digest\": \"sha256:26c37a68f0b4c6a4b8b6f6b8c0d2e4f6081a2b3c4d5e6f708192a3b4c5d6e7f8\"}\n"
    "  ]\n"
    "}\n";

void TestJsonParse() {
  auto info = ParseLatestReleaseJson(kValidRelease, "1.3.0");
  assert(info.has_value());
  assert(info->available);
  assert(info->tag == "v1.4.0");
  assert(info->version == "1.4.0");
  assert(info->name == "WWE2K14-Recomp-v1.1.0.zip");
  assert(info->url.find("WWE2K14-Recomp-v1.1.0.zip") != std::string::npos);
  assert(info->size == 123456);
  assert(info->sha256 == "26c37a68f0b4c6a4b8b6f6b8c0d2e4f6081a2b3c4d5e6f708192a3b4c5d6e7f8");
  assert(info->notes.find("Fixes") != std::string::npos);
  assert(info->notes.find("\xE2\x9C\x94") != std::string::npos);  // \u2714 -> UTF-8 check mark

  // The same release is not offered when the launcher is already on it (or newer).
  auto same = ParseLatestReleaseJson(kValidRelease, "1.4.0");
  assert(same.has_value() && !same->available);
  auto older = ParseLatestReleaseJson(kValidRelease, "1.5.0");
  assert(older.has_value() && !older->available);

  // Missing digest: parsed, but never offered because the download could not be verified.
  const char* kNoDigest =
      "{\"tag_name\":\"v1.4.0\",\"draft\":false,\"prerelease\":false,\"assets\":[{"
      "\"name\":\"WWE2K14-Recomp-v1.1.0.zip\",\"size\":5,"
      "\"browser_download_url\":\"https://github.com/x/WWE2K14-Recomp-v1.1.0.zip\"}]}";
  auto missing = ParseLatestReleaseJson(kNoDigest, "1.3.0");
  assert(missing.has_value());
  assert(!missing->available);
  assert(missing->sha256.empty());

  // Drafts and prereleases are ignored.
  std::string prerelease = kValidRelease;
  prerelease.replace(prerelease.find("\"prerelease\": false"), std::string("\"prerelease\": false").size(),
                     "\"prerelease\": true");
  auto pre = ParseLatestReleaseJson(prerelease, "1.3.0");
  assert(pre.has_value() && !pre->available);

  // Malformed and tag-less documents are rejected outright.
  assert(!ParseLatestReleaseJson("{not json", "1.3.0").has_value());
  assert(!ParseLatestReleaseJson("{\"draft\": false}", "1.3.0").has_value());
  std::cout << "PASS release JSON parse (digest, missing digest, drafts, malformed)\n";
}

void TestNeverTouch() {
  assert(IsNeverTouchPath("userdata/x"));
  assert(IsNeverTouchPath("USERDATA/x"));
  assert(IsNeverTouchPath("userdata"));
  assert(IsNeverTouchPath("userdata\\x"));
  assert(IsNeverTouchPath("music/song.mp3"));
  assert(IsNeverTouchPath("logs/launcher.log"));
  assert(IsNeverTouchPath("backups/wwe2k14-saves.zip"));
  assert(IsNeverTouchPath("WWE 2K14/default.xex"));
  assert(IsNeverTouchPath("wwe2k14-enhanced.ini"));
  assert(IsNeverTouchPath("wwe2k14.toml"));
  assert(!IsNeverTouchPath("wwe2k14"));
  assert(!IsNeverTouchPath("wwe2k14-launcher"));
  assert(!IsNeverTouchPath("WWE 2K14 Launcher.exe"));
  assert(!IsNeverTouchPath("README.md"));
  assert(!IsNeverTouchPath("Linux-x86_64/wwe2k14"));
  assert(!IsNeverTouchPath("shader-cache/545108B4.xsh"));

  // URL rules: by default only https GitHub assets are trusted; the test override opens http.
  assert(update_internal::IsAllowedDownloadUrl("https://github.com/x/y.zip"));
  assert(update_internal::IsAllowedDownloadUrl("https://objects.githubusercontent.com/x/y.zip"));
  assert(!update_internal::IsAllowedDownloadUrl("http://github.com/x/y.zip"));
  assert(!update_internal::IsAllowedDownloadUrl("https://evil.example.com/y.zip"));
  std::cout << "PASS never-touch paths and URL rules\n";
}

// The release zip carries both platforms (a Windows root and Linux-x86_64/); the updater picks the one
// matching the running launcher. Shared top-level documents are covered too.
void BuildReleaseZip(const fs::path& zip) {
  std::vector<internal::ZipEntry> entries;
  entries.push_back({"WWE2K14-Recomp-x64/Linux-x86_64/wwe2k14", ToBytes("new-game")});
  entries.push_back({"WWE2K14-Recomp-x64/Linux-x86_64/wwe2k14-launcher", ToBytes("new-launcher")});
  entries.push_back({"WWE2K14-Recomp-x64/Linux-x86_64/wwe2k14.toml", ToBytes("new-toml")});
  entries.push_back({"WWE2K14-Recomp-x64/Linux-x86_64/shader-cache/seed.bin", ToBytes("seed")});
  entries.push_back({"WWE2K14-Recomp-x64/Linux-x86_64/shader-cache/player.bin", ToBytes("zip-player")});
  entries.push_back({"WWE2K14-Recomp-x64/Linux-x86_64/userdata/x", ToBytes("zip-userdata")});
  entries.push_back({"WWE2K14-Recomp-x64/wwe2k14.exe", ToBytes("new-game")});
  entries.push_back({"WWE2K14-Recomp-x64/WWE 2K14 Launcher.exe", ToBytes("new-launcher")});
  entries.push_back({"WWE2K14-Recomp-x64/wwe2k14.toml", ToBytes("new-toml")});
  entries.push_back({"WWE2K14-Recomp-x64/shader-cache/seed.bin", ToBytes("seed")});
  entries.push_back({"WWE2K14-Recomp-x64/shader-cache/player.bin", ToBytes("zip-player")});
  entries.push_back({"WWE2K14-Recomp-x64/userdata/x", ToBytes("zip-userdata")});
  entries.push_back({"WWE2K14-Recomp-x64/README.md", ToBytes("new-readme")});
  entries.push_back({"WWE2K14-Recomp-x64/WWE 2K14/PUT-GAME-FILES-HERE.txt", ToBytes("marker")});
  assert(internal::CreateZip(zip, entries));
}

// Layout helpers: on Windows the program files are the package root; on Linux they are Linux-x86_64/,
// with the shared top-level documents one folder above the launcher.
#ifdef _WIN32
constexpr const char* kGameName = "wwe2k14.exe";
constexpr const char* kLauncherName = "WWE 2K14 Launcher.exe";
constexpr const char* kGameBackup = "wwe2k14.exe";
constexpr const char* kLauncherBackup = "WWE 2K14 Launcher.exe";
constexpr const char* kReadmeBackup = "README.md";
#else
constexpr const char* kGameName = "wwe2k14";
constexpr const char* kLauncherName = "wwe2k14-launcher";
constexpr const char* kGameBackup = "wwe2k14";
constexpr const char* kLauncherBackup = "wwe2k14-launcher";
constexpr const char* kReadmeBackup = "__root__/README.md";
#endif

Paths MakeUpdatePaths(const fs::path& exe_dir) {
  Paths paths = ResolvePaths(exe_dir / "wwe2k14-launcher");
  paths.exe_dir = exe_dir;
#ifdef _WIN32
  paths.game_exe = exe_dir / "wwe2k14.exe";
#else
  paths.game_exe = exe_dir / "wwe2k14";
#endif
  paths.settings_file = exe_dir / "wwe2k14-enhanced.ini";
  paths.userdata_dir = exe_dir / "userdata";
  paths.music_dir = exe_dir / "music";
  paths.logs_dir = exe_dir / "logs";
  paths.backups_dir = exe_dir / "backups";
  paths.default_game_folder = exe_dir / "WWE 2K14";
  return paths;
}

void TestApply(const fs::path& root) {
  const fs::path directory = root / "apply";
#ifdef _WIN32
  const fs::path exe_dir = directory;
  const fs::path root_docs = directory;
#else
  const fs::path exe_dir = directory / "Linux-x86_64";
  const fs::path root_docs = directory;
#endif
  std::error_code error;
  fs::create_directories(exe_dir, error);
  assert(!error);
  const Paths paths = MakeUpdatePaths(exe_dir);

  WriteAll(exe_dir / kGameName, "old-game");
  WriteAll(exe_dir / kLauncherName, "old-launcher");
  WriteAll(exe_dir / "wwe2k14.toml", "edited-toml");
  WriteAll(exe_dir / "wwe2k14-enhanced.ini", "player-ini");
  WriteAll(exe_dir / "userdata" / "x", "player-data");
  WriteAll(exe_dir / "music" / "x", "player-music");
  WriteAll(exe_dir / "logs" / "x", "player-logs");
  WriteAll(exe_dir / "backups" / "x", "player-backups");
  WriteAll(exe_dir / "WWE 2K14" / "x", "player-game");
  WriteAll(exe_dir / "shader-cache" / "player.bin", "player-shader");
  WriteAll(root_docs / "README.md", "old-readme");

  const fs::path zip = directory / "WWE2K14-Recomp-v1.1.0.zip";
  BuildReleaseZip(zip);
  UpdateInfo info;
  info.name = "WWE2K14-Recomp-v1.1.0.zip";
  info.version = "1.4.0";
  CancelFlag cancel{false};
  const Result result = ApplyUpdate(paths, zip, info, "1.3.0", ProgressFn(), cancel);
  assert(result.ok);

  // Program files replaced.
  assert(ReadAll(exe_dir / kGameName) == "new-game");
  assert(ReadAll(exe_dir / kLauncherName) == "new-launcher");
  assert(ReadAll(root_docs / "README.md") == "new-readme");
  // Missing seed files added; existing ones kept.
  assert(ReadAll(exe_dir / "shader-cache" / "seed.bin") == "seed");
  assert(ReadAll(exe_dir / "shader-cache" / "player.bin") == "player-shader");
  // The player's own controls file, settings and every user folder are untouched.
  assert(ReadAll(exe_dir / "wwe2k14.toml") == "edited-toml");
  assert(ReadAll(exe_dir / "wwe2k14-enhanced.ini") == "player-ini");
  assert(ReadAll(exe_dir / "userdata" / "x") == "player-data");
  assert(ReadAll(exe_dir / "music" / "x") == "player-music");
  assert(ReadAll(exe_dir / "logs" / "x") == "player-logs");
  assert(ReadAll(exe_dir / "backups" / "x") == "player-backups");
  assert(ReadAll(exe_dir / "WWE 2K14" / "x") == "player-game");
  // Previous program files backed up.
  assert(ReadAll(exe_dir / "backups" / "update-1.3.0" / kGameBackup) == "old-game");
  assert(ReadAll(exe_dir / "backups" / "update-1.3.0" / kLauncherBackup) == "old-launcher");
  assert(ReadAll(exe_dir / "backups" / "update-1.3.0" / "wwe2k14.toml") == "edited-toml");
  assert(ReadAll(exe_dir / "backups" / "update-1.3.0" / kReadmeBackup) == "old-readme");
  assert(!fs::exists(exe_dir / "backups" / "update-staging", error));
  std::cout << "PASS apply: program files replaced, seed/never-touch files kept, backup written\n";
}

void TestRollback(const fs::path& root) {
  const fs::path directory = root / "rollback";
#ifdef _WIN32
  const fs::path exe_dir = directory;
#else
  const fs::path exe_dir = directory / "Linux-x86_64";
#endif
  std::error_code error;
  fs::create_directories(exe_dir, error);
  assert(!error);
  const Paths paths = MakeUpdatePaths(exe_dir);

  WriteAll(exe_dir / kLauncherName, "old-launcher");
  // A directory where the game binary should go makes the install fail after the launcher was replaced.
  fs::create_directories(exe_dir / kGameName, error);
  assert(!error);

  const fs::path zip = directory / "broken.zip";
  std::vector<internal::ZipEntry> entries;
  entries.push_back({"WWE2K14-Recomp-x64/Linux-x86_64/wwe2k14-launcher", ToBytes("new-launcher")});
  entries.push_back({"WWE2K14-Recomp-x64/Linux-x86_64/wwe2k14", ToBytes("new-game")});
  entries.push_back({"WWE2K14-Recomp-x64/WWE 2K14 Launcher.exe", ToBytes("new-launcher")});
  entries.push_back({"WWE2K14-Recomp-x64/wwe2k14.exe", ToBytes("new-game")});
  assert(internal::CreateZip(zip, entries));

  UpdateInfo info;
  info.name = "broken.zip";
  CancelFlag cancel{false};
  const Result result = ApplyUpdate(paths, zip, info, "1.3.0", ProgressFn(), cancel);
  assert(!result.ok);
  assert(result.error.find("restored") != std::string::npos);
  // The launcher that was replaced before the failure is back to its old content.
  assert(ReadAll(exe_dir / kLauncherName) == "old-launcher");
  assert(fs::is_directory(exe_dir / kGameName, error));
  std::cout << "PASS rollback: a failed install restores the previous program files\n";
}

// The two updater settings survive a save/load round trip, and "check for updates" defaults to on.
void TestUpdateSettings(const fs::path& root) {
  const fs::path directory = root / "settings";
  fs::create_directories(directory);
  const Paths paths = MakeUpdatePaths(directory);
  Settings settings = LoadSettings(paths);
  assert(settings.check_updates);
  assert(settings.skipped_update_version.empty());
  settings.check_updates = false;
  settings.skipped_update_version = "1.4.0";
  assert(SaveSettings(paths, settings).ok);
  const Settings loaded = LoadSettings(paths);
  assert(!loaded.check_updates);
  assert(loaded.skipped_update_version == "1.4.0");
  std::cout << "PASS update settings (check_updates default, skip version round trip)\n";
}

}  // namespace

// The 2K14 game has no single-instance lock, so the updater finds a running game by its executable path.
void TestGameProcessRunning(const fs::path& root) {
#ifndef _WIN32
  const fs::path exe_dir = root / "running" / "Linux-x86_64";
  std::error_code error;
  fs::create_directories(exe_dir, error);
  assert(!error);
  Paths paths;
  paths.exe_dir = exe_dir;
  paths.game_exe = exe_dir / "wwe2k14";
  assert(!GameProcessRunning(paths));
  // Stand in for the game with a copy of /bin/sleep named like it.
  fs::copy_file("/bin/sleep", paths.game_exe, fs::copy_options::overwrite_existing, error);
  assert(!error);
  fs::permissions(paths.game_exe, fs::perms::owner_all, fs::perm_options::replace, error);
  std::string path = paths.game_exe.string();
  std::string seconds = "30";
  char* argv[] = {path.data(), seconds.data(), nullptr};
  pid_t child = 0;
  assert(posix_spawn(&child, path.c_str(), nullptr, nullptr, argv, environ) == 0);
  bool seen = false;
  for (int attempt = 0; attempt < 50 && !seen; ++attempt) {
    seen = GameProcessRunning(paths);
    if (!seen) usleep(20000);
  }
  // Another install's game (same file name, different folder) does not count.
  Paths other = paths;
  other.game_exe = root / "elsewhere" / "wwe2k14";
  const bool other_seen = GameProcessRunning(other);
  ::kill(child, SIGKILL);
  ::waitpid(child, nullptr, 0);
  assert(seen);
  assert(!other_seen);
  assert(!GameProcessRunning(paths));
  std::cout << "PASS game process check (running, stopped, another install)\n";
#else
  (void)root;
#endif
}

int main() {
  const fs::path output = fs::path(WWE2K14_LAUNCHER_TESTDATA).parent_path() / "update";
  std::error_code error;
  fs::remove_all(output, error);
  error.clear();
  fs::create_directories(output, error);
  assert(!error);
  TestVersionCompare();
  TestJsonParse();
  TestNeverTouch();
  TestUpdateSettings(output);
  TestApply(output);
  TestRollback(output);
  TestGameProcessRunning(output);
  std::cout << "All update tests passed.\n";
  return 0;
}
