#include "wwe13_install.h"
#include "wwe15_install.h"

#include "wwe13_audio.h"
#include "wwe13_shared.h"
#include "path_case.h"

#include "internal/stfs.h"
#include "internal/util.h"
#include "update.h"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <cstdio>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>

namespace wwe2k14::launcher {
namespace fs = std::filesystem;
namespace {

constexpr uint64_t kMib = 1024ull * 1024ull;
constexpr uint64_t kGib = 1024ull * kMib;
std::atomic<uint64_t> g_token{0};
// Manifest value for a file the import took away (the host pack's old name): nothing of it is installed.
const std::string kRemovedHash(64, '0');

// Renames retry for about 3 s: on Windows a virus scanner or the search indexer briefly opens a file the
// launcher has just written, and a rename during that moment fails (Discord report 11-08, undo record).
void RenameRetry(const fs::path& from, const fs::path& to, std::error_code& ec) {
  for (int attempt = 0;; ++attempt) {
    ec.clear();
    fs::rename(from, to, ec);
    if (!ec || attempt >= 30) return;
    std::error_code exists_error;
    if (!fs::exists(fs::symlink_status(from, exists_error))) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
constexpr const char* kHostPackOld = "745BEE31BC19100A3EE21606E089EC25F739099C54";
constexpr const char* kContentRoot = "userdata/0000000000000000/545408B2/00000002/";
constexpr const char* kHeaderRoot = "userdata/0000000000000000/545408B2/Headers/00000002/";

struct FileRecord {
  std::string key;
  bool original_exists = false;
  char original_kind = 'F';  // F = regular bytes, L = symbolic link target text
  std::string original_hash;
  std::string applied_hash;
};

struct Manifest {
  std::string id;
  std::vector<FileRecord> files;
};

struct TransactionFile {
  std::string key;
  fs::path target;
  fs::path snapshot;
  bool existed = false;
};

struct Transaction {
  std::string id;
  std::vector<TransactionFile> files;
  std::set<std::string> keys;
};

std::string NewToken() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  return std::to_string(now) + "-" + std::to_string(g_token.fetch_add(1, std::memory_order_relaxed));
}

Result Failure(std::string message) { return Result::Fail(std::move(message)); }

bool IsSafeRelative(const fs::path& relative) {
  if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) return false;
  for (const fs::path& part : relative) if (part == ".." || part == "." || part.empty()) return false;
  return true;
}

bool IsSafeKey(const std::string& key) {
  if (key.empty() || key.find('\\') != std::string::npos || key.find('\t') != std::string::npos ||
      key.find('\n') != std::string::npos || key.find('\r') != std::string::npos) return false;
  const fs::path relative = fs::u8path(key);
  if (!IsSafeRelative(relative)) return false;
  const auto first = relative.begin();
  return first != relative.end() && (*first == "game" || *first == "userdata" || *first == "launcher");
}

bool IsExistingNoFollow(const fs::path& path, fs::file_status* status = nullptr) {
  std::error_code ec;
  const fs::file_status current = fs::symlink_status(path, ec);
  if (ec || current.type() == fs::file_type::not_found) return false;
  if (status) *status = current;
  return true;
}

std::string HashText(const std::string& text) {
  return picosha2::hash256_hex_string(text.begin(), text.end());
}

// The undo set's folder. Wrestler imports use backups/wwe13-import; arena imports (wwe13_arena_install.cpp) have their own
// set, backups/wwe13-arenas, so either can be undone without touching the other's records (see BackupAreaScope).
thread_local const char* g_backup_area = "wwe13-import";
fs::path ImportBackupRoot(const Paths& paths) { return paths.backups_dir / g_backup_area; }
// Which import the messages name (the WWE '13 wrestler and arena sets, or the WWE 2K15 set).
std::string LayerName() { return std::string(g_backup_area) == "wwe15-import" ? "WWE 2K15" : "WWE '13"; }
fs::path CurrentPointer(const Paths& paths) { return ImportBackupRoot(paths) / "current.txt"; }
fs::path BackupSet(const Paths& paths, const std::string& id) { return ImportBackupRoot(paths) / "sets" / id; }
fs::path OriginalArtifact(const fs::path& set, const FileRecord& record) {
  // A short name from the key: Windows refuses paths over 260 characters, and keys are long.
  fs::path path = set / "original" / HashText(record.key).substr(0, 20);
  if (record.original_kind == 'L') path += ".link";
  return path;
}

fs::path TargetPath(const Paths& paths, const fs::path& game_folder, const std::string& key) {
  if (!IsSafeKey(key)) return {};
  const fs::path relative = fs::u8path(key);
  auto part = relative.begin();
  const std::string area = part->string();
  ++part;
  fs::path tail;
  for (; part != relative.end(); ++part) tail /= *part;
  if (area == "game") return game_folder / tail;
  if (area == "userdata") return paths.userdata_dir / tail;
  return paths.exe_dir / tail;
}

std::string HashFile(const fs::path& path, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error) *error = "Could not read " + internal::PathToUtf8(path) + ".";
    return {};
  }
  picosha2::hash256_one_by_one hasher;
  std::vector<unsigned char> buffer(1024 * 1024);
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) hasher.process(buffer.begin(), buffer.begin() + count);
  }
  hasher.finish();
  return picosha2::get_hash_hex_string(hasher);
}

bool HashTarget(const fs::path& path, char* kind, std::string* hash, std::string* error) {
  fs::file_status status;
  if (!IsExistingNoFollow(path, &status)) {
    if (error) *error = "A file that should exist is missing: " + internal::PathToUtf8(path);
    return false;
  }
  if (fs::is_symlink(status)) {
    std::error_code ec;
    const fs::path link = fs::read_symlink(path, ec);
    if (ec) {
      if (error) *error = "Could not read a symbolic link: " + internal::PathToUtf8(path);
      return false;
    }
    *kind = 'L';
    *hash = HashText(internal::PathToUtf8(link));
    return true;
  }
  if (!fs::is_regular_file(status)) {
    if (error) *error = "An import target is not a regular file: " + internal::PathToUtf8(path);
    return false;
  }
  *kind = 'F';
  *hash = HashFile(path, error);
  return !hash->empty();
}

bool CopyStream(const fs::path& source, const fs::path& destination,
                const ProgressFn& progress, CancelFlag* cancel,
                uint64_t base = 0, uint64_t total = 0, const std::string& label = {}) {
  std::ifstream input(source, std::ios::binary);
  if (!input) return false;
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) return false;
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  std::vector<char> buffer(1024 * 1024);  // not on the stack: a Windows thread only has 1 MB of it
  uint64_t copied = 0;
  for (;;) {
    if (cancel && cancel->load(std::memory_order_relaxed)) return false;
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      output.write(buffer.data(), count);
      if (!output) return false;
      copied += static_cast<uint64_t>(count);
      if (progress && total) progress(std::min(total, base + copied), total, label);
    }
    if (input.eof()) break;
    if (!input) return false;
  }
  output.flush();
  return static_cast<bool>(output);
}

bool WriteTextFile(const fs::path& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.flush();
  return static_cast<bool>(output);
}

bool WriteAtomicText(const fs::path& path, const std::string& text, const std::string& token,
                     std::string* reason = nullptr) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) { if (reason) *reason = ec.message(); return false; }
  const fs::path temp = path.parent_path() / (".wwe13-new-" + token + ".tmp");
  const fs::path old = path.parent_path() / (".wwe13-old-" + token + ".tmp");
  if (!WriteTextFile(temp, text)) { if (reason) *reason = "the new record could not be written"; return false; }
  const bool had_old = IsExistingNoFollow(path);
  if (had_old) {
    RenameRetry(path, old, ec);
    if (ec) { if (reason) *reason = ec.message(); fs::remove(temp, ec); return false; }
  }
  RenameRetry(temp, path, ec);
  if (ec) {
    if (reason) *reason = ec.message();
    std::error_code restore_error;
    if (had_old) RenameRetry(old, path, restore_error);
    fs::remove(temp, restore_error);
    return false;
  }
  if (had_old) fs::remove(old, ec);
  return true;
}

bool SaveOriginal(const fs::path& target, const fs::path& set, FileRecord* record,
                  const ProgressFn& progress, CancelFlag& cancel, std::string* error) {
  fs::file_status status;
  if (!IsExistingNoFollow(target, &status)) {
    record->original_exists = false;
    record->original_kind = 'F';
    record->original_hash.clear();
    return true;
  }
  record->original_exists = true;
  if (fs::is_symlink(status)) {
    std::error_code ec;
    const fs::path link = fs::read_symlink(target, ec);
    if (ec) { if (error) *error = "Could not save a symbolic link before import."; return false; }
    record->original_kind = 'L';
    const fs::path backup = OriginalArtifact(set, *record);
    const std::string link_text = internal::PathToUtf8(link);
    record->original_hash = HashText(link_text);
    if (!WriteTextFile(backup, link_text)) { if (error) *error = "Could not back up a symbolic link before import."; return false; }
    return true;
  }
  if (!fs::is_regular_file(status)) { if (error) *error = "An import target is not a regular file: " + internal::PathToUtf8(target); return false; }
  record->original_kind = 'F';
  const fs::path backup = OriginalArtifact(set, *record);
  if (!CopyStream(target, backup, progress, &cancel, 0, 0, "Backing up original files…")) {
    if (error) *error = cancel.load(std::memory_order_relaxed) ? "WWE '13 import cancelled." : "Could not back up " + internal::PathToUtf8(target) + ".";
    return false;
  }
  record->original_hash = HashFile(backup, error);
  return !record->original_hash.empty();
}

bool CopyOriginalArtifact(const fs::path& old_set, const fs::path& new_set,
                          const FileRecord& old_record, std::string* error) {
  if (!old_record.original_exists) return true;
  const fs::path source = OriginalArtifact(old_set, old_record);
  const fs::path destination = OriginalArtifact(new_set, old_record);
  if (old_record.original_kind == 'L') {
    std::ifstream input(source, std::ios::binary);
    const std::string link_text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input && link_text.empty()) { if (error) *error = "The previous WWE '13 backup is incomplete."; return false; }
    return WriteTextFile(destination, link_text);
  }
  CancelFlag never_cancel{false};
  if (!CopyStream(source, destination, {}, &never_cancel)) {
    if (error) *error = "The previous WWE '13 backup is incomplete: " + internal::PathToUtf8(source);
    return false;
  }
  return true;
}

