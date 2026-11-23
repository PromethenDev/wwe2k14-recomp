#include "launcher_core.h"

#include "internal/util.h"
#include "update.h"
#include "wwe13_convert.h"
#include "wwe13_pipeline.h"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>

namespace wwe2k14::launcher {
namespace {

using Bytes = WWE13ConvertBytes;
constexpr std::array<const char*, 3> kPackFiles{
    "pac/edit/edit_dlc05.pac", "info/catalog_hd.arc", "info/catalog_sd.arc"};
constexpr uint32_t kTemplateCategory = 30;
constexpr uint32_t kMankind = 246;
constexpr uint32_t kFirstStringKey = 60000;
constexpr uint32_t kLastStringKey = 60500;
constexpr uint64_t kMaxFileBytes = 512ull * 1024 * 1024;
std::atomic<uint64_t> g_token{0};

struct RecordInsert {
  uint32_t target_id;
  const char* expected_name;
  uint32_t donor_id;
  const char* donor_table;
};

// HB7's verified 12 records. These are recipes, not game data: donor bytes and names are
// read from the player's own installed game and add-on files.
constexpr std::array<RecordInsert, 12> kHB7Records{{
    {344, "THE ROCK (RETRO)", 100, "EXSS"},
    {346, "UNDERTAKER (RETRO)", 103, "EXSS"},
    {210, "UNDERTAKER (BIKER)", 103, "EXSS"},
    {347, "CHRIS JERICHO (RETRO)", 104, "EXSS"},
    {348, "KANE (RETRO)", 107, "EXSS"},
    {356, "JOHN CENA (RETRO)", 139, "EXSS"},
    {358, "TRIPLE H (RETRO)", 102, "EXSS"},
    {145, "SHAWN MICHAELS (RETRO)", 365, "EXSS"},
    {373, "SHAWN MICHAELS", 365, "EXSS"},
    {375, "SHAWN MICHAELS", 365, "EXSS"},
    {372, "CM PUNK", 131, "EXSS"},
    {144, "MICK FOLEY", 246, "SSH5"},
}};

struct FileHashes {
  std::string original;
  std::string applied;
};
struct Manifest {
  std::string token;
  std::string pack_folder;
  std::array<FileHashes, kPackFiles.size()> files;
};
struct PackCandidate {
  std::string folder;
  fs::path root;
  Bytes edit_pac;
  std::vector<Bytes> catalogs;
  std::vector<WWE13PachLeafData> category_rows;
};
struct TransactionFile {
  fs::path target;
  fs::path snapshot;
};

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

std::string LowerAscii(std::string value) {
  for (char& ch : value)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
  return value;
}

std::string TrimAscii(std::string value) {
  while (!value.empty() && (value.back() == '\0' || value.back() == ' ' || value.back() == '\t' ||
                            value.back() == '\r' || value.back() == '\n'))
    value.pop_back();
  size_t first = 0;
  while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
  if (first) value.erase(0, first);
  return value;
}

std::string NewToken() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  return std::to_string(now) + "-" + std::to_string(g_token.fetch_add(1, std::memory_order_relaxed));
}

fs::path ContentRoot(const Paths& paths) {
  return paths.userdata_dir / "0000000000000000" / "545408B2" / "00000002";
}
fs::path BackupRoot(const Paths& paths) { return paths.backups_dir / "extra-superstar-heads"; }
fs::path CurrentPointer(const Paths& paths) { return BackupRoot(paths) / "current.txt"; }
fs::path BackupSet(const Paths& paths, const std::string& token) {
  return BackupRoot(paths) / "sets" / token;
}

bool SafeFolderId(const std::string& value) {
  return value.size() >= 16 && value.size() <= 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f');
         });
}

bool RegularFile(const fs::path& path) {
  std::error_code ec;
  const fs::file_status status = fs::symlink_status(path, ec);
  return !ec && fs::is_regular_file(status);
}

bool ReadFile(const fs::path& path, Bytes* bytes, std::string* error) {
  if (!bytes) return Fail(error, "A file read buffer is missing.");
  std::error_code ec;
  const uintmax_t size = fs::file_size(path, ec);
  if (ec || size > kMaxFileBytes || size > std::numeric_limits<size_t>::max())
    return Fail(error, "A required game file could not be read or is too large: " + internal::PathToUtf8(path));
  std::ifstream input(path, std::ios::binary);
  if (!input) return Fail(error, "A required game file could not be opened: " + internal::PathToUtf8(path));
  Bytes result(static_cast<size_t>(size));
  if (!result.empty()) input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
  if (!input || static_cast<size_t>(input.gcount()) != result.size())
    return Fail(error, "A required game file could not be read completely: " + internal::PathToUtf8(path));
  *bytes = std::move(result);
  return true;
}

