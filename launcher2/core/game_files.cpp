#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

namespace wwe2k14::launcher {
namespace {

constexpr uint32_t kXex2Magic = 0x58455832;
constexpr uint32_t kExecutionInfoKey = 0x00040006;
constexpr uint32_t kDeltaPatchDescriptorKey = 0x000005FF;
constexpr uint32_t kWwe2k14TitleId = 0x545408B2;
constexpr uint32_t kWwe2k14TuVersion = 0x00000102;

struct XexFile {
  std::ifstream stream;
  uint64_t size = 0;
  uint32_t header_size = 0;
  uint32_t optional_header_count = 0;
};

bool ReadBytes(XexFile& file, uint64_t offset, void* output, size_t count) {
  if (offset > file.size || count > file.size - offset ||
      offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
      count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) return false;
  file.stream.clear();
  file.stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file.stream) return false;
  file.stream.read(static_cast<char*>(output), static_cast<std::streamsize>(count));
  return file.stream.good();
}

bool ReadBe32(XexFile& file, uint64_t offset, uint32_t& output) {
  std::array<uint8_t, 4> bytes{};
  if (!ReadBytes(file, offset, bytes.data(), bytes.size())) return false;
  output = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
  return true;
}

bool OpenXex(const fs::path& path, XexFile& file) {
  std::error_code error;
  file.size = fs::file_size(path, error);
  if (error || file.size < 0x18) return false;
  file.stream.open(path, std::ios::binary);
  uint32_t magic = 0;
  if (!file.stream || !ReadBe32(file, 0, magic) || magic != kXex2Magic ||
      !ReadBe32(file, 8, file.header_size) || !ReadBe32(file, 0x14, file.optional_header_count)) {
    return false;
  }
  return file.header_size >= 0x18 && file.header_size <= file.size &&
         file.optional_header_count <= (file.header_size - 0x18) / 8;
}

bool FindOptionalHeader(XexFile& file, uint32_t key, uint32_t& value) {
  bool found = false;
  for (uint32_t index = 0; index < file.optional_header_count; ++index) {
    const uint64_t entry_offset = 0x18 + uint64_t(index) * 8;
    uint32_t current_key = 0;
    uint32_t current_value = 0;
    if (!ReadBe32(file, entry_offset, current_key) || !ReadBe32(file, entry_offset + 4, current_value)) {
      return false;
    }
    if (current_key != key) continue;
    if (found || current_value > file.header_size) return false;
    found = true;
    value = current_value;
  }
  return found;
}

bool ReadExecutionInfo(XexFile& file, uint32_t& version, uint32_t& base_version, uint32_t& title_id) {
  uint32_t offset = 0;
  return FindOptionalHeader(file, kExecutionInfoKey, offset) && offset <= file.header_size &&
         file.header_size - offset >= 0x18 && ReadBe32(file, offset + 4, version) &&
         ReadBe32(file, offset + 8, base_version) && ReadBe32(file, offset + 0xC, title_id);
}

bool ReadDeltaVersions(XexFile& file, uint32_t& source_version, uint32_t& target_version) {
  uint32_t offset = 0;
  uint32_t descriptor_size = 0;
  return FindOptionalHeader(file, kDeltaPatchDescriptorKey, offset) && offset <= file.header_size &&
         file.header_size - offset >= 0x4C && ReadBe32(file, offset, descriptor_size) &&
         descriptor_size >= 0x4C && descriptor_size <= file.header_size - offset &&
         ReadBe32(file, offset + 4, target_version) && ReadBe32(file, offset + 8, source_version);
}

FileState CheckRegularFile(const fs::path& path) {
  std::error_code error;
  if (!fs::exists(path, error) || error) return FileState::kMissing;
  if (!fs::is_regular_file(path, error) || error) return FileState::kCorrupt;
  return FileState::kFound;
}

std::string StateName(FileState state) {
  switch (state) {
    case FileState::kMissing: return "not found";
    case FileState::kFound: return "ready";
    case FileState::kWrongVersion: return "not the supported version";
    case FileState::kCorrupt: return "could not be read";
  }
  return "could not be read";
}

int CountDlc(const Paths& paths) {
  const fs::path content = paths.userdata_dir / "0000000000000000" / "545408B2" / "00000002";
  std::error_code error;
  fs::directory_iterator iterator(content, fs::directory_options::skip_permission_denied, error);
  if (error) return 0;
  int count = 0;
  for (const auto& entry : iterator) {
    if (count >= 3) break;
    error.clear();
    if (entry.is_directory(error) && !error && internal::LowerAscii(internal::PathToUtf8(entry.path().filename())) != "headers") {
      ++count;
    }
  }
  return count;
}

}  // namespace

GameFilesStatus CheckGameFolder(const fs::path& folder, const Paths& paths) {
  GameFilesStatus status;
  status.game_folder = folder;
  status.dlc_installed = CountDlc(paths);
  try {
    const fs::path executable_path = folder / "default.xex";
    const fs::path update_path = folder / "default.xexp";
    status.base_game = CheckRegularFile(executable_path);
    status.title_update = CheckRegularFile(update_path);

    uint32_t exe_version = 0;
    uint32_t exe_base_version = 0;
    if (status.base_game == FileState::kFound) {
      XexFile executable;
      uint32_t title_id = 0;
      if (!OpenXex(executable_path, executable) ||
          !ReadExecutionInfo(executable, exe_version, exe_base_version, title_id)) {
        status.base_game = FileState::kCorrupt;
      } else if (title_id != kWwe2k14TitleId) {
        status.base_game = FileState::kWrongVersion;
      }
    }
    if (status.title_update == FileState::kFound) {
      XexFile update;
      uint32_t source_version = 0;
      uint32_t target_version = 0;
      if (!OpenXex(update_path, update) || !ReadDeltaVersions(update, source_version, target_version)) {
        status.title_update = FileState::kCorrupt;
      } else if (target_version != kWwe2k14TuVersion ||
                 (status.base_game == FileState::kFound && source_version != exe_version &&
                  source_version != exe_base_version)) {
        status.title_update = FileState::kWrongVersion;
      }
    }

    std::ostringstream detail;
    detail << "Base game: " << StateName(status.base_game) << "\n"
           << "Title update 1.0.3: " << StateName(status.title_update) << "\n"
           << "DLC packs: " << status.dlc_installed << " of " << status.dlc_known << " installed";
    status.detail = detail.str();
  } catch (...) {
    status.base_game = FileState::kCorrupt;
    status.title_update = FileState::kCorrupt;
    status.detail = "The selected folder could not be read.";
  }
  return status;
}

}  // namespace wwe2k14::launcher
