#include "launcher_core.h"

#include "internal/archive.h"
#include "internal/util.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <set>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace wwe2k14::launcher {
namespace {

constexpr std::string_view kAccountId = "B13EBABEBABEBABE";
constexpr std::string_view kTitleId = "545408B2";
constexpr std::string_view kSaveFile = "SaveData.Dat";

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string BackupTimestamp() {
  return internal::LocalTimestamp(std::chrono::system_clock::now(), "%Y%m%d-%H%M%S");
}

fs::path UniqueBackupPath(const Paths& paths, std::string stamp, bool automatic) {
  const std::string suffix = automatic ? "-auto" : "";
  fs::path candidate = paths.backups_dir / ("wwe2k14-saves-" + stamp + suffix + ".zip");
  unsigned number = 2;
  std::error_code error;
  while (fs::exists(candidate, error) && !error) {
    candidate = paths.backups_dir / ("wwe2k14-saves-" + stamp + "-" + std::to_string(number++) + suffix + ".zip");
    error.clear();
  }
  return candidate;
}

bool HasAnyFiles(const fs::path& directory) {
  std::error_code error;
  fs::recursive_directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, error);
  if (error) return false;
  const fs::recursive_directory_iterator end;
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      error.clear();
      continue;
    }
    if (iterator->is_symlink(error)) {
      iterator.disable_recursion_pending();
      continue;
    }
    if (!error && iterator->is_regular_file(error) && !error) return true;
    error.clear();
  }
  return false;
}

bool BuildSaveArchive(const Paths& paths, const fs::path& destination) {
  const fs::path account = paths.userdata_dir / std::string(kAccountId);
  std::vector<internal::ZipEntry> entries;
  std::error_code error;
  if (fs::is_directory(account, error) && !error) {
    fs::recursive_directory_iterator iterator(account, fs::directory_options::skip_permission_denied, error);
    if (!error) {
      const fs::recursive_directory_iterator end;
      for (; iterator != end; iterator.increment(error)) {
        if (error) {
          error.clear();
          continue;
        }
        const auto entry = *iterator;
        if (entry.is_symlink(error)) {
          iterator.disable_recursion_pending();
          error.clear();
          continue;
        }
        if (error || !entry.is_regular_file(error) || error) {
          error.clear();
          continue;
        }
        const fs::path relative = entry.path().lexically_relative(account);
        if (relative.empty() || relative.is_absolute() || relative.has_root_name()) continue;
        bool escaped = false;
        for (const auto& component : relative) if (component == "..") escaped = true;
        if (escaped) continue;
        internal::ZipEntry item;
        std::string archive_relative = internal::PathToUtf8(relative);
        std::replace(archive_relative.begin(), archive_relative.end(), '\\', '/');
        item.name = "userdata/" + std::string(kAccountId) + "/" + archive_relative;
        if (!internal::ReadBinaryFile(entry.path(), item.bytes)) return false;
        entries.push_back(std::move(item));
      }
    }
  }
  error.clear();
  if (fs::is_regular_file(paths.settings_file, error) && !error) {
    internal::ZipEntry settings;
    settings.name = "wwe2k14-enhanced.ini";
    if (!internal::ReadBinaryFile(paths.settings_file, settings.bytes)) return false;
    entries.push_back(std::move(settings));
  }
  return !entries.empty() && internal::CreateZip(destination, entries);
}

bool IsSafeRelative(const fs::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
  for (const auto& component : path) {
    if (component == ".." || component == ".") return false;
  }
  return true;
}