std::string HashBytes(const Bytes& bytes) {
  return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

std::string HashFile(const fs::path& path, std::string* error) {
  if (!RegularFile(path)) {
    if (error) *error = "A required game file is missing or is not a regular file: " + internal::PathToUtf8(path);
    return {};
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error) *error = "A required game file could not be opened: " + internal::PathToUtf8(path);
    return {};
  }
  picosha2::hash256_one_by_one hasher;
  std::array<unsigned char, 1024 * 1024> buffer{};
  for (;;) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) hasher.process(buffer.begin(), buffer.begin() + count);
    if (input.eof()) break;
    if (!input) {
      if (error) *error = "A required game file could not be read: " + internal::PathToUtf8(path);
      return {};
    }
  }
  hasher.finish();
  return picosha2::get_hash_hex_string(hasher);
}

bool WriteBytes(const fs::path& path, const Bytes& bytes) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  output.flush();
  return static_cast<bool>(output);
}

bool CopyFile(const fs::path& source, const fs::path& destination, const ProgressFn& progress,
              CancelFlag& cancel, const std::string& label, std::string* error) {
  std::ifstream input(source, std::ios::binary);
  if (!input) return Fail(error, "Could not open a file needed for the head edit.");
  std::error_code ec;
  const uint64_t total = static_cast<uint64_t>(fs::file_size(source, ec));
  if (ec || total > kMaxFileBytes) return Fail(error, "A file needed for the head edit is too large.");
  fs::create_directories(destination.parent_path(), ec);
  if (ec) return Fail(error, "The head-edit backup folder could not be created.");
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output) return Fail(error, "A head-edit backup file could not be created.");
  std::array<char, 1024 * 1024> buffer{};
  uint64_t copied = 0;
  while (input) {
    if (cancel.load(std::memory_order_relaxed)) return Fail(error, "Adding extra Superstar Heads was cancelled.");
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      output.write(buffer.data(), count);
      if (!output) return Fail(error, "A head-edit backup file could not be written.");
      copied += static_cast<uint64_t>(count);
      if (progress && total) progress(copied, total, label);
    }
  }
  output.flush();
  if (!input.eof() || !output) return Fail(error, "A head-edit backup file could not be completed.");
  return true;
}

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

bool WriteTextAtomic(const fs::path& path, const std::string& text, const std::string& token,
                     std::string* error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return Fail(error, "The extra Superstar Heads Undo record could not be prepared.");
  const fs::path temp = path.parent_path() / (".heads-new-" + token + ".tmp");
  const fs::path old = path.parent_path() / (".heads-old-" + token + ".tmp");
  Bytes data(text.begin(), text.end());
  if (!WriteBytes(temp, data)) return Fail(error, "The extra Superstar Heads Undo record could not be written.");
  const bool had_old = RegularFile(path);
  if (had_old) {
    RenameRetry(path, old, ec);
    if (ec) {
      fs::remove(temp, ec);
      return Fail(error, "The extra Superstar Heads Undo record could not be updated.");
    }
  }
  RenameRetry(temp, path, ec);
  if (ec) {
    std::error_code restore_error;
    if (had_old) RenameRetry(old, path, restore_error);
    fs::remove(temp, restore_error);
    return Fail(error, "The extra Superstar Heads Undo record could not be activated.");
  }
  if (had_old) fs::remove(old, ec);
  return true;
}

bool ReadManifest(const Paths& paths, Manifest* manifest, bool* found, std::string* error) {
  *found = false;
  std::ifstream pointer(CurrentPointer(paths), std::ios::binary);
  if (!pointer) return true;
  std::getline(pointer, manifest->token);
  if (manifest->token.empty() || manifest->token.size() > 64 ||
      !std::all_of(manifest->token.begin(), manifest->token.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || ch == '-';
      }))
    return Fail(error, "The extra Superstar Heads Undo record is invalid.");
  std::ifstream input(BackupSet(paths, manifest->token) / "manifest.tsv", std::ios::binary);
  std::string line;
  if (!input || !std::getline(input, line) || line != "EXTRASUPERSTARHEADS\t1" ||
      !std::getline(input, line) || line.rfind("pack\t", 0) != 0)
    return Fail(error, "The extra Superstar Heads Undo record is missing or damaged.");
  manifest->pack_folder = line.substr(5);
  if (!SafeFolderId(manifest->pack_folder))
    return Fail(error, "The extra Superstar Heads Undo record contains an invalid add-on folder.");
  for (size_t i = 0; i < kPackFiles.size(); ++i) {
    if (!std::getline(input, line)) return Fail(error, "The extra Superstar Heads Undo record is incomplete.");
    std::array<std::string, 3> fields;
    size_t start = 0;
    for (size_t field = 0; field < fields.size(); ++field) {
      const size_t end = line.find('\t', start);
      fields[field] = line.substr(start, end == std::string::npos ? end : end - start);
      if (field + 1 < fields.size() && end == std::string::npos)
        return Fail(error, "The extra Superstar Heads Undo record is damaged.");
      if (field + 1 == fields.size() && end != std::string::npos)
        return Fail(error, "The extra Superstar Heads Undo record is damaged.");
      start = end == std::string::npos ? line.size() : end + 1;
    }
    if (fields[0] != kPackFiles[i] || fields[1].size() != 64 || fields[2].size() != 64 ||
        !std::all_of(fields[1].begin(), fields[1].end(), [](unsigned char c) { return std::isxdigit(c); }) ||
        !std::all_of(fields[2].begin(), fields[2].end(), [](unsigned char c) { return std::isxdigit(c); }))
      return Fail(error, "The extra Superstar Heads Undo record contains an invalid file entry.");
    manifest->files[i] = {fields[1], fields[2]};
  }
  if (std::getline(input, line) && !line.empty())
    return Fail(error, "The extra Superstar Heads Undo record contains extra data.");
  *found = true;
  return true;
}

