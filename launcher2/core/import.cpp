#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace wwe2k14::launcher {
namespace {

constexpr std::string_view kSaveAccount = "b13ebabebabebabe";
constexpr std::string_view kDlcAccount = "0000000000000000";
constexpr std::string_view kCacheDirectory = "cache";
constexpr size_t kCopyChunk = 1024 * 1024;

struct FileItem {
  fs::path path;
  fs::path relative;
  uint64_t bytes = 0;
};

struct IniChoices {
  std::map<std::string, std::string> values;
};

std::string Lower(std::string value) {
  return internal::LowerAscii(value);
}

std::string PathText(const fs::path& path) {
  return internal::PathToUtf8(path);
}

// Windows: the extended-length form (\\?\C:\... or \\?\UNC\server\share\...) lifts the 260-character MAX_PATH
// limit. Deep DLC files (~114 characters below userdata) inside the temporary import folder of a nested install folder
// exceeded it (owner 2026-10-28: "A folder could not be created" in ...\WWE2K14-Recomp-v1.4.2-TEST\WWE2K14-Recomp-x64).
fs::path LongPath(const fs::path& path) {
#if defined(_WIN32)
  if (path.empty()) return path;
  std::error_code error;
  fs::path absolute = fs::absolute(path, error);
  if (error) return path;
  const std::wstring text = absolute.lexically_normal().make_preferred().wstring();
  if (text.rfind(L"\\\\?\\", 0) == 0) return fs::path(text);
  if (text.rfind(L"\\\\", 0) == 0) return fs::path(L"\\\\?\\UNC\\" + text.substr(2));
  return fs::path(L"\\\\?\\" + text);
#else
  return path;
#endif
}

// The plain form of a LongPath() result, for paths shown to the player.
fs::path ShortPath(const fs::path& path) {
#if defined(_WIN32)
  const std::wstring text = path.wstring();
  if (text.rfind(L"\\\\?\\UNC\\", 0) == 0) return fs::path(L"\\\\" + text.substr(8));
  if (text.rfind(L"\\\\?\\", 0) == 0) return fs::path(text.substr(4));
#endif
  return path;
}

Paths LongPaths(const Paths& paths) {
  Paths result = paths;
  for (fs::path* path : {&result.exe_dir, &result.game_exe, &result.settings_file, &result.userdata_dir,
                         &result.music_dir, &result.logs_dir, &result.backups_dir, &result.default_game_folder}) {
    *path = LongPath(*path);
  }
  return result;
}

bool IsSafeRelative(const fs::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
  for (const auto& component : path) {
    if (component == fs::path(".") || component == fs::path("..")) return false;
  }
  return true;
}

fs::path Canonical(const fs::path& path) {
  std::error_code error;
  fs::path result = fs::weakly_canonical(path, error);
  if (error) {
    error.clear();
    result = fs::absolute(path, error);
    if (error) result = path;
  }
  return result.lexically_normal();
}

bool ComponentEqual(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
  return Lower(PathText(left)) == Lower(PathText(right));
#else
  return left == right;
#endif
}

std::string PathKey(const fs::path& path) {
  const std::string text = PathText(path);
#ifdef _WIN32
  return Lower(text);
#else
  return text;
#endif
}

bool IsWithin(const fs::path& path, const fs::path& root) {
  auto path_it = path.begin();
  for (auto root_it = root.begin(); root_it != root.end(); ++root_it, ++path_it) {
    if (path_it == path.end() || !ComponentEqual(*path_it, *root_it)) return false;
  }
  return true;
}

bool IsDirectory(const fs::path& path) {
  std::error_code error;
  return fs::is_directory(path, error) && !error;
}

bool IsRegularFile(const fs::path& path) {
  std::error_code error;
  return fs::is_regular_file(path, error) && !error;
}

fs::path ControlsFile(const Paths& paths) {
  const fs::path game_directory = paths.game_exe.empty() ? paths.exe_dir : paths.game_exe.parent_path();
  return game_directory / "wwe2k14.toml";
}

fs::path SourceControlsFile(const fs::path& root) {
  const fs::path root_file = root / "wwe2k14.toml";
  if (IsRegularFile(root_file)) return root_file;
  const fs::path bin_file = root / "bin" / "wwe2k14.toml";
  if (IsRegularFile(bin_file)) return bin_file;
  return {};
}

bool HasInstallationMarkers(const fs::path& root) {
  return IsDirectory(root / "userdata") || IsDirectory(root / "music") ||
         IsRegularFile(root / "wwe2k14-enhanced.ini") || !SourceControlsFile(root).empty();
}

bool HasSaveOrDlcData(const fs::path& root) {
  for (const char* account : {"B13EBABEBABEBABE", "0000000000000000"}) {
    std::error_code error;
    fs::recursive_directory_iterator it(root / "userdata" / account, fs::directory_options::skip_permission_denied, error);
    for (const fs::recursive_directory_iterator end; !error && it != end; it.increment(error)) {
      if (it->is_regular_file(error)) return true;
    }
  }
  return false;
}

// A release folder holds the Windows installation at its root and the Linux one in Linux-x86_64/ (each with its own
// userdata/; a fresh one only has the shipped prewarm list). Prefer the candidate that actually has saves or DLC,
// in this platform's order, then any candidate with installation files.
fs::path ResolveSourceRoot(const fs::path& selected) {
  std::vector<fs::path> order;
#ifdef _WIN32
  order = {selected, selected / "WWE2K14-Recomp-x64", selected / "Linux-x86_64"};
#else
  order = {selected / "Linux-x86_64", selected, selected / "WWE2K14-Recomp-x64"};
#endif
  for (const fs::path& candidate : order) {
    if (HasSaveOrDlcData(candidate)) return candidate;
  }
  for (const fs::path& candidate : order) {
    if (HasInstallationMarkers(candidate)) return candidate;
  }
  return selected;
}

fs::path ResolveSelectedSourceRoot(const fs::path& selected) {
  for (fs::path ancestor = selected; !ancestor.empty();) {
    if (Lower(PathText(ancestor.filename())) == "userdata" && IsDirectory(ancestor)) {
      return ResolveSourceRoot(ancestor.parent_path());
    }
    const fs::path parent = ancestor.parent_path();
    if (parent == ancestor) break;
    ancestor = parent;
  }
  return ResolveSourceRoot(selected);
}

bool HasNamedRegularFile(const fs::path& folder, std::string_view name) {
  std::error_code error;
  fs::directory_iterator iterator(folder, fs::directory_options::skip_permission_denied, error);
  if (error) return false;
  for (const auto& entry : iterator) {
    error.clear();
    if (Lower(PathText(entry.path().filename())) != name) continue;
    if (entry.is_regular_file(error) && !error) return true;
  }
  return false;
}

bool HasInstallationExecutable(const fs::path& root) {
  for (const std::string_view name : {"wwe 2k14 launcher.exe", "wwe2k14-launcher", "wwe2k14.exe",
                                      "wwe2k14"}) {
    if (HasNamedRegularFile(root, name)) return true;
  }
  const fs::path bin = root / "bin";
  return HasNamedRegularFile(bin, "wwe2k14.exe") || HasNamedRegularFile(bin, "wwe2k14");
}

fs::path FindNamedDirectory(const fs::path& parent, std::string_view name) {
  std::error_code error;
  fs::directory_iterator iterator(parent, fs::directory_options::skip_permission_denied, error);
  if (error) return {};
  for (const auto& entry : iterator) {
    if (Lower(PathText(entry.path().filename())) != name) continue;
    error.clear();
    if (entry.is_directory(error) && !error) return entry.path();
  }
  return {};
}

bool SkipInstallationSearchDirectory(const fs::path& directory) {
  const std::string name = Lower(PathText(directory.filename()));
  static const std::set<std::string> skipped = {
      ".git", ".hg", ".svn", ".cache", "node_modules", "appdata", "windows", "windowsapps",
      "program files", "program files (x86)", "programdata", "system volume information",
      "$recycle.bin", "recovery", "lost+found", "proc", "sys", "dev", "run"};
  return skipped.contains(name);
}

bool HasRegularFileBelow(const fs::path& root) {
  std::error_code error;
  fs::recursive_directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error);
  if (error) return false;
  const fs::recursive_directory_iterator end;
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      error.clear();
      continue;
    }
    const fs::file_status status = iterator->symlink_status(error);
    if (error) {
      error.clear();
      continue;
    }
    if (fs::is_symlink(status)) {
      iterator.disable_recursion_pending();
    } else if (fs::is_regular_file(status)) {
      return true;
    }
  }
  return false;
}

