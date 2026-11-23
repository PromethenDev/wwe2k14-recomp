#pragma once

#include <array>
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

// Writes a minimal single-file Xbox 360 saved-game package (STFS type "CON ", content type 1) holding one root
// file. Layout matches what StfsReader opens: header 0xB000 bytes, read/write-format hash tables (2 blocks each) at
// every level the size needs (up to three), file-table block 0, file data from block 1 chained to the end marker.
// Block hashes and the top hash are real SHA-1 values; the console signature is NOT valid (the package is unsigned),
// so a real Xbox 360 would not accept it. No size limit beyond the 32-bit file size field (4 GB).
bool WriteStfsSaveGamePackage(const std::filesystem::path& package_path, const std::string& display_name_ascii,
                              uint32_t title_id, const std::string& file_name,
                              const std::vector<uint8_t>& file_bytes, std::string* error);
// SHA-1 (used for STFS block hashes); exposed for tests.
std::array<uint8_t, 20> Sha1(const uint8_t* data, size_t size);

bool DecodeWwe2k14TitleId(const std::filesystem::path& executable, uint32_t* title_id);
bool DecodeXexTitleId(const std::filesystem::path& executable, uint32_t* title_id);

}  // namespace wwe2k14::launcher::internal
