#include "launcher_core.h"

#include <cctype>

#include "internal/stfs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <system_error>
#include <utility>

namespace wwe2k14::launcher {
namespace {

constexpr uint32_t kWwe2k14TitleId = 0x545408B2;
constexpr uint32_t kDlcContentType = 0x00000002;
constexpr uint32_t kTitleUpdateContentType = 0x000B0000;
constexpr uint64_t kCopyChunkSize = 64 * 1024;
constexpr size_t kContentHeaderSize = 0x14C;

struct SourceFile {
  fs::path source;
  fs::path relative_path;
  uint64_t size = 0;
  const internal::StfsEntry* stfs_entry = nullptr;
};

struct PackageSource {
  PackageInfo info;
  std::string package_name;
  std::string display_name;
  fs::path folder_header;
  uint64_t total_bytes = 0;
  bool is_folder = false;
  std::vector<SourceFile> files;
  std::vector<fs::path> directories;
  std::unique_ptr<internal::StfsReader> stfs;
};

std::string PathToUtf8(const fs::path& path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

fs::path PathFromUtf8(const std::string& text) {
#if __cplusplus >= 202002L
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else
  return fs::u8path(text.begin(), text.end());
#endif
}

void WriteBigEndianU32(uint8_t* bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value >> 24);
  bytes[1] = static_cast<uint8_t>(value >> 16);
  bytes[2] = static_cast<uint8_t>(value >> 8);
  bytes[3] = static_cast<uint8_t>(value);
}

void WriteBigEndianU64(uint8_t* bytes, uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    bytes[index] = static_cast<uint8_t>(value >> (56 - index * 8));
  }
}

bool IsValidComponent(const std::string& name) {
  if (name.empty() || name == "." || name == "..") {
    return false;
  }
  for (unsigned char character : name) {
    if (character == 0 || character == '/' || character == '\\' || character == ':' ||
        character < 0x20) {
      return false;
    }
  }
  return true;
}

bool IsSafeRelativePath(const fs::path& path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  const fs::path normalized = path.lexically_normal();
  if (normalized.empty() || normalized.is_absolute() || *normalized.begin() == fs::path("..")) {
    return false;
  }
  for (const auto& component : normalized) {
    const std::string text = PathToUtf8(component);
    if (!IsValidComponent(text)) {
      return false;
    }
  }
  return true;
}

bool MakeSafeOutputPath(const fs::path& base, const fs::path& relative, fs::path* output) {
  if (!IsSafeRelativePath(relative)) {
    return false;
  }
  const fs::path normalized_base = base.lexically_normal();
  const fs::path candidate = (normalized_base / relative).lexically_normal();
  const fs::path within = candidate.lexically_relative(normalized_base);
  if (within.empty() || within.is_absolute() || *within.begin() == fs::path("..")) {
    return false;
  }
  *output = candidate;
  return true;
}

bool ParseHexTitleId(const fs::path& path, uint32_t* title_id) {
  const std::string name = path.filename().string();
  if (name.size() != 8) {
    return false;
  }
  uint32_t value = 0;
  for (unsigned char character : name) {
    uint32_t digit = 0;
    if (character >= '0' && character <= '9') {
      digit = character - '0';
    } else if (character >= 'A' && character <= 'F') {
      digit = character - 'A' + 10;
    } else if (character >= 'a' && character <= 'f') {
      digit = character - 'a' + 10;
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  *title_id = value;
  return true;
}

fs::path XeniaContentHeaderPath(const fs::path& content_folder) {
  const fs::path content_type = content_folder.parent_path().filename();
  if (content_type != fs::path("00000002")) {
    return {};
  }
  uint32_t title_id = 0;
  if (!ParseHexTitleId(content_folder.parent_path().parent_path(), &title_id)) {
    return {};
  }
  return content_folder.parent_path().parent_path() / "Headers" / content_type /
         PathFromUtf8(PathToUtf8(content_folder.filename()) + ".header");
}

PackageKind KindFromContentType(uint32_t content_type) {
  if (content_type == kTitleUpdateContentType) {
    return PackageKind::kTitleUpdate;
  }
  if (content_type == kDlcContentType) {
    return PackageKind::kDlc;
  }
  return PackageKind::kUnknown;
}

bool ReadFolderPackageInfo(const fs::path& path, PackageInfo* info, std::string* display_name) {
  std::error_code filesystem_error;
  if (!fs::is_directory(path, filesystem_error) || filesystem_error) {
    return false;
  }
  const std::string content_type_name = path.parent_path().filename().string();
  uint32_t content_type = 0;
  if (content_type_name == "00000002") {
    content_type = kDlcContentType;
  } else if (content_type_name == "000B0000") {
    content_type = kTitleUpdateContentType;
  }
  uint32_t title_id = 0;
  bool found_title_id = false;
  if (content_type) {
    found_title_id = ParseHexTitleId(path.parent_path().parent_path(), &title_id);
  } else {
    fs::path ancestor = path.parent_path();
    for (unsigned depth = 0; depth < 6 && !ancestor.empty(); ++depth, ancestor = ancestor.parent_path()) {
      const std::string component = ancestor.filename().string();
      if (component == "00000002" || component == "000B0000") {
        continue;
      }
      if (ParseHexTitleId(ancestor, &title_id)) {
        found_title_id = true;
        break;
      }
    }
  }
  if (!content_type) {
    const fs::path update = path / "default.xexp";
    if (fs::is_regular_file(update, filesystem_error) && !filesystem_error) {
      content_type = kTitleUpdateContentType;
      if (!found_title_id) {
        title_id = kWwe2k14TitleId;
        found_title_id = true;
      }
    }
  }
  if (!content_type || !found_title_id) {
    return false;
  }
  info->kind = KindFromContentType(content_type);
  info->title_id = title_id;
  *display_name = PathToUtf8(path.filename());
  if (display_name->empty()) {
    return false;
  }
  return true;
}

bool LoadPackageSource(const fs::path& path, PackageSource* source, std::string* error) {
  std::error_code filesystem_error;
  if (fs::is_directory(path, filesystem_error) && !filesystem_error) {
    source->is_folder = true;
    if (!ReadFolderPackageInfo(path, &source->info, &source->display_name)) {
      if (error) {
        *error = "Choose an Xbox 360 content package or an extracted game content folder.";
      }
      return false;
    }
    source->package_name = PathToUtf8(path.filename());
    source->info.display_name = source->display_name;
    const fs::path candidate_header = XeniaContentHeaderPath(path);
    if (!candidate_header.empty()) {
      filesystem_error.clear();
      const bool header_exists = fs::exists(candidate_header, filesystem_error);
      if (filesystem_error) {
        if (error) *error = "The content information could not be read.";
        return false;
      }
      if (header_exists) {
        filesystem_error.clear();
        if (!fs::is_regular_file(candidate_header, filesystem_error) || filesystem_error) {
          if (error) *error = "The content information could not be read.";
          return false;
        }
        source->folder_header = candidate_header;
      }
    }
    return true;
  }

  source->stfs = std::make_unique<internal::StfsReader>();
  if (!source->stfs->Open(path, error)) {
    return false;
  }
  const auto& metadata = source->stfs->metadata();
  source->info.kind = KindFromContentType(metadata.content_type);
  source->info.title_id = metadata.title_id;
  source->display_name = metadata.display_name;
  source->info.display_name = metadata.display_name;
  source->package_name = PathToUtf8(path.filename());
  if (!IsValidComponent(source->package_name)) {
    if (error) {
      *error = "This package has an invalid name.";
    }
    return false;
  }
  for (const auto& entry : source->stfs->entries()) {
    if (entry.directory) {
      source->directories.push_back(entry.relative_path);
      continue;
    }
    if (entry.size > std::numeric_limits<uint64_t>::max() - source->total_bytes) {
      if (error) {
        *error = "This package is too large to install.";
      }
      return false;
    }
    source->total_bytes += entry.size;
    source->files.push_back({{}, entry.relative_path, entry.size, &entry});
  }
  if (source->files.empty()) {
    if (error) {
      *error = "This package does not contain any files.";
    }
    return false;
  }
  return true;
}

bool GatherFolderFiles(const fs::path& root, PackageSource* source, std::string* error) {
  std::error_code filesystem_error;
  fs::recursive_directory_iterator iterator(root, fs::directory_options::none, filesystem_error);
  const fs::recursive_directory_iterator end;
  if (filesystem_error) {
    if (error) {
      *error = "The selected content folder could not be read.";
    }
    return false;
  }
  for (; iterator != end; iterator.increment(filesystem_error)) {
    if (filesystem_error) {
      if (error) {
        *error = "The selected content folder could not be read.";
      }
      return false;
    }
    const fs::path item_path = iterator->path();
    const fs::file_status status = iterator->symlink_status(filesystem_error);
    if (filesystem_error || fs::is_symlink(status)) {
      if (error) {
        *error = "The selected content folder contains an unsupported link.";
      }
      return false;
    }
    const fs::path relative = item_path.lexically_relative(root);
    if (!IsSafeRelativePath(relative)) {
      if (error) {
        *error = "The selected content folder contains an invalid file path.";
      }
      return false;
    }
    if (fs::is_directory(status)) {
      source->directories.push_back(relative);
    } else if (fs::is_regular_file(status)) {
      const uint64_t size = fs::file_size(item_path, filesystem_error);
      if (filesystem_error || size > std::numeric_limits<uint64_t>::max() - source->total_bytes) {
        if (error) {
          *error = "The selected content folder could not be read.";
        }
        return false;
      }
      source->total_bytes += size;
      source->files.push_back({item_path, relative, size, nullptr});
    } else {
      if (error) {
        *error = "The selected content folder contains an unsupported file.";
      }
      return false;
    }
  }
  if (source->files.empty()) {
    if (error) {
      *error = "The selected content folder does not contain any files.";
    }
    return false;
  }
  return true;
}

bool ContainsDefaultXexp(const PackageSource& source) {
  for (const auto& file : source.files) {
    if (file.relative_path.parent_path().empty() &&
        file.relative_path.filename().string() == "default.xexp") {
      return true;
    }
  }
  return false;
}

bool Utf8ToUtf16Be(const std::string& input, std::vector<uint8_t>* output) {
  output->clear();
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
    if (offset + count > input.size()) {
      return false;
    }
    for (size_t index = 1; index < count; ++index) {
      const uint8_t continuation = static_cast<uint8_t>(input[offset + index]);
      if ((continuation & 0xC0) != 0x80) {
        return false;
      }
      codepoint = (codepoint << 6) | (continuation & 0x3F);
    }
    if ((count == 2 && codepoint < 0x80) || (count == 3 && codepoint < 0x800) ||
        (count == 4 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return false;
    }
    auto append_unit = [&](uint16_t unit) {
      output->push_back(static_cast<uint8_t>(unit >> 8));
      output->push_back(static_cast<uint8_t>(unit));
    };
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

bool MakeContentHeader(const PackageSource& source, std::vector<uint8_t>* header,
                       std::string* error) {
  if (source.package_name.size() > 42) {
    if (error) {
      *error = "This content has a name that is too long to install.";
    }
    return false;
  }
  std::vector<uint8_t> display_name;
  if (!Utf8ToUtf16Be(source.display_name, &display_name) || display_name.size() > 0xFE) {
    if (error) {
      *error = "This content has a display name that is too long to install.";
    }
    return false;
  }
  header->assign(kContentHeaderSize, 0);
  WriteBigEndianU32(header->data(), 1);       // HDD device ID
  WriteBigEndianU32(header->data() + 4, 2);   // Marketplace content
  std::copy(display_name.begin(), display_name.end(), header->begin() + 8);
  std::copy(source.package_name.begin(), source.package_name.end(), header->begin() + 0x108);
  WriteBigEndianU64(header->data() + 0x138, 0);
  WriteBigEndianU32(header->data() + 0x140, source.info.title_id);
  (*header)[0x148] = 0xFF;
  (*header)[0x149] = 0xFF;
  (*header)[0x14A] = 0xFF;
  (*header)[0x14B] = 0xFF;
  return true;
}

bool CopyFolderFile(const SourceFile& file, std::ostream& output,
                    const std::function<bool(uint64_t)>& after_chunk, std::string* error) {
  std::ifstream input(file.source, std::ios::binary);
  if (!input) {
    if (error) {
      *error = "A file in the selected content folder could not be opened.";
    }
    return false;
  }
  std::array<char, kCopyChunkSize> buffer{};
  uint64_t remaining = file.size;
  while (remaining) {
    const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
    input.read(buffer.data(), static_cast<std::streamsize>(count));
    if (input.gcount() != static_cast<std::streamsize>(count)) {
      if (error) {
        *error = "A file in the selected content folder is incomplete.";
      }
      return false;
    }
    errno = 0;
    output.write(buffer.data(), static_cast<std::streamsize>(count));
    if (!output) {
      if (error) {
        *error = "There is not enough free space to install this package.";
      }
      return false;
    }
    remaining -= count;
    if (after_chunk && !after_chunk(count)) {
      if (error) {
        *error = "The import was cancelled.";
      }
      return false;
    }
  }
  return true;
}

void RemovePath(const fs::path& path) {
  std::error_code ignored;
  fs::remove_all(path, ignored);
}

Result StagePackageFiles(PackageSource* source, const fs::path& stage,
                         const ProgressFn& progress, const CancelFlag& cancel) {
  std::error_code filesystem_error;
  if (!fs::create_directory(stage, filesystem_error) || filesystem_error) {
    return Result::Fail("A temporary installation folder could not be created.");
  }
  for (const auto& directory : source->directories) {
    if (cancel.load()) {
      return Result::Fail("The import was cancelled.");
    }
    fs::path output_path;
    if (!MakeSafeOutputPath(stage, directory, &output_path)) {
      return Result::Fail("This package contains an invalid file path.");
    }
    filesystem_error.clear();
    fs::create_directories(output_path, filesystem_error);
    if (filesystem_error) {
      return Result::Fail("A folder could not be created while installing this package.");
    }
  }
  if (progress) {
    progress(0, source->total_bytes, "Preparing files");
  }
  if (cancel.load()) {
    return Result::Fail("The import was cancelled.");
  }
  uint64_t completed = 0;
  for (const auto& file : source->files) {
    if (cancel.load()) {
      return Result::Fail("The import was cancelled.");
    }
    fs::path output_path;
    if (!MakeSafeOutputPath(stage, file.relative_path, &output_path)) {
      return Result::Fail("This package contains an invalid file path.");
    }
    filesystem_error.clear();
    fs::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
      return Result::Fail("A folder could not be created while installing this package.");
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      return Result::Fail("A package file could not be created.");
    }
    auto after_chunk = [&](uint64_t count) {
      completed += count;
      if (progress) {
        progress(completed, source->total_bytes, file.relative_path.generic_string());
      }
      return !cancel.load();
    };
    std::string error;
    const bool copied = file.stfs_entry
                            ? source->stfs->CopyFile(*file.stfs_entry, output, after_chunk, &error)
                            : CopyFolderFile(file, output, after_chunk, &error);
    output.close();
    if (!copied) {
      return Result::Fail(cancel.load() ? "The import was cancelled."
                                        : (error.empty() ? "A package file could not be read." : error));
    }
    if (!output) {
      return Result::Fail("A package file could not be written.");
    }
  }
  return Result::Ok();
}

bool HasSufficientSpace(const fs::path& parent, uint64_t bytes) {
  std::error_code filesystem_error;
  const auto available = fs::space(parent, filesystem_error);
  return filesystem_error || available.available >= bytes;
}

Result ImportTitleUpdate(PackageSource* source, const fs::path& game_folder,
                         const ProgressFn& progress, const CancelFlag& cancel) {
  if (source->info.title_id != kWwe2k14TitleId) {
    return Result::Fail("This title update is not for WWE 2K14.");
  }
  uint32_t base_title_id = 0;
  if (!internal::DecodeWwe2k14TitleId(game_folder / "default.xex", &base_title_id) ||
      base_title_id != kWwe2k14TitleId) {
    return Result::Fail("Choose the matching WWE 2K14 game folder before installing this update.");
  }
  if (!ContainsDefaultXexp(*source)) {
    return Result::Fail("This title update does not contain default.xexp.");
  }
  std::error_code filesystem_error;
  if (!fs::is_directory(game_folder, filesystem_error) || filesystem_error) {
    return Result::Fail("The game folder could not be opened.");
  }
  const fs::path stage = game_folder / ".wwe2k14-title-update.partial";
  RemovePath(stage);
  if (!HasSufficientSpace(game_folder, source->total_bytes)) {
    return Result::Fail("There is not enough free space to install this update.");
  }
  const Result staged = StagePackageFiles(source, stage, progress, cancel);
  if (!staged.ok) {
    RemovePath(stage);
    return staged;
  }
  for (const auto& file : source->files) {
    if (cancel.load()) {
      RemovePath(stage);
      return Result::Fail("The import was cancelled.");
    }
    fs::path staged_file;
    if (!MakeSafeOutputPath(stage, file.relative_path, &staged_file)) {
      RemovePath(stage);
      return Result::Fail("This package contains an invalid file path.");
    }
    fs::path destination;
    if (!MakeSafeOutputPath(game_folder, file.relative_path, &destination)) {
      RemovePath(stage);
      return Result::Fail("This package contains an invalid file path.");
    }
    filesystem_error.clear();
    fs::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error || !fs::copy_file(staged_file, destination, fs::copy_options::overwrite_existing,
                                           filesystem_error) || filesystem_error) {
      RemovePath(stage);
      return Result::Fail("A title update file could not be installed.");
    }
  }
  RemovePath(stage);
  return Result::Ok();
}

Result ImportDlc(PackageSource* source, const Paths& paths,
                 const ProgressFn& progress, const CancelFlag& cancel) {
  if (source->info.title_id != kWwe2k14TitleId) {
    return Result::Fail("This content is not for WWE 2K14.");
  }
  if (source->package_name.size() > 42) {
    return Result::Fail("This content has a name that is too long to install.");
  }
  std::vector<uint8_t> content_header;
  std::string error;
  if (source->folder_header.empty() && !MakeContentHeader(*source, &content_header, &error)) {
    return Result::Fail(error);
  }
  const fs::path content_root = paths.userdata_dir / "0000000000000000" / "545408B2";
  const fs::path parent = content_root / "00000002";
  const fs::path destination = parent / PathFromUtf8(source->package_name);
  fs::path stage = destination;
  stage += ".partial";
  const fs::path headers_dir = content_root / "Headers" / "00000002";
  const fs::path header_path = headers_dir / PathFromUtf8(source->package_name + ".header");
  fs::path header_stage = header_path;
  header_stage += ".partial";
  std::error_code filesystem_error;
  if (fs::exists(destination, filesystem_error) || fs::exists(header_path, filesystem_error) ||
      filesystem_error) {
    return Result::Fail("This content is already installed.");
  }
  fs::create_directories(parent, filesystem_error);
  if (filesystem_error) {
    return Result::Fail("The content folder could not be created.");
  }
  fs::create_directories(headers_dir, filesystem_error);
  if (filesystem_error) {
    return Result::Fail("The content folder could not be created.");
  }
  if (!HasSufficientSpace(parent, source->total_bytes)) {
    return Result::Fail("There is not enough free space to install this content.");
  }
  const Result staged = StagePackageFiles(source, stage, progress, cancel);
  if (!staged.ok) {
    RemovePath(stage);
    return staged;
  }
  if (!source->folder_header.empty()) {
    fs::copy_file(source->folder_header, header_stage, fs::copy_options::none, filesystem_error);
    if (filesystem_error) {
      RemovePath(stage);
      RemovePath(header_stage);
      return Result::Fail("The content information could not be written.");
    }
  } else {
    std::ofstream output(header_stage, std::ios::binary | std::ios::trunc);
    if (!output) {
      RemovePath(stage);
      return Result::Fail("The content information could not be written.");
    }
    output.write(reinterpret_cast<const char*>(content_header.data()),
                 static_cast<std::streamsize>(content_header.size()));
    output.close();
    if (!output) {
      RemovePath(stage);
      RemovePath(header_stage);
      return Result::Fail("The content information could not be written.");
    }
  }
  if (cancel.load()) {
    RemovePath(stage);
    RemovePath(header_stage);
    return Result::Fail("The import was cancelled.");
  }
  fs::rename(stage, destination, filesystem_error);
  if (filesystem_error) {
    RemovePath(stage);
    RemovePath(header_stage);
    return Result::Fail("The content could not be installed.");
  }
  fs::rename(header_stage, header_path, filesystem_error);
  if (filesystem_error) {
    RemovePath(destination);
    RemovePath(header_stage);
    return Result::Fail("The content information could not be installed.");
  }
  return Result::Ok();
}

}  // namespace

std::optional<PackageInfo> InspectPackage(const fs::path& package_or_folder) {
  try {
    PackageSource source;
    std::string error;
    if (!LoadPackageSource(package_or_folder, &source, &error)) {
      return std::nullopt;
    }
    if (source.is_folder) {
      if (!GatherFolderFiles(package_or_folder, &source, &error)) {
        return std::nullopt;
      }
    }
    return source.info;
  } catch (...) {
    return std::nullopt;
  }
}

FolderInstallables FindInstallableFiles(const fs::path& folder) {
  FolderInstallables found;
  try {
    std::error_code error;
    if (!fs::is_directory(folder, error)) return found;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
      const fs::path path = it->path();
      if (!it->is_regular_file(error)) continue;
      std::string extension = path.extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (extension == ".iso" || extension == ".img") {
        found.disc_images.push_back(path);
        continue;
      }
      if (extension == ".txt" || extension == ".xex" || extension == ".xexp" || extension == ".def" ||
          extension == ".arc") {
        continue;
      }
      const std::optional<PackageInfo> package = InspectPackage(path);
      if (package && package->title_id == 0x545408B2 && package->kind != PackageKind::kUnknown) {
        found.packages.push_back(path);
        found.has_title_update = found.has_title_update || package->kind == PackageKind::kTitleUpdate;
      }
    }
  } catch (...) {
  }
  std::sort(found.disc_images.begin(), found.disc_images.end());
  std::sort(found.packages.begin(), found.packages.end());
  return found;
}

Result ImportPackage(const fs::path& package_or_folder, const Paths& paths,
                     const fs::path& game_folder, const ProgressFn& progress,
                     const CancelFlag& cancel) {
  try {
    if (cancel.load()) {
      return Result::Fail("The import was cancelled.");
    }
    PackageSource source;
    std::string error;
    if (!LoadPackageSource(package_or_folder, &source, &error)) {
      return Result::Fail(error.empty() ? "This package could not be read." : error);
    }
    if (source.is_folder && !GatherFolderFiles(package_or_folder, &source, &error)) {
      return Result::Fail(error.empty() ? "The selected content folder could not be read." : error);
    }
    if (source.info.kind == PackageKind::kTitleUpdate) {
      return ImportTitleUpdate(&source, game_folder, progress, cancel);
    }
    if (source.info.kind == PackageKind::kDlc) {
      return ImportDlc(&source, paths, progress, cancel);
    }
    return Result::Fail("This package type is not supported yet.");
  } catch (const std::exception&) {
    return Result::Fail("This package could not be installed.");
  } catch (...) {
    return Result::Fail("This package could not be installed.");
  }
}

}  // namespace wwe2k14::launcher