std::string SerializeManifest(const Manifest& manifest) {
  std::string text = "EXTRASUPERSTARHEADS\t1\npack\t" + manifest.pack_folder + "\n";
  for (size_t i = 0; i < kPackFiles.size(); ++i)
    text += std::string(kPackFiles[i]) + "\t" + manifest.files[i].original + "\t" + manifest.files[i].applied + "\n";
  return text;
}

fs::path PackFile(const Paths& paths, const Manifest& manifest, size_t index) {
  return ContentRoot(paths) / manifest.pack_folder / kPackFiles[index];
}

bool StashAndReplace(const fs::path& target, const Bytes& replacement, const std::string& token,
                     size_t index, std::vector<TransactionFile>* transaction, std::string* error) {
  if (!RegularFile(target)) return Fail(error, "An add-on file changed before it could be safely replaced.");
  const fs::path snapshot = target.parent_path() /
      (".heads-old-" + token + "-" + std::to_string(index) + ".tmp");
  std::error_code ec;
  RenameRetry(target, snapshot, ec);
  if (ec) return Fail(error, "An add-on file could not be safely replaced. Close the game and try again.");
  transaction->push_back({target, snapshot});
  const fs::path temp = target.parent_path() /
      (".heads-new-" + token + "-" + std::to_string(index) + ".tmp");
  if (!WriteBytes(temp, replacement)) {
    fs::remove(temp, ec);
    return Fail(error, "A replacement add-on file could not be written.");
  }
  RenameRetry(temp, target, ec);
  if (ec) {
    fs::remove(temp, ec);
    return Fail(error, "A replacement add-on file could not be put in place.");
  }
  return true;
}

bool StashAndRestore(const fs::path& target, const fs::path& original, const std::string& token,
                     size_t index, const ProgressFn& progress, CancelFlag& cancel,
                     std::vector<TransactionFile>* transaction, std::string* error) {
  if (!RegularFile(target)) return Fail(error, "An add-on file changed before it could be safely restored.");
  const fs::path snapshot = target.parent_path() /
      (".heads-old-" + token + "-" + std::to_string(index) + ".tmp");
  std::error_code ec;
  RenameRetry(target, snapshot, ec);
  if (ec) return Fail(error, "An add-on file could not be safely restored. Close the game and try again.");
  transaction->push_back({target, snapshot});
  const fs::path temp = target.parent_path() /
      (".heads-restore-" + token + "-" + std::to_string(index) + ".tmp");
  if (!CopyFile(original, temp, progress, cancel, "Restoring the original Superstar Heads files…", error)) {
    fs::remove(temp, ec);
    return false;
  }
  RenameRetry(temp, target, ec);
  if (ec) {
    fs::remove(temp, ec);
    return Fail(error, "An original add-on file could not be restored.");
  }
  return true;
}

bool Rollback(std::vector<TransactionFile>* transaction, std::string* error) {
  bool success = true;
  for (auto it = transaction->rbegin(); it != transaction->rend(); ++it) {
    std::error_code ec;
    fs::remove(it->target, ec);
    ec.clear();
    RenameRetry(it->snapshot, it->target, ec);
    if (ec) success = false;
  }
  if (!success) return Fail(error, "Recovery files were kept beside the affected add-on files.");
  return true;
}

void Cleanup(const std::vector<TransactionFile>& transaction) {
  for (const TransactionFile& file : transaction) {
    std::error_code ignored;
    fs::remove(file.snapshot, ignored);
  }
}

bool FindEntry(std::vector<WWE13PacGroupData>* groups, const std::string& group_name,
               const std::string& entry_name, WWE13PacEntryData** found, std::string* error) {
  *found = nullptr;
  for (WWE13PacGroupData& group : *groups) {
    if (TrimAscii(group.name) != group_name) continue;
    for (WWE13PacEntryData& entry : group.entries) {
      if (TrimAscii(entry.name) != entry_name) continue;
      if (*found) return Fail(error, "A Superstar Heads archive contains duplicate table entries.");
      *found = &entry;
    }
  }
  return *found != nullptr || Fail(error, "A required Superstar Heads table was not found.");
}