uint64_t CountDlcPacks(const fs::path& root, uint64_t dlc_files) {
  fs::path pack_root = FindNamedDirectory(root / "userdata", kDlcAccount);
  pack_root = FindNamedDirectory(pack_root, "545408b2");
  pack_root = FindNamedDirectory(pack_root, "00000002");
  std::error_code error;
  fs::directory_iterator iterator(pack_root, fs::directory_options::skip_permission_denied, error);
  uint64_t count = 0;
  if (!error) {
    for (const auto& entry : iterator) {
      error.clear();
      if (entry.is_directory(error) && !error && HasRegularFileBelow(entry.path())) ++count;
    }
  }
  return count ? count : (dlc_files ? 1 : 0);
}

std::pair<int64_t, std::string> NewestSave(const fs::path& root) {
  fs::path saves = FindNamedDirectory(root / "userdata", kSaveAccount);
  saves = FindNamedDirectory(saves, "545408b2");
  std::error_code error;
  fs::recursive_directory_iterator iterator(saves, fs::directory_options::skip_permission_denied, error);
  if (error) return {};
  const fs::recursive_directory_iterator end;
  int64_t newest = 0;
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      error.clear();
      continue;
    }
    const fs::file_status status = iterator->symlink_status(error);
    if (error) {
      error.clear();
      continue;
    }
    if (fs::is_symlink(status)) {
      iterator.disable_recursion_pending();
      continue;
    }
    if (!fs::is_regular_file(status)) continue;
    const fs::file_time_type modified = fs::last_write_time(iterator->path(), error);
    if (error) {
      error.clear();
      continue;
    }
    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        modified - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const int64_t seconds = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count());
    newest = std::max(newest, seconds);
  }
  if (!newest) return {};
  const std::time_t timestamp = static_cast<std::time_t>(newest);
  std::tm local{};
#ifdef _WIN32
  if (localtime_s(&local, &timestamp) != 0) return {newest, {}};
#else
  if (!localtime_r(&timestamp, &local)) return {newest, {}};
#endif
  std::ostringstream output;
  output << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
  return {newest, output.str()};
}

bool IsCachePath(const fs::path& relative) {
  auto component = relative.begin();
  return component != relative.end() && Lower(PathText(*component)) == kCacheDirectory;
}

Result EnumerateFiles(const fs::path& root, bool skip_userdata_cache,
                      std::vector<FileItem>* files) {
  files->clear();
  if (!IsDirectory(root)) return Result::Ok();
  std::error_code error;
  fs::recursive_directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error);
  if (error) return Result::Fail("A folder in the selected installation could not be read.");
  const fs::recursive_directory_iterator end;
  for (; iterator != end; iterator.increment(error)) {
    if (error) return Result::Fail("A folder in the selected installation could not be read.");
    const fs::path path = iterator->path();
    const fs::file_status status = iterator->symlink_status(error);
    if (error) return Result::Fail("A file in the selected installation could not be checked.");
    if (fs::is_symlink(status)) {
      iterator.disable_recursion_pending();
      continue;
    }
    const fs::path relative = path.lexically_relative(root);
    if (!IsSafeRelative(relative)) return Result::Fail("The selected installation contains an invalid file path.");
    if (skip_userdata_cache && IsCachePath(relative)) {
      if (fs::is_directory(status)) iterator.disable_recursion_pending();
      continue;
    }
    if (!fs::is_regular_file(status)) continue;
    const uint64_t size = fs::file_size(path, error);
    if (error) return Result::Fail("A file in the selected installation could not be read.");
    files->push_back({path, relative, size});
  }
  return Result::Ok();
}

bool FirstComponentIs(const fs::path& relative, std::string_view expected) {
  const auto first = relative.begin();
  return first != relative.end() && Lower(PathText(*first)) == expected;
}

