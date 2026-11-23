#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

// Standalone launcher feature. No guest code, renderer state, or game assets.
namespace wwe2k14::updates {
namespace fs = std::filesystem;
inline fs::path FromUtf8(std::string_view text) {
  const auto* begin = reinterpret_cast<const char8_t*>(text.data());
  return fs::path(std::u8string(begin, begin + text.size()));
}
#ifndef WWE2K14_LAUNCHER_VERSION
#define WWE2K14_LAUNCHER_VERSION "1.0.0"
#endif
inline constexpr char kVersion[] = WWE2K14_LAUNCHER_VERSION;
inline constexpr char kRepository[] = "PromethenDev/wwe2k14-recomp";
struct Release {
  std::string version, notes, url, sha256;
  uint64_t size = 0;
  bool available = false;
};
using Progress = std::function<void(uint64_t, uint64_t)>;

// Errors throw std::runtime_error. A missing public release is a normal no-update result.
Release ParseRelease(const std::string& json, const std::string& current_version);
Release Check(const std::string& current_version, std::atomic<bool>& cancel);
bool Newer(const std::string& candidate, const std::string& installed);
bool AllowedFile(const std::string& relative);
std::string HashFile(const fs::path& path);
fs::path InstallRoot(const fs::path& launcher);

// Stages only managed program files, verifies the GitHub SHA-256 first, and never writes installed files.
fs::path Prepare(const Release& release, const fs::path& launcher,
                 std::atomic<bool>& cancel, const Progress& progress = {});
// Exposed for offline archive verification and transactional tests. The stage must be an empty, private directory.
void StageArchive(const fs::path& archive, const fs::path& stage);
// Copies this launcher into the staging directory, starts --apply-update, then returns; caller must exit.
void StartInstaller(const fs::path& stage, const fs::path& launcher);
// Handle before SDL initialization. Returns -1 for normal startup, otherwise helper exit status.
int HelperMain(int argc, char** argv);

// Transaction routines have no network or process effects. Rollback restores a previous interrupted transaction.
void Apply(const fs::path& stage, const fs::path& root);
void Rollback(const fs::path& stage, const fs::path& root);
bool GameRunning(const fs::path& root);
bool InstallInProgress(const fs::path& root);
// Serialize game process creation against the installer's running-game check.
void WithInstallLock(const fs::path& root, const std::function<void()>& action);

namespace detail {
int Get(const std::string& url, const fs::path& file, uint64_t limit,
        std::atomic<bool>& cancel, const Progress& progress = {});
}
}  // namespace wwe2k14::updates