bool RestoreOriginal(const fs::path& set, const FileRecord& record, const fs::path& target,
                     const std::string& token, const ProgressFn& progress, CancelFlag& cancel,
                     std::string* error) {
  if (!record.original_exists) return true;
  const fs::path backup = OriginalArtifact(set, record);
  const fs::path temp = target.parent_path() / (".wwe13-restore-" + token + ".tmp");
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (ec) { if (error) *error = "Could not create the original file folder."; return false; }
  if (record.original_kind == 'L') {
    std::ifstream input(backup, std::ios::binary);
    const std::string link_text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input && link_text.empty()) { if (error) *error = "The saved symbolic link is missing."; return false; }
    fs::create_symlink(fs::u8path(link_text), temp, ec);
    if (ec) { if (error) *error = "Could not restore a symbolic link: " + ec.message(); return false; }
  } else if (!CopyStream(backup, temp, progress, &cancel, 0, 0, "Restoring original files…")) {
    if (error) *error = cancel.load(std::memory_order_relaxed) ? "WWE '13 import cancelled." : "Could not restore " + internal::PathToUtf8(target) + ".";
    return false;
  }
  RenameRetry(temp, target, ec);
  if (ec) {
    fs::remove(temp, ec);
    if (error) *error = "Could not restore " + internal::PathToUtf8(target) + ".";
    return false;
  }
  return true;
}

bool ReadManifest(const Paths& paths, Manifest* manifest, bool* found, std::string* error) {
  *found = false;
  std::ifstream pointer(CurrentPointer(paths), std::ios::binary);
  if (!pointer) return true;
  std::getline(pointer, manifest->id);
  if (manifest->id.empty() || !std::all_of(manifest->id.begin(), manifest->id.end(), [](char c) {
        return (c >= '0' && c <= '9') || c == '-';
      })) {
    if (error) *error = "The WWE '13 undo record is invalid.";
    return false;
  }
  const fs::path file = BackupSet(paths, manifest->id) / "manifest.tsv";
  std::ifstream input(file, std::ios::binary);
  if (!input) { if (error) *error = "The WWE '13 undo record is missing its manifest."; return false; }
  std::string line;
  if (!std::getline(input, line) || line != "WWE13IMPORT\t1") {
    if (error) *error = "The WWE '13 undo manifest has an unsupported version.";
    return false;
  }
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::vector<std::string> fields;
    size_t start = 0;
    for (;;) {
      const size_t end = line.find('\t', start);
      fields.push_back(line.substr(start, end == std::string::npos ? end : end - start));
      if (end == std::string::npos) break;
      start = end + 1;
    }
    if (fields.size() != 5 || !IsSafeKey(fields[0]) || (fields[1] != "0" && fields[1] != "1") ||
        (fields[2] != "F" && fields[2] != "L")) {
      if (error) *error = "The WWE '13 undo manifest contains an invalid file record.";
      return false;
    }
    FileRecord record;
    record.key = fields[0];
    record.original_exists = fields[1] == "1";
    record.original_kind = fields[2][0];
    record.original_hash = fields[3];
    record.applied_hash = fields[4];
    if (record.original_exists && record.original_hash.size() != 64) {
      if (error) *error = "The WWE '13 undo manifest contains an invalid backup hash.";
      return false;
    }
    if (record.applied_hash.size() != 64) {
      if (error) *error = "The WWE '13 undo manifest contains an invalid installed hash.";
      return false;
    }
    manifest->files.push_back(std::move(record));
  }
  if (manifest->files.empty()) { if (error) *error = "The WWE '13 undo manifest is empty."; return false; }
  *found = true;
  return true;
}

std::string SerializeManifest(const Manifest& manifest) {
  std::string text = "WWE13IMPORT\t1\n";
  for (const FileRecord& file : manifest.files) {
    text += file.key + "\t" + (file.original_exists ? "1" : "0") + "\t" + file.original_kind + "\t" +
            file.original_hash + "\t" + file.applied_hash + "\n";
  }
  return text;
}

bool StashTarget(Transaction* transaction, const std::string& key, const fs::path& target,
                 std::string* error) {
  if (transaction->keys.contains(key)) return true;
  TransactionFile file;
  file.key = key;
  file.target = target;
  file.existed = IsExistingNoFollow(target);
  if (file.existed) {
    file.snapshot = target.parent_path() / (".w13-" + std::to_string(transaction->files.size()) + "-" + transaction->id + ".old");
    std::error_code ec;
    RenameRetry(target, file.snapshot, ec);  // same-volume rename; never writes through hard links
    if (ec) { if (error) *error = "Could not safely replace " + internal::PathToUtf8(target) + ": " + ec.message(); return false; }
  }
  transaction->keys.insert(key);
  transaction->files.push_back(std::move(file));
  return true;
}

bool Rollback(Transaction* transaction, std::string* error) {
  bool success = true;
  for (auto it = transaction->files.rbegin(); it != transaction->files.rend(); ++it) {
    std::error_code ec;
    fs::remove(it->target, ec);
    ec.clear();
    if (it->existed) {
      fs::create_directories(it->target.parent_path(), ec);
      if (!ec) RenameRetry(it->snapshot, it->target, ec);
    }
    if (ec) success = false;
  }
  if (!success && error) *error = "The import could not finish restoring its temporary replacements. Recovery files were kept beside the affected folders.";
  return success;
}

void CleanupTransaction(const Transaction& transaction) {
  for (const TransactionFile& file : transaction.files) {
    if (file.snapshot.empty()) continue;
    std::error_code ignored;
    fs::remove(file.snapshot, ignored);
  }
}

bool ReplaceFromStaging(const fs::path& source, const fs::path& target, const std::string& key,
                        const std::string& token, Transaction* transaction,
                        const ProgressFn& progress, CancelFlag& cancel, std::string* error) {
  if (!StashTarget(transaction, key, target, error)) return false;
  const fs::path temp = target.parent_path() / (".wwe13-new-" + token + ".tmp");
  if (!CopyStream(source, temp, progress, &cancel, 0, 0, "Applying imported files…")) {
    if (error) *error = cancel.load(std::memory_order_relaxed) ? "WWE '13 import cancelled." : "Could not stage the replacement for " + internal::PathToUtf8(target) + ".";
    std::error_code ignored;
    fs::remove(temp, ignored);
    return false;
  }
  std::error_code ec;
  RenameRetry(temp, target, ec);
  if (ec) {
    fs::remove(temp, ec);
    if (error) *error = "Could not put the new file in place: " + internal::PathToUtf8(target);
    return false;
  }
  return true;
}

bool WriteStagedText(const fs::path& files, const std::string& relative, const std::string& text,
                     std::string* error) {
  const fs::path destination = files / fs::u8path(relative);
  if (!WriteTextFile(destination, text)) {
    if (error) *error = "Could not prepare launcher Mods metadata for the import.";
    return false;
  }
  return true;
}

bool LoadText(const fs::path& file, std::string* text) {
  std::ifstream input(file, std::ios::binary);
  if (!input) return false;
  text->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return !input.bad();
}

bool ParseDecimal(std::string_view text, uint32_t* value) {
  if (text.empty()) return false;
  uint32_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
  *value = parsed;
  return true;
}

std::set<uint32_t> ReadSlotIds(const std::string& text) {
  std::set<uint32_t> ids;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) continue;
    const size_t last = line.find_last_not_of(" \t\r");
    uint32_t id = 0;
    if (ParseDecimal(std::string_view(line).substr(first, last - first + 1), &id) && id >= 100 && id <= 499)
      ids.insert(id);
  }
  return ids;
}

std::set<uint32_t> ReadRosterTargets(const fs::path& file) {
  std::string text;
  if (!LoadText(file, &text)) return {};
  std::set<uint32_t> ids;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t tab = line.find('\t');
    uint32_t id = 0;
    if (tab != std::string::npos && ParseDecimal(std::string_view(line).substr(0, tab), &id) && id >= 100 && id <= 499)
      ids.insert(id);
  }
  return ids;
}

std::string MakeSlotsText(const std::set<uint32_t>& ids) {
  std::string text = "# Written by the launcher's Mods page.\n";
  for (const uint32_t id : ids) text += std::to_string(id) + "\n";
  return text;
}

std::string MakeRosterText(const std::vector<WWE13ImportPort>& ports) {
  std::string text = "#game WWE '13\n";
  for (const WWE13ImportPort& port : ports)
    text += std::to_string(port.target_id) + "\t" + port.name + "\t" + port.gender + "\n";
  return text;
}

// The Mods page edits these in place (Remove, theme volume): their current bytes need not match the import.
bool EditedByModsPage(const std::string& key) {
  // (also used for the host pack's old files: they are meant to be absent while imported)
  return key == "launcher/own-slots.txt" || key == "game/sound/DLC_01.pck";
}

bool LoadPreviousBaseline(const Paths& paths, const fs::path& game_folder, const Manifest& manifest,
                          Transaction* transaction, const ProgressFn& progress, CancelFlag& cancel,
                          std::string* error) {
  const fs::path set = BackupSet(paths, manifest.id);
  uint64_t done = 0;
  uint64_t total = manifest.files.size();
  for (const FileRecord& record : manifest.files) {
    if (record.key.rfind("game/", 0) != 0 && record.key.rfind("userdata/", 0) != 0) continue;
    if (cancel.load(std::memory_order_relaxed)) { if (error) *error = "WWE '13 import cancelled."; return false; }
    const fs::path target = TargetPath(paths, game_folder, record.key);
    char kind = 'F';
    std::string hash;
    if (!EditedByModsPage(record.key) && record.applied_hash != kRemovedHash &&
        (!HashTarget(target, &kind, &hash, error) || kind != 'F' || hash != record.applied_hash)) {
      if (error && error->empty()) *error = "An installed WWE '13 file changed since import; re-import was stopped before changing anything.";
      return false;
    }
    if (!StashTarget(transaction, record.key, target, error)) return false;
    if (record.original_exists && !RestoreOriginal(set, record, target, transaction->id, progress, cancel, error)) return false;
    ++done;
    if (progress) progress(done, total, "Preparing original game files for re-import…");
  }
  return true;
}