void AddGroup(uint64_t bytes, uint64_t* count, uint64_t* total) {
  ++*count;
  *total += bytes;
}

bool DestinationHasContent(const Paths& paths) {
  std::vector<FileItem> userdata;
  std::vector<FileItem> music;
  if (!EnumerateFiles(paths.userdata_dir, true, &userdata).ok ||
      !EnumerateFiles(paths.music_dir, false, &music).ok) return true;
  return !userdata.empty() || !music.empty();
}

Result GatherSource(const fs::path& root, InstallationImportInfo* info,
                    std::vector<FileItem>* userdata, std::vector<FileItem>* music) {
  info->source_root = root;
  info->has_userdata = IsDirectory(root / "userdata");
  info->has_music = IsDirectory(root / "music");
  info->has_ini = IsRegularFile(root / "wwe2k14-enhanced.ini");
  const fs::path controls_file = SourceControlsFile(root);
  info->has_keyboard_settings = !controls_file.empty();

  Result result = EnumerateFiles(root / "userdata", true, userdata);
  if (!result.ok) return result;
  result = EnumerateFiles(root / "music", false, music);
  if (!result.ok) return result;
  for (const FileItem& file : *userdata) {
    if (FirstComponentIs(file.relative, kSaveAccount)) {
      AddGroup(file.bytes, &info->save_files, &info->save_bytes);
    } else if (FirstComponentIs(file.relative, kDlcAccount)) {
      AddGroup(file.bytes, &info->dlc_files, &info->dlc_bytes);
    } else {
      AddGroup(file.bytes, &info->other_userdata_files, &info->other_userdata_bytes);
    }
  }
  info->music_files = music->size();
  for (const FileItem& file : *music) info->music_bytes += file.bytes;
  info->total_files = userdata->size() + music->size();
  info->total_bytes = info->save_bytes + info->dlc_bytes + info->other_userdata_bytes + info->music_bytes;
  if (info->has_ini) {
    std::error_code error;
    const uint64_t bytes = fs::file_size(root / "wwe2k14-enhanced.ini", error);
    if (error) return Result::Fail("The selected installation settings could not be read.");
    ++info->total_files;
    info->total_bytes += bytes;
  }
  if (info->has_keyboard_settings) {
    std::error_code error;
    const uint64_t bytes = fs::file_size(controls_file, error);
    if (error) return Result::Fail("The selected keyboard settings could not be read.");
    ++info->total_files;
    info->total_bytes += bytes;
  }
  return Result::Ok();
}

std::string UniqueSuffix() {
  return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

fs::path UniqueChild(const fs::path& parent, const std::string& prefix) {
  std::error_code error;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    const fs::path candidate = parent / (prefix + UniqueSuffix() + "-" + std::to_string(attempt));
    if (!fs::exists(candidate, error) && !error) return candidate;
    error.clear();
  }
  return {};
}

bool CopyFile(const fs::path& source, const fs::path& destination, ProgressFn progress,
              CancelFlag* cancel, uint64_t* completed, uint64_t total,
              const std::string& label, std::string* message) {
  std::ifstream input(source, std::ios::binary);
  if (!input) {
    *message = "A file in the selected installation could not be opened.";
    return false;
  }
  std::error_code error;
  fs::create_directories(destination.parent_path(), error);
  if (error) {
    *message = "A folder could not be created while importing the installation (" +
               PathText(ShortPath(destination.parent_path())) + ": " + error.message() + ").";
    return false;
  }
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output) {
    *message = "A file could not be created while importing the installation.";
    return false;
  }
  std::vector<char> buffer(kCopyChunk);  // heap: Windows threads have a 1 MB stack
  while (input) {
    if (cancel && cancel->load()) {
      *message = "The import was canceled.";
      output.close();
      fs::remove(destination, error);
      return false;
    }
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      output.write(buffer.data(), count);
      if (!output) {
        *message = "There is not enough free space to import this installation.";
        output.close();
        fs::remove(destination, error);
        return false;
      }
      *completed += static_cast<uint64_t>(count);
      if (progress) progress(*completed, total, label);
    }
  }
  if (!input.eof()) {
    *message = "A file in the selected installation could not be read completely.";
    output.close();
    fs::remove(destination, error);
    return false;
  }
  output.close();
  if (!output) {
    *message = "A file could not be written while importing the installation.";
    fs::remove(destination, error);
    return false;
  }
  return true;
}

bool CopyTree(const fs::path& root, const std::vector<FileItem>& files, const fs::path& destination,
              ProgressFn progress, CancelFlag* cancel, uint64_t* completed, uint64_t total,
              const std::string& label, std::string* message) {
  std::error_code error;
  fs::create_directories(destination, error);
  if (error) {
    *message = "A folder could not be created while importing the installation (" + PathText(ShortPath(destination)) +
               ": " + error.message() + ").";
    return false;
  }
  for (const FileItem& file : files) {
    if (cancel && cancel->load()) {
      *message = "The import was canceled.";
      return false;
    }
    if (!IsSafeRelative(file.relative)) {
      *message = "The selected installation contains an invalid file path.";
      return false;
    }
    const std::string item_label = label + PathText(file.relative);
    if (!CopyFile(file.path, destination / file.relative, progress, cancel, completed, total,
                  item_label, message)) return false;
  }
  (void)root;
  return true;
}

bool ParseIni(const fs::path& path, IniChoices* choices) {
  std::string text;
  if (!internal::ReadTextFile(path, text)) return false;
  std::string section;
  size_t offset = 0;
  while (offset <= text.size()) {
    const size_t end = text.find('\n', offset);
    std::string line = internal::Trim(std::string_view(text).substr(
        offset, end == std::string::npos ? text.size() - offset : end - offset));
    if (!line.empty() && line.front() == '[' && line.back() == ']') {
      section = Lower(internal::Trim(std::string_view(line).substr(1, line.size() - 2)));
    } else if (section == "settings" && !line.empty() && line.front() != ';' && line.front() != '#') {
      const size_t equals = line.find('=');
      if (equals != std::string::npos) {
        const std::string key = Lower(internal::Trim(std::string_view(line).substr(0, equals)));
        const std::string value = internal::Trim(std::string_view(line).substr(equals + 1));
        choices->values[key] = value;
      }
    }
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return true;
}

bool IsStableGpuId(std::string_view value) {
  const size_t first = value.find(':');
  if (first < 4 || first > 8) return false;
  const size_t second = value.find(':', first + 1);
  const size_t device_end = second == std::string_view::npos ? value.size() : second;
  if (device_end - first - 1 < 4 || device_end - first - 1 > 8) return false;
  if (second != std::string_view::npos && second + 1 == value.size()) return false;
  for (size_t index = 0; index < value.size(); ++index) {
    if (index == first || index == second) continue;
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch)) return false;
  }
  return true;
}

