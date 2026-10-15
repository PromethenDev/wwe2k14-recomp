#include "stfs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <functional>
#include <limits>
#include <set>
#include <system_error>

namespace wwe2k14::launcher::internal {
namespace {

constexpr uint32_t kBlockSize = 0x1000;
constexpr uint32_t kBlocksPerHashTable = 0xAA;
constexpr uint32_t kBlocksPerSecondHashLevel = 0x70E4;
constexpr uint32_t kEndOfChain = 0xFFFFFF;
constexpr uint64_t kVolumeDescriptorOffset = 0x379;
constexpr uint64_t kDisplayNameOffset = 0x411;
constexpr uint64_t kMinimumHeaderBytes = kDisplayNameOffset + 0x80;
constexpr uint64_t kXex2Magic = 0x58455832;
constexpr uint32_t kExecutionInfoKey = 0x00040006;

uint32_t ReadBigEndianU32(const uint8_t* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

uint16_t ReadBigEndianU16(const uint8_t* bytes) {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}

uint16_t ReadLittleEndianU16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
}

uint32_t ReadLittleEndianU24(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16);
}

void AppendUtf8(uint32_t codepoint, std::string* result) {
  if (codepoint <= 0x7F) {
    result->push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    result->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    result->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    result->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    result->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    result->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    result->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    result->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    result->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    result->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

std::string DecodeUtf16Be(const uint8_t* bytes, size_t size) {
  std::string result;
  for (size_t offset = 0; offset + 1 < size; offset += 2) {
    uint32_t codepoint = ReadBigEndianU16(bytes + offset);
    if (codepoint == 0) {
      break;
    }
    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
      if (offset + 3 < size) {
        const uint32_t low = ReadBigEndianU16(bytes + offset + 2);
        if (low >= 0xDC00 && low <= 0xDFFF) {
          codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
          offset += 2;
        } else {
          codepoint = 0xFFFD;
        }
      } else {
        codepoint = 0xFFFD;
      }
    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
      codepoint = 0xFFFD;
    }
    AppendUtf8(codepoint, &result);
  }
  return result;
}

std::string DecodeLatin1(const uint8_t* bytes, size_t size) {
  std::string result;
  result.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    AppendUtf8(bytes[i], &result);
  }
  return result;
}

std::filesystem::path PathFromUtf8(const std::string& text) {
#if __cplusplus >= 202002L
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),
                                             text.size()));
#else
  return std::filesystem::u8path(text.begin(), text.end());
#endif
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

bool IsNoSpaceError() {
  if (errno == ENOSPC) {
    return true;
  }
#ifdef EDQUOT
  if (errno == EDQUOT) {
    return true;
  }
#endif
  return false;
}

}  // namespace

