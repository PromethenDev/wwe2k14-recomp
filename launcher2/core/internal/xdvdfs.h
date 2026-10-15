#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace wwe2k14::launcher::internal {

struct XdvdfsEntry {
  std::string name;
  std::filesystem::path relative_path;
  bool directory = false;
  uint32_t sector = 0;
  uint32_t size = 0;
  std::vector<XdvdfsEntry> children;
};

class XdvdfsReader {
 public:
  bool Open(const std::filesystem::path& image_path, std::string* error);

  const std::vector<XdvdfsEntry>& root_entries() const { return root_entries_; }
  uint64_t total_file_bytes() const { return total_file_bytes_; }
  const std::filesystem::path& image_path() const { return image_path_; }

  const XdvdfsEntry* FindRootFile(const std::string& name) const;
  bool ReadRange(const XdvdfsEntry& entry, uint64_t offset, void* output, size_t size,
                 std::string* error);
  bool CopyFile(const XdvdfsEntry& entry, std::ostream& output,
                const std::function<bool(uint64_t)>& after_chunk, std::string* error);

 private:
  bool ReadAt(uint64_t offset, void* output, size_t size, std::string* error);
  bool ParseTree(const std::vector<uint8_t>& bytes, const std::filesystem::path& parent_path,
                 unsigned directory_depth, std::vector<XdvdfsEntry>* entries,
                 std::string* error);

  std::filesystem::path image_path_;
  std::ifstream stream_;
  uint64_t image_size_ = 0;
  uint64_t partition_offset_ = 0;
  uint64_t total_file_bytes_ = 0;
  std::vector<XdvdfsEntry> root_entries_;
};

}  // namespace wwe2k14::launcher::internal