bool EstimatePackageBytes(const std::vector<WWE13ImportPack>& packs, uint64_t* total, std::string* error) {
  *total = 0;
  for (const WWE13ImportPack& pack : packs) {
    if (!pack.package_file) continue;
    internal::StfsReader reader;
    std::string detail;
    if (!reader.Open(pack.folder, &detail)) { if (error) *error = "Could not read WWE '13 add-on " + pack.display_name + ": " + detail; return false; }
    for (const internal::StfsEntry& entry : reader.entries()) {
      if (!entry.directory) {
        if (entry.size > std::numeric_limits<uint64_t>::max() - *total) { if (error) *error = "WWE '13 add-on size overflow."; return false; }
        *total += entry.size;
      }
    }
  }
  return true;
}

bool CheckFreeSpace(const fs::path& temp, const fs::path& launcher, const fs::path& game,
                    const fs::path& userdata, size_t count, uint64_t packed_bytes, std::string* error) {
  const uint64_t estimated_tree = 512 * kMib + static_cast<uint64_t>(count) * 16 * kMib;
  const uint64_t stage_need = estimated_tree + packed_bytes + 256 * kMib;
  const uint64_t install_need = estimated_tree + 512 * kMib;
  const std::array<std::pair<fs::path, uint64_t>, 4> needs{{
      {temp, stage_need}, {launcher, estimated_tree + 512 * kMib + static_cast<uint64_t>(count) * 10 * kMib}, {game, install_need}, {userdata, install_need}}};
  for (const auto& [path, required] : needs) {
    std::error_code ec;
    const fs::space_info info = fs::space(path, ec);
    if (ec) { if (error) *error = "Could not check free disk space for " + internal::PathToUtf8(path) + "."; return false; }
    if (info.available < required) {
      if (error) {
        const auto mib = [](uint64_t bytes) { return static_cast<unsigned long long>((bytes + kMib - 1) / kMib); };
        *error = "Not enough free disk space at " + internal::PathToUtf8(path) + ". WWE '13 import needs about " +
                 std::to_string(mib(required)) + " MiB free there; " + std::to_string(mib(info.available)) +
                 " MiB is available. Nothing has been changed.";
      }
      return false;
    }
  }
  return true;
}

std::vector<fs::path> ExtractSourcePacks(const std::vector<WWE13ImportPack>& packs,
                                         const fs::path& directory,
                                         const ProgressFn& progress, CancelFlag& cancel,
                                         std::string* error) {
  std::vector<fs::path> roots;
  std::error_code ec;
  fs::create_directories(directory, ec);
  if (ec) { if (error) *error = "Could not prepare the WWE '13 add-on extraction folder."; return {}; }
  uint64_t total = 0;
  for (const WWE13ImportPack& pack : packs) {
    if (!pack.package_file) { roots.push_back(pack.folder); continue; }
    internal::StfsReader reader;
    std::string detail;
    if (!reader.Open(pack.folder, &detail)) { if (error) *error = "Could not open " + pack.display_name + ": " + detail; return {}; }
    for (const internal::StfsEntry& entry : reader.entries()) if (!entry.directory) total += entry.size;
  }
  uint64_t done = 0;
  size_t package_number = 0;
  for (const WWE13ImportPack& pack : packs) {
    if (!pack.package_file) continue;
    if (cancel.load(std::memory_order_relaxed)) { if (error) *error = "WWE '13 import cancelled."; return {}; }
    internal::StfsReader reader;
    std::string detail;
    if (!reader.Open(pack.folder, &detail)) { if (error) *error = "Could not open " + pack.display_name + ": " + detail; return {}; }
    const fs::path root = directory / ("pack-" + std::to_string(++package_number));
    for (const internal::StfsEntry& entry : reader.entries()) {
      if (entry.directory) continue;
      const fs::path relative = entry.relative_path;
      if (!IsSafeRelative(relative)) { if (error) *error = "An add-on file has an unsafe path."; return {}; }
      const fs::path output_path = root / relative;
      std::error_code create_error;
      fs::create_directories(output_path.parent_path(), create_error);
      if (create_error) { if (error) *error = "Could not extract a WWE '13 add-on file."; return {}; }
      std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
      if (!output) { if (error) *error = "Could not create a WWE '13 add-on file."; return {}; }
      const bool copied = reader.CopyFile(entry, output, [&](uint64_t bytes) {
        if (cancel.load(std::memory_order_relaxed)) return false;
        if (progress && total) progress(std::min(total, done + bytes), total, "Extracting " + pack.display_name + "…");
        return true;
      }, &detail);
      output.flush();
      if (!copied || !output) {
        if (error) *error = cancel.load(std::memory_order_relaxed) ? "WWE '13 import cancelled." : "Could not extract " + pack.display_name + ": " + detail;
        return {};
      }
      done += entry.size;
    }
    roots.push_back(root);
  }
  return roots;
}

}  // namespace

// A WWE '13 disc image as the base game: the files the wrestler import reads from the base (measured with a file-open
// trace of stages 2-9 + audio over all 92 base wrestlers, 11-13), by disc path. Only files that are on the disc.
std::vector<std::string> WWE13DiscImportFiles(const std::vector<DiscImageFile>& disc, const std::vector<WWE13ImportPort>& ports) {
  std::set<std::string> wanted{"pac/ch/chetc.pac", "pac/menu/menuhd.pac", "pac/menu/menusd.pac", "pac/evt/evtmov.pac",
                               "sound/music.pck", "sound/ra.pck"};
  for (const WWE13ImportPort& port : ports) {
    wanted.insert("pac/ch/ch" + std::to_string(port.source_id) + ".pac");
    if (port.leaf10000) wanted.insert("pac/ch/ch" + std::to_string(*port.leaf10000) + ".pac");
    char movie[48]{};
    std::snprintf(movie, sizeof(movie), "movies/titantron/%03u.bik2", port.source_id);  // the titantron step's name
    wanted.insert(movie);
  }
  std::vector<std::string> files;
  for (const DiscImageFile& file : disc) {
    const std::string lower = internal::LowerAscii(file.path);
    const fs::path path(lower);
    const std::string folder = path.parent_path().generic_string(), name = path.filename().generic_string();
    const auto starts = [&](const char* prefix) { return name.rfind(prefix, 0) == 0; };
    const bool pac_table = folder == "pac" && name.size() > 4 && name.substr(name.size() - 4) == ".pac" &&
                           (starts("dlc") || starts("gm") || starts("prtcls") || starts("sound") || starts("string"));
    const bool event = folder == "pac/evt" && (starts("entrance") || starts("winning"));
    if (wanted.contains(lower) || pac_table || event) files.push_back(file.path);
  }
  return files;
}

namespace {

}  // namespace

namespace {
// A pack is found by its folder: an empty folder left behind would still count.
void RemoveEmptyFolders(const fs::path& start, const fs::path& stop) {
  std::error_code ec;
  for (fs::path folder = start; !folder.empty() && folder != stop && folder.has_parent_path(); folder = folder.parent_path()) {
    if (!fs::is_directory(folder, ec) || ec || !fs::is_empty(folder, ec) || ec) return;
    if (!fs::remove(folder, ec) || ec) return;
  }
}
}  // namespace

bool WWE13TargetsInDlcRegistry(const std::vector<uint8_t>& dlc_pac, const std::vector<uint32_t>& target_ids,
                               std::vector<uint32_t>* present, std::string* error) {
  present->clear();
  std::vector<std::array<uint16_t, 4>> rows;
  if (!WWE13SharedRegistryRows(dlc_pac, "DLC.pac", 0, &rows, error)) return false;
  std::set<uint32_t> have;
  for (const auto& row : rows) have.insert(row[0]);
  for (const uint32_t id : target_ids) if (have.contains(id)) present->push_back(id);
  return true;
}

bool RestoreWWE13ImportFromOtherLauncher(const Paths& launcher_paths, const fs::path& game_folder,
                                         const std::vector<fs::path>& candidates,
                                         const ProgressFn& progress, CancelFlag& cancel,
                                         fs::path* used, std::string* error) {
  used->clear();
  for (const fs::path& folder : candidates) {
    if (cancel.load(std::memory_order_relaxed)) { if (error) *error = "WWE '13 import cancelled."; return false; }
    std::error_code ec;
    if (fs::equivalent(folder, launcher_paths.exe_dir, ec)) continue;
    Paths other = launcher_paths;
    other.exe_dir = folder;
    other.backups_dir = folder / "backups";
    Manifest manifest;
    bool found = false;
    std::string read_error;
    if (!ReadManifest(other, &manifest, &found, &read_error) || !found) continue;
    // Which records are in the imported state here (to restore) and whether any file is in a third state (skip folder).
    std::vector<const FileRecord*> todo;
    bool usable = true, any_game_imported = false;
    for (const FileRecord& record : manifest.files) {
      const bool game = record.key.rfind("game/", 0) == 0;
      if (!game && record.key.rfind("userdata/", 0) != 0) continue;  // launcher/ state belongs to the other copy
      const fs::path target = TargetPath(launcher_paths, game_folder, record.key);
      if (target.empty()) { usable = false; break; }
      const bool exists = IsExistingNoFollow(target);
      char kind = 'F';
      std::string hash, ignored;
      if (exists && !HashTarget(target, &kind, &hash, &ignored)) { usable = false; break; }
      const bool imported = record.applied_hash == kRemovedHash ? !exists : (exists && hash == record.applied_hash);
      const bool original = record.original_exists ? (exists && hash == record.original_hash) : !exists;
      if (imported && !original) {
        todo.push_back(&record);
        any_game_imported = any_game_imported || game;
      } else if (!original && !EditedByModsPage(record.key)) {
        usable = false;
        break;
      }
    }
    if (!usable || !any_game_imported) continue;
    const fs::path set = BackupSet(other, manifest.id);
    Transaction transaction;
    transaction.id = NewToken();
    std::string restore_error;
    bool ok = true;
    uint64_t done = 0;
    for (const FileRecord* record : todo) {
      const fs::path target = TargetPath(launcher_paths, game_folder, record->key);
      if (!StashTarget(&transaction, record->key, target, &restore_error) ||
          (record->original_exists &&
           !RestoreOriginal(set, *record, target, transaction.id, progress, cancel, &restore_error))) {
        ok = false;
        break;
      }
      ++done;
      if (progress) progress(done, todo.size(), "Putting back the original game files from the earlier import…");
    }
    if (!ok) {
      std::string rollback_error;
      if (Rollback(&transaction, &rollback_error)) CleanupTransaction(transaction);
      if (error) *error = rollback_error.empty() ? restore_error : rollback_error;
      return false;
    }
    CleanupTransaction(transaction);
    *used = folder;
    return true;
  }
  return true;
}