bool StfsReader::Open(const std::filesystem::path& package_path, std::string* error) {
  if (error) {
    error->clear();
  }
  package_path_ = package_path;
  entries_.clear();
  metadata_ = {};
  std::error_code filesystem_error;
  file_size_ = std::filesystem::file_size(package_path, filesystem_error);
  if (filesystem_error || file_size_ < kMinimumHeaderBytes) {
    if (error) {
      *error = "This package is damaged or incomplete.";
    }
    return false;
  }
  stream_.open(package_path, std::ios::binary);
  if (!stream_) {
    if (error) {
      *error = "The package could not be opened.";
    }
    return false;
  }

  std::array<uint8_t, kMinimumHeaderBytes> header{};
  if (!ReadAt(0, header.data(), header.size(), error)) {
    return false;
  }
  if (std::memcmp(header.data(), "LIVE", 4) != 0 && std::memcmp(header.data(), "PIRS", 4) != 0 &&
      std::memcmp(header.data(), "CON ", 4) != 0) {
    if (error) {
      *error = "This is not a supported Xbox 360 content package.";
    }
    return false;
  }

  const uint32_t header_size = ReadBigEndianU32(header.data() + 0x340);
  metadata_.content_type = ReadBigEndianU32(header.data() + 0x344);
  metadata_.title_id = ReadBigEndianU32(header.data() + 0x360);
  const uint32_t volume_type = ReadBigEndianU32(header.data() + 0x3A9);
  const uint8_t* volume = header.data() + kVolumeDescriptorOffset;
  if (header_size < kMinimumHeaderBytes || header_size > file_size_ || volume[0] != 0x24 ||
      volume_type != 0) {
    if (error) {
      *error = volume_type != 0 ? "This package format is not supported yet."
                                : "This package is damaged or incomplete.";
    }
    return false;
  }
  const uint8_t flags = volume[2];
  metadata_.read_only = (flags & 1) != 0;
  root_active_index_ = (flags & 2) != 0;
  file_table_block_count_ = ReadLittleEndianU16(volume + 3);
  file_table_block_ = ReadLittleEndianU24(volume + 5);
  metadata_.total_blocks = ReadBigEndianU32(volume + 0x1C);
  metadata_.display_name = DecodeUtf16Be(header.data() + kDisplayNameOffset, 0x80);
  if (file_table_block_count_ == 0 || metadata_.total_blocks == 0 ||
      file_table_block_ >= metadata_.total_blocks) {
    if (error) {
      *error = "This package is damaged or incomplete.";
    }
    return false;
  }

  blocks_per_hash_table_ = metadata_.read_only ? 1 : 2;
  hash_step0_ = kBlocksPerHashTable + blocks_per_hash_table_;
  hash_step1_ = kBlocksPerSecondHashLevel + (kBlocksPerHashTable + 1) * blocks_per_hash_table_;
  const uint64_t rounded_header = (static_cast<uint64_t>(header_size) + kBlockSize - 1) &
                                  ~(static_cast<uint64_t>(kBlockSize) - 1);
  if (rounded_header > file_size_) {
    if (error) {
      *error = "This package is damaged or incomplete.";
    }
    return false;
  }
  data_base_ = rounded_header;

  std::set<uint32_t> visited_table_blocks;
  uint32_t table_block = file_table_block_;
  for (uint16_t block_index = 0; block_index < file_table_block_count_; ++block_index) {
    if (table_block == kEndOfChain || table_block >= metadata_.total_blocks ||
        !visited_table_blocks.insert(table_block).second) {
      if (error) {
        *error = "This package's file list is damaged.";
      }
      return false;
    }
    std::array<uint8_t, kBlockSize> table{};
    if (!ReadDataBlock(table_block, table.data(), table.size(), error)) {
      return false;
    }
    for (size_t slot = 0; slot < table.size() / 0x40; ++slot) {
      const uint8_t* raw = table.data() + slot * 0x40;
      const uint8_t flags_and_length = raw[0x28];
      const uint8_t name_length = flags_and_length & 0x3F;
      if (name_length == 0) {
        break;
      }
      if (name_length > 0x28) {
        if (error) {
          *error = "This package's file list is damaged.";
        }
        return false;
      }
      StfsEntry entry;
      entry.name = DecodeLatin1(raw, name_length);
      if (!IsValidComponent(entry.name)) {
        if (error) {
          *error = "This package contains an invalid file name.";
        }
        return false;
      }
      entry.directory = (flags_and_length & 0x80) != 0;
      entry.first_block = ReadLittleEndianU24(raw + 0x2F);
      entry.block_count = ReadLittleEndianU24(raw + 0x2C);
      entry.parent_index = ReadBigEndianU16(raw + 0x32);
      entry.size = ReadBigEndianU32(raw + 0x34);
      entries_.push_back(std::move(entry));
    }
    if (block_index + 1 < file_table_block_count_) {
      uint32_t next = kEndOfChain;
      if (!ReadNextBlock(table_block, &next, error) || next == kEndOfChain) {
        if (error && error->empty()) {
          *error = "This package's file list is damaged.";
        }
        return false;
      }
      table_block = next;
    }
  }
  if (entries_.empty() || !ResolveEntryPaths(error)) {
    if (error && error->empty()) {
      *error = "This package's file list is damaged.";
    }
    return false;
  }
  return true;
}

bool StfsReader::ReadAt(uint64_t offset, void* output, size_t size, std::string* error) {
  if (offset > file_size_ || size > file_size_ - offset ||
      offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
      size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
    if (error) {
      *error = "This package is damaged or incomplete.";
    }
    return false;
  }
  stream_.clear();
  stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!stream_) {
    if (error) {
      *error = "This package could not be read.";
    }
    return false;
  }
  if (size) {
    stream_.read(static_cast<char*>(output), static_cast<std::streamsize>(size));
    if (stream_.gcount() != static_cast<std::streamsize>(size)) {
      if (error) {
        *error = "This package is damaged or incomplete.";
      }
      return false;
    }
  }
  return true;
}

uint64_t StfsReader::DataOffset(uint32_t block, bool* valid) const {
  uint64_t base = kBlocksPerHashTable;
  uint64_t physical_block = block;
  for (unsigned level = 0; level < 3; ++level) {
    const uint64_t tables = (static_cast<uint64_t>(block) + base) / base;
    if (tables > (std::numeric_limits<uint64_t>::max() - physical_block) /
                     blocks_per_hash_table_) {
      *valid = false;
      return 0;
    }
    physical_block += tables * blocks_per_hash_table_;
    if (block < base) {
      break;
    }
    if (base > std::numeric_limits<uint64_t>::max() / kBlocksPerHashTable) {
      *valid = false;
      return 0;
    }
    base *= kBlocksPerHashTable;
  }
  if (physical_block > (std::numeric_limits<uint64_t>::max() - data_base_) / kBlockSize) {
    *valid = false;
    return 0;
  }
  *valid = true;
  return data_base_ + (physical_block << 12);
}