std::string NormalizeGpu(std::string value) {
  for (char& ch : value) {
    if (ch == ':') break;
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  const size_t first = value.find(':');
  const size_t second = value.find(':', first + 1);
  const size_t end = second == std::string::npos ? value.size() : second;
  for (size_t index = first + 1; index < end; ++index) {
    value[index] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[index])));
  }
  return value;
}

bool ParseBool(std::string_view value, bool* result) {
  const std::string normalized = Lower(std::string(value));
  if (normalized == "1" || normalized == "true") {
    *result = true;
    return true;
  }
  if (normalized == "0" || normalized == "false") {
    *result = false;
    return true;
  }
  return false;
}

bool HasDestinationImportData(const Paths& paths) {
  std::vector<FileItem> userdata;
  std::vector<FileItem> music;
  if (!EnumerateFiles(paths.userdata_dir, true, &userdata).ok ||
      !EnumerateFiles(paths.music_dir, false, &music).ok) return true;
  return !userdata.empty() || !music.empty();
}

Result MakeDestinationBackup(const Paths& paths, ProgressFn progress, uint64_t* completed,
                             uint64_t total, const CancelFlag& cancel, fs::path* created) {
  if (cancel.load()) return Result::Fail("The import was canceled.");
  BackupInfo save_backup;
  const Result standard_backup = BackupSaves(paths, false, &save_backup);
  if (!standard_backup.ok && standard_backup.error != "No saved game data was found to back up.") {
    return standard_backup;
  }

  if (!internal::EnsureDirectory(paths.backups_dir)) {
    return Result::Fail("The launcher could not create the backup folder.");
  }
  const fs::path partial = UniqueChild(paths.backups_dir, ".wwe2k14-import-backup-");
  const fs::path final = UniqueChild(paths.backups_dir, "wwe2k14-import-backup-");
  if (partial.empty() || final.empty()) return Result::Fail("The launcher could not prepare a backup folder.");
  std::error_code error;
  if (!fs::create_directory(partial, error) || error) {
    return Result::Fail("The launcher could not prepare a backup folder.");
  }
  std::string message;
  std::vector<FileItem> userdata, music;
  Result result = EnumerateFiles(paths.userdata_dir, true, &userdata);
  if (result.ok) result = EnumerateFiles(paths.music_dir, false, &music);
  if (result.ok && !CopyTree(paths.userdata_dir, userdata, partial / "userdata", progress,
                             const_cast<CancelFlag*>(&cancel), completed, total,
                             "Backing up current userdata/", &message)) {
    result = Result::Fail(message);
  }
  if (result.ok && !CopyTree(paths.music_dir, music, partial / "music", progress,
                             const_cast<CancelFlag*>(&cancel), completed, total,
                             "Backing up current music/", &message)) {
    result = Result::Fail(message);
  }
  for (const auto& [source, name] : std::array<std::pair<fs::path, const char*>, 2>{
           std::pair{paths.settings_file, "wwe2k14-enhanced.ini"},
           std::pair{ControlsFile(paths), "wwe2k14.toml"}}) {
    if (!result.ok || !IsRegularFile(source)) continue;
    if (!CopyFile(source, partial / name, progress, const_cast<CancelFlag*>(&cancel), completed,
                  total, std::string("Backing up ") + name, &message)) {
      result = Result::Fail(message);
    }
  }
  if (!result.ok) {
    fs::remove_all(partial, error);
    return result;
  }
  fs::rename(partial, final, error);
  if (error) {
    fs::remove_all(partial, error);
    return Result::Fail("The launcher could not finish the backup.");
  }
  *created = final;
  return Result::Ok();
}

struct DirectorySwap {
  fs::path target;
  fs::path staged;
  fs::path old;
  bool preserve_cache = false;
  bool had_old = false;
  bool cache_moved = false;
  bool installed = false;
};

Result CommitDirectory(DirectorySwap* swap) {
  std::error_code error;
  if (fs::exists(swap->target, error) && !error) {
    swap->old = UniqueChild(swap->target.parent_path(), ".wwe2k14-import-old-");
    if (swap->old.empty()) return Result::Fail("The launcher could not prepare to replace imported files.");
    fs::rename(swap->target, swap->old, error);
    if (error) return Result::Fail("The launcher could not replace the current folder.");
    swap->had_old = true;
  } else if (error) {
    return Result::Fail("The current folder could not be checked before import.");
  }
  if (swap->preserve_cache && swap->had_old) {
    const fs::path old_cache = swap->old / "cache";
    error.clear();
    if (fs::exists(old_cache, error) && !error) {
      fs::create_directories(swap->staged, error);
      if (!error) fs::rename(old_cache, swap->staged / "cache", error);
      if (error) return Result::Fail("The new installation's cache could not be preserved.");
      swap->cache_moved = true;
    } else if (error) {
      return Result::Fail("The new installation's cache could not be checked.");
    }
  }
  error.clear();
  fs::rename(swap->staged, swap->target, error);
  if (error) return Result::Fail("The launcher could not put the imported files into place.");
  swap->installed = true;
  return Result::Ok();
}

void RollbackDirectory(DirectorySwap* swap) {
  std::error_code error;
  if (swap->cache_moved && swap->had_old) {
    error.clear();
    fs::create_directories(swap->old, error);
    if (!error) {
      const fs::path cache_source = swap->installed ? swap->target / "cache" : swap->staged / "cache";
      fs::rename(cache_source, swap->old / "cache", error);
    }
  }
  if (swap->installed) {
    error.clear();
    fs::remove_all(swap->target, error);
  }
  if (swap->had_old) {
    error.clear();
    fs::rename(swap->old, swap->target, error);
  }
}

