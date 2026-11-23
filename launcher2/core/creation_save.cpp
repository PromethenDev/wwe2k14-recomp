#include "launcher_core.h"

#include "internal/archive.h"
#include "internal/creation_save.h"
#include "internal/stfs.h"
#include "internal/util.h"
#include "update.h"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace wwe2k14::launcher {
namespace {

constexpr std::string_view kAccountId = "B13EBABEBABEBABE";
constexpr std::string_view kTitleId = "545408B2";
constexpr std::string_view kContainerMagic = "ZLIB";
constexpr uint32_t kContainerVersion = 0x3012;
constexpr size_t kMainContainerOffset = 0x2CA000;
constexpr size_t kHeaderContentTypeOffset = 0x04;
constexpr size_t kHeaderDisplayNameOffset = 0x08;
constexpr size_t kHeaderDisplayNameSize = 0x100;
constexpr size_t kHeaderPackageNameOffset = 0x108;
constexpr size_t kHeaderPackageNameSize = 0x30;
constexpr size_t kHeaderTitleIdOffset = 0x140;
constexpr size_t kCreationHeaderSize = 0x148;
constexpr uint32_t kSaveContentType = 1;
constexpr uint32_t kTitleIdValue = 0x545408B2;
constexpr std::string_view kCreationJournalPrefix = ".creation-journal-";
constexpr std::string_view kCreationOldMarker = ".creation-old-";
constexpr std::string_view kCreationNewMarker = ".creation-new-";

struct ContainerView {
  size_t chunk_offset = 0;
  std::vector<uint8_t> plain;
};

bool ReadLe32(const std::vector<uint8_t>& bytes, size_t offset, uint32_t* value) {
  if (offset > bytes.size() || bytes.size() - offset < 4) return false;
  *value = static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
  return true;
}

void WriteLe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 16);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value >> 24);
}

std::optional<ContainerView> Unpack(const std::vector<uint8_t>& file) {
  const auto marker = std::search(file.begin(), file.end(), kContainerMagic.begin(), kContainerMagic.end());
  if (marker == file.end()) return std::nullopt;
  const size_t offset = static_cast<size_t>(marker - file.begin());
  if (offset > file.size() || file.size() - offset < 16) return std::nullopt;
  uint32_t version = 0, compressed_size = 0, plain_size = 0;
  if (!ReadLe32(file, offset + 4, &version) || !ReadLe32(file, offset + 8, &compressed_size) ||
      !ReadLe32(file, offset + 12, &plain_size) || version != kContainerVersion ||
      offset + 16ull + compressed_size != file.size() || plain_size == 0 ||
      plain_size > static_cast<uint32_t>((std::numeric_limits<int>::max)())) {
    return std::nullopt;
  }
  ContainerView result;
  result.chunk_offset = offset;
  result.plain.resize(plain_size);
  mz_ulong actual_size = static_cast<mz_ulong>(result.plain.size());
  const int status = mz_uncompress(result.plain.data(), &actual_size, file.data() + offset + 16,
                                   static_cast<mz_ulong>(compressed_size));
  if (status != MZ_OK || actual_size != plain_size) return std::nullopt;
  return result;
}

fs::path MainSavePath(const Paths& paths) {
  return paths.userdata_dir / std::string(kAccountId) / std::string(kTitleId) /
         "00000001" / "SaveData.dat" / "SaveData.Dat";
}

fs::path MainHeaderPath(const Paths& paths) {
  return paths.userdata_dir / std::string(kAccountId) / std::string(kTitleId) /
         "Headers" / "00000001" / "SaveData.dat.header";
}

fs::path ContentRoot(const Paths& paths) {
  return paths.userdata_dir / std::string(kAccountId) / std::string(kTitleId) / "00000001";
}

fs::path HeadersRoot(const Paths& paths) {
  return paths.userdata_dir / std::string(kAccountId) / std::string(kTitleId) /
         "Headers" / "00000001";
}

bool IsRegularNonSymlink(const fs::path& path) {
  std::error_code error;
  const fs::file_status status = fs::symlink_status(path, error);
  return !error && fs::is_regular_file(status) && !fs::is_symlink(status);
}

bool IsRegularUnderUserdataNoSymlink(const fs::path& userdata_root, const fs::path& target) {
  if (userdata_root.empty() || target.empty()) return false;
  std::error_code error;
  const fs::file_status root_status = fs::symlink_status(userdata_root, error);
  if (error || fs::is_symlink(root_status) || !fs::is_directory(root_status)) return false;
  const fs::path relative = target.lexically_relative(userdata_root);
  if (relative.empty() || relative.is_absolute() || relative.has_root_name()) return false;
  fs::path current = userdata_root;
  auto component = relative.begin();
  for (; component != relative.end(); ++component) {
    if (*component == "." || *component == "..") return false;
    current /= *component;
    const bool final = std::next(component) == relative.end();
    error.clear();
    const fs::file_status status = fs::symlink_status(current, error);
    if (error || fs::is_symlink(status)) return false;
    if (final) return fs::is_regular_file(status);
    if (!fs::is_directory(status)) return false;
  }
  return false;
}

bool IsPathWithin(const fs::path& candidate, const fs::path& root) {
  std::error_code error;
  const fs::path canonical_root = fs::weakly_canonical(root, error);
  if (error) return true;
  const fs::path canonical_candidate = fs::weakly_canonical(candidate, error);
  if (error) return true;
  auto equal_root = [](const fs::path& left, const fs::path& right) {
    std::string a = internal::PathToUtf8(left), b = internal::PathToUtf8(right);
    if (a.size() == 2 && b.size() == 2 && a[1] == ':' && b[1] == ':') {
      a[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(a[0])));
      b[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(b[0])));
    }
    return a == b;
  };
  if (!equal_root(canonical_candidate.root_name(), canonical_root.root_name()) ||
      canonical_candidate.root_directory() != canonical_root.root_directory()) return false;
  const fs::path relative = canonical_candidate.lexically_relative(canonical_root);
  if (relative.empty()) return canonical_candidate == canonical_root;
  if (relative.is_absolute()) return false;
  const auto first = relative.begin();
  return first == relative.end() || *first != "..";
}

bool IsCreationFolderName(const fs::path& folder_path, bool superstar) {
  const std::string folder = internal::LowerAscii(internal::PathToUtf8(folder_path.filename()));
  const std::string required_name = superstar ? "customsuperstar.cas" : "customarena.car";
  return folder.size() == required_name.size() + 2 &&
         std::isdigit(static_cast<unsigned char>(folder[0])) &&
         std::isdigit(static_cast<unsigned char>(folder[1])) && folder.substr(2) == required_name;
}

bool IsCreationPackageFile(const fs::path& package_path, bool superstar) {
  const std::string extension = internal::LowerAscii(internal::PathToUtf8(package_path.extension()));
  return extension == (superstar ? ".cas" : ".car");
}

// Community downloads unpack to <name>/545408B2/00000001/NNCustomSuperStar.cas/SaveData.Dat (+ Headers/): a picked
// parent folder is accepted when exactly one creation folder of the requested kind lies within a few levels.
bool FindSingleCreationFolder(const fs::path& root, bool superstar, fs::path* found) {
  std::error_code error;
  fs::recursive_directory_iterator it(root, fs::directory_options::none, error), end;
  if (error) return false;
  size_t matches = 0;
  for (; it != end; it.increment(error)) {
    if (error) return false;
    if (it.depth() > 4) {
      it.disable_recursion_pending();
      continue;
    }
    const fs::file_status status = it->symlink_status(error);
    if (error || fs::is_symlink(status) || !fs::is_directory(status)) continue;
    if (IsCreationFolderName(it->path(), superstar) && IsRegularNonSymlink(it->path() / "SaveData.Dat")) {
      if (++matches > 1) return false;
      *found = it->path();
      it.disable_recursion_pending();
    }
  }
  return matches == 1;
}

bool ResolveCreationSave(const fs::path& input, bool superstar, fs::path* resolved) {
  std::error_code error;
  fs::file_status status = fs::symlink_status(input, error);
  if (error || fs::is_symlink(status)) return false;
  if (fs::is_directory(status)) {
    fs::path folder = input;
    if (!IsCreationFolderName(folder, superstar) && !FindSingleCreationFolder(input, superstar, &folder)) return false;
    *resolved = folder / "SaveData.Dat";
  } else if (fs::is_regular_file(status) &&
             internal::LowerAscii(internal::PathToUtf8(input.filename())) == "savedata.dat") {
    if (!IsCreationFolderName(input.parent_path(), superstar)) return false;
    *resolved = input;
  } else {
    return false;
  }
  return IsRegularNonSymlink(*resolved);
}

std::string TwoDigitSlot(uint32_t slot) {
  std::string result = std::to_string(slot);
  if (result.size() < 2) result.insert(result.begin(), '0');
  return result;
}

std::string CreationPackageName(bool superstar, uint32_t slot) {
  return TwoDigitSlot(slot) + (superstar ? "CustomSuperStar.cas" : "CustomArena.car");
}

uint32_t ReadBe32(const std::vector<uint8_t>& bytes, size_t offset) {
  return (static_cast<uint32_t>(bytes[offset]) << 24) |
         (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<uint32_t>(bytes[offset + 3]);
}

void WriteUtf16BeUnit(std::vector<uint8_t>* bytes, size_t offset, uint16_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value);
}

bool FindSourceHeader(const fs::path& source_folder, const std::string& source_name,
                      fs::path* header_path) {
  const fs::path title_root = source_folder.parent_path().parent_path();
  const std::array<fs::path, 4> candidates{
      title_root / "Headers" / "00000001" / (source_name + ".header"),
      source_folder.parent_path() / "Headers" / "00000001" / (source_name + ".header"),
      source_folder.parent_path() / "Headers" / (source_name + ".header"),
      source_folder.parent_path() / (source_name + ".header")};
  for (const fs::path& candidate : candidates) {
    if (IsRegularNonSymlink(candidate)) {
      *header_path = candidate;
      return true;
    }
  }
  return false;
}

void WriteBe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value >> 24);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 16);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value);
}

bool EncodeUtf8AsUtf16Be(std::string_view input, std::vector<uint8_t>* output) {
  output->clear();
  auto append_unit = [&](uint16_t unit) {
    output->push_back(static_cast<uint8_t>(unit >> 8));
    output->push_back(static_cast<uint8_t>(unit));
  };
  for (size_t offset = 0; offset < input.size();) {
    const uint8_t first = static_cast<uint8_t>(input[offset]);
    uint32_t codepoint = 0;
    size_t count = 0;
    if (first <= 0x7F) {
      codepoint = first;
      count = 1;
    } else if ((first & 0xE0) == 0xC0) {
      codepoint = first & 0x1F;
      count = 2;
    } else if ((first & 0xF0) == 0xE0) {
      codepoint = first & 0x0F;
      count = 3;
    } else if ((first & 0xF8) == 0xF0) {
      codepoint = first & 0x07;
      count = 4;
    } else {
      return false;
    }
    if (offset + count > input.size()) return false;
    for (size_t index = 1; index < count; ++index) {
      const uint8_t continuation = static_cast<uint8_t>(input[offset + index]);
      if ((continuation & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3F);
    }
    if ((count == 2 && codepoint < 0x80) || (count == 3 && codepoint < 0x800) ||
        (count == 4 && codepoint < 0x10000) || codepoint == 0 || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return false;
    }
    if (codepoint <= 0xFFFF) {
      append_unit(static_cast<uint16_t>(codepoint));
    } else {
      codepoint -= 0x10000;
      append_unit(static_cast<uint16_t>(0xD800 + (codepoint >> 10)));
      append_unit(static_cast<uint16_t>(0xDC00 + (codepoint & 0x3FF)));
    }
    offset += count;
  }
  return true;
}

bool BuildCreationPackageHeader(const internal::StfsMetadata& metadata,
                               const std::string& package_name, bool superstar,
                               std::vector<uint8_t>* header) {
  if (metadata.content_type != kSaveContentType || metadata.title_id != kTitleIdValue ||
      package_name.size() >= kHeaderPackageNameSize || package_name.size() < 2 ||
      !std::isdigit(static_cast<unsigned char>(package_name[0])) ||
      !std::isdigit(static_cast<unsigned char>(package_name[1]))) {
    return false;
  }
  std::string display_name = metadata.display_name;
  if (display_name.empty()) {
    const uint32_t source_slot = static_cast<uint32_t>(package_name[0] - '0') * 10 +
                                 static_cast<uint32_t>(package_name[1] - '0');
    display_name = TwoDigitSlot(source_slot + 1) +
                   (superstar ? ".CUSTOM SUPERSTAR" : ".CUSTOM ARENA");
  }
  std::vector<uint8_t> display_bytes;
  if (!EncodeUtf8AsUtf16Be(display_name, &display_bytes) ||
      display_bytes.size() + 2 > kHeaderDisplayNameSize) {
    return false;
  }
  header->assign(kCreationHeaderSize, 0);
  WriteBe32(header, 0, 1);  // HDD device ID from the extracted creation sidecar header.
  WriteBe32(header, kHeaderContentTypeOffset, kSaveContentType);
  std::copy(display_bytes.begin(), display_bytes.end(),
            header->begin() + static_cast<std::ptrdiff_t>(kHeaderDisplayNameOffset));
  std::copy(package_name.begin(), package_name.end(),
            header->begin() + static_cast<std::ptrdiff_t>(kHeaderPackageNameOffset));
  WriteBe32(header, kHeaderTitleIdOffset, metadata.title_id);
  std::fill(header->begin() + 0x144, header->end(), 0xFF);
  return true;
}

bool ExtractCreationConPackageImpl(const fs::path& package, bool superstar,
                                   std::vector<uint8_t>* save_data,
                                   std::vector<uint8_t>* header, std::string* error) {
  if (!save_data || !header || !IsCreationPackageFile(package, superstar)) {
    if (error) *error = superstar ? "Choose a .cas CON package." : "Choose a .car CON package.";
    return false;
  }
  std::ifstream signature_stream(package, std::ios::binary);
  std::array<char, 4> signature{};
  if (!signature_stream.read(signature.data(), signature.size()) ||
      std::string_view(signature.data(), signature.size()) != "CON ") {
    if (error) *error = "The selected .cas/.car file is not an Xbox 360 CON package.";
    return false;
  }

  internal::StfsReader stfs;
  if (!stfs.Open(package, error)) return false;
  if (stfs.metadata().content_type != kSaveContentType ||
      stfs.metadata().title_id != kTitleIdValue) {
    if (error) *error = "This CON package is not a WWE 2K14 creation save.";
    return false;
  }
  const internal::StfsEntry* payload = nullptr;
  for (const internal::StfsEntry& entry : stfs.entries()) {
    if (entry.directory) continue;
    const std::string relative_name = internal::LowerAscii(
        internal::PathToUtf8(entry.relative_path.filename()));
    if (relative_name != "savedata.dat" || !entry.relative_path.parent_path().empty() || payload) {
      if (error) *error = "The CON package must contain exactly one root SaveData.Dat file.";
      return false;
    }
    payload = &entry;
  }
  if (!payload) {
    if (error) *error = "The CON package does not contain SaveData.Dat.";
    return false;
  }

  std::ostringstream output(std::ios::out | std::ios::binary);
  if (!stfs.CopyFile(*payload, output, {}, error)) return false;
  const std::string extracted = output.str();
  save_data->assign(extracted.begin(), extracted.end());
  // Arbitrary user-facing export names are supported. The importer materializes those packages under this
  // canonical temporary name; existing NNCustomSuperStar.cas / NNCustomArena.car packages keep their name.
  const std::string package_name = IsCreationFolderName(package, superstar)
                                       ? internal::PathToUtf8(package.filename())
                                       : CreationPackageName(superstar, 0);
  if (save_data->empty() ||
      !BuildCreationPackageHeader(stfs.metadata(), package_name, superstar, header)) {
    if (error) *error = "The CON package metadata could not be converted to a creation header.";
    return false;
  }
  return true;
}

bool BuildTargetCreationHeader(const std::vector<uint8_t>& source_header,
                               const std::string& source_name, const std::string& target_name,
                               uint32_t target_slot, std::vector<uint8_t>* target_header) {
  if (source_header.size() < kCreationHeaderSize ||
      ReadBe32(source_header, kHeaderContentTypeOffset) != kSaveContentType ||
      ReadBe32(source_header, kHeaderTitleIdOffset) != kTitleIdValue) return false;

  size_t package_length = 0;
  while (package_length < kHeaderPackageNameSize &&
         source_header[kHeaderPackageNameOffset + package_length] != 0) ++package_length;
  const std::string embedded_name(
      reinterpret_cast<const char*>(source_header.data() + kHeaderPackageNameOffset), package_length);
  if (internal::LowerAscii(embedded_name) != internal::LowerAscii(source_name) ||
      target_name.size() >= kHeaderPackageNameSize) return false;

  std::vector<uint16_t> display_units;
  for (size_t offset = 0; offset + 1 < kHeaderDisplayNameSize; offset += 2) {
    const uint16_t unit = static_cast<uint16_t>(
        (source_header[kHeaderDisplayNameOffset + offset] << 8) |
        source_header[kHeaderDisplayNameOffset + offset + 1]);
    if (unit == 0) break;
    display_units.push_back(unit);
  }
  size_t dot = 0;
  while (dot < display_units.size() && display_units[dot] >= '0' && display_units[dot] <= '9') ++dot;
  const bool hasNumberedDisplayPrefix = dot != 0 && dot < display_units.size() && display_units[dot] == '.';
  std::vector<uint16_t> target_display;
  if (hasNumberedDisplayPrefix) {
    std::string display_prefix = std::to_string(target_slot + 1);
    if (display_prefix.size() < 2) display_prefix.insert(display_prefix.begin(), '0');
    target_display.reserve(display_prefix.size() + 1 + display_units.size());
    for (unsigned char character : display_prefix) target_display.push_back(character);
    target_display.push_back('.');
    target_display.insert(target_display.end(), display_units.begin() + static_cast<std::ptrdiff_t>(dot + 1),
                          display_units.end());
    if ((target_display.size() + 1) * 2 > kHeaderDisplayNameSize) return false;
  }

  *target_header = source_header;
  std::fill(target_header->begin() + kHeaderPackageNameOffset,
            target_header->begin() + kHeaderPackageNameOffset + kHeaderPackageNameSize, 0);
  std::copy(target_name.begin(), target_name.end(),
            target_header->begin() + static_cast<std::ptrdiff_t>(kHeaderPackageNameOffset));
  if (hasNumberedDisplayPrefix) {
    std::fill(target_header->begin() + kHeaderDisplayNameOffset,
              target_header->begin() + kHeaderDisplayNameOffset + kHeaderDisplayNameSize, 0);
    for (size_t index = 0; index < target_display.size(); ++index) {
      WriteUtf16BeUnit(target_header, kHeaderDisplayNameOffset + index * 2, target_display[index]);
    }
  }
  return true;
}

bool IsSafeRelative(const fs::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
  for (const fs::path& component : path) {
    if (component == "." || component == "..") return false;
  }
  return true;
}

bool EnsureDirectoryTreeNoSymlink(const fs::path& root, const fs::path& target, bool create) {
  if (root.empty() || target.empty()) return false;
  std::error_code error;
  fs::file_status root_status = fs::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory && create) {
    error.clear();
    fs::create_directories(root, error);
    if (error) return false;
    root_status = fs::symlink_status(root, error);
  }
  if (error || fs::is_symlink(root_status) || !fs::is_directory(root_status)) return false;
  const fs::path relative = target.lexically_relative(root);
  if (!IsSafeRelative(relative)) return false;
  fs::path current = root;
  for (const fs::path& component : relative) {
    current /= component;
    error.clear();
    const fs::file_status status = fs::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory && create) {
      error.clear();
      if (!fs::create_directory(current, error) || error) return false;
      continue;
    }
    if (error || fs::is_symlink(status) || !fs::is_directory(status)) return false;
  }
  return true;
}

bool GetSafeFileState(const fs::path& userdata_root, const fs::path& target,
                      bool create_parent, bool* exists) {
  if (!exists || !EnsureDirectoryTreeNoSymlink(userdata_root, target.parent_path(), create_parent)) return false;
  const fs::path relative = target.lexically_relative(userdata_root);
  if (!IsSafeRelative(relative)) return false;
  std::error_code error;
  const fs::file_status status = fs::symlink_status(target, error);
  if (error == std::errc::no_such_file_or_directory) {
    *exists = false;
    return true;
  }
  if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) return false;
  *exists = true;
  return true;
}

constexpr uint64_t kMaxCreationZipFileBytes = 128ull * 1024 * 1024;
constexpr uint64_t kMaxCreationZipTotalBytes = 256ull * 1024 * 1024;
constexpr size_t kMaxCreationZipEntries = 4096;

class ScopedTempDirectory {
 public:
  ScopedTempDirectory() = default;
  ~ScopedTempDirectory() {
    if (!path_.empty()) {
      std::error_code ignored;
      fs::remove_all(path_, ignored);
    }
  }
  ScopedTempDirectory(const ScopedTempDirectory&) = delete;
  ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