bool HasWWE13Import(const Paths& launcher_paths) {
  Manifest manifest;
  bool found = false;
  std::string error;
  return ReadManifest(launcher_paths, &manifest, &found, &error) && found;
}

Result ApplyWWE13StagedFiles(const Paths& launcher_paths, const fs::path& game_folder,
                             const fs::path& staged_files, const ProgressFn& progress,
                             CancelFlag& cancel, WWE13ImportSummary* summary,
                             const std::vector<std::string>& removal_keys) {
  std::string error;
  if (GameProcessRunning(launcher_paths)) return Failure("Close WWE 2K14 before importing or undoing wrestlers.");
  Manifest previous;
  bool has_previous = false;
  if (!ReadManifest(launcher_paths, &previous, &has_previous, &error)) return Failure(error);

  std::error_code ec;
  if (!fs::is_directory(staged_files, ec) || ec) return Failure("The staged " + LayerName() + " import files could not be found.");
  const std::string token = NewToken();
  Manifest next;
  next.id = token;
  const fs::path next_set = BackupSet(launcher_paths, token);
  fs::create_directories(next_set / "original", ec);
  if (ec) return Failure("The " + LayerName() + " backup folder could not be created.");

  std::map<std::string, FileRecord> previous_records;
  for (const FileRecord& record : previous.files) previous_records.emplace(record.key, record);
  Transaction transaction;
  transaction.id = token;
  std::vector<fs::path> staged;
  for (fs::recursive_directory_iterator it(staged_files, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    const fs::file_status status = it->symlink_status(ec);
    if (ec) break;
    if (!fs::is_regular_file(status)) continue;
    const fs::path relative = it->path().lexically_relative(staged_files);
    if (!IsSafeRelative(relative)) { error = "The staging folder contains an unsafe path."; break; }
    const std::string key = relative.generic_string();
    if (!IsSafeKey(key)) { error = "The staging folder contains an unsupported output area."; break; }
    staged.push_back(it->path());
  }
  if (ec && error.empty()) error = "The staged import files could not be listed.";
  if (!error.empty()) {
    fs::remove_all(next_set, ec);
    return Failure(error);
  }
  if (staged.empty()) { fs::remove_all(next_set, ec); return Failure("The staged import contains no files."); }
  std::sort(staged.begin(), staged.end());

  for (size_t index = 0; index < staged.size(); ++index) {
    if (cancel.load(std::memory_order_relaxed)) { error = LayerName() + " import cancelled."; break; }
    const fs::path source = staged[index];
    const std::string key = source.lexically_relative(staged_files).generic_string();
    const fs::path target = TargetPath(launcher_paths, game_folder, key);
    if (target.empty()) { error = "The staged import contains an unsafe destination."; break; }
    FileRecord record;
    record.key = key;
    auto old = previous_records.find(key);
    if (old != previous_records.end()) {
      record.original_exists = old->second.original_exists;
      record.original_kind = old->second.original_kind;
      record.original_hash = old->second.original_hash;
      if (!CopyOriginalArtifact(BackupSet(launcher_paths, previous.id), next_set, old->second, &error)) break;
    } else if (!SaveOriginal(target, next_set, &record, progress, cancel, &error)) {
      break;
    }
    record.applied_hash = HashFile(source, &error);
    if (record.applied_hash.empty()) { if (error.empty()) error = "A staged import file could not be hashed."; break; }
    if (!ReplaceFromStaging(source, target, key, token, &transaction, progress, cancel, &error)) break;
    next.files.push_back(std::move(record));
    if (progress) progress(index + 1, staged.size(), "Applying imported files…");
  }

  // Files the import takes away: backed up like the rest, then moved aside with the transaction.
  for (const std::string& key : removal_keys) {
    if (!error.empty()) break;
    const fs::path target = TargetPath(launcher_paths, game_folder, key);
    if (target.empty()) { error = "The import lists an unsafe file to remove."; break; }
    FileRecord record;
    record.key = key;
    auto old = previous_records.find(key);
    if (old != previous_records.end()) {
      record.original_exists = old->second.original_exists;
      record.original_kind = old->second.original_kind;
      record.original_hash = old->second.original_hash;
      if (!CopyOriginalArtifact(BackupSet(launcher_paths, previous.id), next_set, old->second, &error)) break;
    } else if (!SaveOriginal(target, next_set, &record, progress, cancel, &error)) {
      break;
    }
    if (!StashTarget(&transaction, key, target, &error)) break;
    record.applied_hash = kRemovedHash;
    next.files.push_back(std::move(record));
  }
  if (error.empty() && cancel.load(std::memory_order_relaxed)) error = LayerName() + " import cancelled.";
  if (error.empty()) {
    std::sort(next.files.begin(), next.files.end(), [](const FileRecord& a, const FileRecord& b) { return a.key < b.key; });
    if (!WriteTextFile(next_set / "manifest.tsv", SerializeManifest(next))) error = "The " + LayerName() + " undo manifest could not be saved.";
  }
  if (error.empty() && cancel.load(std::memory_order_relaxed)) error = LayerName() + " import cancelled.";
  std::string pointer_reason;
  if (error.empty() && !WriteAtomicText(CurrentPointer(launcher_paths), token + "\n", token, &pointer_reason))
    error = "The " + LayerName() + " undo record could not be activated (" + pointer_reason +
            "). Nothing was changed. Close other programs using the launcher folder and try again.";

  if (!error.empty()) {
    std::string rollback_error;
    const bool rolled_back = Rollback(&transaction, &rollback_error);
    if (!rolled_back) return Failure(rollback_error);
    std::error_code ignored;
    fs::remove_all(next_set, ignored);
    CleanupTransaction(transaction);
    return Failure(error);
  }
  CleanupTransaction(transaction);
  for (const std::string& key : removal_keys) {
    const fs::path target = TargetPath(launcher_paths, game_folder, key);
    RemoveEmptyFolders(target.parent_path(), launcher_paths.userdata_dir);
  }
  if (has_previous) {
    std::error_code ignored;
    fs::remove_all(BackupSet(launcher_paths, previous.id), ignored);
  }
  if (summary) {
    summary->imported_wrestlers = ReadRosterTargets(launcher_paths.exe_dir / "mods" / "wwe13" / "roster.tsv").size();
    summary->lines.clear();
  }
  return Result::Ok();
}

Result UndoWWE13Import(const Paths& launcher_paths, const fs::path& game_folder,
                       const ProgressFn& progress, CancelFlag& cancel) {
  if (std::string(g_backup_area) == "wwe13-import" && HasWWE13ArenaImport(launcher_paths))
    return Failure("Undo the imported WWE '13 arenas first: they changed some of the same game files.");
  if (std::string(g_backup_area) == "wwe13-import" && HasWWE15Import(launcher_paths))
    return Failure("Undo the WWE 2K15 import first: its wrestlers were added to the WWE '13 import's add-on folder.");
  if (GameProcessRunning(launcher_paths)) return Failure("Close WWE 2K14 before importing or undoing wrestlers.");
  Manifest manifest;
  bool found = false;
  std::string error;
  if (!ReadManifest(launcher_paths, &manifest, &found, &error)) return Failure(error);
  if (!found) return Failure("There is no " + LayerName() + " import to undo.");
  Transaction transaction;
  transaction.id = NewToken();
  const fs::path set = BackupSet(launcher_paths, manifest.id);

  for (size_t index = 0; index < manifest.files.size(); ++index) {
    const FileRecord& record = manifest.files[index];
    if (cancel.load(std::memory_order_relaxed)) { error = LayerName() + " undo cancelled."; break; }
    const fs::path target = TargetPath(launcher_paths, game_folder, record.key);
    if (target.empty()) { error = "The " + LayerName() + " undo manifest contains an unsafe destination."; break; }
    if (!EditedByModsPage(record.key) && record.applied_hash != kRemovedHash) {
      char kind = 'F';
      std::string hash;
      if (!HashTarget(target, &kind, &hash, &error) || kind != 'F' || hash != record.applied_hash) {
        if (error.empty()) error = "An imported " + LayerName() + " file changed after installation. Undo stopped before changing it.";
        break;
      }
    }
    if (!StashTarget(&transaction, record.key, target, &error)) break;
    if (record.original_exists && !RestoreOriginal(set, record, target, transaction.id, progress, cancel, &error)) break;
    if (progress) progress(index + 1, manifest.files.size(), "Restoring original game files…");
  }
  if (error.empty() && cancel.load(std::memory_order_relaxed)) error = LayerName() + " undo cancelled.";
  if (error.empty()) {
    std::error_code ec;
    fs::remove(CurrentPointer(launcher_paths), ec);
    if (ec) error = "The " + LayerName() + " undo record could not be removed.";
  }
  if (!error.empty()) {
    std::string rollback_error;
    if (!Rollback(&transaction, &rollback_error)) return Failure(rollback_error);
    CleanupTransaction(transaction);
    return Failure(error);
  }
  CleanupTransaction(transaction);
  if (std::string(g_backup_area) != "wwe15-import") {
    std::error_code cache_error;
    fs::remove_all(launcher_paths.exe_dir / "w13cache", cache_error);
  }
  for (const FileRecord& record : manifest.files) {
    if (record.original_exists || record.key.rfind("userdata/", 0) != 0) continue;
    RemoveEmptyFolders(TargetPath(launcher_paths, game_folder, record.key).parent_path(), launcher_paths.userdata_dir);
  }
  std::error_code ignored;
  fs::remove_all(set, ignored);
  return Result::Ok();
}

namespace {
Result RunWWE13ImportOnce(const Paths& launcher_paths, const fs::path& game_folder,
                          const WWE13ImportScan& scan, const std::vector<uint32_t>& selected_source_ids,
                          const ProgressFn& progress, CancelFlag& cancel, WWE13ImportSummary* summary);
}  // namespace

// Owner 11-16: import from WWE '13 and WWE 2K15 in any order. The WWE 2K15 wrestlers live in the WWE '13 import's add-on
// folder, whose shared files the WWE '13 import rebuilds; so with a WWE 2K15 import in place the launcher takes it out
// (its own Undo), runs the WWE '13 import, and imports the same WWE 2K15 wrestlers again from the same files
// (mods/wwe15/import.txt). If the WWE '13 import fails, the WWE 2K15 wrestlers are still put back.
Result RunWWE13Import(const Paths& launcher_paths, const fs::path& game_folder,
                      const WWE13ImportScan& scan, const std::vector<uint32_t>& selected_source_ids,
                      const ProgressFn& progress, CancelFlag& cancel, WWE13ImportSummary* summary) {
  if (!HasWWE15Import(launcher_paths))
    return RunWWE13ImportOnce(launcher_paths, game_folder, scan, selected_source_ids, progress, cancel, summary);
  // the cheap WWE '13 checks first, so nothing is undone for an import that cannot start
  if (HasWWE13ArenaImport(launcher_paths))
    return Failure("Undo the imported WWE '13 arenas first: they changed some of the same game files.");
  if (GameProcessRunning(launcher_paths)) return Failure("Close WWE 2K14 before importing wrestlers.");
  if (!scan.error.empty()) return Failure(scan.error);
  if (!scan.base_found) return Failure("WWE '13 base game files are required. Choose the folder containing your WWE '13 disc image (.iso) or the extracted game files and try again.");
  if (selected_source_ids.empty()) return Failure("Choose at least one available wrestler to import.");
  WWE15ImportRecord record;
  if (!ReadWWE15ImportRecord(launcher_paths, &record))
    return Failure("Undo the WWE 2K15 import first, then import from WWE '13, then import from WWE 2K15 again (this WWE 2K15 "
                   "import was made by an older launcher, which did not note which wrestlers it brought). Nothing has been changed.");
  if (progress) progress(0, 1, "Checking your WWE 2K15 files…");
  const WWE15ImportScan scan15 = ScanWWE15Import(launcher_paths, record.source);
  std::string missing;
  if (scan15.error.empty())
    for (const WWE15ImportWrestler& w : scan15.wrestlers)
      if (!w.available && std::find(record.source_ids.begin(), record.source_ids.end(), w.source_id) != record.source_ids.end())
        missing = w.name;
  if (!scan15.error.empty() || !missing.empty())
    return Failure("Your WWE 2K15 wrestlers have to be taken out and put back for this import, but WWE 2K15 is no longer at " +
                   internal::PathToUtf8(record.source) + (missing.empty() ? "" : " (" + missing + " is missing)") +
                   ". Put it back there, or Undo the WWE 2K15 import yourself first. Nothing has been changed.");
  const Result taken_out = UndoWWE15Import(launcher_paths, game_folder, progress, cancel);
  if (!taken_out.ok) return taken_out;

  const Result imported = RunWWE13ImportOnce(launcher_paths, game_folder, scan, selected_source_ids, progress, cancel, summary);
  // Putting them back is not cancelled by the WWE '13 Cancel: it restores what the player had.
  CancelFlag keep_going{false};
  WWE15ImportOptions options;
  options.existing_wrestler_looks = record.existing_wrestler_looks;
  WWE15ImportSummary back;
  const Result returned = RunWWE15Import(launcher_paths, game_folder, scan15, record.source_ids, progress, keep_going, &back, options);
  if (!imported.ok) {
    if (returned.ok) return Failure(imported.error + " Your WWE 2K15 wrestlers were put back.");
    return Failure(imported.error + " Your WWE 2K15 wrestlers could not be put back (" + returned.error +
                   "): use Import from WWE 2K15 again.");
  }
  if (!returned.ok)
    return Failure("The WWE '13 wrestlers were imported, but your WWE 2K15 wrestlers could not be put back: " + returned.error +
                   " Use Import from WWE 2K15 again.");
  if (summary) summary->lines.push_back("Your " + std::to_string(back.imported_wrestlers) + " WWE 2K15 wrestlers were put back.");
  return Result::Ok();
}

namespace {
Result RunWWE13ImportOnce(const Paths& launcher_paths, const fs::path& game_folder,
                          const WWE13ImportScan& scan, const std::vector<uint32_t>& selected_source_ids,
                          const ProgressFn& progress, CancelFlag& cancel, WWE13ImportSummary* summary) {
  if (HasWWE13ArenaImport(launcher_paths))
    return Failure("Undo the imported WWE '13 arenas first: they changed some of the same game files.");
  if (HasWWE15Import(launcher_paths))
    return Failure("Undo the WWE 2K15 import first, then import from WWE '13, then import from WWE 2K15 again: the WWE '13 import "
                   "rebuilds the add-on folder's portrait, celebration and catalog files for its own wrestlers and would drop "
                   "the WWE 2K15 wrestlers from them.");
  if (summary) *summary = {};
  if (GameProcessRunning(launcher_paths)) return Failure("Close WWE 2K14 before importing wrestlers.");
  if (!scan.error.empty()) return Failure(scan.error);
  if (!scan.base_found) return Failure("WWE '13 base game files are required. Choose the folder containing your WWE '13 disc image (.iso) or the extracted game files and try again.");
  if (selected_source_ids.empty()) return Failure("Choose at least one available wrestler to import.");

  std::vector<WWE13ImportPort> recipes;
  std::string error;
  if (!ReadWWE13ImportRecipes(launcher_paths.exe_dir / "data" / "wwe13-ports.tsv", &recipes, &error)) return Failure(error);
  std::map<uint32_t, WWE13ImportPort> by_source;
  std::map<uint32_t, WWE13ImportPort> by_target;
  for (const WWE13ImportPort& recipe : recipes) {
    by_target.emplace(recipe.target_id, recipe);
    by_source.emplace(recipe.source_id, recipe);
  }
  std::map<uint32_t, WWE13ImportWrestler> availability;
  for (const WWE13ImportWrestler& wrestler : scan.wrestlers) availability.emplace(wrestler.source_id, wrestler);
  std::set<uint32_t> old_target_ids = ReadRosterTargets(launcher_paths.exe_dir / "mods" / "wwe13" / "roster.tsv");
  // Installed by something other than this button: its files are not stock, and there is no undo record to build on.
  if (!old_target_ids.empty() && !HasWWE13Import(launcher_paths))
    return Failure("WWE '13 wrestlers from another installer are already in this game folder. Remove them with that installer first, then import here.");
  std::set<uint32_t> requested(selected_source_ids.begin(), selected_source_ids.end());
  std::vector<uint32_t> all_source_ids;
  for (const uint32_t source_id : requested) all_source_ids.push_back(source_id);
  for (const uint32_t target_id : old_target_ids) {
    const auto recipe = by_target.find(target_id);
    if (recipe == by_target.end()) return Failure("The installed WWE '13 roster contains a target ID missing from recipe data.");
    if (requested.insert(recipe->second.source_id).second) all_source_ids.push_back(recipe->second.source_id);
  }
  // In the recipe file's order: the packs are built in this order, so a given set always gives the same bytes.
  for (const uint32_t source_id : all_source_ids) {
    if (!by_source.contains(source_id)) return Failure("A selected wrestler has no conversion recipe.");
    const auto found = availability.find(source_id);
    if (found == availability.end() || !found->second.available)
      return Failure(found != availability.end() && !found->second.missing_reason.empty()
                         ? found->second.name + ": " + found->second.missing_reason
                         : "The selected wrestler's WWE '13 source files were not found.");
  }
  std::vector<WWE13ImportPort> ports;
  for (const WWE13ImportPort& recipe : recipes)
    if (requested.contains(recipe.source_id)) ports.push_back(recipe);

  std::set<uint32_t> slot_ids;
  std::string slots_text;
  if (LoadText(launcher_paths.exe_dir / "own-slots.txt", &slots_text)) slot_ids = ReadSlotIds(slots_text);
  const std::vector<ModWrestler> installed_mods = LoadModWrestlers(launcher_paths);
  for (const WWE13ImportPort& port : ports) {
    if (!old_target_ids.contains(port.target_id) && slot_ids.contains(port.target_id))
      return Failure(port.name + " needs target slot " + std::to_string(port.target_id) + ", which is already used by another mod.");
    const auto conflict = std::find_if(installed_mods.begin(), installed_mods.end(), [&](const ModWrestler& item) {
      return item.id == port.target_id && item.game != "WWE '13";
    });
    if (conflict != installed_mods.end()) return Failure(port.name + " conflicts with the existing " + conflict->game + " wrestler using slot " + std::to_string(port.target_id) + ".");
    slot_ids.insert(port.target_id);
  }
  // GitHub #141: with no undo record here, the game folder's DLC.pac must be the stock one. If it already lists some of
  // these wrestlers, an earlier import (usually another copy of the launcher sharing this game folder) changed it, and
  // building on it fails half way ("LSD registry already contains character ID"). Stop before changing anything.
  std::string originals_note;  // GitHub #160: shown with the result when the other copy's backup was used
  if (!HasWWE13Import(launcher_paths)) {
    std::vector<uint8_t> dlc_pac;
    std::vector<uint32_t> wanted, present;
    for (const WWE13ImportPort& port : ports) wanted.push_back(port.target_id);
    const fs::path dlc_path = ResolvePathCase(game_folder, "pac/DLC.pac");
    std::ifstream dlc_input(dlc_path, std::ios::binary);
    if (dlc_input) dlc_pac.assign(std::istreambuf_iterator<char>(dlc_input), std::istreambuf_iterator<char>());
    std::string registry_error;
    if (!dlc_pac.empty() && WWE13TargetsInDlcRegistry(dlc_pac, wanted, &present, &registry_error) && !present.empty()) {
      // First try to undo it ourselves with the other launcher copy's undo record (found by searching like the
      // first-run "bring over" step; used only when its fingerprints match this game folder exactly).
      if (progress) progress(0, 0, "This game folder already has WWE '13 wrestlers from another copy of the launcher. Looking for that copy…");
      std::vector<fs::path> candidates;
      for (const FoundInstallationImport& found : FindInstallationImports(launcher_paths, nullptr, cancel))
        candidates.push_back(found.folder);
      fs::path used;
      std::string restore_error;
      if (!RestoreWWE13ImportFromOtherLauncher(launcher_paths, game_folder, candidates, progress, cancel, &used,
                                               &restore_error))
        return Failure(restore_error + " Nothing else has been changed.");
      // GitHub #160: no copy is exactly in its imported state any more -> originals from its backup (only when this
      // copy has no import of its own that those files could belong to).
      bool originals_used = false;
      if (used.empty() && !HasWWE15Import(launcher_paths) && !HasWWE13ArenaImport(launcher_paths)) {
        if (!RestoreWWE13OriginalsFromOtherBackup(launcher_paths, game_folder, candidates, progress, cancel, &used,
                                                  &restore_error))
          return Failure(restore_error + " Nothing else has been changed.");
        originals_used = !used.empty();
      }
      if (originals_used) originals_note = (
          "The original WWE 2K14 game files were put back from the backup of the launcher copy in " +
          internal::PathToUtf8(used) + " before importing. That copy's own Undo will no longer work; use this copy from now on.");
      if (!used.empty()) {
        dlc_pac.clear();
        std::ifstream again(dlc_path, std::ios::binary);
        if (again) dlc_pac.assign(std::istreambuf_iterator<char>(again), std::istreambuf_iterator<char>());
        present.clear();
        if (!dlc_pac.empty()) WWE13TargetsInDlcRegistry(dlc_pac, wanted, &present, &registry_error);
      }
    }
    if (!present.empty()) {
      std::string names;
      for (const uint32_t id : present) {
        if (!names.empty()) names += ", ";
        names += by_target.at(id).name;
      }
      return Failure("This game folder already has WWE '13 wrestlers in it (" + names + ") from an earlier import that "
                     "this launcher has no undo record for, usually another copy of the launcher using the same game "
                     "folder. The launcher searched for that copy but could not find one that matches. Open that copy and "
                     "undo its WWE '13 import there, or copy your original WWE 2K14 game files back "
                     "into this game folder, then import here again. Nothing has been changed.");
    }
  }

  WWE13ImportPaths sources;
  sources.wwe13_root = scan.base_folder;
  sources.wwe14_root = game_folder;
  sources.userdata_root = launcher_paths.userdata_dir;
  sources.titantron_cache = launcher_paths.exe_dir / "w13cache";
  {
    fs::path converter = launcher_paths.exe_dir / "wwe2k14-bink-convert";
#ifdef _WIN32
    converter += ".exe";
#endif
    std::error_code converter_error;
    if (fs::is_regular_file(converter, converter_error) && !converter_error) sources.bink_converter_executable = converter;
  }
  for (const WWE13ImportPack& pack : scan.packs) if (!pack.package_file) sources.wwe13_pack_roots.push_back(pack.folder);

  // The deepest file the import installs sits about 115 characters below the launcher, temporary names add about 20, and Windows stops at 260.
  if (internal::PathToUtf8(launcher_paths.exe_dir).size() > 125)
    return Failure("This game folder's location is too long for Windows. Move the folder closer to the top of the drive, for example C:\\Games, and try again. Nothing has been changed.");

  uint64_t package_bytes = 0;
  if (!EstimatePackageBytes(scan.packs, &package_bytes, &error)) return Failure(error);
  // A disc image base: its files are copied into the temporary folder like the add-on packages, so they count the same.
  std::vector<std::string> disc_files;
  if (scan.base_is_disc_image) {
    const std::vector<DiscImageFile> disc = ListDiscImageFiles(scan.base_folder);
    if (disc.empty()) return Failure("The WWE '13 disc image " + internal::PathToUtf8(scan.base_folder) + " could not be read.");
    disc_files = WWE13DiscImportFiles(disc, ports);
    for (const DiscImageFile& file : disc)
      if (std::find(disc_files.begin(), disc_files.end(), file.path) != disc_files.end()) package_bytes += file.size;
  }
  if (progress) progress(0, 1, "Checking free disk space before importing…");
  // The system temp folder has the shortest path, which matters for Windows' 260 limit. Beside the launcher if that drive is small.
  fs::path temp_root;
  {
    std::error_code temp_error;
    fs::path system_temp = fs::temp_directory_path(temp_error);
    std::string ignored_error;
    if (!temp_error && CheckFreeSpace(system_temp, launcher_paths.exe_dir, game_folder, launcher_paths.userdata_dir,
                                      ports.size(), package_bytes, &ignored_error)) {
      temp_root = system_temp;
    } else {
      temp_root = launcher_paths.exe_dir / "w13tmp";
      std::error_code stale;
      fs::remove_all(temp_root, stale);
      fs::create_directories(temp_root, stale);
      if (stale) return Failure("Could not create temporary space for the WWE '13 import.");
      if (!CheckFreeSpace(temp_root, launcher_paths.exe_dir, game_folder, launcher_paths.userdata_dir,
                          ports.size(), package_bytes, &error)) return Failure(error);
    }
  }
  if (cancel.load(std::memory_order_relaxed)) return Failure("WWE '13 import cancelled.");

  // Left by a run that was killed or crashed: temporary files and a backup set nothing points to.
  {
    std::error_code stale;
    for (fs::directory_iterator it(temp_root, fs::directory_options::skip_permission_denied, stale), end; !stale && it != end; it.increment(stale))
      if (internal::PathToUtf8(it->path().filename()).rfind("w13i-", 0) == 0) { std::error_code one; fs::remove_all(it->path(), one); }
    Manifest current;
    bool has_current = false;
    std::string current_error;
    if (ReadManifest(launcher_paths, &current, &has_current, &current_error)) {
      stale.clear();
      for (fs::directory_iterator it(ImportBackupRoot(launcher_paths) / "sets", fs::directory_options::skip_permission_denied, stale), end; !stale && it != end; it.increment(stale))
        if (!has_current || it->path().filename() != fs::u8path(current.id)) { std::error_code one; fs::remove_all(it->path(), one); }
    }
  }
  const std::string token = NewToken();
  const fs::path work = temp_root / ("w13i-" + token);
  const fs::path staged = work / "stage";
  std::error_code ec;
  fs::create_directories(work, ec);
  if (ec) return Failure("Could not create temporary space for the WWE '13 import.");

  Manifest previous;
  bool has_previous = false;
  if (!ReadManifest(launcher_paths, &previous, &has_previous, &error)) { fs::remove_all(work, ec); return Failure(error); }
  Transaction baseline;
  baseline.id = token;
  if (has_previous && !LoadPreviousBaseline(launcher_paths, game_folder, previous, &baseline, progress, cancel, &error)) {
    std::string rollback_error;
    const bool restored = Rollback(&baseline, &rollback_error);
    if (restored) {
      CleanupTransaction(baseline);
      fs::remove_all(work, ec);
    }
    return Failure(restored ? error : rollback_error);
  }

  bool success = false;
  std::vector<std::string> removal_keys;
  try {
    sources.wwe13_pack_roots = ExtractSourcePacks(scan.packs, work / "source-packs", progress, cancel, &error);
    if (error.empty() && scan.base_is_disc_image) {
      const Result copied = CopyDiscImageFiles(scan.base_folder, disc_files, work / "source-base", progress, &cancel);
      if (copied.ok) sources.wwe13_root = work / "source-base";
      else error = copied.error;
    }
    if (error.empty()) {
      WWE13ImportRunResult stage_result;
      WWE13ImportOptions options;
      if (!RunWWE13ImportStages2To9(sources, ports, staged, options, &stage_result, &error, progress, &cancel)) {
        if (error.empty()) error = "The WWE '13 conversion could not be completed.";
      } else {
        // Music and announcer names read the stock files and add to the arcs the stages already rebuilt.
        sources.staged_game_root = staged / "files" / "game";
        WWE13ImportAudioResult audio;
        const fs::path audio_stage = work / "audio";
        if (!RunWWE13ImportAudio(sources, ports, audio_stage, progress, cancel, &audio, &error)) {
          if (error.empty()) error = "The WWE '13 audio import did not finish.";
        } else {
          for (const std::string& relative : audio.files) {
            const fs::path from = audio_stage / "files" / fs::u8path(relative);
            const fs::path to = staged / "files" / fs::u8path(relative);
            std::error_code move_error;
            fs::create_directories(to.parent_path(), move_error);
            if (!move_error) fs::rename(from, to, move_error);
            if (move_error) { error = "Could not prepare the WWE '13 audio files."; break; }
          }
        }
        if (error.empty()) {
          std::sort(ports.begin(), ports.end(), [](const auto& a, const auto& b) { return a.target_id < b.target_id; });
          // The host pack must load first, so it takes the lower name: same package, renamed. Its header names it at +0x108.
          // The old name would otherwise stay beside it as a second pack claiming the same offers.
          {
            // Add-On 2 may have been added under another file name (#103): use the folder the import read it from.
            std::string host_old = FindWWE2K14HostPack(launcher_paths.userdata_dir);
            if (host_old.empty()) host_old = kHostPackOld;
            // The output is the host renamed with '0' first (045BEE31.. from the standard Add-On 2, 067E5576.. from the
            // 4-offer version, #118/#120); it must match the folder the pipeline staged (HostOutputPack).
            const std::string host_new = WWE13HostOutputPack(host_old);
            const fs::path old_pack = launcher_paths.userdata_dir / "0000000000000000/545408B2/00000002" / fs::u8path(host_old);
            std::error_code list_error;
            if (fs::is_directory(old_pack, list_error) && !list_error) {
              for (fs::recursive_directory_iterator it(old_pack, fs::directory_options::skip_permission_denied, list_error), end;
                   !list_error && it != end; it.increment(list_error)) {
                if (!it->is_regular_file(list_error) || list_error) continue;
                const std::string key = std::string(kContentRoot) + host_old + "/" +
                                        it->path().lexically_relative(old_pack).generic_string();
                const fs::path staged_new = staged / "files" / fs::u8path(std::string(kContentRoot) + host_new + "/" +
                                            it->path().lexically_relative(old_pack).generic_string());
                std::error_code exists_error;
                if (!fs::exists(staged_new, exists_error)) {
                  fs::create_directories(staged_new.parent_path(), exists_error);
                  fs::copy_file(it->path(), staged_new, exists_error);
                  if (exists_error) { error = "Could not prepare the WWE '13 add-on pack."; break; }
                }
                removal_keys.push_back(key);
              }
              if (list_error && error.empty()) error = "Could not read the WWE 2K14 add-on pack.";
            }
            const fs::path old_header = launcher_paths.userdata_dir / "0000000000000000/545408B2/Headers/00000002" / fs::u8path(host_old + ".header");
            std::string header;
            // V153 review (Luna): a missing header is an error, never skipped - otherwise the output pack would have no
            // matching header and the old one would stay behind (hand-renamed add-on folders).
            if (error.empty() && !LoadText(old_header, &header))
              error = "The WWE 2K14 add-on's header file was not found (" + host_old + ".header). Add the add-on again on "
                      "the Game files page (Add TU or DLC). Nothing has been changed.";
            if (error.empty()) {
              constexpr size_t kNameAt = 0x108;
              constexpr size_t kNameSize = 42;  // zero-padded name field
              const bool name_matches = header.size() >= kNameAt + kNameSize && host_old.size() <= kNameSize &&
                  header.compare(kNameAt, host_old.size(), host_old) == 0 &&
                  header.substr(kNameAt + host_old.size(), kNameSize - host_old.size()).find_first_not_of('\0') == std::string::npos;
              if (!name_matches) {
                error = "The WWE 2K14 add-on pack's header is not in the expected form.";
              } else {
                std::string name_field = host_new;
                name_field.resize(kNameSize, '\0');
                header.replace(kNameAt, kNameSize, name_field);
                if (!WriteStagedText(staged / "files", std::string(kHeaderRoot) + host_new + ".header", header, &error) && error.empty())
                  error = "Could not prepare the WWE '13 add-on pack.";
                removal_keys.push_back(std::string(kHeaderRoot) + host_old + ".header");
              }
            }
          }
          if (!error.empty()) {
            // reported below
          } else if (!WriteStagedText(staged / "files", "launcher/own-slots.txt", MakeSlotsText(slot_ids), &error) ||
              !WriteStagedText(staged / "files", "launcher/mods/wwe13/roster.tsv", MakeRosterText(ports), &error) ||
              !WriteStagedText(staged / "files", "launcher/mods/wwe13/theme-levels.tsv", [&audio] {
                std::string levels;
                for (const auto& [id, db] : audio.theme_levels) levels += std::to_string(id) + "\t" + std::to_string(db) + "\n";
                return levels;
              }(), &error)) {
            if (error.empty()) error = "Could not prepare the WWE '13 Mods list.";
          } else {
            WWE13ImportSummary applied;
            Result result = ApplyWWE13StagedFiles(launcher_paths, game_folder, staged / "files", progress, cancel, &applied, removal_keys);
            if (!result.ok) error = result.error;
            else {
              success = true;
              if (summary) {
                summary->imported_wrestlers = ports.size();
                summary->lines = stage_result.notices;
                if (!originals_note.empty()) summary->lines.insert(summary->lines.begin(), originals_note);
                if (!audio.songs_without_audio.empty())
                  summary->lines.push_back(std::to_string(audio.songs_without_audio.size()) + " wrestler(s) keep their stand-in theme: no WWE '13 song was found.");
                if (!audio.names_without_audio.empty())
                  summary->lines.push_back(std::to_string(audio.names_without_audio.size()) + " wrestler(s) have no announcer name: no WWE '13 line was found.");
              }
            }
          }
        }
      }
    }
  } catch (const std::exception& exception) {
    error = std::string("Something went wrong while importing WWE '13 wrestlers: ") + exception.what();
  } catch (...) {
    error = "Something went wrong while importing WWE '13 wrestlers.";
  }

  if (!success) {
    // Owner: a cancelled import leaves nothing behind. The kept videos go too.
    std::error_code cache_error;
    fs::remove_all(sources.titantron_cache, cache_error);
    std::string rollback_error;
    const bool restored = Rollback(&baseline, &rollback_error);
    if (restored) CleanupTransaction(baseline);
    fs::remove_all(work, ec);
    return Failure(restored ? (error.empty() ? "The WWE '13 import did not finish." : error) : rollback_error);
  }
  CleanupTransaction(baseline);
  fs::remove_all(work, ec);
  return Result::Ok();
}
}  // namespace

namespace {
struct BackupAreaScope {
  const char* saved;
  explicit BackupAreaScope(const char* area) : saved(g_backup_area) { g_backup_area = area; }
  ~BackupAreaScope() { g_backup_area = saved; }
};
constexpr const char* kArenaBackupArea = "wwe13-arenas";
}  // namespace

Result ApplyWWE13ArenaStagedFiles(const Paths& launcher_paths, const fs::path& game_folder,
                                  const fs::path& staged_files, const ProgressFn& progress, CancelFlag& cancel) {
  BackupAreaScope scope(kArenaBackupArea);
  if (HasWWE13Import(launcher_paths))
    return Failure("Undo the imported WWE '13 arenas before importing arenas again.");
  return ApplyWWE13StagedFiles(launcher_paths, game_folder, staged_files, progress, cancel, nullptr);
}

Result UndoWWE13ArenaImport(const Paths& launcher_paths, const fs::path& game_folder, const ProgressFn& progress,
                            CancelFlag& cancel) {
  BackupAreaScope scope(kArenaBackupArea);
  return UndoWWE13Import(launcher_paths, game_folder, progress, cancel);
}

bool HasWWE13ArenaImport(const Paths& launcher_paths) {
  BackupAreaScope scope(kArenaBackupArea);
  return HasWWE13Import(launcher_paths);
}

}  // namespace wwe2k14::launcher