void FinalizeDirectory(DirectorySwap* swap) {
  if (!swap->had_old) return;
  std::error_code error;
  fs::remove_all(swap->old, error);
}

struct FileSwap {
  fs::path target;
  fs::path staged;
  fs::path old;
  bool had_old = false;
  bool installed = false;
};

Result CommitFile(FileSwap* swap) {
  std::error_code error;
  if (fs::exists(swap->target, error) && !error) {
    swap->old = UniqueChild(swap->target.parent_path(), ".wwe2k14-import-old-file-");
    if (swap->old.empty()) return Result::Fail("The launcher could not prepare to replace settings.");
    fs::rename(swap->target, swap->old, error);
    if (error) return Result::Fail("The launcher could not replace the current settings file.");
    swap->had_old = true;
  } else if (error) {
    return Result::Fail("The current settings file could not be checked.");
  }
  error.clear();
  fs::create_directories(swap->target.parent_path(), error);
  if (error) return Result::Fail("A folder could not be created for the imported keyboard settings.");
  error.clear();
  fs::rename(swap->staged, swap->target, error);
  if (error) return Result::Fail("The imported keyboard settings could not be installed.");
  swap->installed = true;
  return Result::Ok();
}

void RollbackFile(FileSwap* swap) {
  std::error_code error;
  if (swap->installed) fs::remove(swap->target, error);
  if (swap->had_old) {
    error.clear();
    fs::rename(swap->old, swap->target, error);
  }
}

void FinalizeFile(FileSwap* swap) {
  if (!swap->had_old) return;
  std::error_code error;
  fs::remove(swap->old, error);
}

// GitHub #132: the WWE '13 / 2K15 importers keep their state next to the launcher, outside userdata/: own-slots.txt (the
// game gives only these ids a roster slot), mods/<game>/ (roster lists, arenas.txt) and their undo sets under backups/.
// A new folder that misses them shows imported wrestlers as "Error Roster". Carried the same stage / swap / rollback way.
const char* const kUndoSets[] = {"wwe13-import", "wwe13-arenas", "wwe15-import"};

struct ModStateCarry {
  bool slots = false;                // own-slots.txt carried (merged with the target's)
  bool mods = false;                 // mods/ gains files
  std::vector<std::string> undo_sets;  // backups/<set> folders the target lacks
  DirectorySwap mods_swap;
  FileSwap slots_swap;
  std::vector<DirectorySwap> undo_swaps;
  bool mods_committed = false;
  bool slots_committed = false;
  size_t undo_committed = 0;
  std::vector<FileItem> source_mods, target_mods;
  std::vector<std::vector<FileItem>> source_undo;
  std::set<uint32_t> merged_ids;
  uint64_t bytes = 0;
};

fs::path StagedOf(const fs::path& stage_root, const std::string& name) { return stage_root / name; }

void PlanModStateCarry(const Paths& paths, const fs::path& source_root, ModStateCarry* carry) {
  // Undo sets first: the wrestler ids follow the records (Fable 1.6.1 item 9).
  for (const char* name : kUndoSets) {
    std::vector<FileItem> files, target_files;
    // The target's own record is kept; an EMPTY leftover folder is not a record and must not block the carry (item 9).
    if (IsDirectory(paths.backups_dir / name) &&
        (!EnumerateFiles(paths.backups_dir / name, false, &target_files).ok || !target_files.empty()))
      continue;
    if (!EnumerateFiles(source_root / "backups" / name, false, &files).ok || files.empty()) continue;
    carry->undo_sets.push_back(name);
    carry->source_undo.push_back(std::move(files));
    for (const FileItem& file : carry->source_undo.back()) carry->bytes += file.bytes;
  }
  const std::set<uint32_t> source_ids = ReadOwnSlotIds(source_root / "own-slots.txt");
  // An existing but EMPTY source file is the player's choice (every import switched off on the Mods page): carry it too, or
  // the startup repair would switch every roster id back on in the new folder (Fable review 11-15).
  const bool source_empty_file = source_ids.empty() && IsRegularFile(source_root / "own-slots.txt") &&
                                 !IsRegularFile(paths.exe_dir / "own-slots.txt");
  // A target that already keeps its own wrestler list and gets no import record from the source keeps its list as it is:
  // merging would switch back on ids the player removed in the new folder (Fable 1.6.1 item 9).
  const bool target_owns_list = !ReadOwnSlotIds(paths.exe_dir / "own-slots.txt").empty() && carry->undo_sets.empty();
  if ((!source_ids.empty() || source_empty_file) && !target_owns_list) {
    carry->merged_ids = ReadOwnSlotIds(paths.exe_dir / "own-slots.txt");
    const size_t before = carry->merged_ids.size();
    carry->merged_ids.insert(source_ids.begin(), source_ids.end());
    carry->slots = carry->merged_ids.size() != before || !IsRegularFile(paths.exe_dir / "own-slots.txt");
  }
  if (EnumerateFiles(source_root / "mods", false, &carry->source_mods).ok && !carry->source_mods.empty() &&
      EnumerateFiles(paths.exe_dir / "mods", false, &carry->target_mods).ok) {
    std::set<std::string> have;
    for (const FileItem& file : carry->target_mods) have.insert(PathText(file.relative));
    std::vector<FileItem> fresh;
    for (const FileItem& file : carry->source_mods) {
      if (have.count(PathText(file.relative)) == 0) {
        fresh.push_back(file);
        carry->bytes += file.bytes;
      }
    }
    carry->source_mods = std::move(fresh);  // roster.tsv / arenas.txt / anything the target has: the target's stays
    carry->mods = !carry->source_mods.empty();
    if (!carry->mods) carry->source_mods.clear();
  } else {
    carry->source_mods.clear();
  }
}