bool ParseCategory30(const Bytes& raw_table, std::vector<WWE13PachLeafData>* categories,
                     std::vector<WWE13PachLeafData>* category_rows, std::string* error) {
  if (!ReadWWE13PachLeaves(raw_table, categories, error)) return false;
  const WWE13PachLeafData* target = nullptr;
  for (const WWE13PachLeafData& category : *categories) {
    if (category.id != kTemplateCategory) continue;
    if (target) return Fail(error, "A Superstar Heads table contains duplicate category 30 entries.");
    target = &category;
  }
  if (!target) return Fail(error, "The Superstar Heads list is missing category 30.");
  if (!ReadWWE13PachLeaves(target->data, category_rows, error)) return false;
  if (category_rows->empty() || !category_rows->back().data.empty())
    return Fail(error, "The Superstar Heads list has no final empty row.");
  for (size_t i = 0; i + 1 < category_rows->size(); ++i)
    if (category_rows->at(i).data.empty())
      return Fail(error, "The Superstar Heads list contains an unexpected empty row.");
  return true;
}

bool ExtractTemplateRows(const WWE13PacEntryData& entry, std::vector<WWE13PachLeafData>* categories,
                         std::vector<WWE13PachLeafData>* rows, std::string* error) {
  Bytes decoded;
  if (!DecodeWWE13Bpe(entry.data, &decoded, error)) return false;
  return ParseCategory30(decoded, categories, rows, error);
}

bool ReadNames(const Paths& paths, const fs::path& game_folder,
               std::map<uint32_t, std::string>* names, std::string* error) {
  std::vector<fs::path> files{game_folder / "pac/string.pac"};
  std::error_code ec;
  const fs::path content = ContentRoot(paths);
  for (fs::directory_iterator it(content, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec) || ec || !SafeFolderId(internal::PathToUtf8(it->path().filename()))) continue;
    const fs::path pac_dir = it->path() / "pac";
    std::error_code child_ec;
    for (fs::directory_iterator file(pac_dir, fs::directory_options::skip_permission_denied, child_ec), file_end;
         !child_ec && file != file_end; file.increment(child_ec)) {
      const std::string filename = internal::PathToUtf8(file->path().filename());
      if (filename.rfind("string_dlc", 0) == 0 && RegularFile(file->path())) files.push_back(file->path());
    }
  }
  if (ec) return Fail(error, "The installed add-on folders could not be checked for wrestler names.");
  std::sort(files.begin() + 1, files.end());
  std::set<fs::path> seen_paths;
  std::map<uint32_t, std::string> result;
  for (const fs::path& file : files) {
    if (!seen_paths.insert(file).second) continue;
    Bytes pac;
    if (!ReadFile(file, &pac, error)) return false;
    WWE13StringKeyMap values;
    if (!ReadWWE13StringKeyValues(pac, &values, error)) return false;
    for (const auto& [key, value] : values) {
      if (key < kFirstStringKey || key >= kLastStringKey) continue;
      Bytes clean = value;
      while (!clean.empty() && clean.back() == 0) clean.pop_back();
      result.try_emplace(key - kFirstStringKey,
                         TrimAscii(std::string(clean.begin(), clean.end())));
    }
  }
  *names = std::move(result);
  return true;
}

bool ValidateRecord(const Bytes& record, uint32_t expected_id, uint8_t expected_face_dlc,
                    std::string* error) {
  if (record.size() < 248) return Fail(error, "A Superstar Heads donor record is too short.");
  const uint16_t layers = uint16_t(record[0]) | (uint16_t(record[1]) << 8);
  const uint16_t one = uint16_t(record[2]) | (uint16_t(record[3]) << 8);
  const uint32_t id = uint32_t(record[4]) | (uint32_t(record[5]) << 8) |
                      (uint32_t(record[6]) << 16) | (uint32_t(record[7]) << 24);
  if (!layers || one != 1 || id != expected_id || record.size() != 8ull + 40ull * layers + 240ull)
    return Fail(error, "A Superstar Heads donor record has an unsupported layout.");
  size_t faces = 0;
  for (uint16_t i = 0; i < layers; ++i) {
    const size_t at = 8 + size_t(i) * 40;
    const uint16_t category = uint16_t(record[at + 4]) | (uint16_t(record[at + 5]) << 8);
    if (category == 4) {
      ++faces;
      if (record[at + 22] != expected_face_dlc)
        return Fail(error, "A Superstar Heads donor uses an unexpected face part.");
    }
  }
  if (faces != 1) return Fail(error, "A Superstar Heads donor does not have exactly one face part.");
  return true;
}

