#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
#undef MINIZ_HEADER_FILE_ONLY

namespace wwe2k14::launcher::internal {

struct ZipEntry {
  std::string name;
  std::vector<uint8_t> bytes;
};

class ZipReader {
 public:
  ZipReader() = default;
  ~ZipReader();
  ZipReader(const ZipReader&) = delete;
  ZipReader& operator=(const ZipReader&) = delete;

  bool Open(const std::filesystem::path& file);
  bool ContainsSaveData() const;
  bool Extract(size_t index, std::vector<uint8_t>& bytes);
  mz_uint Count() const { return initialized_ ? archive_.m_total_files : 0; }
  bool Stat(size_t index, mz_zip_archive_file_stat& stat) const;
  bool IsDirectory(size_t index) const;
  const std::string& error() const { return error_; }

 private:
  std::vector<uint8_t> source_;
  mz_zip_archive archive_{};
  bool initialized_ = false;
  std::string error_;
};

bool CreateZip(const std::filesystem::path& destination, const std::vector<ZipEntry>& entries);

}  // namespace wwe2k14::launcher::internal