bool StageModStateCarry(const Paths& paths, const fs::path& source_root, const fs::path& stage_root,
                        const ProgressFn& progress, CancelFlag* cancel, uint64_t* completed, uint64_t total,
                        ModStateCarry* carry, std::string* message) {
  if (carry->slots) {
    const fs::path staged = StagedOf(stage_root, "own-slots.txt");
    const std::string text = MakeOwnSlotsText(carry->merged_ids);
    // Plain bytes: the game's reader does not understand UTF-16.
    if (!internal::WriteBinaryFile(staged, text.data(), text.size())) {
      *message = "The launcher could not stage the imported wrestler slots.";
      return false;
    }
    carry->slots_swap.target = paths.exe_dir / "own-slots.txt";
    carry->slots_swap.staged = staged;
  }
  if (carry->mods) {
    const fs::path staged = StagedOf(stage_root, "mods");
    if (!CopyTree(paths.exe_dir / "mods", carry->target_mods, staged, progress, cancel, completed, total,
                  "Preparing current mods/", message) ||
        !CopyTree(source_root / "mods", carry->source_mods, staged, progress, cancel, completed, total,
                  "Importing mods/", message))
      return false;
    carry->mods_swap.target = paths.exe_dir / "mods";
    carry->mods_swap.staged = staged;
  }
  for (size_t i = 0; i < carry->undo_sets.size(); ++i) {
    const fs::path staged = StagedOf(stage_root, "undo-" + carry->undo_sets[i]);
    if (!CopyTree(source_root / "backups" / carry->undo_sets[i], carry->source_undo[i], staged, progress, cancel,
                  completed, total, "Importing import records ", message))
      return false;
    DirectorySwap swap;
    swap.target = paths.backups_dir / carry->undo_sets[i];
    swap.staged = staged;
    carry->undo_swaps.push_back(swap);
  }
  return true;
}

void RollbackModStateCarry(ModStateCarry* carry) {
  for (size_t i = carry->undo_committed; i-- > 0;) RollbackDirectory(&carry->undo_swaps[i]);
  carry->undo_committed = 0;
  if (carry->mods_committed) RollbackDirectory(&carry->mods_swap);
  if (carry->slots_committed) RollbackFile(&carry->slots_swap);
  carry->mods_committed = carry->slots_committed = false;
}

Result CommitModStateCarry(const Paths& paths, ModStateCarry* carry) {
  Result result = Result::Ok();
  if (carry->slots) {
    result = CommitFile(&carry->slots_swap);
    if (!result.ok) { RollbackFile(&carry->slots_swap); return result; }
    carry->slots_committed = true;
  }
  if (carry->mods) {
    result = CommitDirectory(&carry->mods_swap);
    if (!result.ok) { RollbackDirectory(&carry->mods_swap); RollbackModStateCarry(carry); return result; }
    carry->mods_committed = true;
  }
  std::error_code error;
  fs::create_directories(paths.backups_dir, error);
  for (DirectorySwap& swap : carry->undo_swaps) {
    result = CommitDirectory(&swap);
    if (!result.ok) { RollbackDirectory(&swap); RollbackModStateCarry(carry); return result; }
    ++carry->undo_committed;
  }
  return Result::Ok();
}

void FinalizeModStateCarry(ModStateCarry* carry) {
  if (carry->slots_committed) FinalizeFile(&carry->slots_swap);
  if (carry->mods_committed) FinalizeDirectory(&carry->mods_swap);
  for (DirectorySwap& swap : carry->undo_swaps) FinalizeDirectory(&swap);
}

bool ShouldImportUserdata(const InstallationImportInfo& info) {
  return info.has_userdata;
}

std::string IniValue(const IniChoices& choices, std::string_view key) {
  const auto it = choices.values.find(std::string(key));
  return it == choices.values.end() ? std::string{} : it->second;
}

bool ApplyImportChoices(const Paths& paths, const InstallationImportInfo& info, Settings* settings,
                        bool* changed) {
  *changed = false;
  if (!info.has_ini) return true;
  IniChoices choices;
  if (!ParseIni(info.source_root / "wwe2k14-enhanced.ini", &choices)) return false;

  const fs::path bundled_game = info.source_root / "WWE 2K14";
  if (IsRegularFile(bundled_game / "default.xex")) {
    std::error_code error;
    settings->game_folder = fs::absolute(bundled_game, error);
    if (error) settings->game_folder = bundled_game;
    *changed = true;
  } else {
    const std::string game_folder = IniValue(choices, "game_folder");
    if (!game_folder.empty()) {
      fs::path imported = internal::PathFromUtf8(game_folder);
      if (imported.is_relative()) imported = info.source_root / imported;
      std::error_code error;
      settings->game_folder = fs::absolute(imported, error);
      if (error) settings->game_folder = imported.lexically_normal();
      *changed = true;
    }
  }

  const std::string gpu = IniValue(choices, "gpu");
  if (!gpu.empty()) {
    if (Lower(gpu) == "auto") {
      settings->gpu_id.clear();
      *changed = true;
    } else if (IsStableGpuId(gpu)) {
      settings->gpu_id = NormalizeGpu(gpu);
      *changed = true;
    }
  }
  const std::string display = Lower(IniValue(choices, "display"));
  if (display == "fullscreen" || display == "windowed" || display == "borderless") {
    settings->display = display == "windowed"     ? DisplayMode::kWindowed
                        : display == "borderless" ? DisplayMode::kBorderless
                                                  : DisplayMode::kFullscreen;
    *changed = true;
  }
  std::string stretch = IniValue(choices, "stretch_to_fill_wide_screens");
  if (stretch.empty()) stretch = IniValue(choices, "stretch_to_fill");
  bool stretch_choice = false;
  if (ParseBool(stretch, &stretch_choice)) {
    settings->stretch_to_fill_wide_screens = stretch_choice;
    *changed = true;
  }
  (void)paths;
  return true;
}

}  // namespace