bool StfsReader::ReadDataBlock(uint32_t block, void* output, size_t size, std::string* error) {
  if (block >= metadata_.total_blocks) {
    if (error) {
      *error = "This package's file list is damaged.";
    }
    return false;
  }
  bool valid = false;
  const uint64_t offset = DataOffset(block, &valid);
  if (!valid) {
    if (error) {
      *error = "This package is damaged or incomplete.";
    }
    return false;
  }
  return ReadAt(offset, output, size, error);
}

uint64_t StfsReader::HashBlockNumber(uint32_t block, uint32_t level) const {
  if (level == 0) {
    if (block < kBlocksPerHashTable) {
      return 0;
    }
    uint64_t number = (block / kBlocksPerHashTable) * hash_step0_;
    number += ((block / kBlocksPerSecondHashLevel) + 1) * blocks_per_hash_table_;
    if (block >= kBlocksPerSecondHashLevel) {
      number += blocks_per_hash_table_;
    }
    return number;
  }
  if (level == 1) {
    if (block < kBlocksPerSecondHashLevel) {
      return hash_step0_;
    }
    return (block / kBlocksPerSecondHashLevel) * hash_step1_ + blocks_per_hash_table_;
  }
  return hash_step1_;
}

bool StfsReader::ReadHashInfo(uint32_t block, uint32_t level, uint64_t* info,
                              std::string* error) {
  uint64_t secondary_offset = root_active_index_ ? kBlockSize : 0;
  if (!metadata_.read_only && metadata_.total_blocks > kBlocksPerHashTable && level == 0) {
    uint64_t upper_info = 0;
    if (metadata_.total_blocks > kBlocksPerSecondHashLevel) {
      if (!ReadHashInfo(block, 2, &upper_info, error)) {
        return false;
      }
      if (upper_info & 0x40000000) {
        secondary_offset = kBlockSize;
      } else {
        secondary_offset = 0;
      }
    }
    const uint32_t record = (block / kBlocksPerSecondHashLevel) % kBlocksPerHashTable;
    const uint64_t table_block = HashBlockNumber(block, 1);
    if (table_block > (std::numeric_limits<uint64_t>::max() - data_base_) / kBlockSize) {
      if (error) {
        *error = "This package is damaged or incomplete.";
      }
      return false;
    }
    const uint64_t offset = data_base_ + (table_block << 12) + secondary_offset +
                            static_cast<uint64_t>(record) * 0x18 + 0x14;
    std::array<uint8_t, 4> bytes{};
    if (!ReadAt(offset, bytes.data(), bytes.size(), error)) {
      return false;
    }
    const uint32_t info_value = ReadBigEndianU32(bytes.data());
    secondary_offset = (info_value & 0x40000000) ? kBlockSize : 0;
  } else if (metadata_.read_only) {
    secondary_offset = 0;
  }

  const uint64_t table_block = HashBlockNumber(block, level);
  if (table_block > (std::numeric_limits<uint64_t>::max() - data_base_) / kBlockSize) {
    if (error) {
      *error = "This package is damaged or incomplete.";
    }
    return false;
  }
  uint64_t record = (block % kBlocksPerHashTable) * 0x18 + 0x14;
  if (level == 1) {
    record = ((block / kBlocksPerHashTable) % kBlocksPerHashTable) * 0x18 + 0x14;
  } else if (level == 2) {
    record = ((block / kBlocksPerSecondHashLevel) % kBlocksPerHashTable) * 0x18 + 0x14;
  }
  const uint64_t offset = data_base_ + (table_block << 12) + secondary_offset + record;
  std::array<uint8_t, 4> bytes{};
  if (!ReadAt(offset, bytes.data(), bytes.size(), error)) {
    return false;
  }
  *info = ReadBigEndianU32(bytes.data());
  return true;
}

bool StfsReader::ReadNextBlock(uint32_t block, uint32_t* next, std::string* error) {
  uint64_t info = 0;
  if (!ReadHashInfo(block, 0, &info, error)) {
    return false;
  }
  *next = static_cast<uint32_t>(info & 0xFFFFFF);
  return true;
}