  bool Create() {
    std::error_code error;
    const fs::path parent = fs::temp_directory_path(error);
    if (error || parent.empty()) return false;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      path_ = parent / ("wwe2k14-creation-import-" + std::to_string(stamp) + "-" +
                       std::to_string(attempt));
      error.clear();
      if (fs::create_directory(path_, error)) return true;
      if (error != std::errc::file_exists) {
        path_.clear();
        return false;
      }
    }
    path_.clear();
    return false;
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

bool IsSafeArchiveRelativePath(const fs::path& relative) {
  if (!IsSafeRelative(relative)) return false;
  for (const fs::path& component : relative) {
    const std::string text = internal::PathToUtf8(component);
    if (text.find(':') != std::string::npos) return false;
    for (unsigned char character : text) {
      if (character < 0x20) return false;
    }
  }
  return true;
}

bool ExtractCreationZip(const fs::path& archive_path, const fs::path& output_root) {
  internal::ZipReader zip;
  if (!zip.Open(archive_path) || zip.Count() == 0 || zip.Count() > kMaxCreationZipEntries) return false;
  uint64_t total_bytes = 0;
  std::set<std::string> extracted_paths;
  for (size_t index = 0; index < zip.Count(); ++index) {
    mz_zip_archive_file_stat stat{};
    if (!zip.Stat(index, stat)) return false;
    std::string name = stat.m_filename;
    std::replace(name.begin(), name.end(), '\\', '/');
    const bool directory = zip.IsDirectory(index);
    while (!name.empty() && name.back() == '/') name.pop_back();
    if (name.empty()) return false;
    const fs::path raw_relative = internal::PathFromUtf8(name);
    if (!IsSafeArchiveRelativePath(raw_relative)) return false;
    const fs::path relative = raw_relative.lexically_normal();
    const std::string normalized = internal::LowerAscii(relative.generic_string());
    if (!extracted_paths.insert(normalized).second) return false;
    const fs::path target = (output_root / relative).lexically_normal();
    const fs::path within = target.lexically_relative(output_root);
    if (!IsSafeArchiveRelativePath(within)) return false;
    std::error_code error;
    if (directory) {
      fs::create_directories(target, error);
      if (error) return false;
      continue;
    }
    if (stat.m_uncomp_size > kMaxCreationZipFileBytes ||
        stat.m_uncomp_size > kMaxCreationZipTotalBytes - total_bytes ||
        stat.m_uncomp_size > (std::numeric_limits<size_t>::max)()) {
      return false;
    }
    total_bytes += stat.m_uncomp_size;
    std::vector<uint8_t> bytes;
    if (!zip.Extract(index, bytes) || bytes.size() != stat.m_uncomp_size) return false;
    static constexpr uint8_t kEmpty = 0;
    const void* data = bytes.empty() ? static_cast<const void*>(&kEmpty)
                                     : static_cast<const void*>(bytes.data());
    if (mz_crc32(0, static_cast<const mz_uint8*>(data), bytes.size()) != stat.m_crc32) return false;
    fs::create_directories(target.parent_path(), error);
    if (error || !internal::WriteBinaryFile(target, bytes)) return false;
  }
  return true;
}

bool FindSingleCreationZipSource(const fs::path& root, bool superstar, fs::path* found) {
  if (!found) return false;
  std::error_code error;
  fs::recursive_directory_iterator iterator(root, fs::directory_options::none, error), end;
  if (error) return false;
  size_t matches = 0;
  for (; iterator != end; iterator.increment(error)) {
    if (error) return false;
    if (iterator.depth() > 4) {
      iterator.disable_recursion_pending();
      continue;
    }
    const fs::file_status status = iterator->symlink_status(error);
    if (error || fs::is_symlink(status)) return false;
    bool package_file = false;
    bool creation_folder = false;
    if (fs::is_directory(status) && IsCreationFolderName(iterator->path(), superstar)) {
      creation_folder = IsRegularNonSymlink(iterator->path() / "SaveData.Dat");
      if (creation_folder) iterator.disable_recursion_pending();
    } else if (fs::is_regular_file(status) && IsCreationFolderName(iterator->path(), superstar)) {
      package_file = true;
    }
    if (creation_folder || package_file) {
      if (++matches > 1) return false;
      *found = iterator->path();
    }
  }
  return matches == 1;
}

bool VerifyBackupArchive(const fs::path& archive_path) {
  internal::ZipReader zip;
  if (!zip.Open(archive_path) || zip.Count() == 0) return false;
  const std::string main_name = "userdata/" + std::string(kAccountId) + "/" + std::string(kTitleId) +
                                "/00000001/SaveData.dat/SaveData.Dat";
  const std::string header_name = "userdata/" + std::string(kAccountId) + "/" + std::string(kTitleId) +
                                  "/Headers/00000001/SaveData.dat.header";
  bool found_main = false, found_header = false;
  for (size_t index = 0; index < zip.Count(); ++index) {
    if (zip.IsDirectory(index)) continue;
    mz_zip_archive_file_stat stat{};
    std::vector<uint8_t> extracted;
    if (!zip.Stat(index, stat) || !zip.Extract(index, extracted) || extracted.size() != stat.m_uncomp_size) {
      return false;
    }
    static constexpr uint8_t kEmpty = 0;
    const void* data = extracted.empty() ? static_cast<const void*>(&kEmpty)
                                         : static_cast<const void*>(extracted.data());
    if (mz_crc32(0, static_cast<const mz_uint8*>(data), extracted.size()) != stat.m_crc32) return false;
    const std::string name = stat.m_filename;
    if (name == main_name) found_main = true;
    if (name == header_name) found_header = true;
  }
  return found_main && found_header;
}

struct FileChange {
  FileChange(fs::path target_path, std::optional<std::vector<uint8_t>> replacement_bytes,
             bool require_existing = false, bool require_absent = false,
             bool check_main_plain = false, std::vector<uint8_t> expected = {})
      : target(std::move(target_path)), replacement(std::move(replacement_bytes)),
        must_exist(require_existing), must_be_absent(require_absent),
        validate_main_plain(check_main_plain), expected_plain(std::move(expected)) {}

  fs::path target;
  std::optional<std::vector<uint8_t>> replacement;
  bool must_exist = false;
  bool must_be_absent = false;
  bool validate_main_plain = false;
  std::vector<uint8_t> expected_plain;
  bool had_original = false;
  fs::path staged;
  fs::path old;
};

void RemoveIfPresent(const fs::path& path, std::error_code* error) {
  error->clear();
  fs::remove(path, *error);
  if (*error == std::errc::no_such_file_or_directory) error->clear();
}

bool WriteTransactionJournal(const fs::path& userdata_root, const fs::path& journal,
                             const std::vector<FileChange>& changes) {
  std::string manifest = "WWE2K14-CREATION-TXN-1\n";
  for (const FileChange& change : changes) {
    const fs::path relative = change.target.lexically_relative(userdata_root);
    if (!IsSafeRelative(relative)) return false;
    manifest += change.had_original ? "E\t" : "N\t";
    manifest += internal::PathToUtf8(relative);
    manifest.push_back('\n');
  }
  return internal::WriteBinaryFile(journal, manifest.data(), manifest.size());
}

int g_commit_fault_stage = 0;  // test hook: 1 = throw before the commit marker is complete, 2 = throw after commit, 3 = throw in the first rollback item

// Best effort and non-throwing: every item is restored independently (an exception in one item marks the rollback as
// incomplete and continues with the rest). Returns false when anything could not be restored; callers then keep the
// journal so startup recovery finishes the job.
bool RollBackTransaction(const fs::path& userdata_root, std::vector<FileChange>* changes,
                         const std::string& id) {
  bool okay = true;
  bool first = true;
  for (auto it = changes->rbegin(); it != changes->rend(); ++it) {
    try {
      std::error_code error;
      FileChange& change = *it;
      if (first && g_commit_fault_stage == 3) {
        first = false;
        throw std::runtime_error("injected rollback fault");
      }
      first = false;
      change.old = change.target;
      change.old += std::string(kCreationOldMarker) + id;
      change.staged = change.target;
      change.staged += std::string(kCreationNewMarker) + id;
      const bool old_exists = fs::exists(change.old, error) && !error;
      if (error) okay = false;
      if (old_exists) {
        RemoveIfPresent(change.target, &error);
        if (!error) {
          error.clear();
          fs::rename(change.old, change.target, error);
        }
        if (error) okay = false;
      } else if (!change.had_original) {
        RemoveIfPresent(change.target, &error);
        if (error) okay = false;
      }
      RemoveIfPresent(change.staged, &error);
      if (error) okay = false;
    } catch (...) {
      okay = false;
    }
  }
  (void)userdata_root;
  return okay;
}

int g_commit_fault_after_renames = -1;  // test hook: throw after this many successful swap renames

bool CommitFileChangesImpl(const Paths& paths, std::vector<FileChange>& changes) {
  const fs::path& userdata_root = paths.userdata_dir;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::string id = std::to_string(stamp);
  fs::path journal = userdata_root / (std::string(kCreationJournalPrefix) + id);
  std::error_code error;
  unsigned suffix = 2;
  while (fs::exists(journal, error) && !error) {
    id = std::to_string(stamp) + "-" + std::to_string(suffix++);
    journal = userdata_root / (std::string(kCreationJournalPrefix) + id);
    error.clear();
  }
  if (error) return false;

  for (FileChange& change : changes) {
    if (!GetSafeFileState(userdata_root, change.target, false, &change.had_original) ||
        (change.must_exist && !change.had_original) || (change.must_be_absent && change.had_original)) {
      return false;
    }
    change.staged = change.target;
    change.staged += std::string(kCreationNewMarker) + id;
    change.old = change.target;
    change.old += std::string(kCreationOldMarker) + id;
    error.clear();
    if (fs::exists(change.staged, error) || error) return false;
    error.clear();
    if (fs::exists(change.old, error) || error) return false;
  }

  auto remove_staged_files = [&] {
    for (FileChange& change : changes) RemoveIfPresent(change.staged, &error);
  };
  for (FileChange& change : changes) {
    if (change.replacement) {
      if (!internal::WriteBinaryFile(change.staged, *change.replacement)) {
        remove_staged_files();
        return false;
      }
      std::vector<uint8_t> staged_bytes;
      if (!internal::ReadBinaryFile(change.staged, staged_bytes) || staged_bytes != *change.replacement) {
        remove_staged_files();
        return false;
      }
      if (change.validate_main_plain) {
        const auto staged_main = Unpack(staged_bytes);
        if (!staged_main || staged_main->chunk_offset != kMainContainerOffset ||
            staged_main->plain != change.expected_plain) {
          remove_staged_files();
          return false;
        }
      }
    }
  }

  if (!WriteTransactionJournal(userdata_root, journal, changes)) {
    for (FileChange& change : changes) RemoveIfPresent(change.staged, &error);
    return false;
  }

  bool swapped = true;
  int renames_done = 0;
  auto fault_check = [&] {
    if (g_commit_fault_after_renames >= 0 && ++renames_done > g_commit_fault_after_renames) {
      throw std::runtime_error("injected commit fault");
    }
  };
  // Native path operations only (no narrow string round trip: names may hold non-ANSI characters on Windows).
  fs::path commit_marker = journal;
  fs::path commit_stage;
  // The whole section from the first rename until the commit marker is durable is covered by rollback: any failure or
  // exception restores the original files. Only after the marker rename succeeded is the change declared committed.
  try {
    for (FileChange& change : changes) {
      if (change.had_original) {
        error.clear();
        fs::rename(change.target, change.old, error);
        if (error) { swapped = false; break; }
        fault_check();
      }
      if (change.replacement) {
        error.clear();
        fs::rename(change.staged, change.target, error);
        if (error) { swapped = false; break; }
        fault_check();
      }
    }
    if (swapped) {
      // The marker distinguishes a completed multi-path replacement from an interrupted swap. Renames of the
      // individual files are atomic, but the combined update is not.
      commit_marker += ".commit";
      commit_stage = commit_marker;
      commit_stage += ".new";
      static constexpr char kCommitted[] = "committed\n";
      if (!internal::WriteBinaryFile(commit_stage, kCommitted, sizeof(kCommitted) - 1)) {
        swapped = false;
      } else {
        if (g_commit_fault_stage == 1) throw std::runtime_error("injected fault before the commit marker is complete");
        error.clear();
        fs::rename(commit_stage, commit_marker, error);
        if (error) swapped = false;
      }
    }
  } catch (...) {
    swapped = false;
  }
  if (!swapped) {
    if (RollBackTransaction(userdata_root, &changes, id)) {
      RemoveIfPresent(journal, &error);
    }
    if (!commit_stage.empty()) {
      std::error_code ignore;
      fs::remove(commit_stage, ignore);
    }
    return false;
  }

  try {
    bool cleaned = true;
    for (FileChange& change : changes) {
      RemoveIfPresent(change.old, &error);
      if (error) cleaned = false;
      RemoveIfPresent(change.staged, &error);
      if (error) cleaned = false;
    }
    if (cleaned) {
      RemoveIfPresent(journal, &error);
      if (!error) RemoveIfPresent(commit_marker, &error);
    }
    if (g_commit_fault_stage == 2) throw std::runtime_error("injected post-commit fault");
  } catch (...) {
    // Committed already; leftover markers are tidied by the next startup recovery.
  }
  return true;
}

// Never throws: before the swap any failure leaves the files untouched; during/after the swap an exception rolls
// everything back (or, once the commit marker exists, leaves the finished result for startup recovery to tidy).
bool CommitFileChanges(const Paths& paths, std::vector<FileChange> changes) {
  try {
    return CommitFileChangesImpl(paths, changes);
  } catch (...) {
    // Exceptions before the swap leave the target files unchanged; the swap and rollback paths catch their own.
    std::error_code error;
    for (FileChange& change : changes) {
      if (!change.staged.empty()) RemoveIfPresent(change.staged, &error);
    }
    return false;
  }
}

bool ReadSupportedCreationState(const Paths& paths, std::vector<uint8_t>* main_file,
                                std::vector<uint8_t>* header, fs::path* main_path,
                                fs::path* header_path) {
  *main_path = MainSavePath(paths);
  *header_path = MainHeaderPath(paths);
  if (!IsRegularUnderUserdataNoSymlink(paths.userdata_dir, *main_path) ||
      !IsRegularUnderUserdataNoSymlink(paths.userdata_dir, *header_path) ||
      !internal::ReadBinaryFile(*main_path, *main_file) || !internal::ReadBinaryFile(*header_path, *header) ||
      header->size() != kCreationHeaderSize || ReadBe32(*header, kHeaderContentTypeOffset) != kSaveContentType ||
      ReadBe32(*header, kHeaderTitleIdOffset) != kTitleIdValue) {
    return false;
  }
  const auto unpacked = Unpack(*main_file);
  return unpacked && unpacked->chunk_offset == kMainContainerOffset &&
         internal::IsTolerableCreationPlain(unpacked->plain);
}

// Names the first failing check of ReadSupportedCreationState, in the same order, so the UI can say WHY the save
// was refused. Returns "" when the save reads safely.
std::string DescribeCreationStateProblemImpl(const Paths& paths) {
  try {
    const fs::path main_path = MainSavePath(paths);
    const fs::path header_path = MainHeaderPath(paths);
    if (!IsRegularUnderUserdataNoSymlink(paths.userdata_dir, main_path)) {
      return "save file missing or a linked folder: " + internal::PathToUtf8(main_path);
    }
    if (!IsRegularUnderUserdataNoSymlink(paths.userdata_dir, header_path)) {
      return "header file missing or a linked folder: " + internal::PathToUtf8(header_path);
    }
    std::vector<uint8_t> main_file, header;
    if (!internal::ReadBinaryFile(main_path, main_file)) {
      return "save file could not be read: " + internal::PathToUtf8(main_path);
    }
    if (!internal::ReadBinaryFile(header_path, header)) {
      return "header file could not be read: " + internal::PathToUtf8(header_path);
    }
    if (header.size() != kCreationHeaderSize) {
      return "header size " + std::to_string(header.size()) + ", expected " +
             std::to_string(kCreationHeaderSize);
    }
    if (ReadBe32(header, kHeaderContentTypeOffset) != kSaveContentType) {
      return "save content type " + std::to_string(ReadBe32(header, kHeaderContentTypeOffset)) +
             ", expected " + std::to_string(kSaveContentType);
    }
    if (ReadBe32(header, kHeaderTitleIdOffset) != kTitleIdValue) {
      std::ostringstream out;
      out << "save title id 0x" << std::hex << ReadBe32(header, kHeaderTitleIdOffset)
          << ", expected 0x" << kTitleIdValue;
      return out.str();
    }
    const auto unpacked = Unpack(main_file);
    if (!unpacked) return "unpack failed";
    if (unpacked->chunk_offset != kMainContainerOffset) {
      std::ostringstream out;
      out << "main container offset 0x" << std::hex << unpacked->chunk_offset
          << ", expected 0x" << kMainContainerOffset;
      return out.str();
    }
    if (!internal::IsTolerableCreationPlain(unpacked->plain)) {
      return "unsupported save layout: " + ("plain size " + std::to_string(unpacked->plain.size()) + ", expected " + "1790648");
    }
    return "";
  } catch (...) {
    return "exception while reading the save";
  }
}

struct JournalEntry {
  fs::path target;
  bool had_original = false;
};

bool ReadJournal(const fs::path& journal, std::vector<JournalEntry>* entries) {
  std::vector<uint8_t> bytes;
  if (!internal::ReadBinaryFile(journal, bytes)) return false;
  const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  constexpr std::string_view kHeader = "WWE2K14-CREATION-TXN-1\n";
  if (!text.starts_with(kHeader)) return false;
  entries->clear();
  size_t start = kHeader.size();
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    if (end == std::string::npos) return false;
    const std::string_view line(text.data() + start, end - start);
    if (line.size() < 3 || line[1] != '\t' || (line[0] != 'E' && line[0] != 'N')) return false;
    const fs::path relative = internal::PathFromUtf8(line.substr(2));
    if (!IsSafeRelative(relative)) return false;
    entries->push_back({relative, line[0] == 'E'});
    start = end + 1;
  }
  return !entries->empty();
}

bool RemoveSafeFileIfPresent(const fs::path& path, std::error_code* error) {
  error->clear();
  const fs::file_status status = fs::symlink_status(path, *error);
  if (*error == std::errc::no_such_file_or_directory) {
    error->clear();
    return true;
  }
  if (*error || fs::is_symlink(status) || !fs::is_regular_file(status)) return false;
  fs::remove(path, *error);
  return !*error;
}

bool RecoverCreationTransactions(const Paths& paths) {
  const fs::path& root = paths.userdata_dir;
  std::error_code error;
  const fs::file_status root_status = fs::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory) return true;
  if (error || fs::is_symlink(root_status) || !fs::is_directory(root_status)) return false;

  std::vector<fs::path> journals;
  for (fs::directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const std::string name = internal::PathToUtf8(iterator->path().filename());
    if (!name.starts_with(kCreationJournalPrefix) || name.ends_with(".commit") || name.ends_with(".commit.new")) {
      continue;
    }
    const fs::file_status status = iterator->symlink_status(error);
    if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) return false;
    journals.push_back(iterator->path());
  }
  if (error) return false;

  for (const fs::path& journal : journals) {
    const std::string journal_name = internal::PathToUtf8(journal.filename());
    const std::string id = journal_name.substr(kCreationJournalPrefix.size());
    if (id.empty()) return false;
    std::vector<JournalEntry> entries;
    if (!ReadJournal(journal, &entries)) return false;
    fs::path commit_marker = journal;
    commit_marker += ".commit";
    error.clear();
    const fs::file_status commit_status = fs::symlink_status(commit_marker, error);
    const bool committed = !error && fs::is_regular_file(commit_status) && !fs::is_symlink(commit_status);
    if (error && error != std::errc::no_such_file_or_directory) return false;
    error.clear();

    for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
      const fs::path target = root / entry->target;
      fs::path old_path = target;
      old_path += std::string(kCreationOldMarker) + id;
      fs::path staged_path = target;
      staged_path += std::string(kCreationNewMarker) + id;
      if (!entry->had_original) {
        // A new file whose folder is already gone (the importer removes its emptied folder) has nothing to undo.
        std::error_code parent_error;
        const fs::file_status parent_status = fs::symlink_status(target.parent_path(), parent_error);
        if (parent_error == std::errc::no_such_file_or_directory && !fs::exists(parent_status)) continue;
      }
      if (!EnsureDirectoryTreeNoSymlink(root, target.parent_path(), false)) return false;
      if (committed) {
        if (!RemoveSafeFileIfPresent(old_path, &error) || !RemoveSafeFileIfPresent(staged_path, &error)) return false;
      } else {
        bool old_exists = false;
        if (!GetSafeFileState(root, old_path, false, &old_exists)) return false;
        if (entry->had_original) {
          if (old_exists) {
            if (!RemoveSafeFileIfPresent(target, &error)) return false;
            error.clear();
            fs::rename(old_path, target, error);
            if (error) return false;
          }
        } else if (!RemoveSafeFileIfPresent(target, &error)) {
          return false;
        }
        if (!RemoveSafeFileIfPresent(staged_path, &error)) return false;
      }
    }
    if (!RemoveSafeFileIfPresent(journal, &error) || !RemoveSafeFileIfPresent(commit_marker, &error)) return false;
    fs::path commit_stage = commit_marker;
    commit_stage += ".new";
    if (!RemoveSafeFileIfPresent(commit_stage, &error)) return false;
  }

  // Older or interrupted builds may have left a sibling backup without a manifest. Restore those original
  // files rather than silently discarding them; orphaned staged files are never live game data.
  std::vector<fs::path> artifacts;
  for (fs::recursive_directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error), end;
       !error && iterator != end; iterator.increment(error)) {
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
    const std::string name = internal::PathToUtf8(iterator->path().filename());
    if (name.find(kCreationOldMarker) != std::string::npos ||
        name.find(kCreationNewMarker) != std::string::npos) artifacts.push_back(iterator->path());
  }
  if (error) return false;
  for (const fs::path& artifact : artifacts) {
    const std::string name = internal::PathToUtf8(artifact.filename());
    const size_t old_marker = name.rfind(kCreationOldMarker);
    if (old_marker != std::string::npos) {
      if (old_marker == 0) return false;
      fs::path target = artifact.parent_path() / name.substr(0, old_marker);
      if (!EnsureDirectoryTreeNoSymlink(root, target.parent_path(), false) ||
          !RemoveSafeFileIfPresent(target, &error)) return false;
      error.clear();
      fs::rename(artifact, target, error);
      if (error) return false;
    } else if (!RemoveSafeFileIfPresent(artifact, &error)) {
      return false;
    }
  }

  // A completed transaction removes its journal only after its old files. A lone marker is safe to discard.
  for (fs::directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const std::string name = internal::PathToUtf8(iterator->path().filename());
    if (!name.starts_with(kCreationJournalPrefix) || (!name.ends_with(".commit") && !name.ends_with(".commit.new"))) {
      continue;
    }
    if (!RemoveSafeFileIfPresent(iterator->path(), &error)) return false;
  }
  return !error;
}

