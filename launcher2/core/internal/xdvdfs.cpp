#include "xdvdfs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

namespace wwe2k14::launcher::internal {
namespace {

constexpr uint64_t kSectorSize = 0x800;
constexpr uint64_t kDescriptorSector = 32;
constexpr uint64_t kMaxDirectoryBytes = 32 * 1024 * 1024;
constexpr size_t kMaxDirectoryEntries = 1000000;
constexpr unsigned kMaxTreeDepth = 1024;
constexpr unsigned kMaxDirectoryDepth = 256;
constexpr size_t kCopyChunkSize = 64 * 1024;

uint32_t ReadLe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

uint16_t ReadLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
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

std::string AsciiLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
  });
  return value;
}

std::filesystem::path PathFromUtf8(const std::string& text) {
#if __cplusplus >= 202002L
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),
                                             text.size()));
#else
  return std::filesystem::u8path(text.begin(), text.end());
#endif
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

bool XdvdfsReader::Open(const std::filesystem::path& image_path, std::string* error) {
  if (error) {
    error->clear();
  }
  image_path_ = image_path;
  root_entries_.clear();
  total_file_bytes_ = 0;
  partition_offset_ = 0;

  std::error_code filesystem_error;
  if (std::filesystem::is_directory(image_path, filesystem_error)) {
    if (error) {
      *error = "Games-on-Demand folders are not supported yet.";
    }
    return false;
  }
  if (filesystem_error) {
    if (error) {
      *error = "The disc image could not be opened.";
    }
    return false;
  }
  image_size_ = std::filesystem::file_size(image_path, filesystem_error);
  if (filesystem_error || image_size_ < (kDescriptorSector + 1) * kSectorSize) {
    if (error) {
      *error = "This is not an Xbox 360 disc image.";
    }
    return false;
  }
  stream_.open(image_path, std::ios::binary);
  if (!stream_) {
    if (error) {
      *error = "The disc image could not be opened.";
    }
    return false;
  }

  constexpr std::array<uint64_t, 3> kPartitionOffsets = {0, 0x02080000, 0x0FD90000};
  std::array<uint8_t, 20> magic{};
  bool found_partition = false;
  for (const uint64_t candidate : kPartitionOffsets) {
    if (candidate > image_size_ || kDescriptorSector * kSectorSize > image_size_ - candidate ||
        magic.size() > image_size_ - candidate - kDescriptorSector * kSectorSize) {
      continue;
    }
    std::string read_error;
    if (!ReadAt(candidate + kDescriptorSector * kSectorSize, magic.data(), magic.size(),
                &read_error)) {
      continue;
    }
    if (std::memcmp(magic.data(), "MICROSOFT*XBOX*MEDIA", magic.size()) == 0) {
      partition_offset_ = candidate;
      found_partition = true;
      break;
    }
  }
  if (!found_partition) {
    if (error) {
      *error = "This is not an Xbox 360 disc image.";
    }
    return false;
  }

  std::array<uint8_t, 28> descriptor{};
  if (!ReadAt(partition_offset_ + kDescriptorSector * kSectorSize, descriptor.data(),
              descriptor.size(), error)) {
    return false;
  }
  const uint32_t root_sector = ReadLe32(descriptor.data() + 20);
  const uint32_t root_size = ReadLe32(descriptor.data() + 24);
  if (root_size < 14 || root_size > kMaxDirectoryBytes ||
      root_sector > (std::numeric_limits<uint64_t>::max() - partition_offset_) / kSectorSize) {
    if (error) {
      *error = "The disc image's directory is damaged.";
    }
    return false;
  }
  const uint64_t root_offset = partition_offset_ + static_cast<uint64_t>(root_sector) * kSectorSize;
  if (root_offset > image_size_ || root_size > image_size_ - root_offset) {
    if (error) {
      *error = "The disc image's directory is damaged.";
    }
    return false;
  }
  std::vector<uint8_t> root_buffer(root_size);
  if (!ReadAt(root_offset, root_buffer.data(), root_buffer.size(), error)) {
    return false;
  }
  if (!ParseTree(root_buffer, {}, 0, &root_entries_, error)) {
    root_entries_.clear();
    return false;
  }
  return true;
}