Result ScanInstallationImport(const Paths& paths, const fs::path& selected_folder,
                              InstallationImportInfo* info) {
  if (!info) return Result::Fail("The selected folder could not be checked.");
  try {
    std::error_code error;
    if (!fs::is_directory(selected_folder, error) || error) {
      return Result::Fail("Choose the folder of another WWE 2K14 installation.");
    }
    const fs::path root = Canonical(ResolveSelectedSourceRoot(selected_folder));
    const fs::path current = Canonical(paths.exe_dir);
    if (IsWithin(current, root) || IsWithin(root, current)) {
      return Result::Fail("Choose a different WWE 2K14 installation, not this one.");
    }
    if (!HasInstallationMarkers(root)) {
      return Result::Fail("This folder does not contain a WWE 2K14 installation to import.");
    }
    InstallationImportInfo scanned;
    std::vector<FileItem> userdata, music;
    const Result gathered = GatherSource(root, &scanned, &userdata, &music);
    if (!gathered.ok) return gathered;
    if (scanned.save_files == 0 && scanned.dlc_files == 0 && scanned.music_files == 0) {
      return Result::Fail("No saves, DLC or music were found in that installation, so there is nothing to bring over.");
    }
    scanned.backup_required = DestinationHasContent(paths);
    *info = std::move(scanned);
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The selected installation could not be checked.");
  }
}

std::vector<FoundInstallationImport> FindInstallationImports(const Paths& paths,
                                                              const ProgressFn& progress,
                                                              const CancelFlag& cancel) {
  try {
    constexpr unsigned kMaximumDepth = 5;
    constexpr auto kTimeBudget = std::chrono::seconds(20);
    const auto deadline = std::chrono::steady_clock::now() + kTimeBudget;
    const fs::path current = Canonical(paths.exe_dir);
    std::vector<FoundInstallationImport> found;
    std::set<std::string> visited;
    std::set<std::string> searched;
    uint64_t visited_count = 0;
    bool time_expired = false;

    auto inspect = [&](const fs::path& folder) {
      if (cancel.load(std::memory_order_relaxed) || !IsDirectory(folder) ||
          !IsDirectory(folder / "userdata") || !HasInstallationExecutable(folder)) return;
      const fs::path normalized = Canonical(folder);
      if (!visited.insert(PathKey(normalized)).second) return;
      if (IsWithin(current, normalized) || IsWithin(normalized, current)) return;

      InstallationImportInfo info;
      std::vector<FileItem> userdata, music;
      if (!GatherSource(normalized, &info, &userdata, &music).ok ||
          (!info.save_files && !info.dlc_files)) return;
      FoundInstallationImport candidate;
      candidate.folder = normalized;
      candidate.saves = info.save_files;
      candidate.dlc_packs = CountDlcPacks(normalized, info.dlc_files);
      candidate.bytes = info.total_bytes;
      const auto newest = NewestSave(normalized);
      candidate.newest_save_time = newest.first;
      candidate.newest_save_when = newest.second;
      found.push_back(std::move(candidate));
    };

    for (const fs::path& root : InstallationSearchRoots(paths)) {
      if (cancel.load(std::memory_order_relaxed)) break;
      if (std::chrono::steady_clock::now() >= deadline) {
        time_expired = true;
        break;
      }
      std::error_code error;
      if (!fs::is_directory(root, error) || error) continue;
      std::vector<std::pair<fs::path, unsigned>> pending{{root, 0}};
      while (!pending.empty() && !cancel.load(std::memory_order_relaxed)) {
        if (std::chrono::steady_clock::now() >= deadline) {
          time_expired = true;
          break;
        }
        auto [folder, depth] = std::move(pending.back());
        pending.pop_back();
        const fs::path normalized_folder = Canonical(folder);
        if (!searched.insert(PathKey(normalized_folder)).second) continue;
        ++visited_count;
        if (progress) progress(visited_count, 0, "Searching... " + PathText(normalized_folder));
        inspect(normalized_folder);
        if (depth >= kMaximumDepth || cancel.load(std::memory_order_relaxed)) continue;

        error.clear();
        fs::directory_iterator iterator(folder, fs::directory_options::skip_permission_denied, error);
        if (error) continue;
        for (const auto& entry : iterator) {
          if (cancel.load(std::memory_order_relaxed)) break;
          if (std::chrono::steady_clock::now() >= deadline) {
            time_expired = true;
            break;
          }
          error.clear();
          const bool symlink = entry.is_symlink(error);
          if (error || !entry.is_directory(error) || error) continue;
          if (symlink) {
            inspect(entry.path());
            continue;
          }
          if (SkipInstallationSearchDirectory(entry.path())) continue;
          pending.emplace_back(entry.path(), depth + 1);
        }
        if (time_expired) break;
      }
      if (time_expired) break;
    }

    std::stable_sort(found.begin(), found.end(), [](const FoundInstallationImport& left,
                                                     const FoundInstallationImport& right) {
      if (left.newest_save_time != right.newest_save_time) {
        return left.newest_save_time > right.newest_save_time;
      }
      return PathText(left.folder) < PathText(right.folder);
    });
    (void)time_expired;
    return found;
  } catch (...) {
    return {};
  }
}

