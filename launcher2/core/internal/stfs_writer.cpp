#include "stfs.h"

#include "util.h"

#include <algorithm>
#include <cstring>
#include <system_error>

namespace wwe2k14::launcher::internal {
namespace {

constexpr uint32_t kBlockSize = 0x1000;
constexpr uint32_t kBlocksPerHashTable = 0xAA;                         // entries per hash table block
constexpr uint32_t kBlocksPerLevel1 = kBlocksPerHashTable * kBlocksPerHashTable;  // blocks under one level-1 table
constexpr size_t kStep0 = kBlocksPerHashTable + 2;                             // StfsReader hash_step0_ (2 copies)
constexpr size_t kStep1 = kBlocksPerLevel1 + (kBlocksPerHashTable + 1) * 2;    // StfsReader hash_step1_
constexpr size_t kHeaderBytes = 0xB000;      // data area starts here (header_size below rounds up to it)
constexpr uint32_t kHeaderSizeField = 0xAD0E;  // value the game's own CON packages carry
constexpr uint32_t kEndOfChain = 0xFFFFFF;

void PutBe32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  out[offset] = static_cast<uint8_t>(value >> 24);
  out[offset + 1] = static_cast<uint8_t>(value >> 16);
  out[offset + 2] = static_cast<uint8_t>(value >> 8);
  out[offset + 3] = static_cast<uint8_t>(value);
}

void PutLe24(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  out[offset] = static_cast<uint8_t>(value);
  out[offset + 1] = static_cast<uint8_t>(value >> 8);
  out[offset + 2] = static_cast<uint8_t>(value >> 16);
}

uint32_t Rotl(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

// Read/write-format packages keep two copies (primary + secondary) of every hash table block, interleaved with the
// data: [L0 #0][data 0..169][L1 #0][L0 #1][data 170..339][L0 #2]...; past 0xAA * 0xAA blocks the single level-2
// table and the next level-1 table follow. Same mapping as StfsReader::DataOffset / HashBlockNumber (and Xenia);
// the two-level case is verified on the game-written Best of Save 01CustomArena.car.
size_t PhysicalDataBlock(uint32_t block) {
  size_t physical = block;
  uint64_t base = kBlocksPerHashTable;
  for (int level = 0; level < 3; ++level) {
    physical += static_cast<size_t>((block + base) / base) * 2;
    if (block < base) break;
    base *= kBlocksPerHashTable;
  }
  return physical;
}

// Physical block of the hash table at `level` that covers data block `block`.
size_t PhysicalHashTable(uint32_t block, int level) {
  if (level == 0) {
    if (block < kBlocksPerHashTable) return 0;
    const size_t number = (block / kBlocksPerHashTable) * kStep0 + (block / kBlocksPerLevel1 + 1) * 2;
    return block < kBlocksPerLevel1 ? number : number + 2;
  }
  if (level == 1) return block < kBlocksPerLevel1 ? kStep0 : (block / kBlocksPerLevel1) * kStep1 + 2;
  return kStep1;
}

}  // namespace

std::array<uint8_t, 20> Sha1(const uint8_t* data, size_t size) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::vector<uint8_t> message(data, data + size);
  message.push_back(0x80);
  while (message.size() % 64 != 56) message.push_back(0);
  const uint64_t bits = static_cast<uint64_t>(size) * 8;
  for (int i = 7; i >= 0; --i) message.push_back(static_cast<uint8_t>(bits >> (8 * i)));
  for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(message[chunk + 4 * i]) << 24) |
             (static_cast<uint32_t>(message[chunk + 4 * i + 1]) << 16) |
             (static_cast<uint32_t>(message[chunk + 4 * i + 2]) << 8) |
             static_cast<uint32_t>(message[chunk + 4 * i + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      const uint32_t temp = Rotl(a, 5) + f + e + k + w[i];
      e = d; d = c; c = Rotl(b, 30); b = a; a = temp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::array<uint8_t, 20> digest{};
  for (int i = 0; i < 5; ++i) {
    digest[4 * i] = static_cast<uint8_t>(h[i] >> 24);
    digest[4 * i + 1] = static_cast<uint8_t>(h[i] >> 16);
    digest[4 * i + 2] = static_cast<uint8_t>(h[i] >> 8);
    digest[4 * i + 3] = static_cast<uint8_t>(h[i]);
  }
  return digest;
}

bool WriteStfsSaveGamePackage(const std::filesystem::path& package_path, const std::string& display_name_ascii,
                              uint32_t title_id, const std::string& file_name,
                              const std::vector<uint8_t>& file_bytes, std::string* error) {
  auto fail = [&](const char* text) {
    if (error) *error = text;
    return false;
  };
  const uint32_t data_blocks = static_cast<uint32_t>((file_bytes.size() + kBlockSize - 1) / kBlockSize);
  const uint32_t total_blocks = 1 + data_blocks;  // block 0 = file table
  // No size limit of our own: three hash levels address far more than the 32-bit file size field (4 GB) allows.
  if (file_bytes.empty() || file_bytes.size() > 0xFFFFFFFFu) {
    return fail(file_bytes.empty() ? "The file is empty." : "The file is too large to export (over 4 GB).");
  }
  if (file_name.empty() || file_name.size() > 0x28 || display_name_ascii.size() > 0x3F) {
    return fail("The package names are too long.");
  }

  // Physical layout: [header 0xB000][hash tables and data blocks, see PhysicalDataBlock]. Block 0 = file table.
  const int levels = total_blocks <= kBlocksPerHashTable ? 1 : total_blocks <= kBlocksPerLevel1 ? 2 : 3;
  const size_t data_base = kHeaderBytes;
  const auto block_at = [&](size_t physical) { return data_base + physical * kBlockSize; };
  std::vector<uint8_t> out(block_at(PhysicalDataBlock(total_blocks - 1) + 1), 0);

  // File table (block 0): one 0x40-byte entry.
  const size_t table_offset = block_at(PhysicalDataBlock(0));
  uint8_t* entry = out.data() + table_offset;
  std::memcpy(entry, file_name.data(), file_name.size());
  entry[0x28] = static_cast<uint8_t>(0x40 | file_name.size());  // consecutive blocks, name length
  PutLe24(out, table_offset + 0x29, data_blocks);
  PutLe24(out, table_offset + 0x2C, data_blocks);
  PutLe24(out, table_offset + 0x2F, 1);
  out[table_offset + 0x32] = 0xFF;  // parent: root
  out[table_offset + 0x33] = 0xFF;
  PutBe32(out, table_offset + 0x34, static_cast<uint32_t>(file_bytes.size()));
  for (uint32_t block = 1; block < total_blocks; ++block) {
    const size_t source = static_cast<size_t>(block - 1) * kBlockSize;
    const size_t size = std::min<size_t>(kBlockSize, file_bytes.size() - source);
    std::copy(file_bytes.begin() + static_cast<std::ptrdiff_t>(source),
              file_bytes.begin() + static_cast<std::ptrdiff_t>(source + size),
              out.begin() + static_cast<std::ptrdiff_t>(block_at(PhysicalDataBlock(block))));
  }

  // Level-0 hash tables: SHA-1 of each block, status byte, next block (chain, end = 0xFFFFFF).
  for (uint32_t block = 0; block < total_blocks; ++block) {
    const auto digest = Sha1(out.data() + block_at(PhysicalDataBlock(block)), kBlockSize);
    uint8_t* record = out.data() + block_at(PhysicalHashTable(block, 0)) +
                      static_cast<size_t>(block % kBlocksPerHashTable) * 0x18;
    std::copy(digest.begin(), digest.end(), record);
    const uint32_t next = (block == 0 || block + 1 >= total_blocks) ? kEndOfChain : block + 1;
    record[0x14] = 0x80;
    record[0x15] = static_cast<uint8_t>(next >> 16);
    record[0x16] = static_cast<uint8_t>(next >> 8);
    record[0x17] = static_cast<uint8_t>(next);
  }
  // Upper levels: SHA-1 of each lower table's primary copy; info 0x80000000 = in use, primary copy active.
  for (int level = 1; level < levels; ++level) {
    const uint64_t span = level == 1 ? kBlocksPerHashTable : kBlocksPerLevel1;  // data blocks per lower table
    for (uint64_t first = 0; first < total_blocks; first += span) {
      const uint32_t block = static_cast<uint32_t>(first);
      const auto digest = Sha1(out.data() + block_at(PhysicalHashTable(block, level - 1)), kBlockSize);
      uint8_t* record = out.data() + block_at(PhysicalHashTable(block, level)) +
                        static_cast<size_t>((first / span) % kBlocksPerHashTable) * 0x18;
      std::copy(digest.begin(), digest.end(), record);
      record[0x14] = 0x80;
    }
  }
  const auto top_hash = Sha1(out.data() + block_at(PhysicalHashTable(0, levels - 1)), kBlockSize);

  // Metadata header.
  std::memcpy(out.data(), "CON ", 4);
  PutBe32(out, 0x340, kHeaderSizeField);
  PutBe32(out, 0x344, 1);  // saved game
  PutBe32(out, 0x348, 1);  // metadata version
  PutBe32(out, 0x360, title_id);
  uint8_t* volume = out.data() + 0x379;
  volume[0] = 0x24;
  volume[2] = 0;  // read/write format, primary hash table active
  volume[3] = 1;  // file table block count (LE16)
  PutLe24(out, 0x379 + 5, 0);
  std::copy(top_hash.begin(), top_hash.end(), volume + 8);
  PutBe32(out, 0x379 + 0x1C, total_blocks);
  // Display name: UTF-16BE at 0x411 (0x80 bytes); non-ASCII is not expected (story names are ASCII).
  for (size_t i = 0; i < display_name_ascii.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(display_name_ascii[i]);
    out[0x411 + 2 * i + 1] = c < 0x80 ? c : '?';
  }
  // Content id = SHA-1 of the header from 0x344 to header_size.
  const auto content_id = Sha1(out.data() + 0x344, kHeaderSizeField - 0x344);
  std::copy(content_id.begin(), content_id.end(), out.begin() + 0x32C);

  std::filesystem::path temp = package_path;  // native path operations (no narrow string round trip)
  temp += ".tmp";
  std::error_code ec;
  std::filesystem::remove(temp, ec);
  if (!WriteBinaryFile(temp, out)) return fail("The package file could not be written.");
  std::filesystem::remove(package_path, ec);
  ec.clear();
  std::filesystem::rename(temp, package_path, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return fail("The package file could not be written.");
  }
  return true;
}

}  // namespace wwe2k14::launcher::internal