std::optional<std::string> NormalizeArchiveName(std::string name) {
  std::replace(name.begin(), name.end(), '\\', '/');
  while (!name.empty() && name.front() == '/') name.erase(name.begin());
  const std::string lowered = Lower(name);
  if (lowered == "wwe2k14-enhanced.ini") return std::string("wwe2k14-enhanced.ini");

  const std::string account_id = Lower(std::string(kAccountId));
  size_t account = lowered.find(account_id);
  if (account != std::string::npos) {
    name.erase(0, account + kAccountId.size());
    while (!name.empty() && name.front() == '/') name.erase(name.begin());
  } else {
    const std::string title_id = Lower(std::string(kTitleId));
    const size_t title = lowered.find(title_id);
    if (title != std::string::npos) {
      name.erase(0, title);
    } else {
      const size_t slash = name.find_last_of('/');
      const std::string basename = Lower(slash == std::string::npos ? name : name.substr(slash + 1));
      if (basename == "savedata.dat") name = std::string(kTitleId) + "/" + std::string(kSaveFile);
      else return std::nullopt;
    }
  }
  fs::path relative = internal::PathFromUtf8(name);
  if (!IsSafeRelative(relative)) return std::nullopt;
  return internal::PathToUtf8(relative);
}

bool IsAutoBackup(const fs::path& path) {
  return Lower(internal::PathToUtf8(path.filename())).ends_with("-auto.zip");
}

}  // namespace

std::vector<BackupInfo> ListBackups(const Paths& paths) {
  std::vector<BackupInfo> backups;
  std::error_code error;
  fs::directory_iterator iterator(paths.backups_dir, fs::directory_options::skip_permission_denied, error);
  if (error) return backups;
  for (const auto& entry : iterator) {
    error.clear();
    if (!entry.is_regular_file(error) || error) continue;
    const std::string filename = Lower(internal::PathToUtf8(entry.path().filename()));
    if (!filename.starts_with("wwe2k14-saves-") || !filename.ends_with(".zip")) continue;
    BackupInfo info;
    info.file = entry.path();
    info.when = internal::IsoLocalTimestamp(entry.last_write_time(error));
    if (error) info.when.clear();
    error.clear();
    info.bytes = entry.file_size(error);
    if (error) info.bytes = 0;
    info.automatic = IsAutoBackup(entry.path());
    backups.push_back(std::move(info));
  }
  std::sort(backups.begin(), backups.end(), [](const BackupInfo& left, const BackupInfo& right) {
    std::error_code left_error, right_error;
    const auto left_time = fs::last_write_time(left.file, left_error);
    const auto right_time = fs::last_write_time(right.file, right_error);
    if (!left_error && !right_error && left_time != right_time) return left_time > right_time;
    return internal::PathToUtf8(left.file.filename()) > internal::PathToUtf8(right.file.filename());
  });
  return backups;
}

Result BackupSaves(const Paths& paths, bool automatic, BackupInfo* created) {
  try {
    const fs::path account = paths.userdata_dir / std::string(kAccountId);
    if (!HasAnyFiles(account) && !fs::is_regular_file(paths.settings_file)) {
      if (automatic) return Result::Ok();
      return Result::Fail("No saved game data was found to back up.");
    }
    if (!internal::EnsureDirectory(paths.backups_dir)) {
      return Result::Fail("The launcher could not create the backup folder.");
    }
    const fs::path destination = UniqueBackupPath(paths, BackupTimestamp(), automatic);
    if (!BuildSaveArchive(paths, destination)) {
      std::error_code error;
      fs::remove(destination, error);
      return Result::Fail("The launcher could not create a save backup.");
    }
    if (created) {
      created->file = destination;
      created->when = internal::LocalTimestamp(std::chrono::system_clock::now(), "%Y-%m-%dT%H:%M:%S");
      created->bytes = fs::file_size(destination);
      created->automatic = automatic;
    }
    if (automatic) {
      std::vector<BackupInfo> automatic_backups;
      for (const auto& backup : ListBackups(paths)) if (backup.automatic) automatic_backups.push_back(backup);
      for (size_t index = 5; index < automatic_backups.size(); ++index) {
        std::error_code error;
        fs::remove(automatic_backups[index].file, error);
      }
    }
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The launcher could not create a save backup.");
  }
}