bool BuildHB7Output(const Paths& paths, const fs::path& game_folder, PackCandidate* candidate,
                    std::array<Bytes, kPackFiles.size()>* output, std::string* error) {
  Bytes base_edit;
  if (!ReadFile(game_folder / "pac/edit/edit.pac", &base_edit, error)) return false;
  std::vector<WWE13PacGroupData> head_groups, base_groups;
  if (!ReadWWE13PacGroups(candidate->edit_pac, &head_groups, error) ||
      !ReadWWE13PacGroups(base_edit, &base_groups, error)) return false;
  WWE13PacEntryData* ssh5 = nullptr;
  WWE13PacEntryData* exss = nullptr;
  if (!FindEntry(&head_groups, "EDIT", "SSH5", &ssh5, error) ||
      !FindEntry(&base_groups, "EDIT", "EXSS", &exss, error)) return false;

  std::vector<WWE13PachLeafData> ssh_categories, ssh_rows, exss_categories, exss_rows;
  if (!ExtractTemplateRows(*ssh5, &ssh_categories, &ssh_rows, error) ||
      !ExtractTemplateRows(*exss, &exss_categories, &exss_rows, error)) return false;
  std::map<uint32_t, Bytes> ssh_by_id, exss_by_id;
  auto index_rows = [&](const std::vector<WWE13PachLeafData>& rows, std::map<uint32_t, Bytes>* indexed) {
    for (size_t i = 0; i + 1 < rows.size(); ++i) {
      const Bytes& record = rows[i].data;
      if (record.size() < 8) return false;
      const uint32_t id = uint32_t(record[4]) | (uint32_t(record[5]) << 8) |
                          (uint32_t(record[6]) << 16) | (uint32_t(record[7]) << 24);
      if (!indexed->emplace(id, record).second) return false;
    }
    return true;
  };
  if (!index_rows(ssh_rows, &ssh_by_id) || !index_rows(exss_rows, &exss_by_id))
    return Fail(error, "A Superstar Heads table contains duplicate or invalid wrestler IDs.");
  if (!ssh_by_id.count(kMankind)) return Fail(error, "The add-on head pack is missing the Mankind head used by the extra heads.");

  std::map<uint32_t, std::string> names;
  if (!ReadNames(paths, game_folder, &names, error)) return false;
  std::set<uint32_t> existing;
  for (const auto& [id, record] : ssh_by_id) {
    (void)record;
    existing.insert(id);
    if (!names.count(id) || names.at(id).empty())
      return Fail(error, "An existing Superstar Head is missing its installed name, so the list cannot be safely reordered.");
  }
  struct NamedRecord { uint32_t id; std::string name; Bytes record; };
  std::vector<NamedRecord> additions;
  additions.reserve(kHB7Records.size());
  for (const RecordInsert& insert : kHB7Records) {
    const auto name = names.find(insert.target_id);
    if (name == names.end() || LowerAscii(name->second) != LowerAscii(insert.expected_name))
      return Fail(error, "A required wrestler name was not found in the installed game files.");
    if (existing.count(insert.target_id))
      return Fail(error, "This add-on already contains one or more extra Superstar Heads. Restore its original files first.");
    const auto& donors = std::string_view(insert.donor_table) == "EXSS" ? exss_by_id : ssh_by_id;
    const auto donor = donors.find(insert.donor_id);
    if (donor == donors.end()) return Fail(error, "A required Superstar Heads donor record is missing.");
    const uint8_t expected_face_dlc = std::string_view(insert.donor_table) == "EXSS" ? 0 : 5;
    if (!ValidateRecord(donor->second, insert.donor_id, expected_face_dlc, error)) return false;
    Bytes clone = donor->second;
    clone[4] = static_cast<uint8_t>(insert.target_id);
    clone[5] = static_cast<uint8_t>(insert.target_id >> 8);
    clone[6] = static_cast<uint8_t>(insert.target_id >> 16);
    clone[7] = static_cast<uint8_t>(insert.target_id >> 24);
    additions.push_back({insert.target_id, name->second, std::move(clone)});
    existing.insert(insert.target_id);
  }

  std::vector<NamedRecord> rows;
  rows.reserve(ssh_rows.size() - 1 + additions.size());
  for (size_t i = 0; i + 1 < ssh_rows.size(); ++i) {
    const uint32_t id = uint32_t(ssh_rows[i].data[4]) | (uint32_t(ssh_rows[i].data[5]) << 8) |
                        (uint32_t(ssh_rows[i].data[6]) << 16) | (uint32_t(ssh_rows[i].data[7]) << 24);
    rows.push_back({id, names.at(id), ssh_rows[i].data});
  }
  rows.insert(rows.end(), additions.begin(), additions.end());
  std::sort(rows.begin(), rows.end(), [](const NamedRecord& a, const NamedRecord& b) {
    const std::string left = LowerAscii(a.name);
    const std::string right = LowerAscii(b.name);
    return left == right ? a.id < b.id : left < right;
  });
  std::vector<WWE13PachLeafData> sorted_rows;
  sorted_rows.reserve(rows.size() + 1);
  for (size_t i = 0; i < rows.size(); ++i)
    sorted_rows.push_back({static_cast<uint32_t>(i), std::move(rows[i].record)});
  sorted_rows.push_back({static_cast<uint32_t>(sorted_rows.size()), {}});  // empty terminator remains last
  Bytes new_category;
  if (!BuildWWE13PachLeaves(sorted_rows, &new_category, false, error)) return false;
  for (WWE13PachLeafData& category : ssh_categories)
    if (category.id == kTemplateCategory) category.data = std::move(new_category);
  Bytes new_table;
  if (!BuildWWE13PachLeaves(ssh_categories, &new_table, true, error)) return false;
  // HB7's verified PAC replacement stores SSH5 as the rebuilt nested PACH bytes,
  // without the original outer BPE wrapper. DecodeWWE13Bpe accepts either form.
  ssh5->data = std::move(new_table);

  Bytes new_edit;
  if (!RebuildWWE13PacGroups(candidate->edit_pac, head_groups, &new_edit, error)) return false;
  std::vector<Bytes> arcs = candidate->catalogs;
  if (!SyncWWE13PacArchives(candidate->edit_pac, new_edit, &arcs, error, true)) return false;
  (*output)[0] = std::move(new_edit);
  (*output)[1] = std::move(arcs[0]);
  (*output)[2] = std::move(arcs[1]);
  return true;
}