Result ImportInstallation(const Paths& short_paths, const InstallationImportInfo& short_info,
                          const ProgressFn& progress, const CancelFlag& cancel,
                          fs::path* backup_created) {
  const Paths paths = LongPaths(short_paths);
  InstallationImportInfo info = short_info;
  info.source_root = LongPath(short_info.source_root);
  try {
    if (cancel.load()) return Result::Fail("The import was canceled.");
    InstallationImportInfo current_info;
    const Result scanned = ScanInstallationImport(paths, info.source_root, &current_info);
    if (!scanned.ok) return scanned;

    std::vector<FileItem> source_userdata, source_music;
    InstallationImportInfo resolved_info;
    Result result = GatherSource(current_info.source_root, &resolved_info, &source_userdata, &source_music);
    if (!result.ok) return result;
    current_info = std::move(resolved_info);
    std::vector<FileItem> old_userdata, old_music;
    result = EnumerateFiles(paths.userdata_dir, true, &old_userdata);
    if (result.ok) result = EnumerateFiles(paths.music_dir, false, &old_music);
    if (!result.ok) return result;

    ModStateCarry carry;
    PlanModStateCarry(paths, current_info.source_root, &carry);
    uint64_t work_total = current_info.total_bytes + carry.bytes * 2;
    auto add_bytes = [&work_total](const std::vector<FileItem>& files) {
      for (const FileItem& file : files) work_total += file.bytes;
    };
    if (ShouldImportUserdata(current_info)) add_bytes(old_userdata);
    if (current_info.has_music) add_bytes(old_music);
    const bool backup_needed = HasDestinationImportData(paths);
    if (backup_needed) {
      add_bytes(old_userdata);
      add_bytes(old_music);
      if (IsRegularFile(paths.settings_file)) {
        std::error_code error;
        work_total += fs::file_size(paths.settings_file, error);
      }
      if (IsRegularFile(ControlsFile(paths))) {
        std::error_code error;
        work_total += fs::file_size(ControlsFile(paths), error);
      }
    }
    uint64_t completed = 0;
    if (progress) progress(0, work_total, "Preparing the import");
    fs::path backup_path;
    if (backup_needed) {
      result = MakeDestinationBackup(paths, progress, &completed, work_total, cancel, &backup_path);
      if (!result.ok) return result;
    }

    const fs::path stage_root = UniqueChild(paths.exe_dir, ".wwe2k14-import-stage-");
    if (stage_root.empty()) return Result::Fail("The launcher could not prepare a temporary import folder.");
    std::error_code error;
    if (!fs::create_directory(stage_root, error) || error) {
      return Result::Fail("The launcher could not prepare a temporary import folder.");
    }
    const fs::path staged_userdata = stage_root / "userdata";
    const fs::path staged_music = stage_root / "music";
    const fs::path staged_toml = stage_root / "wwe2k14.toml";
    std::string message;
    if (ShouldImportUserdata(current_info)) {
      if (!CopyTree(paths.userdata_dir, old_userdata, staged_userdata, progress,
                    const_cast<CancelFlag*>(&cancel), &completed, work_total,
                    "Preparing current userdata/", &message) ||
          !CopyTree(current_info.source_root / "userdata", source_userdata, staged_userdata, progress,
                    const_cast<CancelFlag*>(&cancel), &completed, work_total,
                    "Importing userdata/", &message)) {
        fs::remove_all(stage_root, error);
        return Result::Fail(message);
      }
    }
    if (current_info.has_music) {
      if (!CopyTree(paths.music_dir, old_music, staged_music, progress,
                    const_cast<CancelFlag*>(&cancel), &completed, work_total,
                    "Preparing current music/", &message) ||
          !CopyTree(current_info.source_root / "music", source_music, staged_music, progress,
                    const_cast<CancelFlag*>(&cancel), &completed, work_total,
                    "Importing music/", &message)) {
        fs::remove_all(stage_root, error);
        return Result::Fail(message);
      }
    }
    FileSwap toml_swap;
    if (current_info.has_keyboard_settings) {
      if (!CopyFile(SourceControlsFile(current_info.source_root), staged_toml, progress,
                    const_cast<CancelFlag*>(&cancel), &completed, work_total,
                    "Importing keyboard settings", &message)) {
        fs::remove_all(stage_root, error);
        return Result::Fail(message);
      }
      toml_swap.target = ControlsFile(paths);
      toml_swap.staged = staged_toml;
    }
    if (!StageModStateCarry(paths, current_info.source_root, stage_root, progress,
                            const_cast<CancelFlag*>(&cancel), &completed, work_total, &carry, &message)) {
      fs::remove_all(stage_root, error);
      return Result::Fail(message);
    }
    if (cancel.load()) {
      fs::remove_all(stage_root, error);
      return Result::Fail("The import was canceled.");
    }

    DirectorySwap userdata_swap;
    DirectorySwap music_swap;
    userdata_swap.target = paths.userdata_dir;
    userdata_swap.staged = staged_userdata;
    userdata_swap.preserve_cache = true;
    music_swap.target = paths.music_dir;
    music_swap.staged = staged_music;
    bool userdata_committed = false;
    bool music_committed = false;
    if (ShouldImportUserdata(current_info)) {
      result = CommitDirectory(&userdata_swap);
      if (!result.ok) {
        RollbackDirectory(&userdata_swap);
        fs::remove_all(stage_root, error);
        return result;
      }
      userdata_committed = true;
    }
    if (current_info.has_music) {
      result = CommitDirectory(&music_swap);
      if (!result.ok) {
        RollbackDirectory(&music_swap);
        if (userdata_committed) RollbackDirectory(&userdata_swap);
        fs::remove_all(stage_root, error);
        return result;
      }
      music_committed = true;
    }
    if (current_info.has_keyboard_settings) {
      result = CommitFile(&toml_swap);
      if (!result.ok) {
        RollbackFile(&toml_swap);
        if (music_committed) RollbackDirectory(&music_swap);
        if (userdata_committed) RollbackDirectory(&userdata_swap);
        fs::remove_all(stage_root, error);
        return result;
      }
    }
    result = CommitModStateCarry(paths, &carry);
    if (!result.ok) {
      RollbackFile(&toml_swap);
      if (music_committed) RollbackDirectory(&music_swap);
      if (userdata_committed) RollbackDirectory(&userdata_swap);
      fs::remove_all(stage_root, error);
      return result;
    }

    Settings imported_settings = LoadSettings(paths);
    bool settings_changed = false;
    if (!ApplyImportChoices(paths, current_info, &imported_settings, &settings_changed)) {
      RollbackModStateCarry(&carry);
      RollbackFile(&toml_swap);
      if (music_committed) RollbackDirectory(&music_swap);
      if (userdata_committed) RollbackDirectory(&userdata_swap);
      fs::remove_all(stage_root, error);
      return Result::Fail("The selected installation settings could not be read.");
    }
    if (settings_changed) {
      result = SaveSettings(paths, imported_settings);
      if (!result.ok) {
        RollbackModStateCarry(&carry);
        RollbackFile(&toml_swap);
        if (music_committed) RollbackDirectory(&music_swap);
        if (userdata_committed) RollbackDirectory(&userdata_swap);
        fs::remove_all(stage_root, error);
        return result;
      }
    }

    if (userdata_committed) FinalizeDirectory(&userdata_swap);
    if (music_committed) FinalizeDirectory(&music_swap);
    FinalizeFile(&toml_swap);
    FinalizeModStateCarry(&carry);
    fs::remove_all(stage_root, error);
    if (backup_created) *backup_created = ShortPath(backup_path);
    if (progress) progress(work_total, work_total, "Import complete");
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The installation could not be imported. The current files were left in place where possible.");
  }
}

}  // namespace wwe2k14::launcher
