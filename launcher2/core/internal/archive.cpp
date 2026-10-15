#include "archive.h"

#include "util.h"

#include <algorithm>
#include <cstring>
#include <system_error>

namespace wwe2k14::launcher::internal {

ZipReader::~ZipReader() {
  if (initialized_) mz_zip_reader_end(&archive_);
}

bool ZipReader::Open(const std::filesystem::path& file) {
  if (initialized_) {
    mz_zip_reader_end(&archive_);
    archive_ = {};
    initialized_ = false;
  }
  error_.clear();
  if (!ReadBinaryFile(file, source_) || source_.empty()) {
    error_ = "The backup could not be opened.";
    return false;
  }
  archive_ = {};
  if (!mz_zip_reader_init_mem(&archive_, source_.data(), source_.size(), 0)) {
    error_ = "The backup is not a valid ZIP file.";
    return false;
  }
  initialized_ = true;
  return true;
}

bool ZipReader::ContainsSaveData() const {
  if (!initialized_) return false;
  for (mz_uint index = 0; index < archive_.m_total_files; ++index) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(const_cast<mz_zip_archive*>(&archive_), index, &stat)) continue;
    std::string name = LowerAscii(stat.m_filename);
    std::replace(name.begin(), name.end(), '\\', '/');
    while (!name.empty() && name.back() == '/') name.pop_back();
    const size_t slash = name.find_last_of('/');
    if ((slash == std::string::npos ? name : name.substr(slash + 1)) == "savedata.dat") return true;
  }
  return false;
}

bool ZipReader::Extract(size_t index, std::vector<uint8_t>& bytes) {
  if (!initialized_ || index >= archive_.m_total_files || mz_zip_reader_is_file_encrypted(&archive_, static_cast<mz_uint>(index))) {
    error_ = "The backup contains a file that cannot be read.";
    return false;
  }
  size_t size = 0;
  void* data = mz_zip_reader_extract_to_heap(&archive_, static_cast<mz_uint>(index), &size, 0);
  if (!data && size != 0) {
    error_ = "The backup contains a file that could not be extracted.";
    return false;
  }
  bytes.resize(size);
  if (size) std::memcpy(bytes.data(), data, size);
  mz_free(data);
  return true;
}

bool ZipReader::Stat(size_t index, mz_zip_archive_file_stat& stat) const {
  return initialized_ && index < archive_.m_total_files &&
         mz_zip_reader_file_stat(const_cast<mz_zip_archive*>(&archive_), static_cast<mz_uint>(index), &stat) != 0;
}

bool ZipReader::IsDirectory(size_t index) const {
  return initialized_ && index < archive_.m_total_files &&
         mz_zip_reader_is_file_a_directory(const_cast<mz_zip_archive*>(&archive_), static_cast<mz_uint>(index)) != 0;
}

bool CreateZip(const std::filesystem::path& destination, const std::vector<ZipEntry>& entries) {
  mz_zip_archive archive{};
  if (!mz_zip_writer_init_heap(&archive, 0, 64 * 1024)) return false;
  static constexpr char kEmpty = 0;
  bool okay = true;
  for (const auto& entry : entries) {
    const void* bytes = entry.bytes.empty() ? static_cast<const void*>(&kEmpty)
                                             : static_cast<const void*>(entry.bytes.data());
    if (!mz_zip_writer_add_mem(&archive, entry.name.c_str(), bytes, entry.bytes.size(), MZ_DEFAULT_COMPRESSION)) {
      okay = false;
      break;
    }
  }
  void* archive_bytes = nullptr;
  size_t archive_size = 0;
  if (okay) okay = mz_zip_writer_finalize_heap_archive(&archive, &archive_bytes, &archive_size) != 0;
  if (okay) okay = WriteBinaryFile(destination, archive_bytes, archive_size);
  if (archive_bytes) mz_free(archive_bytes);
  mz_zip_writer_end(&archive);
  return okay;
}

}  // namespace wwe2k14::launcher::internal