bool ReadCandidate(const fs::path& root, const std::string& folder, PackCandidate* candidate,
                   std::string* error) {
  PackCandidate result;
  result.folder = folder;
  result.root = root;
  for (const char* relative : kPackFiles)
    if (!RegularFile(root / relative)) return Fail(error, "The Superstar Heads add-on is missing a required file.");
  if (!ReadFile(root / kPackFiles[0], &result.edit_pac, error)) return false;
  for (size_t i = 1; i < kPackFiles.size(); ++i) {
    Bytes arc;
    if (!ReadFile(root / kPackFiles[i], &arc, error)) return false;
    result.catalogs.push_back(std::move(arc));
  }
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(result.edit_pac, &groups, error)) return false;
  WWE13PacEntryData* ssh5 = nullptr;
  if (!FindEntry(&groups, "EDIT", "SSH5", &ssh5, error)) return false;
  std::vector<WWE13PachLeafData> categories;
  if (!ExtractTemplateRows(*ssh5, &categories, &result.category_rows, error)) return false;
  bool has_mankind = false;
  std::set<uint32_t> ids;
  for (size_t i = 0; i + 1 < result.category_rows.size(); ++i) {
    const Bytes& record = result.category_rows[i].data;
    if (record.size() < 8) return Fail(error, "The Superstar Heads add-on has an invalid head record.");
    const uint32_t id = uint32_t(record[4]) | (uint32_t(record[5]) << 8) |
                        (uint32_t(record[6]) << 16) | (uint32_t(record[7]) << 24);
    if (!ids.insert(id).second) return Fail(error, "The Superstar Heads add-on has duplicate head records.");
    has_mankind = has_mankind || id == kMankind;
  }
  if (!has_mankind) return Fail(error, "The Superstar Heads add-on is missing the Mankind head used by the extra heads.");
  std::string detail;
  if (!SyncWWE13PacArchives(result.edit_pac, result.edit_pac, &result.catalogs, &detail, true))
    return Fail(error, "The Superstar Heads add-on catalogs do not match its files.");
  *candidate = std::move(result);
  return true;
}

bool FindHeadPack(const Paths& paths, PackCandidate* candidate, std::string* error) {
  std::error_code ec;
  std::vector<PackCandidate> valid;
  const fs::path root = ContentRoot(paths);
  if (!fs::is_directory(root, ec) || ec)
    return Fail(error, "The Superstar Heads add-on files were not found in this launcher's userdata folder.");
  bool found_candidate_folder = false;
  for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    const fs::file_status status = it->symlink_status(ec);
    if (ec) break;
    if (!fs::is_directory(status)) continue;
    const std::string folder = internal::PathToUtf8(it->path().filename());
    if (!SafeFolderId(folder)) continue;
    size_t present = 0;
    for (const char* relative : kPackFiles) present += RegularFile(it->path() / relative) ? 1 : 0;
    if (present == 0) continue;
    found_candidate_folder = true;
    if (present != kPackFiles.size()) continue;
    PackCandidate parsed;
    std::string detail;
    if (ReadCandidate(it->path(), folder, &parsed, &detail)) valid.push_back(std::move(parsed));
  }
  if (ec) return Fail(error, "The installed add-on folders could not be checked.");
  if (valid.size() > 1)
    return Fail(error, "More than one Superstar Heads add-on was found. Keep one installed copy and try again.");
  if (valid.empty()) {
    return Fail(error, found_candidate_folder
                           ? "The Superstar Heads add-on files could not be verified. Restore the add-on files and try again."
                           : "The Superstar Heads add-on files were not found in this launcher's userdata folder.");
  }
  *candidate = std::move(valid.front());
  return true;
}