std::optional<uint32_t> FindNextAvailableSlot(const Paths& paths, const std::vector<uint8_t>& main_file,
                                              bool superstar) {
  const auto occupied_records = internal::ListKnownCreationSlots(main_file, superstar);
  if (!occupied_records ||
      !EnsureDirectoryTreeNoSymlink(paths.userdata_dir, ContentRoot(paths), false) ||
      !EnsureDirectoryTreeNoSymlink(paths.userdata_dir, HeadersRoot(paths), false)) return std::nullopt;
  for (uint32_t slot = 0; slot <= 99; ++slot) {
    if (std::find(occupied_records->begin(), occupied_records->end(), slot) != occupied_records->end()) continue;
    const std::string package_name = CreationPackageName(superstar, slot);
    const fs::path folder = ContentRoot(paths) / package_name;
    std::error_code error;
    const fs::file_status folder_status = fs::symlink_status(folder, error);
    if (!error && fs::exists(folder_status)) continue;
    if (error && error != std::errc::no_such_file_or_directory) return std::nullopt;
    bool header_exists = false;
    if (!GetSafeFileState(paths.userdata_dir,
                          HeadersRoot(paths) / (package_name + ".header"), false, &header_exists)) {
      return std::nullopt;
    }
    if (!header_exists) return slot;
  }
  return std::nullopt;
}

struct ExistingCreationPayload {
  CreationSaveKind kind = CreationSaveKind::kSuperstar;
  uint32_t slot = 0;
  std::string package_name;
  fs::path payload_path;
  std::vector<uint8_t> payload;
};

std::optional<std::vector<ExistingCreationPayload>> FindUnregisteredCreationPayloads(
    const Paths& paths, const std::vector<uint8_t>& main_file) {
  const auto caw_records = internal::ListKnownCreationSlots(main_file, true);
  const auto arena_records = internal::ListKnownCreationSlots(main_file, false);
  if (!caw_records || !arena_records ||
      !EnsureDirectoryTreeNoSymlink(paths.userdata_dir, ContentRoot(paths), false) ||
      !EnsureDirectoryTreeNoSymlink(paths.userdata_dir, HeadersRoot(paths), false)) {
    return std::nullopt;
  }
  const std::set<uint32_t> occupied_caws(caw_records->begin(), caw_records->end());
  const std::set<uint32_t> occupied_arenas(arena_records->begin(), arena_records->end());
  std::vector<ExistingCreationPayload> candidates;
  std::error_code error;
  for (fs::directory_iterator iterator(ContentRoot(paths), fs::directory_options::none, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const fs::path folder = iterator->path();
    const bool superstar = IsCreationFolderName(folder, true);
    const bool arena = IsCreationFolderName(folder, false);
    if (!superstar && !arena) continue;
    const fs::file_status folder_status = iterator->symlink_status(error);
    if (error || fs::is_symlink(folder_status) || !fs::is_directory(folder_status)) return std::nullopt;
    const std::string package_name = internal::PathToUtf8(folder.filename());
    const uint32_t slot = static_cast<uint32_t>(package_name[0] - '0') * 10 +
                          static_cast<uint32_t>(package_name[1] - '0');
    const CreationSaveKind kind = superstar ? CreationSaveKind::kSuperstar : CreationSaveKind::kArena;
    if ((superstar && occupied_caws.contains(slot)) || (arena && occupied_arenas.contains(slot))) continue;

    const fs::path payload_path = folder / "SaveData.Dat";
    const fs::path creation_header_path = HeadersRoot(paths) / (package_name + ".header");
    bool payload_exists = false, creation_header_exists = false;
    if (!GetSafeFileState(paths.userdata_dir, payload_path, false, &payload_exists) || !payload_exists ||
        !GetSafeFileState(paths.userdata_dir, creation_header_path, false, &creation_header_exists) ||
        !creation_header_exists) {
      return std::nullopt;
    }
    std::vector<uint8_t> payload, source_header, checked_header;
    if (!internal::ReadBinaryFile(payload_path, payload) || payload.empty() ||
        !internal::ReadBinaryFile(creation_header_path, source_header) ||
        !BuildTargetCreationHeader(source_header, package_name, package_name, slot, &checked_header) ||
        checked_header != source_header) {
      return std::nullopt;
    }
    candidates.push_back({kind, slot, package_name, payload_path, std::move(payload)});
  }
  if (error) return std::nullopt;
  std::sort(candidates.begin(), candidates.end(), [](const ExistingCreationPayload& left,
                                                      const ExistingCreationPayload& right) {
    if (left.slot != right.slot) return left.slot < right.slot;
    return left.kind < right.kind;
  });
  return candidates;
}

bool g_auto_register_enabled = true;

// Registers payload folders that have no main-save record and repairs CAW records missing their per-slot data,
// all in memory. `exclude` = a creation that is being removed (its folder is still on disk until the commit).
// Returns an empty string on success; on failure *main/*header are untouched.
std::string AutoRegisterInMemory(const Paths& paths, std::vector<uint8_t>* main, std::vector<uint8_t>* header,
                                 std::optional<std::pair<CreationSaveKind, uint32_t>> exclude,
                                 uint32_t* registered, uint32_t* repaired_out, bool always = false) {
  if (!always && !g_auto_register_enabled) return "disabled";
  auto candidates = FindUnregisteredCreationPayloads(paths, *main);
  if (!candidates) {
    return "An existing creation payload or header is missing, unsafe, or unsupported; no changes were made.";
  }
  if (exclude) {
    std::erase_if(*candidates, [&](const ExistingCreationPayload& item) {
      return item.kind == exclude->first && item.slot == exclude->second;
    });
  }
  std::vector<uint8_t> updated_main = *main, updated_header = *header;
  for (const ExistingCreationPayload& candidate : *candidates) {
    const auto added = internal::AddKnownCreation(
        updated_main, candidate.payload, candidate.kind == CreationSaveKind::kSuperstar, candidate.slot);
    if (!added) {
      return "An existing creation did not match the mapped save record at its folder slot; no changes were made.";
    }
    updated_main = *added;
  }
  std::set<uint32_t> affected_slots;
  for (const ExistingCreationPayload& candidate : *candidates) affected_slots.insert(candidate.slot);
  for (uint32_t slot : affected_slots) {
    const std::string caw_name = CreationPackageName(true, slot);
    const std::string arena_name = CreationPackageName(false, slot);
    const fs::path caw_payload_path = ContentRoot(paths) / caw_name / "SaveData.Dat";
    const fs::path arena_payload_path = ContentRoot(paths) / arena_name / "SaveData.Dat";
    const fs::path caw_header_path = HeadersRoot(paths) / (caw_name + ".header");
    const fs::path arena_header_path = HeadersRoot(paths) / (arena_name + ".header");
    if (exclude && exclude->second == slot) continue;
    if (!IsRegularUnderUserdataNoSymlink(paths.userdata_dir, caw_payload_path) ||
        !IsRegularUnderUserdataNoSymlink(paths.userdata_dir, arena_payload_path) ||
        !IsRegularUnderUserdataNoSymlink(paths.userdata_dir, caw_header_path) ||
        !IsRegularUnderUserdataNoSymlink(paths.userdata_dir, arena_header_path)) {
      continue;
    }
    std::vector<uint8_t> caw_file, arena_file, caw_header, arena_header;
    if (!internal::ReadBinaryFile(caw_payload_path, caw_file) ||
        !internal::ReadBinaryFile(arena_payload_path, arena_file) ||
        !internal::ReadBinaryFile(caw_header_path, caw_header) ||
        !internal::ReadBinaryFile(arena_header_path, arena_header) || caw_file.empty() || arena_file.empty()) {
      return "A paired creation payload or header could not be read safely; no changes were made.";
    }
    if (!internal::ApplyGeneralCreationPairRules(&updated_main, &updated_header, slot, caw_file, arena_file)) {
      return "The paired creation side tables are not in a recognized state; no changes were made.";
    }
  }
  uint32_t repaired = 0;
  auto repaired_main = internal::RepairCawBookkeeping(updated_main, &repaired);
  if (!repaired_main) return "The save's creation records could not be checked; no changes were made.";
  // Arena records from earlier launchers lack the arena settings bytes; rebuild them from their own payload.
  if (const auto arena_slots = internal::ListKnownCreationSlots(*repaired_main, false)) {
    for (const uint32_t slot : *arena_slots) {
      const fs::path payload_path = ContentRoot(paths) / CreationPackageName(false, slot) / "SaveData.Dat";
      std::vector<uint8_t> payload;
      if (!IsRegularUnderUserdataNoSymlink(paths.userdata_dir, payload_path) ||
          !internal::ReadBinaryFile(payload_path, payload)) {
        continue;
      }
      bool upgraded = false;
      auto upgraded_main = internal::UpgradeOlderArenaRecord(*repaired_main, payload, slot, &upgraded);
      if (!upgraded_main) continue;
      if (upgraded) ++repaired;
      repaired_main = std::move(upgraded_main);
    }
  }
  *main = std::move(*repaired_main);
  *header = std::move(updated_header);
  if (registered) *registered = static_cast<uint32_t>(candidates->size());
  if (repaired_out) *repaired_out = repaired;
  return {};
}

}  // namespace

void SetCreationAutoRegisterEnabled(bool enabled) { g_auto_register_enabled = enabled; }

std::string DescribeCreationStateProblem(const Paths& paths) { return DescribeCreationStateProblemImpl(paths); }

std::string DescribeCreationTolerance(const Paths& paths) {
  try {
    std::vector<uint8_t> main_file, header;
    fs::path main_path, header_path;
    if (!ReadSupportedCreationState(paths, &main_file, &header, &main_path, &header_path)) return "";
    const auto unpacked = Unpack(main_file);
    return unpacked ? internal::DescribeCreationTolerance(unpacked->plain) : std::string();
  } catch (...) {
    return "";
  }
}