Result RestoreBackup(const Paths& paths, const fs::path& backup_zip) {
  try {
    internal::ZipReader zip;
    if (!zip.Open(backup_zip)) return Result::Fail(zip.error());
    if (!zip.ContainsSaveData()) return Result::Fail("This backup does not contain a saved game.");

    const fs::path temp_root = paths.userdata_dir / (".restore-" + BackupTimestamp());
    std::error_code error;
    fs::remove_all(temp_root, error);
    error.clear();
    if (!fs::create_directories(temp_root, error) || error) {
      return Result::Fail("The launcher could not prepare the restore folder.");
    }
    bool found_save = false;
    std::optional<std::vector<uint8_t>> ini_data;
    for (mz_uint index = 0; index < zip.Count(); ++index) {
      if (zip.IsDirectory(index)) continue;
      mz_zip_archive_file_stat stat{};
      if (!zip.Stat(index, stat)) continue;
      const auto relative_name = NormalizeArchiveName(stat.m_filename);
      if (!relative_name) continue;
      std::vector<uint8_t> data;
      if (!zip.Extract(index, data)) {
        fs::remove_all(temp_root, error);
        return Result::Fail(zip.error());
      }
      if (*relative_name == "wwe2k14-enhanced.ini") {
        ini_data = std::move(data);
        continue;
      }
      const fs::path relative = internal::PathFromUtf8(*relative_name);
      if (!IsSafeRelative(relative)) continue;
      const std::string basename = Lower(internal::PathToUtf8(relative.filename()));
      if (basename == "savedata.dat") found_save = true;
      const fs::path target = temp_root / relative;
      fs::create_directories(target.parent_path(), error);
      if (error || !internal::WriteBinaryFile(target, data)) {
        fs::remove_all(temp_root, error);
        return Result::Fail("The launcher could not write the restored save files.");
      }
    }
    if (!found_save) {
      fs::remove_all(temp_root, error);
      return Result::Fail("This backup does not contain a saved game.");
    }

    const fs::path account = paths.userdata_dir / std::string(kAccountId);
    if (HasAnyFiles(account)) {
      const Result backup_result = BackupSaves(paths, false, nullptr);
      if (!backup_result.ok) {
        fs::remove_all(temp_root, error);
        return backup_result;
      }
    }
    const fs::path old_account = paths.userdata_dir / (".restore-old-" + BackupTimestamp());
    bool moved_old = false;
    error.clear();
    if (fs::exists(account, error) && !error) {
      fs::rename(account, old_account, error);
      if (error) {
        fs::remove_all(temp_root, error);
        return Result::Fail("The launcher could not replace the current saved game.");
      }
      moved_old = true;
    }
    error.clear();
    fs::rename(temp_root, account, error);
    if (error) {
      if (moved_old) {
        std::error_code restore_error;
        fs::rename(old_account, account, restore_error);
      }
      fs::remove_all(temp_root, error);
      return Result::Fail("The launcher could not replace the current saved game.");
    }
    if (moved_old) fs::remove_all(old_account, error);
    if (ini_data && !internal::WriteBinaryFile(paths.settings_file, *ini_data)) {
      return Result::Fail("The saved game was restored, but launcher settings could not be restored.");
    }
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The launcher could not restore this save backup.");
  }
}

Result ImportBackup(const Paths& paths, const fs::path& external_zip) {
  try {
    internal::ZipReader zip;
    if (!zip.Open(external_zip)) return Result::Fail(zip.error());
    if (!zip.ContainsSaveData()) return Result::Fail("Choose a ZIP file that contains SaveData.Dat.");
    if (!internal::EnsureDirectory(paths.backups_dir)) {
      return Result::Fail("The launcher could not create the backup folder.");
    }
    const std::string stamp = BackupTimestamp();
    const std::string source_name = internal::PathToUtf8(external_zip.filename());
    fs::path destination = paths.backups_dir / ("wwe2k14-saves-import-" + stamp + "-" + source_name);
    std::error_code error;
    unsigned suffix = 2;
    while (fs::exists(destination, error) && !error) {
      destination = paths.backups_dir / ("wwe2k14-saves-import-" + stamp + "-" +
                                         std::to_string(suffix++) + "-" + source_name);
      error.clear();
    }
    std::vector<uint8_t> bytes;
    if (!internal::ReadBinaryFile(external_zip, bytes) || !internal::WriteBinaryFile(destination, bytes)) {
      return Result::Fail("The launcher could not copy that backup into the backup folder.");
    }
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The launcher could not import that backup.");
  }
}

}  // namespace wwe2k14::launcher
