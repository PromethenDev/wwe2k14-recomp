#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace wwe2k14::launcher::internal {

struct StfsEntry {
  std::string name;
  std::filesystem::path relative_path;
  bool directory = false;
  uint32_t first_block = 0;
  uint32_t block_count = 0;
  uint64_t size = 0;
  uint16_t parent_index = 0xFFFF;
};

struct StfsMetadata {
  uint32_t content_type = 0;
  uint32_t title_id = 0;
  std::string display_name;
  bool read_only = false;
  uint32_t total_blocks = 0;
};

class StfsReader {
 public:
  bool Open(const std::filesystem::path& package_path, std::string* error);

  const StfsMetadata& metadata() const { return metadata_; }
  const std::vector<StfsEntry>& entries() const { return entries_; }

  bool CopyFile(const StfsEntry& entry, std::ostream& output,
                const std::function<bool(uint64_t)>& after_chunk, std::string* error);

 private:
  bool ReadAt(uint64_t offset, void* output, size_t size, std::string* error);
  bool ReadDataBlock(uint32_t block, void* output, size_t size, std::string* error);
  bool ReadNextBlock(uint32_t block, uint32_t* next, std::string* error);
  uint64_t DataOffset(uint32_t block, bool* valid) const;
  uint64_t HashBlockNumber(uint32_t block, uint32_t level) const;
  bool ReadHashInfo(uint32_t block, uint32_t level, uint64_t* info, std::string* error);
  bool ResolveEntryPaths(std::string* error);

  std::filesystem::path package_path_;
  std::ifstream stream_;
  uint64_t file_size_ = 0;
  uint64_t data_base_ = 0;
  uint32_t file_table_block_ = 0;
  uint16_t file_table_block_count_ = 0;
  bool root_active_index_ = false;
  uint8_t blocks_per_hash_table_ = 0;
  uint32_t hash_step0_ = 0;
  uint32_t hash_step1_ = 0;
  StfsMetadata metadata_;
  std::vector<StfsEntry> entries_;
};

bool DecodeWwe2k14TitleId(const std::filesystem::path& executable, uint32_t* title_id);

}  // namespace wwe2k14::launcher::internal