bool PackHasAddedIds(const PackCandidate& candidate, bool* any, std::string* error) {
  *any = false;
  for (size_t i = 0; i + 1 < candidate.category_rows.size(); ++i) {
    const Bytes& record = candidate.category_rows[i].data;
    const uint32_t id = uint32_t(record[4]) | (uint32_t(record[5]) << 8) |
                        (uint32_t(record[6]) << 16) | (uint32_t(record[7]) << 24);
    if (std::any_of(kHB7Records.begin(), kHB7Records.end(), [id](const RecordInsert& insert) {
          return insert.target_id == id;
        })) *any = true;
  }
  (void)error;
  return true;
}

bool ActiveOutputMatches(const Paths& paths, const Manifest& manifest, bool* matches,
                         bool* changed, std::string* error) {
  *matches = true;
  *changed = false;
  for (size_t i = 0; i < kPackFiles.size(); ++i) {
    const std::string hash = HashFile(PackFile(paths, manifest, i), error);
    if (hash.empty()) return false;
    if (hash != manifest.files[i].applied) {
      *matches = false;
      *changed = true;
    }
  }
  return true;
}

}  // namespace

ExtraSuperstarHeadsStatus InspectExtraSuperstarHeads(const Paths& paths, const fs::path& game_folder) {
  ExtraSuperstarHeadsStatus status;
  Manifest manifest;
  bool found = false;
  std::string error;
  if (!ReadManifest(paths, &manifest, &found, &error)) {
    status.applied = true;
    status.changed = true;
    status.message = error;
    return status;
  }
  if (found) {
    bool matches = false;
    if (!ActiveOutputMatches(paths, manifest, &matches, &status.changed, &error)) {
      status.applied = true;
      status.changed = true;
      status.message = error;
      return status;
    }
    status.available = true;
    status.applied = true;
    if (status.changed)
      status.message = "An add-on file changed after the extra heads were added. Undo will leave changed files untouched.";
    return status;
  }
  PackCandidate candidate;
  if (!FindHeadPack(paths, &candidate, &error)) {
    status.message = error;
    return status;
  }
  if (!RegularFile(game_folder / "pac/edit/edit.pac")) {
    status.message = "The game files needed for the extra heads were not found. Check the game folder in Game files.";
    return status;
  }
  bool any_added = false;
  PackHasAddedIds(candidate, &any_added, &error);
  if (any_added) {
    status.message = "This add-on already contains one or more extra heads but has no Undo record. Restore its original files first.";
    return status;
  }
  status.available = true;
  return status;
}