// ---- WWE 2K15 import: the same transaction with its own undo set (wwe15_install.h)
namespace wwe2k14::launcher {
namespace {
constexpr const char* kWWE15BackupArea = "wwe15-import";
}  // namespace

Result ApplyWWE15StagedFiles(const Paths& launcher_paths, const fs::path& game_folder, const fs::path& staged_files,
                             const ProgressFn& progress, CancelFlag& cancel, const std::vector<std::string>& removal_keys) {
  if (HasWWE13ArenaImport(launcher_paths))
    return Failure("Undo the imported WWE '13 arenas first: they changed some of the same game files.");
  BackupAreaScope scope(kWWE15BackupArea);
  if (HasWWE13Import(launcher_paths))
    return Failure("Undo the WWE 2K15 import before importing WWE 2K15 wrestlers again.");
  return ApplyWWE13StagedFiles(launcher_paths, game_folder, staged_files, progress, cancel, nullptr, removal_keys);
}

Result UndoWWE15Import(const Paths& launcher_paths, const fs::path& game_folder, const ProgressFn& progress,
                       CancelFlag& cancel) {
  if (HasWWE13ArenaImport(launcher_paths))
    return Failure("Undo the imported WWE '13 arenas first: they changed some of the same game files.");
  BackupAreaScope scope(kWWE15BackupArea);
  if (!HasWWE13Import(launcher_paths)) return Failure("There is no WWE 2K15 import to undo.");
  return UndoWWE13Import(launcher_paths, game_folder, progress, cancel);
}

bool HasWWE15Import(const Paths& launcher_paths) {
  BackupAreaScope scope(kWWE15BackupArea);
  return HasWWE13Import(launcher_paths);
}

// Grid-face repair (owner 11-16): the launcher changed a file that an import recorded. Every import record (WWE '13
// wrestlers, WWE '13 arenas, WWE 2K15) whose applied hash for that key equals the old hash takes the new hash, so Undo
// still sees the file as "imported" and restores the original as before.
std::string ImportFileHash(const fs::path& path) {
  std::string ignored;
  return HashFile(path, &ignored);
}

bool RebaseImportRecords(const Paths& paths, const std::map<std::string, std::pair<std::string, std::string>>& changes,
                         std::string* error) {
  for (const char* area : {"wwe13-import", kArenaBackupArea, kWWE15BackupArea}) {
    BackupAreaScope scope(area);
    Manifest manifest;
    bool found = false;
    if (!ReadManifest(paths, &manifest, &found, error)) return false;
    if (!found) continue;
    bool changed = false;
    for (FileRecord& record : manifest.files) {
      const auto it = changes.find(record.key);
      if (it != changes.end() && record.applied_hash == it->second.first) {
        record.applied_hash = it->second.second;
        changed = true;
      }
    }
    std::string reason;
    if (changed && !WriteAtomicText(BackupSet(paths, manifest.id) / "manifest.tsv", SerializeManifest(manifest), NewToken(),
                                    &reason)) {
      if (error) *error = "An undo record could not be updated (" + reason + ").";
      return false;
    }
  }
  return true;
}

// Grid-face repair (v1.6.1 reviews): before the repair changes files, every file must be held by some import record
// at its current bytes (its applied hash), or Undo would refuse afterwards. Imports made before 1.6.1 never changed
// menuHD/SD.pac, so no record holds them: those are adopted - the file's current bytes are saved as the original in
// the record that holds anchor_key at anchor_hash (the import whose pac-list state is the current one), so Undo
// restores the menu PACs together with the pac lists. A file held only at other bytes (another launcher copy's
// import, an older state), or a non-menu file no record holds, fails the check: nothing is changed.
bool AdoptImportRecords(const Paths& paths, const std::map<std::string, std::pair<fs::path, std::string>>& files,
                        const std::string& anchor_key, const std::string& anchor_hash, std::vector<std::string>* adopted,
                        std::string* error) {
  adopted->clear();
  std::set<std::string> held, matched;
  const char* home = nullptr;
  for (const char* area : {kArenaBackupArea, kWWE15BackupArea, "wwe13-import"}) {
    BackupAreaScope scope(area);
    Manifest manifest;
    bool found = false;
    if (!ReadManifest(paths, &manifest, &found, error)) return false;
    if (!found) continue;
    for (const FileRecord& record : manifest.files) {
      held.insert(record.key);
      const auto f = files.find(record.key);
      if (f != files.end() && record.applied_hash == f->second.second) matched.insert(record.key);
      if (!home && record.key == anchor_key && record.applied_hash == anchor_hash) home = area;
    }
  }
  std::vector<std::pair<std::string, fs::path>> missing;
  for (const auto& [key, file] : files) {
    if (matched.count(key)) continue;
    if (held.count(key) || key.rfind("game/pac/menu/", 0) != 0) {
      if (error) *error = "An import undo record does not match " + key.substr(key.rfind('/') + 1) +
                          " (changed outside this launcher copy?); the grid faces were left unchanged.";
      return false;
    }
    missing.emplace_back(key, file.first);
  }
  if (missing.empty()) return true;
  if (!home) {
    if (error) *error = "No import undo record matches the current pac lists; the grid faces were left unchanged.";
    return false;
  }
  BackupAreaScope scope(home);
  Manifest manifest;
  bool found = false;
  if (!ReadManifest(paths, &manifest, &found, error)) return false;
  if (!found) { if (error) *error = "The import undo record disappeared."; return false; }
  const fs::path set = BackupSet(paths, manifest.id);
  CancelFlag never_cancel{false};
  for (const auto& [key, target] : missing) {
    FileRecord record;
    record.key = key;
    record.original_exists = true;
    record.original_kind = 'F';
    const fs::path backup = OriginalArtifact(set, record);
    if (!CopyStream(target, backup, {}, &never_cancel)) {
      if (error) *error = "Could not back up " + internal::PathToUtf8(target) + ".";
      std::error_code ec;
      fs::remove(backup, ec);
      for (const std::string& done : *adopted) {
        FileRecord r;
        r.key = done;
        fs::remove(OriginalArtifact(set, r), ec);
      }
      adopted->clear();
      return false;
    }
    record.original_hash = HashFile(backup, error);
    if (record.original_hash.empty()) return false;
    record.applied_hash = record.original_hash;  // RebaseImportRecords moves it to the repaired file's hash
    manifest.files.push_back(record);
    adopted->push_back(key);
  }
  std::string reason;
  if (!WriteAtomicText(set / "manifest.tsv", SerializeManifest(manifest), NewToken(), &reason)) {
    std::error_code ec;
    for (const std::string& done : *adopted) {
      FileRecord r;
      r.key = done;
      fs::remove(OriginalArtifact(set, r), ec);
    }
    adopted->clear();
    if (error) *error = "An undo record could not be updated (" + reason + ").";
    return false;
  }
  return true;
}

// GitHub #160: the strict restore above skips another copy whose game files are no longer exactly in its imported state
// (the player re-imported or played in between), and then both copies refuse to import. Second chance: put back the
// ORIGINAL game files from that copy's backups, verified byte for byte against the hashes its records saved, whatever
// state the files are in now. Layers: WWE '13 wrestlers first (the oldest import), then WWE 2K15, then arenas; a file
// takes the original of the oldest layer that changed it. Only game/ files (userdata/ is this copy's own); only when
// every needed original is present and correct; all-or-nothing. The other copy's records are left as they are (its Undo
// will then refuse, which is safe).
bool RestoreWWE13OriginalsFromOtherBackup(const Paths& launcher_paths, const fs::path& game_folder,
                                          const std::vector<fs::path>& candidates, const ProgressFn& progress,
                                          CancelFlag& cancel, fs::path* used, std::string* error) {
  used->clear();
  for (const fs::path& folder : candidates) {
    if (cancel.load(std::memory_order_relaxed)) { if (error) *error = "WWE '13 import cancelled."; return false; }
    std::error_code ec;
    if (fs::equivalent(folder, launcher_paths.exe_dir, ec)) continue;
    Paths other = launcher_paths;
    other.exe_dir = folder;
    other.backups_dir = folder / "backups";
    struct Pick { FileRecord record; fs::path set; };
    std::map<std::string, Pick> picks;  // game/ key -> original from the oldest layer
    bool has_wrestlers = false, usable = true;
    for (const char* area : {"wwe13-import", kWWE15BackupArea, kArenaBackupArea}) {
      BackupAreaScope scope(area);
      Manifest manifest;
      bool found = false;
      std::string read_error;
      if (!ReadManifest(other, &manifest, &found, &read_error)) { usable = false; break; }
      if (!found) continue;
      if (std::string(area) == "wwe13-import") has_wrestlers = true;
      const fs::path set = BackupSet(other, manifest.id);
      for (const FileRecord& record : manifest.files) {
        if (record.key.rfind("game/", 0) != 0 || picks.count(record.key)) continue;
        picks[record.key] = {record, set};
      }
    }
    if (!usable || !has_wrestlers || picks.empty()) continue;
    // Every original must be there and match its saved hash before anything is touched.
    for (const auto& [key, pick] : picks) {
      if (TargetPath(launcher_paths, game_folder, key).empty()) { usable = false; break; }
      if (!pick.record.original_exists) continue;
      if (pick.record.original_kind != 'F') { usable = false; break; }
      std::string ignored;
      if (HashFile(OriginalArtifact(pick.set, pick.record), &ignored) != pick.record.original_hash) { usable = false; break; }
    }
    if (!usable) continue;
    Transaction transaction;
    transaction.id = NewToken();
    std::string restore_error;
    bool ok = true;
    uint64_t done = 0;
    for (const auto& [key, pick] : picks) {
      const fs::path target = TargetPath(launcher_paths, game_folder, key);
      if (!StashTarget(&transaction, key, target, &restore_error) ||
          (pick.record.original_exists &&
           !RestoreOriginal(pick.set, pick.record, target, transaction.id, progress, cancel, &restore_error))) {
        ok = false;
        break;
      }
      ++done;
      if (progress) progress(done, picks.size(), "Putting back the original game files from the other launcher copy's backup…");
    }
    if (!ok) {
      std::string rollback_error;
      if (Rollback(&transaction, &rollback_error)) CleanupTransaction(transaction);
      if (error) *error = rollback_error.empty() ? restore_error : rollback_error;
      return false;
    }
    CleanupTransaction(transaction);
    *used = folder;
    return true;
  }
  return true;
}

bool DropAdoptedImportRecords(const Paths& paths, const std::vector<std::string>& adopted, std::string* error) {
  if (adopted.empty()) return true;
  const std::set<std::string> keys(adopted.begin(), adopted.end());
  for (const char* area : {kArenaBackupArea, kWWE15BackupArea, "wwe13-import"}) {
    BackupAreaScope scope(area);
    Manifest manifest;
    bool found = false;
    if (!ReadManifest(paths, &manifest, &found, error)) return false;
    if (!found) continue;
    const size_t before = manifest.files.size();
    std::erase_if(manifest.files, [&](const FileRecord& r) { return keys.count(r.key) && r.original_exists && r.original_kind == 'F'; });
    if (manifest.files.size() == before) continue;
    const fs::path set = BackupSet(paths, manifest.id);
    std::string reason;
    if (!WriteAtomicText(set / "manifest.tsv", SerializeManifest(manifest), NewToken(), &reason)) {
      if (error) *error = "An undo record could not be updated (" + reason + ").";
      return false;
    }
    std::error_code ec;
    for (const std::string& key : adopted) {
      FileRecord r;
      r.key = key;
      fs::remove(OriginalArtifact(set, r), ec);
    }
    return true;
  }
  return true;
}

// Owner 11-18 Mods switch: the originals of every import layer; a key changed by several layers takes the oldest set's.
bool CollectImportOriginals(const Paths& launcher_paths, std::map<std::string, ImportOriginal>* out, std::string* error) {
  out->clear();
  auto older = [](const std::string& a, const std::string& b) {  // set ids are "<ms>-<counter>"
    const auto split = [](const std::string& id) {
      const size_t dash = id.find('-');
      const unsigned long long ms = std::strtoull(id.substr(0, dash).c_str(), nullptr, 10);
      const unsigned long long n = dash == std::string::npos ? 0 : std::strtoull(id.substr(dash + 1).c_str(), nullptr, 10);
      return std::pair<unsigned long long, unsigned long long>(ms, n);
    };
    return split(a) < split(b);
  };
  for (const char* area : {"wwe13-import", kArenaBackupArea, kWWE15BackupArea}) {
    BackupAreaScope scope(area);
    Manifest manifest;
    bool found = false;
    if (!ReadManifest(launcher_paths, &manifest, &found, error)) return false;
    if (!found) continue;
    const fs::path set = BackupSet(launcher_paths, manifest.id);
    for (const FileRecord& record : manifest.files) {
      auto it = out->find(record.key);
      if (it != out->end() && !older(manifest.id, it->second.set_id)) continue;
      ImportOriginal original;
      original.exists = record.original_exists;
      original.kind = record.original_kind;
      original.hash = record.original_hash;
      original.artifact = record.original_exists ? OriginalArtifact(set, record) : fs::path();
      original.set_id = manifest.id;
      (*out)[record.key] = std::move(original);
    }
  }
  return true;
}

}  // namespace wwe2k14::launcher