bool XdvdfsReader::ParseTree(const std::vector<uint8_t>& bytes,
                             const std::filesystem::path& parent_path,
                             unsigned directory_depth, std::vector<XdvdfsEntry>* entries,
                             std::string* error) {
  if (directory_depth > kMaxDirectoryDepth) {
    if (error) {
      *error = "The disc image contains too many nested folders.";
    }
    return false;
  }
  if (bytes.size() < 14) {
    if (error) {
      *error = "The disc image's directory is damaged.";
    }
    return false;
  }

  std::set<uint16_t> visited;
  size_t node_count = 0;
  std::function<bool(uint16_t, unsigned)> visit = [&](uint16_t ordinal, unsigned depth) {
    if (depth > kMaxTreeDepth || ++node_count > kMaxDirectoryEntries ||
        !visited.insert(ordinal).second) {
      if (error) {
        *error = "The disc image's directory is damaged.";
      }
      return false;
    }
    const uint64_t position = static_cast<uint64_t>(ordinal) * 4;
    if (position > bytes.size() || bytes.size() - position < 14) {
      if (error) {
        *error = "The disc image's directory is damaged.";
      }
      return false;
    }
    const uint8_t* record = bytes.data() + position;
    const uint16_t left = ReadLe16(record);
    const uint16_t right = ReadLe16(record + 2);
    const uint32_t sector = ReadLe32(record + 4);
    const uint32_t size = ReadLe32(record + 8);
    const uint8_t attributes = record[12];
    const uint8_t name_length = record[13];
    if (name_length == 0 || name_length > bytes.size() - position - 14) {
      if (error) {
        *error = "The disc image's directory is damaged.";
      }
      return false;
    }
    if (left && !visit(left, depth + 1)) {
      return false;
    }

    XdvdfsEntry entry;
    entry.name.assign(reinterpret_cast<const char*>(record + 14), name_length);
    if (!IsValidComponent(entry.name)) {
      if (error) {
        *error = "The disc image contains an invalid file name.";
      }
      return false;
    }
    entry.relative_path = parent_path / PathFromUtf8(entry.name);
    entry.directory = (attributes & 0x10) != 0;
    entry.sector = sector;
    entry.size = size;
    const std::filesystem::path normalized = entry.relative_path.lexically_normal();
    const std::filesystem::path relative_check = normalized.lexically_relative(parent_path);
    if (normalized.is_absolute() || relative_check.empty() ||
        *relative_check.begin() == std::filesystem::path("..")) {
      if (error) {
        *error = "The disc image contains an invalid file name.";
      }
      return false;
    }

    if (entry.sector > (std::numeric_limits<uint64_t>::max() - partition_offset_) / kSectorSize) {
      if (error) {
        *error = "The disc image's directory is damaged.";
      }
      return false;
    }
    const uint64_t data_offset = partition_offset_ + static_cast<uint64_t>(entry.sector) * kSectorSize;
    if (entry.directory) {
      if (size > kMaxDirectoryBytes || (size && size < 14) || data_offset > image_size_ ||
          size > image_size_ - data_offset) {
        if (error) {
          *error = "The disc image's directory is damaged.";
        }
        return false;
      }
      if (size) {
        std::vector<uint8_t> child_buffer(size);
        if (!ReadAt(data_offset, child_buffer.data(), child_buffer.size(), error) ||
            !ParseTree(child_buffer, entry.relative_path, directory_depth + 1,
                       &entry.children, error)) {
          return false;
        }
      }
    } else {
      if (data_offset > image_size_ || size > image_size_ - data_offset ||
          size > std::numeric_limits<uint64_t>::max() - total_file_bytes_) {
        if (error) {
          *error = "The disc image contains a file outside its valid range.";
        }
        return false;
      }
      total_file_bytes_ += size;
    }
    entries->push_back(std::move(entry));
    if (right && !visit(right, depth + 1)) {
      return false;
    }
    return true;
  };

  return visit(0, 0);
}

const XdvdfsEntry* XdvdfsReader::FindRootFile(const std::string& name) const {
  const std::string wanted = AsciiLower(name);
  for (const auto& entry : root_entries_) {
    if (!entry.directory && AsciiLower(entry.name) == wanted) {
      return &entry;
    }
  }
  return nullptr;
}

bool XdvdfsReader::ReadAt(uint64_t offset, void* output, size_t size, std::string* error) {
  if (offset > image_size_ || size > image_size_ - offset ||
      offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
      size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
    if (error) {
      *error = "The disc image is damaged or incomplete.";
    }
    return false;
  }
  stream_.clear();
  stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!stream_) {
    if (error) {
      *error = "The disc image could not be read.";
    }
    return false;
  }
  if (size) {
    stream_.read(static_cast<char*>(output), static_cast<std::streamsize>(size));
    if (stream_.gcount() != static_cast<std::streamsize>(size)) {
      if (error) {
        *error = "The disc image is damaged or incomplete.";
      }
      return false;
    }
  }
  return true;
}

bool XdvdfsReader::ReadRange(const XdvdfsEntry& entry, uint64_t offset, void* output, size_t size,
                             std::string* error) {
  if (entry.directory || offset > entry.size || size > entry.size - offset ||
      entry.sector > (std::numeric_limits<uint64_t>::max() - partition_offset_) / kSectorSize) {
    if (error) {
      *error = "The disc image is damaged or incomplete.";
    }
    return false;
  }
  const uint64_t base = partition_offset_ + static_cast<uint64_t>(entry.sector) * kSectorSize;
  if (offset > std::numeric_limits<uint64_t>::max() - base) {
    if (error) {
      *error = "The disc image is damaged or incomplete.";
    }
    return false;
  }
  return ReadAt(base + offset, output, size, error);
}

bool XdvdfsReader::CopyFile(const XdvdfsEntry& entry, std::ostream& output,
                            const std::function<bool(uint64_t)>& after_chunk,
                            std::string* error) {
  if (entry.directory) {
    if (error) {
      *error = "The disc image's directory is damaged.";
    }
    return false;
  }
  std::array<char, kCopyChunkSize> buffer{};
  uint64_t copied = 0;
  while (copied < entry.size) {
    const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), entry.size - copied));
    if (!ReadRange(entry, copied, buffer.data(), count, error)) {
      return false;
    }
    errno = 0;
    output.write(buffer.data(), static_cast<std::streamsize>(count));
    if (!output) {
      if (error) {
        *error = IsNoSpaceError() ? "There is not enough free space to extract the disc image."
                                  : "A file could not be written while extracting the disc image.";
      }
      return false;
    }
    copied += count;
    if (after_chunk && !after_chunk(count)) {
      if (error) {
        *error = "The extraction was cancelled.";
      }
      return false;
    }
  }
  return true;
}

}  // namespace wwe2k14::launcher::internal