bool StfsReader::ResolveEntryPaths(std::string* error) {
  std::vector<uint8_t> state(entries_.size(), 0);
  std::function<bool(size_t)> resolve = [&](size_t index) {
    if (state[index] == 2) {
      return true;
    }
    if (state[index] == 1) {
      if (error) {
        *error = "This package's file list is damaged.";
      }
      return false;
    }
    state[index] = 1;
    auto& entry = entries_[index];
    if (entry.parent_index == 0xFFFF) {
      entry.relative_path = PathFromUtf8(entry.name);
    } else {
      if (entry.parent_index >= entries_.size() || entry.parent_index == index ||
          !entries_[entry.parent_index].directory || !resolve(entry.parent_index)) {
        if (error) {
          *error = "This package's file list is damaged.";
        }
        return false;
      }
      entry.relative_path = entries_[entry.parent_index].relative_path /
                            PathFromUtf8(entry.name);
    }
    entry.relative_path = entry.relative_path.lexically_normal();
    if (entry.relative_path.empty() || entry.relative_path.is_absolute() ||
        *entry.relative_path.begin() == std::filesystem::path("..")) {
      if (error) {
        *error = "This package contains an invalid file path.";
      }
      return false;
    }
    state[index] = 2;
    return true;
  };
  for (size_t index = 0; index < entries_.size(); ++index) {
    if (!resolve(index)) {
      return false;
    }
  }
  return true;
}

bool StfsReader::CopyFile(const StfsEntry& entry, std::ostream& output,
                          const std::function<bool(uint64_t)>& after_chunk, std::string* error) {
  if (entry.directory) {
    if (error) {
      *error = "This package's file list is damaged.";
    }
    return false;
  }
  uint64_t remaining = entry.size;
  uint32_t block = entry.first_block;
  uint64_t copied = 0;
  uint64_t reported = 0;
  std::set<uint32_t> visited;
  std::array<uint8_t, kBlockSize> bytes{};
  while (remaining) {
    if (block == kEndOfChain || block >= metadata_.total_blocks ||
        !visited.insert(block).second || copied / kBlockSize >= entry.block_count) {
      if (error) {
        *error = "A file in this package is incomplete.";
      }
      return false;
    }
    const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, bytes.size()));
    if (!ReadDataBlock(block, bytes.data(), count, error)) {
      return false;
    }
    errno = 0;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(count));
    if (!output) {
      if (error) {
        *error = IsNoSpaceError() ? "There is not enough free space to install this package."
                                  : "A package file could not be written.";
      }
      return false;
    }
    remaining -= count;
    copied += count;
    uint32_t next = kEndOfChain;
    if (!ReadNextBlock(block, &next, error)) {
      return false;
    }
    block = next;
    if (copied - reported >= 16 * kBlockSize || remaining == 0) {
      const uint64_t delta = copied - reported;
      reported = copied;
      if (after_chunk && !after_chunk(delta)) {
        if (error) {
          *error = "The import was cancelled.";
        }
        return false;
      }
    }
  }
  if (copied / kBlockSize + (copied % kBlockSize != 0) != entry.block_count) {
    if (error) {
      *error = "A file in this package is incomplete.";
    }
    return false;
  }
  return true;
}

bool DecodeWwe2k14TitleId(const std::filesystem::path& executable, uint32_t* title_id) {
  std::error_code filesystem_error;
  const uint64_t size = std::filesystem::file_size(executable, filesystem_error);
  if (filesystem_error || size < 0x18) {
    return false;
  }
  std::ifstream stream(executable, std::ios::binary);
  if (!stream) {
    return false;
  }
  auto read_at = [&](uint64_t offset, void* output, size_t count) {
    if (offset > size || count > size - offset ||
        offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
      return false;
    }
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream) {
      return false;
    }
    stream.read(static_cast<char*>(output), static_cast<std::streamsize>(count));
    return stream.gcount() == static_cast<std::streamsize>(count);
  };
  std::array<uint8_t, 0x18> fixed{};
  if (!read_at(0, fixed.data(), fixed.size()) || ReadBigEndianU32(fixed.data()) != kXex2Magic) {
    return false;
  }
  const uint32_t header_size = ReadBigEndianU32(fixed.data() + 8);
  const uint32_t count = ReadBigEndianU32(fixed.data() + 0x14);
  if (header_size < fixed.size() || header_size > size || count > (header_size - fixed.size()) / 8) {
    return false;
  }
  bool found = false;
  uint32_t execution_info_offset = 0;
  for (uint32_t index = 0; index < count; ++index) {
    std::array<uint8_t, 8> optional{};
    if (!read_at(fixed.size() + static_cast<uint64_t>(index) * 8, optional.data(), optional.size())) {
      return false;
    }
    if (ReadBigEndianU32(optional.data()) == kExecutionInfoKey) {
      if (found) {
        return false;
      }
      execution_info_offset = ReadBigEndianU32(optional.data() + 4);
      found = true;
    }
  }
  if (!found || execution_info_offset > header_size || header_size - execution_info_offset < 0x10) {
    return false;
  }
  std::array<uint8_t, 4> title{};
  if (!read_at(static_cast<uint64_t>(execution_info_offset) + 0x0C, title.data(), title.size())) {
    return false;
  }
  *title_id = ReadBigEndianU32(title.data());
  return true;
}

}  // namespace wwe2k14::launcher::internal