std::optional<std::vector<CreationSaveRecord>> ListCreationSaveRecords(const Paths& paths) {
  try {
    std::vector<uint8_t> main_file, header;
    fs::path main_path, header_path;
    if (!ReadSupportedCreationState(paths, &main_file, &header, &main_path, &header_path)) {
      return std::nullopt;
    }
    const auto caws = internal::ListKnownCreationNames(main_file, true);
    const auto arenas = internal::ListKnownCreationNames(main_file, false);
    if (!caws || !arenas) return std::nullopt;
    std::vector<CreationSaveRecord> result;
    result.reserve(caws->size() + arenas->size());
    for (const auto& item : *caws) result.push_back({CreationSaveKind::kSuperstar, item.slot, item.name});
    for (const auto& item : *arenas) result.push_back({CreationSaveKind::kArena, item.slot, item.name});
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<uint32_t> FindFreeCreationSaveSlot(const Paths& paths, CreationSaveKind kind) {
  try {
    std::vector<uint8_t> main_file, header;
    fs::path main_path, header_path;
    if (!ReadSupportedCreationState(paths, &main_file, &header, &main_path, &header_path)) {
      return std::nullopt;
    }
    return FindNextAvailableSlot(paths, main_file, kind == CreationSaveKind::kSuperstar);
  } catch (...) {
    return std::nullopt;
  }
}

Result ImportCreationSave(const Paths& paths, CreationSaveKind kind, const fs::path& creation_folder,
                          uint32_t* selected_slot, CreationChangeSummary* summary) {
  try {
    if (!RecoverCreationTransactions(paths)) {
      return Result::Fail("An interrupted creation update could not be recovered; restore the latest save backup.");
    }
    const bool superstar = kind == CreationSaveKind::kSuperstar;
    if (IsRegularNonSymlink(creation_folder)) {
      const std::string extension = internal::LowerAscii(
          internal::PathToUtf8(creation_folder.extension()));
      if (extension == ".rar") {
        return Result::Fail("RAR4 archives are not supported by this build. Extract the archive, then choose its .cas/.car package or creation folder.");
      }
      if (extension == ".zip") {
        ScopedTempDirectory extracted;
        std::error_code error;
        const uint64_t archive_size = fs::file_size(creation_folder, error);
        if (!extracted.Create() || error || archive_size > kMaxCreationZipTotalBytes ||
            !ExtractCreationZip(creation_folder, extracted.path())) {
          return Result::Fail("The ZIP does not contain a safe, readable WWE 2K14 creation file or folder.");
        }
        fs::path creation_source;
        if (!FindSingleCreationZipSource(extracted.path(), superstar, &creation_source)) {
          return Result::Fail("The ZIP must contain exactly one matching NNCustomSuperStar.cas / NNCustomArena.car package or folder.");
        }
        return ImportCreationSave(paths, kind, creation_source, selected_slot, summary);
      }
      if (extension == ".cas" || extension == ".car") {
        ScopedTempDirectory unpacked;
        std::vector<uint8_t> save_data, source_header;
        std::string package_error;
        if (!unpacked.Create() ||
            !internal::ExtractCreationConPackage(creation_folder, superstar, &save_data,
                                                 &source_header, &package_error)) {
          return Result::Fail(package_error.empty()
              ? "The selected .cas/.car file is not a supported WWE 2K14 CON creation package."
              : package_error);
        }
        const std::string source_name = IsCreationFolderName(creation_folder, superstar)
                                            ? internal::PathToUtf8(creation_folder.filename())
                                            : CreationPackageName(superstar, 0);
        const fs::path source_folder = unpacked.path() / source_name;
        const fs::path source_payload = source_folder / "SaveData.Dat";
        const fs::path source_header_path = unpacked.path() / "Headers" / "00000001" /
                                            (source_name + ".header");
        std::error_code error;
        if (!fs::create_directories(source_folder, error) || error ||
            !fs::create_directories(source_header_path.parent_path(), error) || error ||
            !internal::WriteBinaryFile(source_payload, save_data) ||
            !internal::WriteBinaryFile(source_header_path, source_header)) {
          return Result::Fail("The selected CON creation package could not be unpacked safely.");
        }
        return ImportCreationSave(paths, kind, source_folder, selected_slot, summary);
      }
    }
    fs::path creation_path;
    if (!ResolveCreationSave(creation_folder, superstar, &creation_path)) {
      return Result::Fail(superstar
          ? "Choose the unpacked creation folder that contains one NNCustomSuperStar.cas folder (with SaveData.Dat). "
            "Files must be a matching .cas/.car CON package or a supported .zip."
          : "Choose the unpacked creation folder that contains one NNCustomArena.car folder (with SaveData.Dat). "
            "Files must be a matching .cas/.car CON package or a supported .zip.");
    }
    const fs::path source_folder = creation_path.parent_path();
    const std::string source_name = internal::PathToUtf8(source_folder.filename());
    fs::path source_header_path;
    if (!FindSourceHeader(source_folder, source_name, &source_header_path)) {
      return Result::Fail("The selected creation's matching .header sidecar could not be found safely.");
    }
    std::vector<uint8_t> main_file, source_file, main_header, source_header;
    fs::path main_path, header_path;
    if (!ReadSupportedCreationState(paths, &main_file, &main_header, &main_path, &header_path)) {
      return Result::Fail("Unsupported save layout. Supported saves have the mapped WWE 2K14 CAW/arena tables and recognized record regions; unrelated progression values may differ. No changes were made.");
    }
    const auto slot = FindNextAvailableSlot(paths, main_file, superstar);
    if (!slot) {
      return Result::Fail("No free slot is available in the supported creation-save region; no changes were made.");
    }
    // Preserve the complete SaveData.Dat stream. The mapped CAW record is derived from the leading CAS
    // container, but copied creations can carry additional ZLIB-framed sections after it.
    if (!internal::ReadBinaryFile(creation_path, source_file) ||
        !internal::ReadBinaryFile(source_header_path, source_header)) {
      return Result::Fail("The selected creation SaveData.Dat could not be read safely.");
    }
    const std::string target_name = CreationPackageName(superstar, *slot);
    std::vector<uint8_t> target_header_bytes;
    if (!BuildTargetCreationHeader(source_header, source_name, target_name, *slot, &target_header_bytes)) {
      return Result::Fail("The matching creation header is not a recognized WWE 2K14 header; no changes were made.");
    }
    auto updated_main = internal::AddKnownCreation(main_file, source_file, superstar, *slot);
    if (!updated_main) {
      return Result::Fail("The selected .cas/.car or save layout is unknown or unsupported; no changes were made.");
    }
    std::vector<uint8_t> caw_pair_file, caw_pair_header, arena_pair_file, arena_pair_header;
    const std::string caw_name = CreationPackageName(true, *slot);
    const std::string arena_name = CreationPackageName(false, *slot);
    const fs::path caw_payload = ContentRoot(paths) / caw_name / "SaveData.Dat";
    const fs::path caw_header_path = HeadersRoot(paths) / (caw_name + ".header");
    const fs::path arena_payload = ContentRoot(paths) / arena_name / "SaveData.Dat";
    const fs::path arena_header_path = HeadersRoot(paths) / (arena_name + ".header");
    if (superstar) {
      caw_pair_file = source_file;
      caw_pair_header = target_header_bytes;
      if (IsRegularUnderUserdataNoSymlink(paths.userdata_dir, arena_payload) &&
          IsRegularUnderUserdataNoSymlink(paths.userdata_dir, arena_header_path) &&
          (!internal::ReadBinaryFile(arena_payload, arena_pair_file) ||
           !internal::ReadBinaryFile(arena_header_path, arena_pair_header))) {
        return Result::Fail("The matching arena payload or header could not be read safely; no changes were made.");
      }
    } else {
      arena_pair_file = source_file;
      arena_pair_header = target_header_bytes;
      if (IsRegularUnderUserdataNoSymlink(paths.userdata_dir, caw_payload) &&
          IsRegularUnderUserdataNoSymlink(paths.userdata_dir, caw_header_path) &&
          (!internal::ReadBinaryFile(caw_payload, caw_pair_file) ||
           !internal::ReadBinaryFile(caw_header_path, caw_pair_header))) {
        return Result::Fail("The matching CAW payload or header could not be read safely; no changes were made.");
      }
    }
    if (!caw_pair_file.empty() && !caw_pair_header.empty() &&
        !arena_pair_file.empty() && !arena_pair_header.empty()) {
      bool strict_sample_applied = false;
#if defined(WWE2K14_CREATION_SAMPLE_REPRODUCTION)
      if (*slot == 0) {
        // The strict fixture path only supplies the still-unmodeled three-byte
        // field needed by the byte-exact sample test. General side effects
        // below are also applied in builds where this option is OFF.
        strict_sample_applied = internal::ApplyObservedGameMadePair(
            &*updated_main, &main_header, caw_pair_file, caw_pair_header,
            arena_pair_file, arena_pair_header);
      }
#endif
      if (!strict_sample_applied &&
          !internal::ApplyGeneralCreationPairRules(&*updated_main, &main_header, *slot,
                                                   caw_pair_file, arena_pair_file)) {
        return Result::Fail("The paired creation side tables are not in a recognized state; no changes were made.");
      }
    }
    // Same operation, same backup: also register payload folders without a record and repair old-launcher records.
    // Best effort - if it cannot be done safely the import itself still proceeds.
    uint32_t auto_registered = 0, auto_repaired = 0;
    {
      std::vector<uint8_t> trial_main = *updated_main, trial_header = main_header;
      if (AutoRegisterInMemory(paths, &trial_main, &trial_header, std::nullopt, &auto_registered,
                               &auto_repaired).empty()) {
        *updated_main = std::move(trial_main);
        main_header = std::move(trial_header);
      } else {
        auto_registered = auto_repaired = 0;
      }
    }
    if (summary) {
      summary->registered = auto_registered;
      summary->repaired = auto_repaired;
      summary->name.clear();
      if (const auto names = internal::ListKnownCreationNames(*updated_main, superstar)) {
        for (const auto& item : *names) {
          if (item.slot == *slot) summary->name = item.name;
        }
      }
    }
    const auto updated_plain = Unpack(*updated_main);
    if (!updated_plain || updated_plain->chunk_offset != kMainContainerOffset) {
      return Result::Fail("The rebuilt main save failed ZLIB/Adler validation; no changes were made.");
    }
    // The listing uses this validator; never write a save it would then call unsupported (GitHub #64).
    if (!internal::IsTolerableCreationPlain(updated_plain->plain)) {
      return Result::Fail("The imported creation would leave the save in a layout this launcher cannot read; no changes were made.");
    }

    const fs::path target_folder = ContentRoot(paths) / target_name;
    const fs::path target_payload = target_folder / "SaveData.Dat";
    const fs::path target_header_path = HeadersRoot(paths) / (target_name + ".header");
    if (!EnsureDirectoryTreeNoSymlink(paths.userdata_dir, ContentRoot(paths), false) ||
        !EnsureDirectoryTreeNoSymlink(paths.userdata_dir, HeadersRoot(paths), false)) {
      return Result::Fail("The saved-game creation folders are missing or unsafe; no changes were made.");
    }
    std::error_code error;
    const fs::file_status folder_status = fs::symlink_status(target_folder, error);
    if (error != std::errc::no_such_file_or_directory || fs::exists(folder_status)) {
      return Result::Fail("The selected creation slot already has a payload folder; no changes were made.");
    }
    bool target_header_exists = false;
    if (!GetSafeFileState(paths.userdata_dir, target_header_path, false, &target_header_exists) ||
        target_header_exists) {
      return Result::Fail("The selected creation slot already has a header or cannot be accessed safely.");
    }

    BackupInfo backup_info;
    const Result backup = BackupSaves(paths, false, &backup_info);
    if (!backup.ok) return backup;
    if (!VerifyBackupArchive(backup_info.file)) {
      return Result::Fail("The pre-import save backup could not be reopened and CRC-verified; no files were changed.");
    }

    error.clear();
    if (!fs::create_directory(target_folder, error) || error) {
      return Result::Fail("The selected creation folder could not be prepared safely; no save files were changed.");
    }
    std::vector<FileChange> changes;
    changes.push_back({main_path, *updated_main, true, false, true, updated_plain->plain});
    changes.push_back({header_path, main_header, true});
    changes.push_back({target_payload, source_file, false, true});
    changes.push_back({target_header_path, std::move(target_header_bytes), false, true});
    if (!CommitFileChanges(paths, std::move(changes))) {
      error.clear();
      fs::remove(target_folder, error);  // Removes it only when rollback left it empty.
      return Result::Fail("The creation could not be saved. The pre-import backup is available.");
    }
    if (selected_slot) *selected_slot = *slot;
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The selected creation could not be imported safely; no changes were made.");
  }
}

Result ExportCreation(const Paths& paths, CreationSaveKind kind, uint32_t slot_index,
                      const fs::path& output_file, CreationExportSummary* summary) {
  try {
    if (kind != CreationSaveKind::kSuperstar && kind != CreationSaveKind::kArena) {
      return Result::Fail("That creation type is not supported.");
    }
    const bool superstar = kind == CreationSaveKind::kSuperstar;
    const std::string extension = internal::LowerAscii(internal::PathToUtf8(output_file.extension()));
    if (extension != (superstar ? ".cas" : ".car")) {
      return Result::Fail(superstar ? "Choose an output file ending in .cas." :
                                     "Choose an output file ending in .car.");
    }
    if (output_file.empty() || IsPathWithin(output_file, paths.userdata_dir)) {
      return Result::Fail("Choose an export location outside the player's userdata folder; "
                          "Documents or Desktop is a good place.", "destination-in-userdata");
    }

    std::vector<uint8_t> main_file, main_header;
    fs::path main_path, header_path;
    if (!ReadSupportedCreationState(paths, &main_file, &main_header, &main_path, &header_path)) {
      return Result::Fail("The player's creation save could not be read safely or is not a supported WWE 2K14 save.");
    }
    const auto names = internal::ListKnownCreationNames(main_file, superstar);
    if (!names) return Result::Fail("The player's creation records could not be validated.");
    const auto record = std::find_if(names->begin(), names->end(),
                                     [slot_index](const internal::CreationName& item) {
                                       return item.slot == slot_index;
                                     });
    if (record == names->end()) return Result::Fail("That creation slot is empty or invalid; nothing was exported.");

    const std::string package_name = CreationPackageName(superstar, slot_index);
    const fs::path payload_path = ContentRoot(paths) / package_name / "SaveData.Dat";
    std::vector<uint8_t> payload;
    if (!IsRegularUnderUserdataNoSymlink(paths.userdata_dir, payload_path))
      return Result::Fail("The creation payload is missing or unsafe; nothing was exported.", "payload-missing");
    // Package the complete SaveData.Dat, including any sections after the leading mapped CAS container.
    if (!internal::ReadBinaryFile(payload_path, payload))
      return Result::Fail("The creation payload could not be read; nothing was exported.", "payload-unreadable");
    if (!internal::CreationFileMatchesMainRecord(main_file, payload, superstar, slot_index))
      return Result::Fail("The creation payload does not match its save record; nothing was exported.", "record-mismatch");

    const std::string display_name = TwoDigitSlot(slot_index + 1) + "." +
        (record->name.empty() ? (superstar ? "CUSTOM SUPERSTAR" : "CUSTOM ARENA") : record->name);
    std::string write_error;
    if (!internal::WriteStfsSaveGamePackage(output_file, display_name, kTitleIdValue,
                                             "SaveData.Dat", payload, &write_error)) {
      return Result::Fail(write_error.empty() ? "The creation package could not be written." : write_error);
    }

    std::vector<uint8_t> read_back, package_header;
    std::string read_error;
    if (!internal::ExtractCreationConPackage(output_file, superstar, &read_back,
                                              &package_header, &read_error) || read_back != payload) {
      std::error_code error;
      fs::remove(output_file, error);
      return Result::Fail("The exported creation failed its package read-back check, so it was not kept.");
    }
    if (summary) {
      summary->name = record->name;
      summary->slot_index = slot_index;
    }
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The selected creation could not be exported safely.");
  }
}

Result RegisterExistingCreationSaves(const Paths& paths, uint32_t* registered_count, uint32_t* repaired_count) {
  if (registered_count) *registered_count = 0;
  if (repaired_count) *repaired_count = 0;
  try {
    if (!RecoverCreationTransactions(paths)) {
      return Result::Fail("An interrupted creation update could not be recovered; restore the latest save backup.");
    }
    std::vector<uint8_t> main_file, main_header;
    fs::path main_path, header_path;
    if (!ReadSupportedCreationState(paths, &main_file, &main_header, &main_path, &header_path)) {
      return Result::Fail("Unsupported save layout. The existing creation payloads were not registered and no files were changed.");
    }
    std::vector<uint8_t> updated_main = main_file;
    uint32_t candidates_count = 0, repaired = 0;
    if (const std::string failure = AutoRegisterInMemory(paths, &updated_main, &main_header, std::nullopt,
                                                         &candidates_count, &repaired, true);
        !failure.empty()) {
      return Result::Fail(failure);
    }
    if (candidates_count == 0 && repaired == 0) return Result::Ok();
    const auto updated_plain = Unpack(updated_main);
    if (!updated_plain || updated_plain->chunk_offset != kMainContainerOffset ||
        !internal::IsTolerableCreationPlain(updated_plain->plain)) {
      return Result::Fail("The rebuilt main save failed validation; no changes were made.");
    }
    BackupInfo backup_info;
    const Result backup = BackupSaves(paths, false, &backup_info);
    if (!backup.ok) return backup;
    if (!VerifyBackupArchive(backup_info.file)) {
      return Result::Fail("The pre-registration save backup could not be reopened and CRC-verified; no files were changed.");
    }
    std::vector<FileChange> changes;
    changes.push_back({main_path, std::move(updated_main), true, false, true, updated_plain->plain});
    changes.push_back({header_path, std::move(main_header), true});
    if (!CommitFileChanges(paths, std::move(changes))) {
      return Result::Fail("The creation records could not be registered. The pre-registration backup is available.");
    }
    if (registered_count) *registered_count = candidates_count;
    if (repaired_count) *repaired_count = repaired;
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The existing creations could not be registered safely; no changes were made.");
  }
}

Result RemoveCreationSave(const Paths& paths, CreationSaveKind kind, uint32_t slot_index,
                          CreationChangeSummary* summary) {
  try {
    if (!RecoverCreationTransactions(paths)) {
      return Result::Fail("An interrupted creation update could not be recovered; restore the latest save backup.");
    }
    if (slot_index > 99) {
      return Result::Fail("Removal is limited to mapped two-digit creation slots 00-99.");
    }
    std::vector<uint8_t> main_file, header;
    fs::path main_path, header_path;
    if (!ReadSupportedCreationState(paths, &main_file, &header, &main_path, &header_path)) {
      return Result::Fail("Unsupported save layout. Supported saves have the mapped WWE 2K14 CAW/arena tables and recognized record regions; unrelated progression values may differ. No changes were made.");
    }
    auto updated_main = internal::RemoveKnownCreation(
        main_file, kind == CreationSaveKind::kSuperstar, slot_index);
    if (!updated_main) {
      return Result::Fail("The selected creation slot is empty or unsupported; no changes were made.");
    }
    uint32_t auto_registered = 0, auto_repaired = 0;
    {
      // The removed creation's folder is still on disk until the commit below: exclude it so it cannot come back.
      std::vector<uint8_t> trial_main = *updated_main, trial_header = header;
      if (AutoRegisterInMemory(paths, &trial_main, &trial_header, std::make_pair(kind, slot_index),
                               &auto_registered, &auto_repaired).empty()) {
        *updated_main = std::move(trial_main);
        header = std::move(trial_header);
      } else {
        auto_registered = auto_repaired = 0;
      }
    }
    if (summary) {
      summary->registered = auto_registered;
      summary->repaired = auto_repaired;
      summary->name.clear();
      if (const auto names = internal::ListKnownCreationNames(main_file, kind == CreationSaveKind::kSuperstar)) {
        for (const auto& item : *names) {
          if (item.slot == slot_index) summary->name = item.name;
        }
      }
    }
    const auto updated_plain = Unpack(*updated_main);
    if (!updated_plain || updated_plain->chunk_offset != kMainContainerOffset) {
      return Result::Fail("The rebuilt main save failed ZLIB/Adler validation; no changes were made.");
    }
    const std::string package_name = CreationPackageName(kind == CreationSaveKind::kSuperstar, slot_index);
    const fs::path creation_folder = ContentRoot(paths) / package_name;
    const fs::path payload_path = creation_folder / "SaveData.Dat";
    const fs::path creation_header_path = HeadersRoot(paths) / (package_name + ".header");
    if (!EnsureDirectoryTreeNoSymlink(paths.userdata_dir, ContentRoot(paths), false) ||
        !EnsureDirectoryTreeNoSymlink(paths.userdata_dir, HeadersRoot(paths), false)) {
      return Result::Fail("The saved-game creation folders are missing or unsafe; no changes were made.");
    }
    std::error_code error;
    const fs::file_status folder_status = fs::symlink_status(creation_folder, error);
    bool payload_exists = false;
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
    } else if (error || fs::is_symlink(folder_status) || !fs::is_directory(folder_status) ||
               !GetSafeFileState(paths.userdata_dir, payload_path, false, &payload_exists) || !payload_exists) {
      return Result::Fail("The selected creation payload folder is damaged or unsafe; no changes were made.");
    }
    bool creation_header_exists = false;
    if (!GetSafeFileState(paths.userdata_dir, creation_header_path, false, &creation_header_exists)) {
      return Result::Fail("The selected creation header cannot be accessed safely; no changes were made.");
    }

    BackupInfo backup_info;
    const Result backup = BackupSaves(paths, false, &backup_info);
    if (!backup.ok) return backup;
    if (!VerifyBackupArchive(backup_info.file)) {
      return Result::Fail("The pre-removal save backup could not be reopened and CRC-verified; no files were changed.");
    }
    std::vector<FileChange> changes;
    changes.push_back({main_path, *updated_main, true, false, true, updated_plain->plain});
    changes.push_back({header_path, header, true});
    if (payload_exists) changes.push_back({payload_path, std::nullopt, true});
    if (creation_header_exists) changes.push_back({creation_header_path, std::nullopt, true});
    if (!CommitFileChanges(paths, std::move(changes))) {
      return Result::Fail("The creation could not be removed. The pre-removal backup is available.");
    }
    error.clear();
    fs::remove(creation_folder, error);  // Leave any unrelated folder contents untouched.
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The selected creation could not be removed safely; no changes were made.");
  }
}

Result RecoverCreationSaveFiles(const Paths& paths) {
  try {
    return RecoverCreationTransactions(paths)
               ? Result::Ok()
               : Result::Fail("Interrupted creation files could not be restored; restore the latest save backup.");
  } catch (...) {
    return Result::Fail("Interrupted creation files could not be restored; restore the latest save backup.");
  }
}

namespace internal {

bool ExtractCreationConPackage(const fs::path& package, bool superstar,
                              std::vector<uint8_t>* save_data,
                              std::vector<uint8_t>* header, std::string* error) {
  try {
    return ExtractCreationConPackageImpl(package, superstar, save_data, header, error);
  } catch (...) {
    if (error) *error = "The CON creation package could not be read safely.";
    return false;
  }
}

namespace {

constexpr size_t kMainPlainSize = 1'790'648;
constexpr size_t kCawPlainSize = 1'882'028;
constexpr size_t kArenaPlainSize = 626'764;
constexpr size_t kCawBase = 0x1E517;
constexpr size_t kCawStride = 0xB6C;
constexpr size_t kArenaBase = 0x7368B;
constexpr size_t kArenaStride = 0xCC;
constexpr size_t kCawSize = 0xB6C;
// CAW packages are named with two digits (00..99CustomSuperStar.cas), so the table holds 100 records. The bytes after
// record 99 and before the arena table are other game data: the owner's 2026-10-26 save has 82 non-zero bytes from
// plain 0x65B51 that are not a CAW record, and treating them as slots 100-118 rejected the whole save.
constexpr size_t kCawSlotCount = 100;
static_assert(kCawBase + kCawSlotCount * kCawStride <= kArenaBase);
constexpr size_t kFirstObservedArrayBase = 0x75ECB;
constexpr size_t kArenaSlotCount = (kFirstObservedArrayBase - kArenaBase) / kArenaStride;
constexpr size_t kFirstObservedArrayCount = 159;
constexpr size_t kSecondObservedArrayBase = 0x76387;
constexpr size_t kSecondObservedArrayCount = 256;
constexpr size_t kCreationEntryBase = 0x76CC0;
constexpr size_t kCreationEntryStride = 0x168;
constexpr size_t kCreationMarkerBase = 0x1A6251;
constexpr size_t kCreationMarkerStride = 0x40;
constexpr size_t kThumbnailBase = 0x1980;
constexpr size_t kThumbnailStride = 0x2200;
constexpr size_t kThumbnailSize = 0x1000;
constexpr size_t kCreationPairHeaderByte = 0x144;
constexpr std::array<uint8_t, 6> kCreationEntryBytes{0x01, 0x90, 0x02, 0x8A, 0x02, 0xBC};
constexpr std::array<uint8_t, 9> kInactiveText{'I', 'N', 'A', 'C', 'T', 'I', 'V', 'E', 0};
constexpr std::array<uint8_t, 9> kBeforeInactiveText{'P', 'r', 'e', 's', 's', ' ', 'S', 'T', 'A'};
constexpr std::array<size_t, 9> kFirstPairFlags{
    0x1A3655, 0x1A3C14, 0x1A3C2B, 0x1A3C8B, 0x1A3CFE,
    0x1A3D25, 0x1A3D39, 0x1A3D50, 0x1A3D8D};
constexpr std::array<std::pair<size_t, uint8_t>, 18> kDdsHeaderBytes{{
    {0x00, 0x44}, {0x01, 0x44}, {0x02, 0x53}, {0x03, 0x20}, {0x04, 0x7C},
    {0x08, 0x07}, {0x09, 0x10}, {0x0A, 0x08}, {0x0C, 0x40}, {0x10, 0x40},
    {0x15, 0x10}, {0x4C, 0x20}, {0x50, 0x04}, {0x54, 0x44}, {0x55, 0x58},
    {0x56, 0x54}, {0x57, 0x31}, {0x6D, 0x10}}};

size_t RecordOffset(bool superstar, uint32_t slot);

bool RequireSlice(const std::vector<uint8_t>& source, size_t start, size_t size,
                  std::vector<uint8_t>* output) {
  if (start > source.size() || size > source.size() - start) return false;
  output->assign(source.begin() + static_cast<std::ptrdiff_t>(start),
                 source.begin() + static_cast<std::ptrdiff_t>(start + size));
  return true;
}

std::optional<std::vector<uint8_t>> MakeCawRecord(const std::vector<uint8_t>& cas) {
  std::vector<uint8_t> record(kCawSize, 0);
  std::vector<uint8_t> part;
  if (!RequireSlice(cas, 0x1CA40F, 0x6ED, &part)) return std::nullopt;
  std::copy(part.begin(), part.end(), record.begin());
  if (!RequireSlice(cas, 0x1CA863, 0xAEC - 0x95D, &part)) return std::nullopt;
  std::copy(part.begin(), part.end(), record.begin() + 0x95D);
  if (!RequireSlice(cas, 0x1CB1DA, 9, &part)) return std::nullopt;
  std::copy(part.begin(), part.end(), record.begin() + 0x977);
  // CAS +0x0C usually equals +0x1CB068, but both Best of Save CAWs checked (42, 93) differ and the game's own save
  // records the +0x0C word for them, so +0x0C is the source and the +0x1CB068 copy is not required to match.
  if (cas.size() <= 0x1CB1E2) return std::nullopt;
  record[0x9B7] = cas[0x1CB1E2];
  record[0x9EF] = cas[0x1CB1E2];
  // The two game-written records clear these regions although the CAS source has FF bytes there.
  std::fill(record.begin() + 0xA32, record.begin() + 0xA36, 0);
  std::fill(record.begin() + 0xAB6, record.begin() + 0xABA, 0);
  if (!RequireSlice(cas, 0x1CB33A, 16, &part)) return std::nullopt;
  std::copy(part.begin(), part.end(), record.begin() + 0xAD7);
  if (!RequireSlice(cas, 0x1CB71F, 0x80, &part)) return std::nullopt;
  std::copy(part.begin(), part.end(), record.begin() + 0xAEC);
  record[0x6ED] = 1;
  record[0x7FC] = 0;
  // +0x7FD..+0x800 = the CAS checksum (BE u32 at CAS +0x0C). Both owner CAWs have top byte 0x01, which earlier
  // looked like a constant; community CAWs differ (03: 0x27A13D39).
  std::copy(cas.begin() + 0x0C, cas.begin() + 0x13, record.begin() + 0x7FD);
  record[0x813] = 2;
  return record;
}

// The whole 0xCC-byte arena record is copied from the CAR plain exactly as the game writes it, verified byte-exact
// (zeros included) on 73 game-written pairs: 46 edited arenas in the GitHub #64 player's original save, 25 in the
// community Best of Save and both owner samples. +0x7D..+0xBC are 8 groups of 8 bytes whose first 3 come from
// CAR +0x7700C + 8 * group; +0xBE..+0xC6 come from CAR +0x8F..+0x9B. Earlier launchers left +0x7D..+0xCB zero
// (arena settings lost; players saw black ring/floor), see MakeOlderArenaRecord.
std::optional<std::vector<uint8_t>> MakeArenaRecord(const std::vector<uint8_t>& car) {
  std::vector<uint8_t> record(kArenaStride, 0);
  if (car.size() < 0x7700C + 8 * 8) return std::nullopt;
  std::copy(car.begin() + 0x07, car.begin() + 0x3D, record.begin());
  std::copy(car.begin() + 0x45, car.begin() + 0x8C, record.begin() + 0x36);
  for (size_t group = 0; group < 8; ++group) {
    std::copy(car.begin() + 0x7700C + 8 * group, car.begin() + 0x7700C + 8 * group + 3,
              record.begin() + 0x7D + 8 * group);
  }
  std::copy(car.begin() + 0x8F, car.begin() + 0x92, record.begin() + 0xBE);
  std::copy(car.begin() + 0x9A, car.begin() + 0x9C, record.begin() + 0xC3);
  std::copy(car.begin() + 0x92, car.begin() + 0x94, record.begin() + 0xC5);
  return record;
}

// Records written by earlier launchers, so they still match their payload (export, removal, pairing) and can be
// upgraded to the full record: v1.3.0/v1.4.0 (name cut to 12 characters, settings zero) and the v1.4.1 TEST1 build
// (record +0x00..+0x7C only).
std::optional<std::vector<uint8_t>> MakeOlderArenaRecord(const std::vector<uint8_t>& car, bool test1_layout) {
  std::vector<uint8_t> record(kArenaStride, 0);
  if (car.size() <= 0x8B) return std::nullopt;
  if (test1_layout) {
    std::copy(car.begin() + 0x07, car.begin() + 0x3D, record.begin());
    std::copy(car.begin() + 0x45, car.begin() + 0x8C, record.begin() + 0x36);
    return record;
  }
  record[0x00] = car[0x07];
  std::copy(car.begin() + 0x28, car.begin() + 0x2D, record.begin() + 0x21);
  std::copy(car.begin() + 0x45, car.begin() + 0x54, record.begin() + 0x36);
  record[0x7C] = car[0x8B];
  return record;
}

// True when the arena record at `offset` is what this launcher or an earlier one built from `car`.
bool ArenaRecordMatchesCar(const std::vector<uint8_t>& plain, size_t offset, const std::vector<uint8_t>& car) {
  if (offset > plain.size() || kArenaStride > plain.size() - offset) return false;
  const auto matches = [&](const std::optional<std::vector<uint8_t>>& record) {
    return record && std::equal(record->begin(), record->end(), plain.begin() + static_cast<std::ptrdiff_t>(offset));
  };
  return matches(MakeArenaRecord(car)) || matches(MakeOlderArenaRecord(car, false)) ||
         matches(MakeOlderArenaRecord(car, true));
}

bool UnpackFile(const std::vector<uint8_t>& file, size_t* chunk_offset, std::vector<uint8_t>* plain,
                bool allow_trailing_data = false) {
  constexpr std::string_view magic = "ZLIB";
  const auto marker = std::search(file.begin(), file.end(), magic.begin(), magic.end());
  if (marker == file.end()) return false;
  *chunk_offset = static_cast<size_t>(marker - file.begin());
  if (file.size() - *chunk_offset < 16) return false;
  auto read_le = [&](size_t offset) {
    return static_cast<uint32_t>(file[offset]) |
           (static_cast<uint32_t>(file[offset + 1]) << 8) |
           (static_cast<uint32_t>(file[offset + 2]) << 16) |
           (static_cast<uint32_t>(file[offset + 3]) << 24);
  };
  const uint32_t compressed_size = read_le(*chunk_offset + 8);
  const uint32_t plain_size = read_le(*chunk_offset + 12);
  const uint64_t chunk_end = *chunk_offset + 16ull + compressed_size;
  // Creation inputs may contain additional bytes after their first known SaveData.Dat chunk (slot 62 does);
  // only the checked leading plain chunk feeds the mapped main-save record. Main saves remain terminal-only.
  if (read_le(*chunk_offset + 4) != 0x3012 ||
      (allow_trailing_data ? chunk_end > file.size() : chunk_end != file.size()) || plain_size == 0 ||
      plain_size > static_cast<uint32_t>((std::numeric_limits<int>::max)())) return false;
  plain->resize(plain_size);
  mz_ulong size = static_cast<mz_ulong>(plain->size());
  if (mz_uncompress(plain->data(), &size, file.data() + *chunk_offset + 16,
                    static_cast<mz_ulong>(compressed_size)) != MZ_OK || size != plain_size) return false;
  return true;
}

bool IsAllZero(const std::vector<uint8_t>& bytes, size_t offset, size_t size) {
  return offset <= bytes.size() && size <= bytes.size() - offset &&
         std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                     bytes.begin() + static_cast<std::ptrdiff_t>(offset + size),
                     [](uint8_t byte) { return byte == 0; });
}

bool IsKnownCawHeadId(uint32_t value) {
  return value == 512 || (value >= 100 && value <= 499);
}

bool IsCawRecord(const std::vector<uint8_t>& plain, size_t offset) {
  if (IsAllZero(plain, offset, kCawSize)) return true;
  return offset <= plain.size() && kCawSize <= plain.size() - offset &&
         plain[offset + 0x6ED] == 1 && plain[offset + 0x7FC] == 0 &&
         IsKnownCawHeadId(ReadBe32(plain, offset + 0x811)) && !IsAllZero(plain, offset + 0x7FD, 4);
}

bool IsArenaRecord(const std::vector<uint8_t>& plain, size_t offset) {
  if (IsAllZero(plain, offset, kArenaStride)) return true;
  // Every game-written arena record seen (48: GitHub #64's 46 edited arenas and the owner samples) starts with 0x81.
  // The rest varies per arena, including +0x7C and the undecoded +0x7D..+0xCB, so it is not constrained.
  return offset <= plain.size() && kArenaStride <= plain.size() - offset && plain[offset] == 0x81;
}

bool IsRecognizedCreationSideTables(const std::vector<uint8_t>& plain) {
  const auto is_recognized_table = [&](size_t offset, size_t count, uint8_t maximum) {
    if (offset > plain.size() || count > (plain.size() - offset) / 4) return false;
    for (size_t index = 0; index < count; ++index) {
      const size_t at = offset + index * 4;
      if (plain[at] > maximum || plain[at + 1] != 0 || plain[at + 2] != 0 || plain[at + 3] != 0) return false;
    }
    return true;
  };
  // Their meanings are unknown. The reporter's supplied real save adds u32 value 2 in the second table;
  // recognize and preserve that observed state without assigning it a meaning or accepting larger values.
  return is_recognized_table(kFirstObservedArrayBase, kFirstObservedArrayCount, 1) &&
         is_recognized_table(kSecondObservedArrayBase, kSecondObservedArrayCount, 2);
}

// Main plain u32 big-endian at 0x18 = sum of every plain byte from 0x1C to the end. Holds exactly in the owner's
// save-before, save-after and save-after2 (0x01F85EE0, 0x01F9C784, 0x01FB301F); the game reports a save whose sum
// does not match as corrupted (caw-ingame1/2 kept the old value). Every main repack recomputes it.
constexpr size_t kMainChecksumOffset = 0x18;

uint32_t MainPlainChecksum(const std::vector<uint8_t>& plain) {
  uint32_t sum = 0;
  for (size_t i = kMainChecksumOffset + 4; i < plain.size(); ++i) sum += plain[i];
  return sum;
}

uint64_t SumBytes(const std::vector<uint8_t>& bytes, size_t offset, size_t count) {
  uint64_t sum = 0;
  for (size_t index = 0; index < count; ++index) sum += bytes[offset + index];
  return sum;
}

uint32_t CasChecksum(const std::vector<uint8_t>& data) {
  // The retail writer/validator (sub_822D8578 / sub_822D8690) uses this field-wise sum. The verified
  // ranges are recorded in out/worker-reports/20261026/p5h-cas-check.md; this is not a generic CRC.
  uint64_t total = SumBytes(data, 0, 12) + SumBytes(data, 16, 4);
  for (size_t index = 0; index < 4; ++index) {
    const size_t wide = 0xB9204 + index * 0x44484;
    total += SumBytes(data, wide, 0x40020) + SumBytes(data, wide + 0x40080, 0x4402);

    const size_t animation = 20 + index * 0x2E47C;
    const uint64_t long_count = static_cast<uint64_t>(std::count_if(
        data.begin() + static_cast<std::ptrdiff_t>(animation + 14'872),
        data.begin() + static_cast<std::ptrdiff_t>(animation + 14'882),
        [](uint8_t value) { return value != 0; }));
    const uint64_t short_count = static_cast<uint64_t>(std::count_if(
        data.begin() + static_cast<std::ptrdiff_t>(animation + 14'882),
        data.begin() + static_cast<std::ptrdiff_t>(animation + 14'884),
        [](uint8_t value) { return value != 0; }));
    total += SumBytes(data, animation, 14'884);
    total += long_count * SumBytes(data, animation + 14'884, 43'670 * 4);
    total += short_count * SumBytes(data, animation + 14'884, 33'310 * 4);
  }

  const size_t tail = 29 * 0x10000 - 23'532;
  total += SumBytes(data, tail, 204);
  total += SumBytes(data, tail + 456, 1'272);
  total += SumBytes(data, tail + 1'728, 40);
  total += SumBytes(data, tail + 1'768, 412);
  total += SumBytes(data, tail + 2'180, 704);
  total += SumBytes(data, tail + 2'884, 268);
  total += SumBytes(data, tail + 3'160, 2);
  total += SumBytes(data, tail + 3'886, 8);
  total += SumBytes(data, tail + 3'896, 976);
  total += SumBytes(data, tail + 4'872, 56);
  total += SumBytes(data, tail + 4'928, 8);
  total += SumBytes(data, tail + 5'000, 12);
  for (size_t offset = tail + 4'936; offset < tail + 5'000; offset += 4) {
    total += ReadBe32(data, offset);
  }
  return static_cast<uint32_t>(total);
}

bool RefreshCasChecksum(std::vector<uint8_t>* data) {
  if (!data || data->size() != kCawPlainSize || ReadBe32(*data, 0) != 55 ||
      ReadBe32(*data, 4) != 42 || ReadBe32(*data, 8) != 23) {
    return false;
  }
  const uint32_t checksum = CasChecksum(*data);
  WriteBe32(data, 0x0C, checksum);
  WriteBe32(data, 0x1CB068, checksum);
  WriteBe32(data, 0x1CB7A8, checksum);
  return true;
}

uint32_t ArenaChecksum(const std::vector<uint8_t>& data) {
  // Retail field-wise sum (sub_822ACD08), documented with its component ranges in the P5H evidence report.
  uint64_t total = SumBytes(data, 0, 4) + SumBytes(data, 60, 4) + SumBytes(data, 72, 67);
  for (const size_t offset : {139u, 140u, 142u, 143u, 144u, 145u, 146u, 147u}) {
    total += data[offset];
  }
  total += SumBytes(data, 152, 4) + data[148];
  for (size_t index = 0; index < 19; ++index) total += SumBytes(data, 156 + index * 20, 18);
  for (size_t index = 0; index < 20; ++index) total += SumBytes(data, 536 + index * 4, 4);
  for (size_t index = 0; index < 45; ++index) total += SumBytes(data, 616 + index * 32, 31);
  for (size_t index = 0; index < 103; ++index) total += SumBytes(data, 2'056 + index * 160, 4);
  for (size_t index = 0; index < 103; ++index) total += SumBytes(data, 18'440 + index * 80, 4);
  total += data[26'632];
  for (size_t index = 0; index < 100; ++index) total += SumBytes(data, 26'636 + index * 4'608, 4);

  const size_t tail = 7 * 0x10000 + 28'684;
  for (size_t index = 0; index < 8; ++index) {
    const size_t row = tail + index * 8;
    total += SumBytes(data, row, 3) + SumBytes(data, row + 4, 4) + data[row + 4];
  }
  total += SumBytes(data, tail + 64, 8'192);
  for (size_t page = 0; page < 2; ++page) {
    for (size_t index = 0; index < 101; ++index) {
      total += data[tail + 8'256 + page * 65'536 + index * 655];
    }
  }
  return static_cast<uint32_t>(total);
}

bool RefreshArenaChecksum(std::vector<uint8_t>* data) {
  if (!data || data->size() != kArenaPlainSize || ReadBe32(*data, 0x3C) != 0x81) return false;
  WriteBe32(data, 0x44, ArenaChecksum(*data));
  return true;
}

std::optional<std::vector<uint8_t>> RepackMain(const std::vector<uint8_t>& main_file, size_t main_chunk,
                                               const std::vector<uint8_t>& unsealed_plain,
                                               int compression_level = MZ_DEFAULT_COMPRESSION) {
  if (main_chunk > main_file.size() || unsealed_plain.size() < kMainChecksumOffset + 4) return std::nullopt;
  std::vector<uint8_t> plain = unsealed_plain;
  const uint32_t checksum = MainPlainChecksum(plain);
  for (int i = 0; i < 4; ++i) plain[kMainChecksumOffset + i] = static_cast<uint8_t>(checksum >> (24 - 8 * i));
#if defined(WWE2K14_CREATION_SAMPLE_REPRODUCTION)
  if (compression_level == MZ_BEST_COMPRESSION) {
    std::vector<uint8_t> compressed;
    if (!CompressWithSystemZlibBest(plain, &compressed)) return std::nullopt;
    std::vector<uint8_t> result(main_file.begin(),
                                main_file.begin() + static_cast<std::ptrdiff_t>(main_chunk));
    result.insert(result.end(), {'Z', 'L', 'I', 'B'});
    const size_t header_offset = result.size();
    result.resize(header_offset + 12);
    WriteLe32(&result, header_offset, 0x3012);
    WriteLe32(&result, header_offset + 4, static_cast<uint32_t>(compressed.size()));
    WriteLe32(&result, header_offset + 8, static_cast<uint32_t>(plain.size()));
    result.insert(result.end(), compressed.begin(), compressed.end());
    return result;
  }
#endif
  mz_ulong capacity = mz_compressBound(static_cast<mz_ulong>(plain.size()));
  std::vector<uint8_t> compressed(static_cast<size_t>(capacity));
  mz_ulong compressed_size = capacity;
  if (mz_compress2(compressed.data(), &compressed_size, plain.data(),
                   static_cast<mz_ulong>(plain.size()), compression_level) != MZ_OK) {
    return std::nullopt;
  }
  compressed.resize(static_cast<size_t>(compressed_size));
  std::vector<uint8_t> result(main_file.begin(),
                              main_file.begin() + static_cast<std::ptrdiff_t>(main_chunk));
  result.insert(result.end(), {'Z', 'L', 'I', 'B'});
  const size_t header_offset = result.size();
  result.resize(header_offset + 12);
  WriteLe32(&result, header_offset, 0x3012);
  WriteLe32(&result, header_offset + 4, static_cast<uint32_t>(compressed.size()));
  WriteLe32(&result, header_offset + 8, static_cast<uint32_t>(plain.size()));
  result.insert(result.end(), compressed.begin(), compressed.end());
  return result;
}

std::optional<std::vector<uint8_t>> RepackCreationFile(const std::vector<uint8_t>& file, size_t chunk,
                                                       const std::vector<uint8_t>& plain) {
  const auto read_le = [&file](size_t offset) {
    return static_cast<uint32_t>(file[offset]) |
           (static_cast<uint32_t>(file[offset + 1]) << 8) |
           (static_cast<uint32_t>(file[offset + 2]) << 16) |
           (static_cast<uint32_t>(file[offset + 3]) << 24);
  };
  if (chunk > file.size() || file.size() - chunk < 16 ||
      plain.size() > static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) ||
      read_le(chunk + 4) != kContainerVersion) {
    return std::nullopt;
  }
  const uint32_t old_compressed_size = read_le(chunk + 8);
  const size_t old_chunk_end = chunk + 16ull + old_compressed_size;
  if (old_chunk_end > file.size()) return std::nullopt;

  mz_ulong capacity = mz_compressBound(static_cast<mz_ulong>(plain.size()));
  std::vector<uint8_t> compressed(static_cast<size_t>(capacity));
  mz_ulong compressed_size = capacity;
  if (mz_compress2(compressed.data(), &compressed_size, plain.data(),
                   static_cast<mz_ulong>(plain.size()), MZ_DEFAULT_COMPRESSION) != MZ_OK) {
    return std::nullopt;
  }
  compressed.resize(static_cast<size_t>(compressed_size));

  std::vector<uint8_t> result(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(chunk));
  result.insert(result.end(), {'Z', 'L', 'I', 'B'});
  const size_t header = result.size();
  result.resize(header + 12);
  WriteLe32(&result, header, kContainerVersion);
  WriteLe32(&result, header + 4, static_cast<uint32_t>(compressed.size()));
  WriteLe32(&result, header + 8, static_cast<uint32_t>(plain.size()));
  result.insert(result.end(), compressed.begin(), compressed.end());
  result.insert(result.end(), file.begin() + static_cast<std::ptrdiff_t>(old_chunk_end), file.end());
  return result;
}

struct OtherCreationPairs {
  size_t count = 0;
  std::optional<uint32_t> highest_slot;
};

OtherCreationPairs CountOtherCreationPairs(const std::vector<uint8_t>& plain, uint32_t selected_slot) {
  OtherCreationPairs result;
  const size_t paired_slots = std::min(kCawSlotCount, kArenaSlotCount);
  for (uint32_t slot = 0; slot < paired_slots; ++slot) {
    if (slot == selected_slot) continue;
    const size_t caw_offset = RecordOffset(true, slot);
    const size_t arena_offset = RecordOffset(false, slot);
    // Occupied = non-empty, whether or not the record has a recognized shape (#115/#75).
    const bool caw_present = !IsAllZero(plain, caw_offset, kCawSize);
    const bool arena_present = !IsAllZero(plain, arena_offset, kArenaStride);
    if (caw_present && arena_present) {
      ++result.count;
      result.highest_slot = slot;
    }
  }
  return result;
}

std::vector<uint8_t> MakeObservedThumbnail() {
  std::vector<uint8_t> result(kThumbnailSize, 0);
  for (const auto& [offset, value] : kDdsHeaderBytes) result[offset] = value;
  return result;
}

bool IsRecognizedCreationTable(const std::vector<uint8_t>& plain, size_t offset, size_t count,
                               uint8_t maximum) {
  if (offset > plain.size() || count > (plain.size() - offset) / 4) return false;
  for (size_t index = 0; index < count; ++index) {
    const size_t at = offset + index * 4;
    if (plain[at] > maximum || plain[at + 1] != 0 || plain[at + 2] != 0 || plain[at + 3] != 0) return false;
  }
  return true;
}

bool ApplyGeneralCreationPairRulesImpl(std::vector<uint8_t>* main_file,
                                       std::vector<uint8_t>* main_header,
                                       uint32_t slot,
                                       const std::vector<uint8_t>& caw_file,
                                       const std::vector<uint8_t>& arena_file) {
  if (!main_file || !main_header || slot >= kCawSlotCount || slot >= kArenaSlotCount) return false;
  size_t main_chunk = 0, caw_chunk = 0, arena_chunk = 0;
  std::vector<uint8_t> plain, caw_plain, arena_plain;
  if (!UnpackFile(*main_file, &main_chunk, &plain) || main_chunk != kMainContainerOffset ||
      !UnpackFile(caw_file, &caw_chunk, &caw_plain, true) || caw_chunk != 0 ||
      !UnpackFile(arena_file, &arena_chunk, &arena_plain, true) || arena_chunk != 0) {
    return false;
  }
  if (plain.size() != kMainPlainSize) return false;
  const auto caw_record = MakeCawRecord(caw_plain);
  const size_t caw_offset = RecordOffset(true, slot);
  const size_t arena_offset = RecordOffset(false, slot);
  if (!caw_record ||
      !std::equal(caw_record->begin(), caw_record->end(), plain.begin() + static_cast<std::ptrdiff_t>(caw_offset)) ||
      !ArenaRecordMatchesCar(plain, arena_offset, arena_plain) ||
      main_header->size() <= kCreationPairHeaderByte) {
    return false;
  }

  const OtherCreationPairs other_pairs = CountOtherCreationPairs(plain, slot);
  const size_t entry_offset = kCreationEntryBase + static_cast<size_t>(slot) * kCreationEntryStride;
  const size_t marker_offset = kCreationMarkerBase + static_cast<size_t>(slot) * kCreationMarkerStride;
  const size_t thumbnail_offset = kThumbnailBase + static_cast<size_t>(slot) * kThumbnailStride;
  if (entry_offset > plain.size() || kCreationEntryBytes.size() > plain.size() - entry_offset ||
      marker_offset >= plain.size() ||
      thumbnail_offset > main_chunk || kThumbnailSize > main_chunk - thumbnail_offset ||
      (plain[0x76C9F] != 0 && plain[0x76C9F] != 1) ||
      (plain[0x1A4298] != 0 && plain[0x1A4298] != 3) ||
      (plain[0xFE83F] != 0x64 && plain[0xFE83F] > 0x63)) {
    return false;
  }
  const bool inactive_text_is_before =
      std::equal(kBeforeInactiveText.begin(), kBeforeInactiveText.end(), plain.begin() + 0xB6D24);
  const bool inactive_text_is_set =
      std::equal(kInactiveText.begin(), kInactiveText.end(), plain.begin() + 0xB6D24);
  if (!inactive_text_is_before && !inactive_text_is_set) return false;
  if (!IsAllZero(plain, entry_offset, kCreationEntryBytes.size()) &&
      !std::equal(kCreationEntryBytes.begin(), kCreationEntryBytes.end(),
                  plain.begin() + static_cast<std::ptrdiff_t>(entry_offset))) {
    return false;
  }
  if (plain[marker_offset] != 0 && plain[marker_offset] != 0x9C) return false;
  for (size_t offset : kFirstPairFlags) {
    if (plain[offset] != 0 && plain[offset] != 1) return false;
  }
  const uint8_t previous_last_slot = other_pairs.highest_slot
                                         ? static_cast<uint8_t>(*other_pairs.highest_slot)
                                         : 0x64;
  // 0xFE83F is the last-added CAW index (ApplyCawAddBookkeeping); the range is checked above. The samples only show it
  // moving from the previous pair slot to this slot, so it is rewritten only in that case: a save whose last CAW is
  // elsewhere (Best of Save registered CAWs: 0x3E) keeps its value instead of refusing the pair.
  const bool last_slot_follows_pairs =
      plain[0xFE83F] == previous_last_slot || plain[0xFE83F] == static_cast<uint8_t>(slot);
  if (0x16 + 2 * other_pairs.count > 0xFF) return false;
  const uint8_t expected_header_value =
      other_pairs.count == 0 ? 0x02 : static_cast<uint8_t>(0x16 + 2 * (other_pairs.count - 1));
  const uint8_t desired_header_value = static_cast<uint8_t>(0x16 + 2 * other_pairs.count);
  const bool pair_header_value_observed = (*main_header)[kCreationPairHeaderByte] == expected_header_value ||
                                           (*main_header)[kCreationPairHeaderByte] == desired_header_value;
  // Owner samples establish only the 0x02 -> 0x16 -> 0x18 sequence. The supplied real save starts at 0x7E, and no
  // before/after evidence establishes that value's meaning or an offset rule. Accept an unrecognized value without
  // interpreting it, and leave it unchanged even as additional pairs are imported.

  std::vector<uint8_t> thumbnail = MakeObservedThumbnail();
  const bool thumbnail_is_zero = std::all_of(
      main_file->begin() + static_cast<std::ptrdiff_t>(thumbnail_offset),
      main_file->begin() + static_cast<std::ptrdiff_t>(thumbnail_offset + kThumbnailSize),
      [](uint8_t value) { return value == 0; });
  const bool thumbnail_is_observed = std::equal(
      thumbnail.begin(), thumbnail.end(),
      main_file->begin() + static_cast<std::ptrdiff_t>(thumbnail_offset));
  if (!thumbnail_is_zero && !thumbnail_is_observed) return false;

  // RepackMain recomputes the plain checksum at 0x18.
  plain[0x76C9F] = 1;
  std::copy(kCreationEntryBytes.begin(), kCreationEntryBytes.end(),
            plain.begin() + static_cast<std::ptrdiff_t>(entry_offset));
  std::copy(kInactiveText.begin(), kInactiveText.end(), plain.begin() + 0xB6D24);
  if (last_slot_follows_pairs) plain[0xFE83F] = static_cast<uint8_t>(slot);
  for (const size_t offset : kFirstPairFlags) plain[offset] = 1;
  plain[0x1A4298] = 3;
  plain[marker_offset] = 0x9C;
  if (other_pairs.count > 0) {
    // The observed two-pair transition sets zero entries to one. Do not normalize the accepted value 2: its meaning
    // is unknown and the supplied save already contains it.
    for (size_t index = 0; index < kFirstObservedArrayCount; ++index) {
      if (IsAllZero(plain, kFirstObservedArrayBase + 4 * index, 4)) plain[kFirstObservedArrayBase + 4 * index] = 1;
    }
    for (size_t index = 0; index < kSecondObservedArrayCount; ++index) {
      if (IsAllZero(plain, kSecondObservedArrayBase + 4 * index, 4)) plain[kSecondObservedArrayBase + 4 * index] = 1;
    }
    plain[0x11283C] |= 0x80;
  }

  auto repacked = RepackMain(*main_file, main_chunk, plain);
  if (!repacked || repacked->size() < thumbnail_offset + kThumbnailSize) return false;
  if (thumbnail_is_zero) {
    std::copy(thumbnail.begin(), thumbnail.end(),
              repacked->begin() + static_cast<std::ptrdiff_t>(thumbnail_offset));
  }
  if (pair_header_value_observed) (*main_header)[kCreationPairHeaderByte] = desired_header_value;
  *main_file = std::move(*repacked);
  return true;
}

void ApplyCawRemoveBookkeeping(std::vector<uint8_t>* plain, uint32_t slot);

#if defined(WWE2K14_CREATION_SAMPLE_REPRODUCTION)
std::string Sha256Hex(const std::vector<uint8_t>& bytes) {
  return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}
#endif

bool ApplyObservedGameMadePairImpl(std::vector<uint8_t>* main_file,
                                   std::vector<uint8_t>* main_header,
                                   const std::vector<uint8_t>& caw_file,
                                   const std::vector<uint8_t>& caw_header,
                                   const std::vector<uint8_t>& arena_file,
                                   const std::vector<uint8_t>& arena_header) {
#if defined(WWE2K14_CREATION_SAMPLE_REPRODUCTION)
  // This is deliberately a single exact-sample rule, not an interpretation of
  // the opaque fields. The flag is OFF by default; these identities bind the
  // patch to save-before and the two game-created slot-00 payload/header files.
  constexpr std::string_view kBeforePlainSha256 =
      "c0455011e3506a02ac7e82289f60e68a75c4ccbc89901d115d96692664f41aea";
  constexpr std::string_view kBeforeMainHeaderSha256 =
      "35a041a4b50dc62506c68096a2c622229ec7cca126e61dd44355be73cac93918";
  constexpr std::string_view kGameCawFileSha256 =
      "2819a296f2d3778c13633ad331e73c2872e82fa3d3746fb3dd61c51b3a950867";
  constexpr std::string_view kGameCawHeaderSha256 =
      "f2bce6931ac0c6f2e049d4bcdce7e8811b7900697ace604c7880386f24cf9ad5";
  constexpr std::string_view kGameArenaFileSha256 =
      "475cbf8ff22aea95be10c326d465fb62b0f3b0e07f55fa31121501eeda5590a7";
  constexpr std::string_view kGameArenaHeaderSha256 =
      "9a0a9780831c8c768bef20b5869113267d07af98b3a0f1a458e92422794b743e";

  if (!main_file || !main_header || Sha256Hex(caw_file) != kGameCawFileSha256 ||
      Sha256Hex(caw_header) != kGameCawHeaderSha256 ||
      Sha256Hex(arena_file) != kGameArenaFileSha256 ||
      Sha256Hex(arena_header) != kGameArenaHeaderSha256 ||
      Sha256Hex(*main_header) != kBeforeMainHeaderSha256) {
    return false;
  }

  size_t main_chunk = 0, caw_chunk = 0, arena_chunk = 0;
  std::vector<uint8_t> plain, caw_plain, arena_plain;
  if (!UnpackFile(*main_file, &main_chunk, &plain) || main_chunk != kMainContainerOffset ||
      !UnpackFile(caw_file, &caw_chunk, &caw_plain, true) || caw_chunk != 0 ||
      !UnpackFile(arena_file, &arena_chunk, &arena_plain, true) || arena_chunk != 0) {
    return false;
  }
  const auto caw_record = MakeCawRecord(caw_plain);
  const auto arena_record = MakeArenaRecord(arena_plain);
  if (!caw_record || !arena_record ||
      !std::equal(caw_record->begin(), caw_record->end(), plain.begin() + kCawBase) ||
      !std::equal(arena_record->begin(), arena_record->end(), plain.begin() + kArenaBase)) {
    return false;
  }

  std::vector<uint8_t> normalized = plain;
  std::fill(normalized.begin() + kCawBase, normalized.begin() + kCawBase + kCawStride, 0);
  std::fill(normalized.begin() + kArenaBase, normalized.begin() + kArenaBase + kArenaStride, 0);
  // The single-record inserts also wrote the CAW bookkeeping (entry, marker, flags); undo it to reach save-before.
  ApplyCawRemoveBookkeeping(&normalized, 0);
  // The record inserts already resealed the checksum; compare against save-before's own value.
  const std::array<uint8_t, 4> before_checksum = {0x01, 0xF8, 0x5E, 0xE0};
  std::copy(before_checksum.begin(), before_checksum.end(), normalized.begin() + kMainChecksumOffset);
  if (Sha256Hex(normalized) != kBeforePlainSha256) return false;

  std::vector<uint8_t> generally_updated_file = *main_file;
  std::vector<uint8_t> generally_updated_header = *main_header;
  if (!ApplyGeneralCreationPairRulesImpl(&generally_updated_file, &generally_updated_header,
                                         0, caw_file, arena_file)) {
    return false;
  }
  size_t updated_chunk = 0;
  if (!UnpackFile(generally_updated_file, &updated_chunk, &plain) || updated_chunk != main_chunk) return false;
  auto repacked = RepackMain(generally_updated_file, updated_chunk, plain, MZ_BEST_COMPRESSION);
  if (!repacked) return false;
  *main_file = std::move(*repacked);
  *main_header = std::move(generally_updated_header);
  return true;
#else
  (void)main_file;
  (void)main_header;
  (void)caw_file;
  (void)caw_header;
  (void)arena_file;
  (void)arena_header;
  return false;
#endif
}

size_t RecordOffset(bool superstar, uint32_t slot) {
  return superstar ? kCawBase + static_cast<size_t>(slot) * kCawStride
                   : kArenaBase + static_cast<size_t>(slot) * kArenaStride;
}

size_t RecordSize(bool superstar) {
  return superstar ? kCawSize : kArenaStride;
}

size_t RecordCount(bool superstar) {
  return superstar ? kCawSlotCount : kArenaSlotCount;
}

bool IsRecordEmpty(const std::vector<uint8_t>& plain, bool superstar, uint32_t slot) {
  return IsAllZero(plain, RecordOffset(superstar, slot), RecordSize(superstar));
}

bool IsKnownRecord(const std::vector<uint8_t>& plain, bool superstar, uint32_t slot) {
  const size_t offset = RecordOffset(superstar, slot);
  return superstar ? IsCawRecord(plain, offset) : IsArenaRecord(plain, offset);
}

// Per-CAW bookkeeping the GAME writes when it adds a creation (owner samples save-before -> after -> after2, see
// docs/cards/F1-caw-import-corrupted.md "BLANK NAME"). Tables with 100 slots are CAW-indexed: per-slot entry
// (plain 0x76CC0 + slot*0x168: 01 90 02 8A 02 BC), marker (plain 0x1A6251 + slot*0x40 = 0x9C) and thumbnail DDS
// header (file 0x1980 + slot*0x2200). Without entry+marker the game lists the CAW with a blank name.
// The samples add one CAW and one arena together, so the global "first creation" bytes cannot be attributed to
// one kind; they are applied for CAW adds (FE83F is a 100-slot CAW index: 0x64 = none).
constexpr size_t kLastCawSlotByte = 0xFE83F;
constexpr uint8_t kNoCawSlot = 0x64;
constexpr size_t kInactiveTextOffset = 0xB6D24;
constexpr size_t kFirstCreationFlag = 0x1A4298;
constexpr size_t kSecondCreationFlagByte = 0x11283C;

size_t CountOtherCawRecords(const std::vector<uint8_t>& plain, uint32_t slot,
                            std::optional<uint32_t>* highest) {
  size_t count = 0;
  for (uint32_t other = 0; other < kCawSlotCount; ++other) {
    if (other == slot || IsAllZero(plain, RecordOffset(true, other), kCawSize)) continue;
    ++count;
    if (highest) *highest = other;
  }
  return count;
}

void ApplyCawAddBookkeeping(std::vector<uint8_t>* plain, uint32_t slot) {
  const size_t entry = kCreationEntryBase + static_cast<size_t>(slot) * kCreationEntryStride;
  const size_t marker = kCreationMarkerBase + static_cast<size_t>(slot) * kCreationMarkerStride;
  std::copy(kCreationEntryBytes.begin(), kCreationEntryBytes.end(),
            plain->begin() + static_cast<std::ptrdiff_t>(entry));
  (*plain)[marker] = 0x9C;
  const size_t others = CountOtherCawRecords(*plain, slot, nullptr);
  (*plain)[kLastCawSlotByte] = static_cast<uint8_t>(slot);
  const bool text_before = std::equal(kBeforeInactiveText.begin(), kBeforeInactiveText.end(),
                                      plain->begin() + kInactiveTextOffset);
  const bool text_set = std::equal(kInactiveText.begin(), kInactiveText.end(),
                                   plain->begin() + kInactiveTextOffset);
  if (text_before || text_set) {
    std::copy(kInactiveText.begin(), kInactiveText.end(), plain->begin() + kInactiveTextOffset);
  }
  for (const size_t offset : kFirstPairFlags) (*plain)[offset] = 1;
  (*plain)[kFirstCreationFlag] = 3;
  if (others > 0) {
    // Preserve non-zero entries in these meaning-unknown tables; in particular, the supplied save's value 2 has no
    // measured transition to value 1.
    for (size_t index = 0; index < kFirstObservedArrayCount; ++index) {
      if (IsAllZero(*plain, kFirstObservedArrayBase + 4 * index, 4)) (*plain)[kFirstObservedArrayBase + 4 * index] = 1;
    }
    for (size_t index = 0; index < kSecondObservedArrayCount; ++index) {
      if (IsAllZero(*plain, kSecondObservedArrayBase + 4 * index, 4)) (*plain)[kSecondObservedArrayBase + 4 * index] = 1;
    }
    (*plain)[kSecondCreationFlagByte] |= 0x80;
  }
}

// Undoes the per-slot parts (entry, marker, last-created index). The global flags and arrays are left alone:
// they record that the player has created something and the samples do not show the game clearing them.
void ApplyCawRemoveBookkeeping(std::vector<uint8_t>* plain, uint32_t slot) {
  const size_t entry = kCreationEntryBase + static_cast<size_t>(slot) * kCreationEntryStride;
  const size_t marker = kCreationMarkerBase + static_cast<size_t>(slot) * kCreationMarkerStride;
  if (std::equal(kCreationEntryBytes.begin(), kCreationEntryBytes.end(),
                 plain->begin() + static_cast<std::ptrdiff_t>(entry))) {
    std::fill(plain->begin() + static_cast<std::ptrdiff_t>(entry),
              plain->begin() + static_cast<std::ptrdiff_t>(entry + kCreationEntryBytes.size()), 0);
  }
  if ((*plain)[marker] == 0x9C) (*plain)[marker] = 0;
  if ((*plain)[kLastCawSlotByte] == slot) {
    std::optional<uint32_t> highest;
    CountOtherCawRecords(*plain, slot, &highest);
    (*plain)[kLastCawSlotByte] = highest ? static_cast<uint8_t>(*highest) : kNoCawSlot;
  }
  // Last creation of any kind gone: return the global "first creation" state to its pre-creation (save-before) value.
  bool any_arena = false;
  for (uint32_t arena = 0; arena < kArenaSlotCount && !any_arena; ++arena) {
    any_arena = !IsAllZero(*plain, RecordOffset(false, arena), kArenaStride);
  }
  if (CountOtherCawRecords(*plain, slot, nullptr) == 0 && !any_arena) {
    if (std::equal(kInactiveText.begin(), kInactiveText.end(), plain->begin() + kInactiveTextOffset)) {
      std::copy(kBeforeInactiveText.begin(), kBeforeInactiveText.end(), plain->begin() + kInactiveTextOffset);
    }
    for (const size_t offset : kFirstPairFlags) (*plain)[offset] = 0;
    (*plain)[kFirstCreationFlag] = 0;
    // Only plain recognized entries (u32 0..max) go back to 0; anything else a tolerated save carries stays as it is
    // (Fable review 11-15: zeroing byte 0 of an odd entry changed values the tolerant gate promises to keep).
    const auto reset_table = [&](size_t base, size_t count, uint8_t maximum) {
      for (size_t index = 0; index < count; ++index) {
        const size_t at = base + 4 * index;
        if ((*plain)[at] <= maximum && (*plain)[at + 1] == 0 && (*plain)[at + 2] == 0 && (*plain)[at + 3] == 0) (*plain)[at] = 0;
      }
    };
    reset_table(kFirstObservedArrayBase, kFirstObservedArrayCount, 1);
    reset_table(kSecondObservedArrayBase, kSecondObservedArrayCount, 2);
    (*plain)[kSecondCreationFlagByte] &= 0x7F;
  }
}

std::string ReadCreationName(const std::vector<uint8_t>& plain, bool superstar, uint32_t slot) {
  const size_t base = RecordOffset(superstar, slot);
  // CAW: ASCII display name at record +0xD1 (also at +0x115 short / +0x13D); arena: ASCII at record +0x39, up to 22
  // chars (game-written names in the GitHub #64 save run through +0x4E).
  const size_t begin = base + (superstar ? 0xD1 : 0x39);
  const size_t limit = superstar ? 0x2A : 0x16;
  std::string name;
  for (size_t i = 0; i < limit && begin + i < plain.size() && plain[begin + i] >= 0x20 && plain[begin + i] < 0x7F; ++i) {
    name.push_back(static_cast<char>(plain[begin + i]));
  }
  if (superstar && name.empty()) {
    for (size_t i = 0; i < limit && plain[base + 0x13D + i] >= 0x20 && plain[base + 0x13D + i] < 0x7F; ++i) {
      name.push_back(static_cast<char>(plain[base + 0x13D + i]));
    }
  }
  return name;
}

bool ThumbnailRange(size_t main_chunk, uint32_t slot, size_t* offset) {
  *offset = kThumbnailBase + static_cast<size_t>(slot) * kThumbnailStride;
  return *offset <= main_chunk && kThumbnailSize <= main_chunk - *offset;
}

// Strict check: every side-table value and every record has a recognized shape. Informational only since #115/#75;
// the write paths use the tolerant check below.
bool IsFullyRecognizedCreationPlainImpl(const std::vector<uint8_t>& plain) {
  // Only constrain the byte-mapped record arrays and their bounded ranges. Progression/global fields elsewhere
  // are not written by this single-record builder and must not become an accidental save-identity gate.
  if (plain.size() != kMainPlainSize || !IsRecognizedCreationSideTables(plain)) return false;
  for (uint32_t slot = 0; slot < kCawSlotCount; ++slot) {
    if (!IsKnownRecord(plain, true, slot)) return false;
  }
  for (uint32_t slot = 0; slot < kArenaSlotCount; ++slot) {
    if (!IsKnownRecord(plain, false, slot)) return false;
  }
  return true;
}

// Tolerant gate (#115/#75, two players' game-written saves were refused whole): the plain size is the only hard
// requirement. A non-empty record of unknown shape is simply OCCUPIED (never chosen, never modified, never removed:
// IsRecordEmpty is strict and removal/rename/match still require IsKnownRecord) and side-table values outside the
// whitelist are preserved byte-for-byte (the add bookkeeping only changes whole-u32 zero entries).
bool IsSupportedCreationPlainImpl(const std::vector<uint8_t>& plain) {
  return plain.size() == kMainPlainSize;
}

struct CreationToleranceCounts {
  size_t caw = 0, arena = 0, table_values = 0;
};

CreationToleranceCounts CountUnrecognizedCreationData(const std::vector<uint8_t>& plain) {
  CreationToleranceCounts counts;
  if (plain.size() != kMainPlainSize) return counts;
  for (uint32_t slot = 0; slot < kCawSlotCount; ++slot) {
    if (!IsKnownRecord(plain, true, slot)) ++counts.caw;
  }
  for (uint32_t slot = 0; slot < kArenaSlotCount; ++slot) {
    if (!IsKnownRecord(plain, false, slot)) ++counts.arena;
  }
  const auto count_table = [&](size_t offset, size_t count, uint8_t maximum) {
    for (size_t index = 0; index < count; ++index) {
      const size_t at = offset + index * 4;
      if (plain[at] > maximum || plain[at + 1] != 0 || plain[at + 2] != 0 || plain[at + 3] != 0) ++counts.table_values;
    }
  };
  count_table(kFirstObservedArrayBase, kFirstObservedArrayCount, 1);
  count_table(kSecondObservedArrayBase, kSecondObservedArrayCount, 2);
  return counts;
}

// Names the first check IsFullyRecognizedCreationPlainImpl fails, in the same order (#115: the refusal text alone could
// not tell which part of a player's save was rejected). Returns "" when every check passes.
std::string DescribeUnsupportedCreationPlainImpl(const std::vector<uint8_t>& plain) {
  std::ostringstream out;
  if (plain.size() != kMainPlainSize) {
    out << "plain size " << plain.size() << ", expected " << kMainPlainSize;
    return out.str();
  }
  const auto table_problem = [&](const char* label, size_t offset, size_t count, uint8_t maximum) {
    for (size_t index = 0; index < count; ++index) {
      const size_t at = offset + index * 4;
      if (plain[at] > maximum || plain[at + 1] != 0 || plain[at + 2] != 0 || plain[at + 3] != 0) {
        out << label << " table entry " << index << " = bytes " << static_cast<int>(plain[at]) << ','
            << static_cast<int>(plain[at + 1]) << ',' << static_cast<int>(plain[at + 2]) << ','
            << static_cast<int>(plain[at + 3]) << " (max accepted " << static_cast<int>(maximum) << ')';
        return true;
      }
    }
    return false;
  };
  if (table_problem("first", kFirstObservedArrayBase, kFirstObservedArrayCount, 1) ||
      table_problem("second", kSecondObservedArrayBase, kSecondObservedArrayCount, 2)) {
    return out.str();
  }
  for (uint32_t slot = 0; slot < kCawSlotCount; ++slot) {
    const size_t offset = RecordOffset(true, slot);
    if (IsCawRecord(plain, offset)) continue;
    out << "CAW slot " << slot << " rejected: +0x6ED=" << static_cast<int>(plain[offset + 0x6ED])
        << " (need 1), +0x7FC=" << static_cast<int>(plain[offset + 0x7FC]) << " (need 0), head id +0x811="
        << ReadBe32(plain, offset + 0x811) << " (accepted 100-499 or 512), checksum bytes +0x7FD "
        << (IsAllZero(plain, offset + 0x7FD, 4) ? "all zero (need non-zero)" : "present");
    return out.str();
  }
  for (uint32_t slot = 0; slot < kArenaSlotCount; ++slot) {
    const size_t offset = RecordOffset(false, slot);
    if (IsArenaRecord(plain, offset)) continue;
    out << "arena slot " << slot << " rejected: first byte " << static_cast<int>(plain[offset]) << " (need 0x81)";
    return out.str();
  }
  return "";
}

}  // namespace

std::string DescribeUnsupportedCreationPlain(const std::vector<uint8_t>& plain) {
  return DescribeUnsupportedCreationPlainImpl(plain);
}

bool ApplyGeneralCreationPairRules(std::vector<uint8_t>* main_file,
                                   std::vector<uint8_t>* main_header,
                                   uint32_t slot,
                                   const std::vector<uint8_t>& caw_file,
                                   const std::vector<uint8_t>& arena_file) {
  return ApplyGeneralCreationPairRulesImpl(main_file, main_header, slot, caw_file, arena_file);
}

bool ApplyObservedGameMadePair(std::vector<uint8_t>* main_file,
                               std::vector<uint8_t>* main_header,
                               const std::vector<uint8_t>& caw_file,
                               const std::vector<uint8_t>& caw_header,
                               const std::vector<uint8_t>& arena_file,
                               const std::vector<uint8_t>& arena_header) {
  return ApplyObservedGameMadePairImpl(main_file, main_header, caw_file, caw_header,
                                       arena_file, arena_header);
}

bool IsSupportedCreationPlain(const std::vector<uint8_t>& plain) {
  return IsFullyRecognizedCreationPlainImpl(plain);
}

bool IsTolerableCreationPlain(const std::vector<uint8_t>& plain) {
  return IsSupportedCreationPlainImpl(plain);
}

std::string DescribeCreationTolerance(const std::vector<uint8_t>& plain) {
  const CreationToleranceCounts counts = CountUnrecognizedCreationData(plain);
  if (counts.caw == 0 && counts.arena == 0 && counts.table_values == 0) return "";
  std::ostringstream out;
  out << "save has " << counts.caw << " unrecognized CAW records / " << counts.arena << " arena records / "
      << counts.table_values << " side-table values; they are kept as they are";
  return out.str();
}

std::optional<std::vector<uint32_t>> ListKnownCreationSlots(const std::vector<uint8_t>& main_file,
                                                            bool superstar) {
  size_t chunk = 0;
  std::vector<uint8_t> plain;
  if (!UnpackFile(main_file, &chunk, &plain) || chunk != kMainContainerOffset ||
       !IsSupportedCreationPlainImpl(plain)) {
    return std::nullopt;
  }
  std::vector<uint32_t> result;
  for (uint32_t slot = 0; slot < RecordCount(superstar); ++slot) {
    if (!IsRecordEmpty(plain, superstar, slot)) result.push_back(slot);
  }
  return result;
}

std::optional<uint32_t> FindKnownFreeCreationSlot(const std::vector<uint8_t>& main_file,
                                                  bool superstar) {
  size_t chunk = 0;
  std::vector<uint8_t> plain;
  if (!UnpackFile(main_file, &chunk, &plain) || chunk != kMainContainerOffset ||
       !IsSupportedCreationPlainImpl(plain)) {
    return std::nullopt;
  }
  for (uint32_t slot = 0; slot < RecordCount(superstar); ++slot) {
    if (IsRecordEmpty(plain, superstar, slot)) return slot;
  }
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> AddKnownCreation(const std::vector<uint8_t>& main_file,
                                                     const std::vector<uint8_t>& creation_file,
                                                     bool superstar, uint32_t slot) {
  size_t main_chunk = 0, creation_chunk = 0;
  std::vector<uint8_t> plain, creation_plain;
  if (!UnpackFile(main_file, &main_chunk, &plain) || main_chunk != kMainContainerOffset ||
      !IsSupportedCreationPlainImpl(plain) || slot >= RecordCount(superstar) ||
      !IsRecordEmpty(plain, superstar, slot) ||
      !UnpackFile(creation_file, &creation_chunk, &creation_plain, true) || creation_chunk != 0 ||
      creation_plain.size() != (superstar ? kCawPlainSize : kArenaPlainSize)) {
    return std::nullopt;
  }
  std::optional<std::vector<uint8_t>> record = superstar ? MakeCawRecord(creation_plain)
                                                         : MakeArenaRecord(creation_plain);
  if (!record || record->size() != RecordSize(superstar)) return std::nullopt;
  std::copy(record->begin(), record->end(),
            plain.begin() + static_cast<std::ptrdiff_t>(RecordOffset(superstar, slot)));
  if (!superstar) return RepackMain(main_file, main_chunk, plain);
  size_t thumbnail_offset = 0;
  if (!ThumbnailRange(main_chunk, slot, &thumbnail_offset)) return std::nullopt;
  ApplyCawAddBookkeeping(&plain, slot);
  auto repacked = RepackMain(main_file, main_chunk, plain);
  if (!repacked) return std::nullopt;
  const auto thumbnail_begin = repacked->begin() + static_cast<std::ptrdiff_t>(thumbnail_offset);
  if (std::all_of(thumbnail_begin, thumbnail_begin + static_cast<std::ptrdiff_t>(kThumbnailSize),
                  [](uint8_t value) { return value == 0; })) {
    const std::vector<uint8_t> thumbnail = MakeObservedThumbnail();
    std::copy(thumbnail.begin(), thumbnail.end(), thumbnail_begin);
  }
  return repacked;
}

bool CreationFileMatchesMainRecord(const std::vector<uint8_t>& main_file,
                                   const std::vector<uint8_t>& creation_file,
                                   bool superstar, uint32_t slot) {
  size_t main_chunk = 0, creation_chunk = 0;
  std::vector<uint8_t> main_plain, creation_plain;
  if (!UnpackFile(main_file, &main_chunk, &main_plain) || main_chunk != kMainContainerOffset ||
      !IsSupportedCreationPlainImpl(main_plain) ||
      ReadBe32(main_plain, kMainChecksumOffset) != MainPlainChecksum(main_plain) ||
      slot >= RecordCount(superstar) || IsRecordEmpty(main_plain, superstar, slot) ||
      !IsKnownRecord(main_plain, superstar, slot) ||
      !UnpackFile(creation_file, &creation_chunk, &creation_plain, true) || creation_chunk != 0 ||
      creation_plain.size() != (superstar ? kCawPlainSize : kArenaPlainSize)) {
    return false;
  }
  const size_t offset = RecordOffset(superstar, slot);
  if (!superstar) return ArenaRecordMatchesCar(main_plain, offset, creation_plain);
  const auto expected_record = MakeCawRecord(creation_plain);
  if (!expected_record || expected_record->size() != RecordSize(superstar)) return false;
  if (std::equal(expected_record->begin(), expected_record->end(),
                 main_plain.begin() + static_cast<std::ptrdiff_t>(offset))) {
    return true;
  }
  // Game-edited CAWs (owner save 2026-10-31: slots 1, 2, 3 of nine) keep record bytes the game writes differently from
  // the CAS copy (+0x001, +0x074, WWE-head +0x813/+0x814, ...), so the exact rebuild fails although the payload is the
  // slot's own. The record stores the payload's CAS checksum (+0x7FD..+0x800 = CAS +0x0C..+0x0F); a matching checksum
  // ties the payload to this slot.
  return std::equal(creation_plain.begin() + 0x0C, creation_plain.begin() + 0x10,
                    main_plain.begin() + static_cast<std::ptrdiff_t>(offset + 0x7FD));
}

bool IsValidCreationDisplayName(std::string_view name, bool superstar) noexcept {
  const size_t limit = superstar ? 0x2A : 0x16;
  if (name.empty() || name.size() > limit) return false;
  for (unsigned char character : name) {
    if (character < 0x20 || character > 0x7E) return false;
  }
  return true;
}

namespace {

void WriteFixedAsciiName(std::vector<uint8_t>* bytes, size_t offset, size_t field_size,
                         std::string_view name) {
  std::fill(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
            bytes->begin() + static_cast<std::ptrdiff_t>(offset + field_size), 0);
  std::copy(name.begin(), name.end(), bytes->begin() + static_cast<std::ptrdiff_t>(offset));
}

}  // namespace

std::optional<CreationRenameFiles> RewriteKnownCreationName(const std::vector<uint8_t>& main_file,
                                                            const std::vector<uint8_t>& payload_file,
                                                            bool superstar, uint32_t slot,
                                                            std::string_view new_name) {
  if (!IsValidCreationDisplayName(new_name, superstar)) return std::nullopt;

  size_t main_chunk = 0, payload_chunk = 0;
  std::vector<uint8_t> main_plain, payload_plain;
  if (!UnpackFile(main_file, &main_chunk, &main_plain) || main_chunk != kMainContainerOffset ||
      !IsSupportedCreationPlainImpl(main_plain) ||
      ReadBe32(main_plain, kMainChecksumOffset) != MainPlainChecksum(main_plain) ||
      slot >= RecordCount(superstar) || IsRecordEmpty(main_plain, superstar, slot) ||
      !UnpackFile(payload_file, &payload_chunk, &payload_plain, true) || payload_chunk != 0 ||
      payload_plain.size() != (superstar ? kCawPlainSize : kArenaPlainSize) ||
      !CreationFileMatchesMainRecord(main_file, payload_file, superstar, slot)) {
    return std::nullopt;
  }

  const std::string current_name = ReadCreationName(main_plain, superstar, slot);
  if (current_name == new_name) return CreationRenameFiles{main_file, payload_file, main_plain};

  if (superstar) {
    const uint32_t checksum = CasChecksum(payload_plain);
    if (ReadBe32(payload_plain, 0) != 55 || ReadBe32(payload_plain, 4) != 42 ||
        ReadBe32(payload_plain, 8) != 23 || ReadBe32(payload_plain, 0x1CB7A8) != checksum) {
      return std::nullopt;
    }
    // The full display name is stored twice; +0x115 is the separate nickname and is intentionally preserved.
    WriteFixedAsciiName(&payload_plain, 0x1CA40F + 0xD1, 0x2A, new_name);
    WriteFixedAsciiName(&payload_plain, 0x1CA40F + 0x13D, 0x2A, new_name);
    if (!RefreshCasChecksum(&payload_plain)) return std::nullopt;
  } else {
    if (ReadBe32(payload_plain, 0x3C) != 0x81 ||
        ReadBe32(payload_plain, 0x44) != ArenaChecksum(payload_plain)) {
      return std::nullopt;
    }
    WriteFixedAsciiName(&payload_plain, 0x48, 0x16, new_name);
    if (!RefreshArenaChecksum(&payload_plain)) return std::nullopt;
  }

  const auto updated_payload = RepackCreationFile(payload_file, payload_chunk, payload_plain);
  if (!updated_payload) return std::nullopt;
  size_t verified_payload_chunk = 0;
  std::vector<uint8_t> verified_payload_plain;
  if (!UnpackFile(*updated_payload, &verified_payload_chunk, &verified_payload_plain, true) ||
      verified_payload_chunk != 0 || verified_payload_plain != payload_plain) {
    return std::nullopt;
  }

  const size_t record = RecordOffset(superstar, slot);
  if (superstar) {
    WriteFixedAsciiName(&main_plain, record + 0xD1, 0x2A, new_name);
    WriteFixedAsciiName(&main_plain, record + 0x13D, 0x2A, new_name);
    std::copy(payload_plain.begin() + 0x0C, payload_plain.begin() + 0x13,
              main_plain.begin() + static_cast<std::ptrdiff_t>(record + 0x7FD));
  } else {
    std::copy(payload_plain.begin() + 0x45, payload_plain.begin() + 0x48,
              main_plain.begin() + static_cast<std::ptrdiff_t>(record + 0x36));
    WriteFixedAsciiName(&main_plain, record + 0x39, 0x16, new_name);
  }

  const auto updated_main = RepackMain(main_file, main_chunk, main_plain);
  if (!updated_main) return std::nullopt;
  const auto verified_main = Unpack(*updated_main);
  if (!verified_main || verified_main->chunk_offset != kMainContainerOffset ||
      !IsSupportedCreationPlainImpl(verified_main->plain) ||
      ReadBe32(verified_main->plain, kMainChecksumOffset) != MainPlainChecksum(verified_main->plain) ||
      !CreationFileMatchesMainRecord(*updated_main, *updated_payload, superstar, slot)) {
    return std::nullopt;
  }
  const auto listed = ListKnownCreationNames(*updated_main, superstar);
  if (!listed || std::none_of(listed->begin(), listed->end(), [slot, new_name](const CreationName& item) {
        return item.slot == slot && item.name == new_name;
      })) {
    return std::nullopt;
  }
  return CreationRenameFiles{*updated_main, *updated_payload, verified_main->plain};
}

std::optional<std::vector<uint8_t>> UpgradeOlderArenaRecord(const std::vector<uint8_t>& main_file,
                                                            const std::vector<uint8_t>& creation_file,
                                                            uint32_t slot, bool* upgraded) {
  *upgraded = false;
  size_t main_chunk = 0, creation_chunk = 0;
  std::vector<uint8_t> plain, car;
  if (!UnpackFile(main_file, &main_chunk, &plain) || main_chunk != kMainContainerOffset ||
      !IsSupportedCreationPlainImpl(plain) || slot >= kArenaSlotCount || IsRecordEmpty(plain, false, slot) ||
      !UnpackFile(creation_file, &creation_chunk, &car, true) || creation_chunk != 0 ||
      car.size() != kArenaPlainSize) {
    return std::nullopt;
  }
  const size_t offset = RecordOffset(false, slot);
  const auto full = MakeArenaRecord(car);
  if (!full) return std::nullopt;
  if (std::equal(full->begin(), full->end(), plain.begin() + static_cast<std::ptrdiff_t>(offset))) return main_file;
  // Only a record an earlier launcher built from this exact payload is rewritten; anything else is left alone.
  if (!ArenaRecordMatchesCar(plain, offset, car)) return main_file;
  std::copy(full->begin(), full->end(), plain.begin() + static_cast<std::ptrdiff_t>(offset));
  auto repacked = RepackMain(main_file, main_chunk, plain);
  if (repacked) *upgraded = true;
  return repacked;
}

std::optional<std::vector<uint8_t>> RepairCawBookkeeping(const std::vector<uint8_t>& main_file,
                                                          uint32_t* repaired) {
  if (repaired) *repaired = 0;
  size_t main_chunk = 0;
  std::vector<uint8_t> plain;
  if (!UnpackFile(main_file, &main_chunk, &plain) || main_chunk != kMainContainerOffset ||
      !IsSupportedCreationPlainImpl(plain)) {
    return std::nullopt;
  }
  const std::vector<uint8_t> thumbnail = MakeObservedThumbnail();
  std::vector<uint32_t> fixed;
  std::vector<size_t> thumbnail_offsets;
  for (uint32_t slot = 0; slot < kCawSlotCount; ++slot) {
    if (IsRecordEmpty(plain, true, slot) || !IsKnownRecord(plain, true, slot)) continue;  // unrecognized: untouched
    const size_t entry = kCreationEntryBase + static_cast<size_t>(slot) * kCreationEntryStride;
    const size_t marker = kCreationMarkerBase + static_cast<size_t>(slot) * kCreationMarkerStride;
    size_t thumbnail_offset = 0;
    if (!ThumbnailRange(main_chunk, slot, &thumbnail_offset)) return std::nullopt;
    const bool entry_ok = std::equal(kCreationEntryBytes.begin(), kCreationEntryBytes.end(),
                                     plain.begin() + static_cast<std::ptrdiff_t>(entry));
    const bool thumbnail_ok = !std::all_of(main_file.begin() + static_cast<std::ptrdiff_t>(thumbnail_offset),
                                           main_file.begin() + static_cast<std::ptrdiff_t>(thumbnail_offset + kThumbnailSize),
                                           [](uint8_t value) { return value == 0; });
    if (entry_ok && plain[marker] == 0x9C && thumbnail_ok) continue;
    // Only fill what is missing: never overwrite a non-zero entry/marker the game wrote.
    if (!entry_ok && IsAllZero(plain, entry, kCreationEntryBytes.size())) {
      std::copy(kCreationEntryBytes.begin(), kCreationEntryBytes.end(),
                plain.begin() + static_cast<std::ptrdiff_t>(entry));
    }
    if (plain[marker] == 0) plain[marker] = 0x9C;
    fixed.push_back(slot);
    if (!thumbnail_ok) thumbnail_offsets.push_back(thumbnail_offset);
  }
  if (fixed.empty()) return main_file;
  if (plain[kFirstCreationFlag] == 0) {
    // First-creation globals are unset: set them like the game (same values as a normal add).
    ApplyCawAddBookkeeping(&plain, fixed.back());
  }
  auto repacked = RepackMain(main_file, main_chunk, plain);
  if (!repacked) return std::nullopt;
  for (const size_t offset : thumbnail_offsets) {
    std::copy(thumbnail.begin(), thumbnail.end(), repacked->begin() + static_cast<std::ptrdiff_t>(offset));
  }
  if (repaired) *repaired = static_cast<uint32_t>(fixed.size());
  return repacked;
}

std::optional<std::vector<CreationName>> ListKnownCreationNames(const std::vector<uint8_t>& main_file,
                                                                bool superstar) {
  size_t chunk = 0;
  std::vector<uint8_t> plain;
  if (!UnpackFile(main_file, &chunk, &plain) || chunk != kMainContainerOffset ||
      !IsSupportedCreationPlainImpl(plain)) {
    return std::nullopt;
  }
  std::vector<CreationName> result;
  for (uint32_t slot = 0; slot < RecordCount(superstar); ++slot) {
    if (!IsRecordEmpty(plain, superstar, slot)) result.push_back({slot, ReadCreationName(plain, superstar, slot)});
  }
  return result;
}

std::optional<std::vector<uint8_t>> RemoveKnownCreation(const std::vector<uint8_t>& main_file,
                                                        bool superstar, uint32_t slot) {
  size_t main_chunk = 0;
  std::vector<uint8_t> plain;
  if (!UnpackFile(main_file, &main_chunk, &plain) || main_chunk != kMainContainerOffset ||
      !IsSupportedCreationPlainImpl(plain) || slot >= RecordCount(superstar) ||
      IsRecordEmpty(plain, superstar, slot) || !IsKnownRecord(plain, superstar, slot)) {
    return std::nullopt;
  }
  const size_t offset = RecordOffset(superstar, slot);
  std::fill(plain.begin() + static_cast<std::ptrdiff_t>(offset),
            plain.begin() + static_cast<std::ptrdiff_t>(offset + RecordSize(superstar)), 0);
  if (!superstar) return RepackMain(main_file, main_chunk, plain);
  ApplyCawRemoveBookkeeping(&plain, slot);
  auto repacked = RepackMain(main_file, main_chunk, plain);
  size_t thumbnail_offset = 0;
  if (repacked && ThumbnailRange(main_chunk, slot, &thumbnail_offset)) {
    const std::vector<uint8_t> thumbnail = MakeObservedThumbnail();
    const auto thumbnail_begin = repacked->begin() + static_cast<std::ptrdiff_t>(thumbnail_offset);
    if (std::equal(thumbnail.begin(), thumbnail.end(), thumbnail_begin)) {
      std::fill(thumbnail_begin, thumbnail_begin + static_cast<std::ptrdiff_t>(kThumbnailSize), 0);
    }
  }
  return repacked;
}

}  // namespace internal
}  // namespace wwe2k14::launcher

namespace wwe2k14::launcher {

Result RenameCreation(const Paths& paths, CreationSaveKind kind, uint32_t slot_index,
                      std::string_view new_name) {
  try {
    const bool superstar = kind == CreationSaveKind::kSuperstar;
    if (!internal::IsValidCreationDisplayName(new_name, superstar)) {
      return Result::Fail(superstar ? "Use 1 to 42 printable ASCII characters for a CAW name."
                                    : "Use 1 to 22 printable ASCII characters for an arena name.");
    }
    if (GameProcessRunning(paths)) return Result::Fail("Close WWE 2K14 before renaming a creation.");
    if (!RecoverCreationTransactions(paths)) {
      return Result::Fail("An interrupted creation update could not be recovered; restore the latest save backup.");
    }
    std::vector<uint8_t> main_file, main_header;
    fs::path main_path, main_header_path;
    if (!ReadSupportedCreationState(paths, &main_file, &main_header, &main_path, &main_header_path)) {
      return Result::Fail("Unsupported save layout. No changes were made.");
    }
    const auto known_slots = internal::ListKnownCreationSlots(main_file, superstar);
    if (!known_slots || std::find(known_slots->begin(), known_slots->end(), slot_index) == known_slots->end()) {
      return Result::Fail("That creation slot is empty or outside the supported save region.");
    }
    const std::string package_name = CreationPackageName(superstar, slot_index);
    const fs::path payload_path = ContentRoot(paths) / package_name / "SaveData.Dat";
    const fs::path creation_header_path = HeadersRoot(paths) / (package_name + ".header");
    if (!IsRegularUnderUserdataNoSymlink(paths.userdata_dir, payload_path) ||
        !IsRegularUnderUserdataNoSymlink(paths.userdata_dir, creation_header_path)) {
      return Result::Fail("The selected creation payload or package header is missing or unsafe. No changes were made.");
    }
    std::vector<uint8_t> payload_file;
    if (!internal::ReadBinaryFile(payload_path, payload_file)) {
      return Result::Fail("The selected creation payload could not be read. No changes were made.");
    }
    auto rewritten = internal::RewriteKnownCreationName(main_file, payload_file, superstar, slot_index, new_name);
    if (!rewritten) {
      return Result::Fail("The selected creation data or checksum was not recognized. No changes were made.");
    }
    if (rewritten->main_file == main_file && rewritten->payload_file == payload_file) return Result::Ok();

    BackupInfo backup;
    const Result backup_result = BackupSaves(paths, false, &backup);
    if (!backup_result.ok) return backup_result;
    if (!VerifyBackupArchive(backup.file)) {
      return Result::Fail("The pre-rename save backup could not be reopened and CRC-verified; no files were changed.");
    }

    std::vector<FileChange> changes;
    changes.emplace_back(main_path, std::move(rewritten->main_file), true, false, true,
                         std::move(rewritten->main_plain));
    changes.emplace_back(payload_path, std::move(rewritten->payload_file), true);
    if (!CommitFileChanges(paths, std::move(changes))) {
      return Result::Fail("The creation could not be renamed. The verified save backup is available.");
    }
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The creation could not be renamed safely. No changes were made.");
  }
}

}  // namespace wwe2k14::launcher

// ====================================================================================================== stories (.sas)
namespace wwe2k14::launcher {
namespace {

constexpr uint32_t kStoryPlainSize = 0xCE4134;       // 13,517,108 (S1: 4 x 2,703,421 + 2,703,424)
constexpr uint32_t kStoryChunkPlainMax = 16u * 1024 * 1024;
constexpr size_t kStoryNameOffset = 4;                // ASCII, NUL-terminated (two samples: "CUSTOM STORY", "6")
constexpr size_t kStoryNameEnd = 0x46;                // bytes 0x46..0x47 are FF FF in both samples
constexpr size_t kStoryDdsFirst = 0xB3D3B8;
constexpr size_t kStoryDdsStride = 0x2A4D0;
constexpr size_t kStoryDdsCount = 10;
constexpr uint32_t kCawFilePlain = 1'882'028;
constexpr uint32_t kArenaFilePlain = 626'764;

bool IsStoryFolderName(const fs::path& folder_path) {
  const std::string folder = internal::LowerAscii(internal::PathToUtf8(folder_path.filename()));
  const std::string required_name = "customstory.sas";
  return folder.size() == required_name.size() + 2 &&
         std::isdigit(static_cast<unsigned char>(folder[0])) &&
         std::isdigit(static_cast<unsigned char>(folder[1])) && folder.substr(2) == required_name;
}

std::string StoryPackageName(uint32_t slot) { return TwoDigitSlot(slot) + "CustomStory.sas"; }

uint32_t StorySlotFromFolder(const fs::path& folder) {
  const std::string name = internal::PathToUtf8(folder.filename());
  return static_cast<uint32_t>(name[0] - '0') * 10 + static_cast<uint32_t>(name[1] - '0');
}

// Returns "" when the bytes are a structurally valid story container; otherwise a player-readable reason.
std::string CheckStoryBytes(const std::vector<uint8_t>& file, std::string* name_out) {
  constexpr const char* kNotStory =
      "This is not a WWE 2K14 story file. Choose a .sas story exported from the game or from this launcher.";
  if (file.size() < 16 + 4) return kNotStory;
  if (std::memcmp(file.data(), "ZLIB", 4) != 0) return kNotStory;
  std::vector<uint8_t> plain;
  size_t position = 0;
  size_t chunks = 0;
  while (position < file.size()) {
    if (file.size() - position < 16 || std::memcmp(file.data() + position, "ZLIB", 4) != 0) {
      return "The story file is damaged (its data blocks do not line up).";
    }
    auto le32 = [&](size_t offset) {
      return static_cast<uint32_t>(file[offset]) | (static_cast<uint32_t>(file[offset + 1]) << 8) |
             (static_cast<uint32_t>(file[offset + 2]) << 16) | (static_cast<uint32_t>(file[offset + 3]) << 24);
    };
    const uint32_t version = le32(position + 4);
    const uint32_t compressed = le32(position + 8);
    const uint32_t expanded = le32(position + 12);
    if (version != 0x3012) return "The story file is from a different game version (unknown container version).";
    if (compressed < 6 || compressed > file.size() - position - 16 || expanded == 0 ||
        expanded > kStoryChunkPlainMax || plain.size() + expanded > kStoryPlainSize + kStoryChunkPlainMax) {
      return "The story file is damaged (a data block has an impossible size).";
    }
    const uint8_t* stream = file.data() + position + 16;
    const size_t old_size = plain.size();
    plain.resize(old_size + expanded);
    mz_ulong produced = expanded;
    if (mz_uncompress(plain.data() + old_size, &produced, stream, compressed) != MZ_OK || produced != expanded) {
      return "The story file is damaged (a data block could not be unpacked).";
    }
    const uint32_t stored_adler = (static_cast<uint32_t>(stream[compressed - 4]) << 24) |
                                  (static_cast<uint32_t>(stream[compressed - 3]) << 16) |
                                  (static_cast<uint32_t>(stream[compressed - 2]) << 8) |
                                  static_cast<uint32_t>(stream[compressed - 1]);
    if (mz_adler32(MZ_ADLER32_INIT, plain.data() + old_size, expanded) != stored_adler) {
      return "The story file is damaged (a data block failed its checksum).";
    }
    position += 16 + compressed;
    ++chunks;
  }
  if (plain.size() != kStoryPlainSize) {
    if (chunks == 1 && plain.size() == kCawFilePlain) {
      return "This is a superstar (CAW) file, not a story. Use the Superstars tab.";
    }
    if (chunks == 1 && plain.size() == kArenaFilePlain) {
      return "This is an arena file, not a story. Use the Arenas tab.";
    }
    return "This is not a WWE 2K14 story file (its size does not match a story).";
  }
  if (plain[0] != 0x0F || plain[1] != 0 || plain[2] != 0 || plain[3] != 0x01) {
    return "This is not a WWE 2K14 story file (its header does not match).";
  }
  for (size_t i = 0; i < kStoryDdsCount; ++i) {
    const size_t offset = kStoryDdsFirst + i * kStoryDdsStride;
    if (offset + 0x58 > plain.size() || std::memcmp(plain.data() + offset, "DDS ", 4) != 0 ||
        std::memcmp(plain.data() + offset + 0x54, "DXT5", 4) != 0) {
      return "This is not a WWE 2K14 story file (its picture blocks are missing).";
    }
  }
  std::string name;
  size_t i = kStoryNameOffset;
  for (; i < kStoryNameEnd && plain[i] != 0; ++i) {
    if (plain[i] < 0x20 || plain[i] > 0x7E) return "The story file is damaged (its name is unreadable).";
    name.push_back(static_cast<char>(plain[i]));
  }
  if (i >= kStoryNameEnd) return "The story file is damaged (its name is unreadable).";
  if (name_out) *name_out = name;
  return {};
}

std::string StoryDisplayName(uint32_t slot, const std::string& name) {
  return TwoDigitSlot(slot + 1) + "." + (name.empty() ? std::string("CUSTOM STORY") : name);
}

// Reads the single root SaveData.Dat of a CON/LIVE/PIRS saved-game package for WWE 2K14.
bool ReadStoryPackage(const fs::path& package, std::vector<uint8_t>* payload, std::string* error) {
  internal::StfsReader stfs;
  if (!stfs.Open(package, error)) {
    if (error && error->empty()) *error = "The story package could not be read.";
    return false;
  }
  if (stfs.metadata().content_type != kSaveContentType || stfs.metadata().title_id != kTitleIdValue) {
    if (error) *error = "This package is not a WWE 2K14 saved game.";
    return false;
  }
  const internal::StfsEntry* found = nullptr;
  for (const internal::StfsEntry& entry : stfs.entries()) {
    if (entry.directory) continue;
    if (found || !entry.relative_path.parent_path().empty() ||
        internal::LowerAscii(internal::PathToUtf8(entry.relative_path.filename())) != "savedata.dat") {
      if (error) *error = "The story package must contain exactly one SaveData.Dat file.";
      return false;
    }
    found = &entry;
  }
  if (!found) {
    if (error) *error = "The story package does not contain SaveData.Dat.";
    return false;
  }
  std::ostringstream output(std::ios::out | std::ios::binary);
  if (!stfs.CopyFile(*found, output, {}, error)) return false;
  const std::string bytes = output.str();
  payload->assign(bytes.begin(), bytes.end());
  return true;
}

bool FindSingleStorySource(const fs::path& root, fs::path* found) {
  std::error_code error;
  fs::recursive_directory_iterator iterator(root, fs::directory_options::none, error), end;
  if (error) return false;
  size_t matches = 0;
  for (; iterator != end; iterator.increment(error)) {
    if (error) return false;
    if (iterator.depth() > 4) {
      iterator.disable_recursion_pending();
      continue;
    }
    const fs::file_status status = iterator->symlink_status(error);
    if (error || fs::is_symlink(status)) return false;
    bool hit = false;
    if (fs::is_directory(status) && IsStoryFolderName(iterator->path()) &&
        IsRegularNonSymlink(iterator->path() / "SaveData.Dat")) {
      hit = true;
      iterator.disable_recursion_pending();
    } else if (fs::is_regular_file(status) &&
               internal::LowerAscii(internal::PathToUtf8(iterator->path().extension())) == ".sas") {
      hit = true;
    }
    if (hit) {
      if (++matches > 1) return false;
      *found = iterator->path();
    }
  }
  return matches == 1;
}

// Turns whatever the player picked into the story file's bytes.
bool LoadStorySource(const fs::path& input, std::vector<uint8_t>* bytes, std::string* error, int depth = 0) {
  constexpr const char* kUnsupported =
      "Choose a .sas story file (or a .zip / unpacked folder containing one).";
  std::error_code ec;
  const fs::file_status status = fs::symlink_status(input, ec);
  if (ec || fs::is_symlink(status) || depth > 2) {
    *error = kUnsupported;
    return false;
  }
  if (fs::is_directory(status)) {
    fs::path folder = input;
    if (!IsStoryFolderName(folder)) {
      if (!FindSingleStorySource(input, &folder)) {
        *error = "The folder must contain exactly one story (an NNCustomStory.sas folder or a .sas file).";
        return false;
      }
    }
    if (fs::is_directory(folder, ec)) folder /= "SaveData.Dat";
    return LoadStorySource(folder, bytes, error, depth + 1);
  }
  if (!fs::is_regular_file(status)) {
    *error = kUnsupported;
    return false;
  }
  const std::string extension = internal::LowerAscii(internal::PathToUtf8(input.extension()));
  const std::string file_name = internal::LowerAscii(internal::PathToUtf8(input.filename()));
  if (extension == ".rar") {
    *error = "RAR archives are not supported. Extract the archive, then choose its .sas file.";
    return false;
  }
  if (extension == ".zip") {
    ScopedTempDirectory extracted;
    const uint64_t archive_size = fs::file_size(input, ec);
    if (!extracted.Create() || ec || archive_size > kMaxCreationZipTotalBytes ||
        !ExtractCreationZip(input, extracted.path())) {
      *error = "The ZIP does not contain a safe, readable story file.";
      return false;
    }
    fs::path inner;
    if (!FindSingleStorySource(extracted.path(), &inner)) {
      *error = "The ZIP must contain exactly one story (a .sas file or an NNCustomStory.sas folder).";
      return false;
    }
    if (fs::is_directory(inner, ec)) inner /= "SaveData.Dat";
    return LoadStorySource(inner, bytes, error, depth + 1);
  }
  std::vector<uint8_t> head;
  if (!internal::ReadBinaryFile(input, head) || head.size() < 4) {
    *error = "The selected file could not be read.";
    return false;
  }
  if (std::memcmp(head.data(), "ZLIB", 4) == 0 && (extension == ".sas" || file_name == "savedata.dat")) {
    *bytes = std::move(head);  // raw story data
    return true;
  }
  if (std::memcmp(head.data(), "CON ", 4) == 0 || std::memcmp(head.data(), "LIVE", 4) == 0 ||
      std::memcmp(head.data(), "PIRS", 4) == 0) {
    if (extension != ".sas" && extension != ".cas" && extension != ".car") {
      *error = kUnsupported;
      return false;
    }
    if (!ReadStoryPackage(input, bytes, error)) return false;
    return true;
  }
  *error = kUnsupported;
  return false;
}

}  // namespace

void SetCommitFaultInjectionForTest(int fault_after_renames, int stage) {
  g_commit_fault_after_renames = fault_after_renames;
  g_commit_fault_stage = stage;
}

std::string CheckStoryFileBytes(const std::vector<uint8_t>& file, std::string* name) {
  try {
    return CheckStoryBytes(file, name);
  } catch (...) {
    return "The story file could not be checked.";
  }
}

std::vector<uint8_t> BuildStoryHeaderBytes(uint32_t slot_index, const std::string& name) {
  std::vector<uint8_t> header(kCreationHeaderSize, 0);
  WriteBe32(&header, 0, 1);  // device id (same in every creation sidecar header)
  WriteBe32(&header, kHeaderContentTypeOffset, kSaveContentType);
  const std::string display = StoryDisplayName(slot_index, name);
  for (size_t i = 0; i < display.size() && (i + 1) * 2 < kHeaderDisplayNameSize; ++i) {
    header[kHeaderDisplayNameOffset + 2 * i + 1] = static_cast<uint8_t>(display[i]);
  }
  const std::string package = StoryPackageName(slot_index);
  std::copy(package.begin(), package.end(), header.begin() + static_cast<std::ptrdiff_t>(kHeaderPackageNameOffset));
  WriteBe32(&header, kHeaderTitleIdOffset, kTitleIdValue);
  // 0x144..0x147: 0 in the game-written header of an owner-made story (slot 0, sample1); unknown meaning.
  return header;
}

std::optional<std::vector<StoryInfo>> ListStories(const Paths& paths) {
  try {
    std::vector<StoryInfo> stories;
    std::error_code error;
    const fs::file_status root_status = fs::symlink_status(ContentRoot(paths), error);
    if (error == std::errc::no_such_file_or_directory) return stories;
    if (error || fs::is_symlink(root_status) || !fs::is_directory(root_status) ||
        !EnsureDirectoryTreeNoSymlink(paths.userdata_dir, ContentRoot(paths), false)) {
      return std::nullopt;
    }
    for (fs::directory_iterator iterator(ContentRoot(paths), fs::directory_options::none, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (!IsStoryFolderName(iterator->path())) continue;
      const fs::file_status status = iterator->symlink_status(error);
      if (error || fs::is_symlink(status) || !fs::is_directory(status)) continue;
      StoryInfo info;
      info.slot_index = StorySlotFromFolder(iterator->path());
      std::vector<uint8_t> bytes;
      const fs::path payload = iterator->path() / "SaveData.Dat";
      if (!IsRegularNonSymlink(payload) || !internal::ReadBinaryFile(payload, bytes) ||
          !CheckStoryBytes(bytes, &info.name).empty()) {
        info.damaged = true;
        info.name.clear();
      }
      stories.push_back(std::move(info));
    }
    if (error) return std::nullopt;
    std::sort(stories.begin(), stories.end(),
              [](const StoryInfo& a, const StoryInfo& b) { return a.slot_index < b.slot_index; });
    return stories;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<uint32_t> FindFreeStorySlot(const Paths& paths) {
  try {
    if (!EnsureDirectoryTreeNoSymlink(paths.userdata_dir, ContentRoot(paths), false) ||
        !EnsureDirectoryTreeNoSymlink(paths.userdata_dir, HeadersRoot(paths), false)) return std::nullopt;
    for (uint32_t slot = 0; slot <= 99; ++slot) {
      const std::string package_name = StoryPackageName(slot);
      std::error_code error;
      const fs::file_status folder_status = fs::symlink_status(ContentRoot(paths) / package_name, error);
      if (!error && fs::exists(folder_status)) continue;
      if (error && error != std::errc::no_such_file_or_directory) return std::nullopt;
      bool header_exists = false;
      if (!GetSafeFileState(paths.userdata_dir, HeadersRoot(paths) / (package_name + ".header"), false,
                            &header_exists)) {
        return std::nullopt;
      }
      if (!header_exists) return slot;
    }
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

Result ExportStory(const Paths& paths, uint32_t slot_index, const fs::path& output_file,
                   StoryChangeSummary* summary) {
  try {
    if (slot_index > 99) return Result::Fail("That story slot does not exist.");
    const fs::path payload_path = ContentRoot(paths) / StoryPackageName(slot_index) / "SaveData.Dat";
    std::vector<uint8_t> bytes;
    if (!IsRegularUnderUserdataNoSymlink(paths.userdata_dir, payload_path) ||
        !internal::ReadBinaryFile(payload_path, bytes)) {
      return Result::Fail("The story could not be read from the save.");
    }
    std::string name;
    const std::string problem = CheckStoryBytes(bytes, &name);
    if (!problem.empty()) return Result::Fail("This story is not exported because it looks damaged: " + problem);
    std::string error;
    if (!internal::WriteStfsSaveGamePackage(output_file, StoryDisplayName(slot_index, name), kTitleIdValue,
                                            "SaveData.Dat", bytes, &error)) {
      return Result::Fail(error.empty() ? "The story file could not be written." : error);
    }
    std::vector<uint8_t> read_back;
    std::string read_error;
    if (!ReadStoryPackage(output_file, &read_back, &read_error) || read_back != bytes) {
      std::error_code ec;
      fs::remove(output_file, ec);
      return Result::Fail("The exported story failed its read-back check, so it was not kept.");
    }
    if (summary) {
      summary->name = name;
      summary->slot_index = slot_index;
    }
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The story could not be exported.");
  }
}

Result ImportStory(const Paths& paths, const fs::path& source, uint32_t* selected_slot,
                   StoryChangeSummary* summary) {
  try {
    if (!RecoverCreationTransactions(paths)) {
      return Result::Fail("An interrupted creation update could not be recovered; restore the latest save backup.");
    }
    std::vector<uint8_t> bytes;
    std::string error;
    if (!LoadStorySource(source, &bytes, &error)) return Result::Fail(error);
    std::string name;
    const std::string problem = CheckStoryBytes(bytes, &name);
    if (!problem.empty()) return Result::Fail(problem);
    // The game must already have a save here (it creates the main save and the content folder itself).
    if (!IsRegularUnderUserdataNoSymlink(paths.userdata_dir, MainSavePath(paths)) ||
        !EnsureDirectoryTreeNoSymlink(paths.userdata_dir, ContentRoot(paths), false) ||
        !EnsureDirectoryTreeNoSymlink(paths.userdata_dir, HeadersRoot(paths), false)) {
      return Result::Fail("No WWE 2K14 save was found yet. Start the game once and save, then import the story.");
    }
    const auto slot = FindFreeStorySlot(paths);
    if (!slot) return Result::Fail("There is no free story slot (00-99 are all used); no changes were made.");

    const std::string target_name = StoryPackageName(*slot);
    const fs::path target_folder = ContentRoot(paths) / target_name;
    const fs::path target_payload = target_folder / "SaveData.Dat";
    const fs::path target_header_path = HeadersRoot(paths) / (target_name + ".header");
    std::error_code ec;
    const fs::file_status folder_status = fs::symlink_status(target_folder, ec);
    if (ec != std::errc::no_such_file_or_directory || fs::exists(folder_status)) {
      return Result::Fail("The selected story slot already has a folder; no changes were made.");
    }
    BackupInfo backup_info;
    const Result backup = BackupSaves(paths, false, &backup_info);
    if (!backup.ok) return backup;
    if (!VerifyBackupArchive(backup_info.file)) {
      return Result::Fail("The pre-import save backup could not be reopened and CRC-verified; no files were changed.");
    }
    ec.clear();
    if (!fs::create_directory(target_folder, ec) || ec) {
      return Result::Fail("The story folder could not be prepared safely; no save files were changed.");
    }
    std::vector<FileChange> changes;
    changes.push_back({target_payload, bytes, false, true});
    changes.push_back({target_header_path, BuildStoryHeaderBytes(*slot, name), false, true});
    if (!CommitFileChanges(paths, std::move(changes))) {
      ec.clear();
      fs::remove(target_folder, ec);  // only succeeds when rollback left it empty
      return Result::Fail("The story could not be saved. The pre-import backup is available.");
    }
    // Committed: nothing below may turn this into a failure report.
    try {
      if (selected_slot) *selected_slot = *slot;
      if (summary) {
        summary->name = name;
        summary->slot_index = *slot;
      }
    } catch (...) {
    }
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The story could not be imported safely; no changes were made.");
  }
}

}  // namespace wwe2k14::launcher