Result ApplyExtraSuperstarHeads(const Paths& paths, const fs::path& game_folder,
                                const ProgressFn& progress, CancelFlag& cancel) {
  if (GameProcessRunning(paths)) return Result::Fail("Close WWE 2K14 before adding or undoing extra Superstar Heads.");
  Manifest current;
  bool has_current = false;
  std::string error;
  if (!ReadManifest(paths, &current, &has_current, &error)) return Result::Fail(error);
  if (has_current) {
    bool matches = false, changed = false;
    if (!ActiveOutputMatches(paths, current, &matches, &changed, &error)) return Result::Fail(error);
    if (matches) return Result::Ok();
    return Result::Fail("An add-on file changed after the extra heads were added. Undo was stopped before changing anything.");
  }
  PackCandidate candidate;
  if (!FindHeadPack(paths, &candidate, &error)) return Result::Fail(error);
  if (!RegularFile(game_folder / "pac/edit/edit.pac"))
    return Result::Fail("The game files needed for the extra heads were not found. Check the game folder in Game files.");
  bool any_added = false;
  PackHasAddedIds(candidate, &any_added, &error);
  if (any_added)
    return Result::Fail("This add-on already contains one or more extra heads but has no Undo record. Restore its original files first.");
  if (cancel.load(std::memory_order_relaxed)) return Result::Fail("Adding extra Superstar Heads was cancelled.");

  std::array<Bytes, kPackFiles.size()> outputs;
  if (!BuildHB7Output(paths, game_folder, &candidate, &outputs, &error)) return Result::Fail(error);
  Manifest next;
  next.token = NewToken();
  next.pack_folder = candidate.folder;
  const fs::path set = BackupSet(paths, next.token);
  std::error_code ec;
  fs::create_directories(set / "original", ec);
  if (ec) return Result::Fail("The extra Superstar Heads backup folder could not be created.");
  for (size_t i = 0; i < kPackFiles.size(); ++i) {
    const fs::path target = candidate.root / kPackFiles[i];
    Bytes original;
    if (!ReadFile(target, &original, &error)) {
      fs::remove_all(set, ec);
      return Result::Fail(error);
    }
    next.files[i] = {HashBytes(original), HashBytes(outputs[i])};
    if (cancel.load(std::memory_order_relaxed) ||
        !WriteBytes(set / "original" / (std::to_string(i) + ".bin"), original)) {
      fs::remove_all(set, ec);
      return Result::Fail(cancel.load(std::memory_order_relaxed)
                              ? "Adding extra Superstar Heads was cancelled."
                              : "The original Superstar Heads files could not be backed up.");
    }
    if (progress) progress(i + 1, kPackFiles.size(), "Backing up original Superstar Heads files…");
  }
  const std::string manifest_text = SerializeManifest(next);
  if (!WriteBytes(set / "manifest.tsv", Bytes(manifest_text.begin(), manifest_text.end()))) {
    fs::remove_all(set, ec);
    return Result::Fail("The extra Superstar Heads Undo record could not be saved.");
  }
  std::vector<TransactionFile> transaction;
  for (size_t i = 0; i < kPackFiles.size(); ++i) {
    if (cancel.load(std::memory_order_relaxed) ||
        !StashAndReplace(candidate.root / kPackFiles[i], outputs[i], next.token,
                         i, &transaction, &error)) {
      if (error.empty()) error = "Adding extra Superstar Heads was cancelled.";
      std::string rollback_error;
      if (!Rollback(&transaction, &rollback_error)) return Result::Fail(rollback_error);
      Cleanup(transaction);
      fs::remove_all(set, ec);
      return Result::Fail(error);
    }
    if (progress) progress(i + 1, kPackFiles.size(), "Adding extra Superstar Heads…");
  }
  if (cancel.load(std::memory_order_relaxed) ||
      !WriteTextAtomic(CurrentPointer(paths), next.token + "\n", next.token, &error)) {
    if (error.empty()) error = "Adding extra Superstar Heads was cancelled.";
    std::string rollback_error;
    if (!Rollback(&transaction, &rollback_error)) return Result::Fail(rollback_error);
    Cleanup(transaction);
    fs::remove_all(set, ec);
    return Result::Fail(error);
  }
  Cleanup(transaction);
  return Result::Ok();
}

Result UndoExtraSuperstarHeads(const Paths& paths, const ProgressFn& progress, CancelFlag& cancel) {
  if (GameProcessRunning(paths)) return Result::Fail("Close WWE 2K14 before adding or undoing extra Superstar Heads.");
  Manifest manifest;
  bool found = false;
  std::string error;
  if (!ReadManifest(paths, &manifest, &found, &error)) return Result::Fail(error);
  if (!found) return Result::Ok();
  std::array<fs::path, kPackFiles.size()> originals;
  for (size_t i = 0; i < kPackFiles.size(); ++i) {
    const fs::path target = PackFile(paths, manifest, i);
    const std::string hash = HashFile(target, &error);
    if (hash.empty()) return Result::Fail(error);
    if (hash != manifest.files[i].applied)
      return Result::Fail("An add-on file changed after the extra heads were added. Undo stopped before changing anything.");
    originals[i] = BackupSet(paths, manifest.token) / "original" / (std::to_string(i) + ".bin");
    const std::string original_hash = HashFile(originals[i], &error);
    if (original_hash.empty()) return Result::Fail("The saved original Superstar Heads files are missing or damaged.");
    if (original_hash != manifest.files[i].original)
      return Result::Fail("A saved original Superstar Heads file is damaged. Undo stopped before changing anything.");
  }
  std::vector<TransactionFile> transaction;
  for (size_t i = 0; i < kPackFiles.size(); ++i) {
    if (cancel.load(std::memory_order_relaxed) ||
        !StashAndRestore(PackFile(paths, manifest, i), originals[i], manifest.token,
                         i, progress, cancel, &transaction, &error)) {
      if (error.empty()) error = "Restoring the original Superstar Heads files was cancelled.";
      std::string rollback_error;
      if (!Rollback(&transaction, &rollback_error)) return Result::Fail(rollback_error);
      Cleanup(transaction);
      return Result::Fail(error);
    }
  }
  if (cancel.load(std::memory_order_relaxed)) {
    std::string rollback_error;
    if (!Rollback(&transaction, &rollback_error)) return Result::Fail(rollback_error);
    Cleanup(transaction);
    return Result::Fail("Restoring the original Superstar Heads files was cancelled.");
  }
  std::error_code ec;
  fs::remove(CurrentPointer(paths), ec);
  if (ec) {
    std::string rollback_error;
    if (!Rollback(&transaction, &rollback_error)) return Result::Fail(rollback_error);
    Cleanup(transaction);
    return Result::Fail("The extra Superstar Heads Undo record could not be removed.");
  }
  Cleanup(transaction);
  fs::remove_all(BackupSet(paths, manifest.token), ec);
  return Result::Ok();
}

}  // namespace wwe2k14::launcher
