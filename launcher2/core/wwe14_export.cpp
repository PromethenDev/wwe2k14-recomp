#include "launcher_core.h"

#include "internal/util.h"
#include "internal/miniz_api.h"
#include "internal/game_codecs.h"

#include <lzx.h>
#include <mspack.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace wwe2k14::launcher {
namespace {

using Bytes = std::vector<uint8_t>;
constexpr uint64_t kMaxInputBytes = 512ull * 1024 * 1024;
constexpr uint64_t kMaxDecodedBytes = 512ull * 1024 * 1024;
constexpr uint32_t kMaxArchiveEntries = 100000;

uint32_t Le32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
         (uint32_t(bytes[3]) << 24);
}

uint32_t Be32(const uint8_t* bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
         uint32_t(bytes[3]);
}

bool HasMagic(const Bytes& bytes, const char* magic) {
  return bytes.size() >= 4 && std::memcmp(bytes.data(), magic, 4) == 0;
}

std::string LowerAscii(std::string value) {
  for (char& c : value)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return value;
}

std::string TrimField(const uint8_t* bytes, size_t length) {
  while (length && (bytes[length - 1] == 0 || bytes[length - 1] == ' ')) --length;
  size_t begin = 0;
  while (begin < length && (bytes[begin] == 0 || bytes[begin] == ' ')) ++begin;
  return std::string(reinterpret_cast<const char*>(bytes + begin), length - begin);
}

std::string Latin1ToUtf8(std::string_view value) {
  std::string out;
  out.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    if (byte < 0x80) {
      out.push_back(static_cast<char>(byte));
    } else {
      out.push_back(static_cast<char>(0xC0 | (byte >> 6)));
      out.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
    }
  }
  return out;
}

bool IsValidUtf8(std::string_view value) {
  size_t i = 0;
  while (i < value.size()) {
    const uint8_t first = static_cast<uint8_t>(value[i++]);
    if (first < 0x80) continue;
    const unsigned extra = first >= 0xF0 && first <= 0xF4 ? 3
                         : first >= 0xE0 ? 2
                         : first >= 0xC2 ? 1
                                         : 0;
    if (extra == 0 || i + extra > value.size()) return false;
    uint32_t codepoint = first & (extra == 3 ? 0x07 : extra == 2 ? 0x0F : 0x1F);
    for (unsigned j = 0; j < extra; ++j) {
      const uint8_t next = static_cast<uint8_t>(value[i++]);
      if ((next & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3F);
    }
    if ((extra == 1 && codepoint < 0x80) || (extra == 2 && codepoint < 0x800) ||
        (extra == 3 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
  }
  return true;
}

std::string SafeToken(std::string_view text) {
  std::string token;
  token.reserve(text.size());
  bool last_separator = false;
  for (const unsigned char c : text) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (safe) {
      token.push_back(static_cast<char>(c));
      last_separator = false;
    } else if (!last_separator && !token.empty()) {
      token.push_back('_');
      last_separator = true;
    }
  }
  while (!token.empty() && (token.back() == '_' || token.back() == '.')) token.pop_back();
  if (token.empty()) token = "wrestler";
  const std::string lower = LowerAscii(token);
  static const std::set<std::string> reserved{"con", "prn", "aux", "nul", "com1", "com2", "com3",
                                               "com4", "com5", "com6", "com7", "com8", "com9",
                                               "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6",
                                               "lpt7", "lpt8", "lpt9"};
  if (reserved.contains(lower)) token.insert(token.begin(), '_');
  return token;
}

bool ReadFile(const fs::path& path, Bytes& bytes, std::string& error) {
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size > kMaxInputBytes || size > std::numeric_limits<size_t>::max()) {
    error = "A game archive could not be read or is too large: " + internal::PathToUtf8(path.filename());
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "A game archive could not be opened: " + internal::PathToUtf8(path.filename());
    return false;
  }
  bytes.resize(static_cast<size_t>(size));
  if (!bytes.empty()) file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!file && !bytes.empty()) {
    error = "A game archive could not be read: " + internal::PathToUtf8(path.filename());
    bytes.clear();
    return false;
  }
  return true;
}

bool WriteFile(const fs::path& path, const Bytes& bytes, std::string& error) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    error = "An export file could not be created.";
    return false;
  }
  if (!bytes.empty()) file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  file.flush();
  if (!file) {
    error = "An export file could not be written. Check that the destination has free space.";
    return false;
  }
  return true;
}

struct MemoryFile {
  const uint8_t* source = nullptr;
  uint8_t* destination = nullptr;
  size_t size = 0;
  size_t position = 0;
};

int MemoryRead(mspack_file* file, void* buffer, int count) {
  auto* memory = reinterpret_cast<MemoryFile*>(file);
  if (count <= 0 || memory->position >= memory->size) return 0;
  const size_t amount = std::min(static_cast<size_t>(count), memory->size - memory->position);
  std::memcpy(buffer, memory->source + memory->position, amount);
  memory->position += amount;
  return static_cast<int>(amount);
}

int MemoryWrite(mspack_file* file, void* buffer, int count) {
  auto* memory = reinterpret_cast<MemoryFile*>(file);
  if (count <= 0 || memory->position >= memory->size) return 0;
  const size_t amount = std::min(static_cast<size_t>(count), memory->size - memory->position);
  std::memcpy(memory->destination + memory->position, buffer, amount);
  memory->position += amount;
  return static_cast<int>(amount);
}

void* MemoryAlloc(mspack_system*, size_t size) { return std::calloc(size, 1); }
void MemoryFree(void* memory) { std::free(memory); }
void MemoryCopy(void* source, void* destination, size_t size) { std::memcpy(destination, source, size); }

bool DecodeLzxFrame(const Bytes& compressed, Bytes& decoded, int window_bits) {
  if (decoded.empty() || decoded.size() > static_cast<size_t>(std::numeric_limits<off_t>::max())) return false;
  mspack_system system{};
  system.read = MemoryRead;
  system.write = MemoryWrite;
  system.alloc = MemoryAlloc;
  system.free = MemoryFree;
  system.copy = MemoryCopy;
  MemoryFile source{compressed.data(), nullptr, compressed.size(), 0};
  MemoryFile destination{nullptr, decoded.data(), decoded.size(), 0};
  auto* stream = lzxd_init(&system, reinterpret_cast<mspack_file*>(&source),
                           reinterpret_cast<mspack_file*>(&destination), window_bits, 0, 0x8000,
                           static_cast<off_t>(decoded.size()), 0);
  if (!stream) return false;
  const int result = lzxd_decompress(stream, static_cast<off_t>(decoded.size()));
  lzxd_free(stream);
  return result == MSPACK_ERR_OK && destination.position == decoded.size();
}

bool DecodeXCompress(const Bytes& input, Bytes& output, std::string& error,
                     const CancelFlag* cancel = nullptr,
                     const std::function<void(size_t, size_t)>& on_chunk = {}) {
  if (input.size() < 20 || !HasMagic(input, "\x0F\xF5\x12\xED")) {
    error = "The XCompress archive header is invalid.";
    return false;
  }
  const uint32_t flags = Be32(input.data() + 12);
  const size_t chunk_count = (flags >> 6) & 0xFFFF;
  const int length_bits = (flags & 0xC00000) ? 32 : 20;
  const unsigned shift = (flags >> 4) & 3;
  const size_t chunk_size = size_t(0x8000) << shift;
  const int window_bits = 15 + static_cast<int>(shift);
  if (chunk_count == 0 || chunk_count > 8192) {
    error = "The XCompress archive has an unsupported chunk count.";
    return false;
  }
  const size_t table_size = ((length_bits * chunk_count + 31) / 32) * 4;
  const size_t data_offset = 16 + table_size;
  if (data_offset > input.size()) {
    error = "The XCompress chunk table is truncated.";
    return false;
  }
  std::vector<uint32_t> lengths;
  lengths.reserve(chunk_count);
  uint64_t output_size = 0;
  for (size_t i = 0; i < chunk_count; ++i) {
    uint32_t length = 0;
    const size_t first_bit = i * static_cast<size_t>(length_bits);
    for (int bit = 0; bit < length_bits; ++bit) {
      const size_t position = first_bit + static_cast<size_t>(bit);
      const uint8_t byte = input[16 + position / 8];
      length = (length << 1) | ((byte >> (7 - (position & 7))) & 1);
    }
    // Lengths describe expanded data and can exceed the fixed-size compressed input slot.
    // In particular, real WWE 2K14 character chunks commonly expand past 0x20000 bytes.
    output_size += length;
    if (output_size > kMaxDecodedBytes) {
      error = "The game archive expands beyond the export size limit.";
      return false;
    }
    lengths.push_back(length);
  }

  output.clear();
  output.reserve(static_cast<size_t>(output_size));
  for (size_t i = 0; i < chunk_count; ++i) {
    if (cancel && cancel->load(std::memory_order_relaxed)) {
      error = "Export cancelled.";
      return false;
    }
    const size_t slot_begin = i == 0 ? data_offset : i * chunk_size;
    const size_t slot_end = std::min(input.size(), (i + 1) * chunk_size);
    Bytes frame;
    if (slot_begin <= slot_end) {
      size_t cursor = slot_begin;
      frame.reserve(slot_end - cursor + 64);
      while (cursor + 2 <= slot_end) {
        uint16_t compressed_size = 0;
        if (input[cursor] == 0xFF) {
          if (cursor + 5 > slot_end) break;
          compressed_size = static_cast<uint16_t>((uint16_t(input[cursor + 3]) << 8) | input[cursor + 4]);
          cursor += 5;
        } else {
          compressed_size = static_cast<uint16_t>((uint16_t(input[cursor]) << 8) | input[cursor + 1]);
          cursor += 2;
        }
        if (compressed_size == 0 || cursor + compressed_size > slot_end) break;
        frame.insert(frame.end(), input.begin() + static_cast<std::ptrdiff_t>(cursor),
                     input.begin() + static_cast<std::ptrdiff_t>(cursor + compressed_size));
        if (compressed_size & 1) frame.push_back(0);
        cursor += compressed_size;
      }
    }
    frame.insert(frame.end(), 64, 0);
    Bytes chunk(lengths[i]);
    const bool lzx_ok = DecodeLzxFrame(frame, chunk, window_bits);
    if (lzx_ok) {
      output.insert(output.end(), chunk.begin(), chunk.end());
    } else {
      // XCompress also stores incompressible chunks in their fixed slots. This is the
      // same fallback used by the read-only game-data study tool.
      const size_t raw_begin = i == 0 ? data_offset : i * chunk_size;
      if (raw_begin > input.size() || lengths[i] > input.size() - raw_begin) {
        error = "An XCompress chunk could not be decompressed or read as stored data.";
        return false;
      }
      output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(raw_begin),
                    input.begin() + static_cast<std::ptrdiff_t>(raw_begin + lengths[i]));
    }
    if (on_chunk) on_chunk(i + 1, chunk_count);
  }
  if (output.size() != output_size) {
    error = "The XCompress output size does not match its header.";
    return false;
  }
  return true;
}

bool DecodeBpe(const Bytes& input, Bytes& output, std::string& error,
               const CancelFlag* cancel = nullptr) {
  if (!HasMagic(input, "BPE ") || input.size() < 16) {
    error = "The BPE leaf header is invalid.";
    return false;
  }
  const uint32_t packed_size = Le32(input.data() + 8);
  const uint32_t decoded_size = Le32(input.data() + 12);
  if (packed_size > input.size() - 16 || decoded_size > kMaxDecodedBytes) {
    error = "The BPE leaf size is invalid.";
    return false;
  }
  const size_t end = 16 + packed_size;
  size_t cursor = 16;
  output.clear();
  output.reserve(decoded_size);
  while (cursor < end) {
    if (cancel && cancel->load(std::memory_order_relaxed)) {
      error = "Export cancelled.";
      return false;
    }
    std::array<uint8_t, 256> left{}, right{};
    for (size_t i = 0; i < 256; ++i) left[i] = static_cast<uint8_t>(i);
    unsigned symbol = 0;
    if (cursor >= end) break;
    unsigned count = input[cursor++];
    for (;;) {
      if (count > 127) {
        symbol += count - 127;
        count = 0;
      }
      if (symbol > 256) {
        error = "The BPE symbol table is invalid.";
        return false;
      }
      if (symbol == 256) break;
      for (unsigned i = 0; i <= count; ++i) {
        if (cursor >= end || symbol > 255) {
          error = "The BPE symbol table is truncated.";
          return false;
        }
        left[symbol] = input[cursor++];
        if (symbol != left[symbol]) {
          if (cursor >= end) {
            error = "The BPE symbol table is truncated.";
            return false;
          }
          right[symbol] = input[cursor++];
        }
        ++symbol;
      }
      if (symbol == 256) break;
      if (cursor >= end) {
        error = "The BPE symbol table is truncated.";
        return false;
      }
      count = input[cursor++];
    }
    if (cursor + 2 > end) {
      error = "The BPE block header is truncated.";
      return false;
    }
    const size_t block_size = uint16_t(input[cursor]) | (uint16_t(input[cursor + 1]) << 8);
    cursor += 2;
    if (block_size > end - cursor) {
      error = "The BPE block is truncated.";
      return false;
    }
    for (size_t i = 0; i < block_size; ++i) {
      if ((i & 0xFFF) == 0 && cancel && cancel->load(std::memory_order_relaxed)) {
        error = "Export cancelled.";
        return false;
      }
      std::array<uint8_t, 512> stack{};
      size_t stack_size = 1;
      uint64_t expansion_steps = 0;
      const uint64_t max_expansion_steps = std::min<uint64_t>((uint64_t(decoded_size) + 1) * 512 + 1000000,
                                                              100000000);
      stack[0] = input[cursor + i];
      while (stack_size) {
        if (++expansion_steps > max_expansion_steps) {
          error = "The BPE symbol table contains a cycle.";
          return false;
        }
        const uint8_t symbol_value = stack[--stack_size];
        if (left[symbol_value] == symbol_value) {
          if (output.size() >= decoded_size) {
            error = "The BPE output is larger than its header.";
            return false;
          }
          output.push_back(symbol_value);
        } else {
          if (stack_size > 510) {
            error = "The BPE symbol tree is too deep.";
            return false;
          }
          stack[stack_size++] = right[symbol_value];
          stack[stack_size++] = left[symbol_value];
        }
      }
    }
    cursor += block_size;
  }
  if (output.size() != decoded_size) {
    error = "The BPE output size does not match its header.";
    return false;
  }
  return true;
}

bool DecodeZlibLeaf(const Bytes& input, Bytes& output, std::string& error) {
  if (!HasMagic(input, "ZLIB") || input.size() < 16) {
    error = "The string table compression header is invalid.";
    return false;
  }
  const uint32_t packed_size = Le32(input.data() + 8);
  const uint32_t decoded_size = Le32(input.data() + 12);
  if (packed_size > input.size() - 16 || decoded_size > 32 * 1024 * 1024) {
    error = "The string table compression sizes are invalid.";
    return false;
  }
  output.resize(decoded_size);
  wwe_mz_ulong output_size = decoded_size;
  const int result = mz_uncompress(output.data(), &output_size, input.data() + 16, packed_size);
  // The retail string PAC includes an 8-byte empty SDB placeholder whose deflate stream omits
  // its Adler trailer. Python's game-file decoder accepts the partial stream; recognize only
  // that exact zero-table payload and leave all other malformed zlib leaves as errors.
  static constexpr uint8_t kEmptySdbPartialStream[] = {0x78, 0xda, 0x63, 0x60, 0x80, 0x00, 0x00, 0x00};
  if (result != kWweMzOk && decoded_size == 8 && packed_size == sizeof(kEmptySdbPartialStream) &&
      input.size() == 16 + sizeof(kEmptySdbPartialStream) &&
      std::equal(std::begin(kEmptySdbPartialStream), std::end(kEmptySdbPartialStream), input.begin() + 16)) {
    output.assign(8, 0);
    return true;
  }
  if (result != kWweMzOk || output_size != decoded_size) {
    error = "A string table could not be decompressed.";
    output.clear();
    return false;
  }
  return true;
}

struct PacEntry {
  std::string group;
  std::string name;
  Bytes data;
};

bool ParseEpac(const Bytes& bytes, std::vector<PacEntry>& entries, std::string& error) {
  const bool epac = HasMagic(bytes, "EPAC");
  const bool epk8 = HasMagic(bytes, "EPK8");
  if ((!epac && !epk8) || bytes.size() < 0x4000) {
    error = "The PAC directory header is invalid.";
    return false;
  }
  entries.clear();
  size_t directory = 0x800;
  while (directory + 12 <= 0x4000) {
    if (bytes[directory] == 0 && bytes[directory + 1] == 0 && bytes[directory + 2] == 0 &&
        bytes[directory + 3] == 0)
      break;
    const uint32_t word = Le32(bytes.data() + directory + 4);
    const bool wide_names = epk8 || ((word >> 16) & 1) != 0;
    const size_t record_size = wide_names ? 16 : 12;
    const size_t name_size = wide_names ? 8 : 4;
    const size_t words_per_entry = record_size / 4;
    const size_t word_count = word & 0xFFFF;
    if (word_count % words_per_entry != 0) {
      error = "The PAC directory has an invalid entry count.";
      return false;
    }
    const size_t count = word_count / words_per_entry;
    if (count > kMaxArchiveEntries || entries.size() + count > kMaxArchiveEntries) {
      error = "The PAC directory has too many entries.";
      return false;
    }
    const std::string group = TrimField(bytes.data() + directory, 4);
    directory += 12;
    if (count > (0x4000 - directory) / record_size) {
      error = "The PAC directory is truncated.";
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* record = bytes.data() + directory;
      const std::string name = TrimField(record, name_size);
      const uint32_t sector = Le32(record + name_size);
      const uint32_t size_units = Le32(record + name_size + 4);
      const uint64_t offset = 0x4000ull + uint64_t(sector) * 0x800ull;
      const uint64_t size = uint64_t(size_units) * 0x100ull;
      if (offset > bytes.size() || size > bytes.size() - offset) {
        error = "A PAC entry points outside its archive.";
        return false;
      }
      PacEntry entry;
      entry.group = group;
      entry.name = name;
      entry.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
      entries.push_back(std::move(entry));
      directory += record_size;
    }
  }
  return !entries.empty();
}

struct PachLeaf {
  uint32_t id = 0;
  Bytes data;
};

bool ParsePach(const Bytes& bytes, std::vector<PachLeaf>& leaves, std::string& error) {
  if (!HasMagic(bytes, "PACH") || bytes.size() < 8) {
    error = "The character PACH header is invalid.";
    return false;
  }
  const uint32_t count = Le32(bytes.data() + 4);
  if (count > kMaxArchiveEntries || uint64_t(count) * 12 + 8 > bytes.size()) {
    error = "The character PACH directory is invalid.";
    return false;
  }
  const size_t base = 8 + size_t(count) * 12;
  leaves.clear();
  leaves.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t record = 8 + size_t(i) * 12;
    const uint32_t id = Le32(bytes.data() + record);
    const uint32_t relative = Le32(bytes.data() + record + 4);
    const uint32_t size = Le32(bytes.data() + record + 8);
    if (relative > bytes.size() - base || size > bytes.size() - base - relative) {
      error = "A character leaf points outside its PACH archive.";
      return false;
    }
    PachLeaf leaf;
    leaf.id = id;
    const size_t begin = base + relative;
    leaf.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                     bytes.begin() + static_cast<std::ptrdiff_t>(begin + size));
    leaves.push_back(std::move(leaf));
  }
  return true;
}

bool ParseSdbTable(const Bytes& plain, std::map<uint32_t, std::string>& names) {
  if (plain.size() < 20 || Le32(plain.data()) != 0) return false;
  const uint32_t count = Le32(plain.data() + 4);
  if (count == 0 || count > kMaxArchiveEntries || uint64_t(count) * 12 + 8 > plain.size()) return false;
  const size_t table_end = 8 + size_t(count) * 12;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = 8 + size_t(i) * 12;
    const uint32_t offset = Le32(plain.data() + at);
    const uint32_t length = Le32(plain.data() + at + 4);
    const uint32_t key = Le32(plain.data() + at + 8);
    if (offset < table_end || offset > plain.size() || length > plain.size() - offset) return false;
    if (key < 60100 || key >= 60500) continue;
    std::string name = TrimField(plain.data() + offset, length);
    if (name.empty()) continue;
    names.try_emplace(key - 60000, IsValidUtf8(name) ? name : Latin1ToUtf8(name));
  }
  return true;
}

bool VisitStringContainer(const Bytes& input, unsigned depth, std::map<uint32_t, std::string>& names,
                          std::string& error) {
  if (depth > 8) return true;
  if (HasMagic(input, "\x0F\xF5\x12\xED")) {
    Bytes decoded;
    if (!DecodeXCompress(input, decoded, error)) return false;
    return VisitStringContainer(decoded, depth + 1, names, error);
  }
  if (HasMagic(input, "EPAC") || HasMagic(input, "EPK8")) {
    std::vector<PacEntry> entries;
    if (!ParseEpac(input, entries, error)) return false;
    for (const PacEntry& entry : entries)
      if (!VisitStringContainer(entry.data, depth + 1, names, error)) return false;
    return true;
  }
  if (HasMagic(input, "PACH")) {
    std::vector<PachLeaf> leaves;
    if (!ParsePach(input, leaves, error)) return false;
    for (const PachLeaf& leaf : leaves)
      if (!VisitStringContainer(leaf.data, depth + 1, names, error)) return false;
    return true;
  }
  if (HasMagic(input, "BPE ")) {
    Bytes decoded;
    if (!DecodeBpe(input, decoded, error)) return false;
    return VisitStringContainer(decoded, depth + 1, names, error);
  }
  if (HasMagic(input, "ZLIB")) {
    Bytes decoded;
    if (!DecodeZlibLeaf(input, decoded, error)) return false;
    ParseSdbTable(decoded, names);
    return true;
  }
  ParseSdbTable(input, names);
  return true;
}

struct WrestlerSource {
  uint32_t id = 0;
  std::string name;
  fs::path file;
};

fs::path PacRoot(const fs::path& game_folder) {
  if (LowerAscii(internal::PathToUtf8(game_folder.filename())) == "pac") return game_folder;
  return game_folder / "pac";
}

bool ParseCharacterFileName(const fs::path& path, uint32_t& id, int& dlc_number) {
  if (LowerAscii(internal::PathToUtf8(path.extension())) != ".pac") return false;
  const std::string stem = LowerAscii(internal::PathToUtf8(path.stem()));
  if (!stem.starts_with("ch")) return false;
  size_t digits_end = 2;
  while (digits_end < stem.size() && stem[digits_end] >= '0' && stem[digits_end] <= '9') ++digits_end;
  if (digits_end == 2) return false;
  uint32_t parsed = 0;
  const char* begin = stem.data() + 2;
  const char* end = stem.data() + digits_end;
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed < 100 || parsed > 499) return false;
  dlc_number = -1;
  if (digits_end < stem.size()) {
    const std::string suffix = stem.substr(digits_end);
    if (!suffix.starts_with("_dlc") || suffix.size() != 6 || !std::isdigit(static_cast<unsigned char>(suffix[4])) ||
        !std::isdigit(static_cast<unsigned char>(suffix[5])) || suffix.substr(4) == "00")
      return false;
    dlc_number = (suffix[4] - '0') * 10 + (suffix[5] - '0');
  }
  id = parsed;
  return true;
}

bool IsStringPac(const fs::path& path) {
  if (LowerAscii(internal::PathToUtf8(path.extension())) != ".pac") return false;
  const std::string stem = LowerAscii(internal::PathToUtf8(path.stem()));
  if (stem == "string") return true;
  if (!stem.starts_with("string_dlc") || stem.size() != 12) return false;
  return std::isdigit(static_cast<unsigned char>(stem[10])) != 0 &&
         std::isdigit(static_cast<unsigned char>(stem[11])) != 0;
}

bool IsBaseStringPac(const fs::path& path) {
  return LowerAscii(internal::PathToUtf8(path.stem())) == "string";
}

std::vector<fs::path> FilesInPacTree(const fs::path& pac_root) {
  std::vector<fs::path> files;
  std::error_code error;
  const fs::path ch_folder = pac_root / "ch";
  if (!fs::is_directory(pac_root, error)) return files;
  error.clear();
  for (fs::recursive_directory_iterator it(pac_root, fs::directory_options::skip_permission_denied, error), end;
       !error && it != end; it.increment(error)) {
    if (!it->is_regular_file(error)) {
      error.clear();
      continue;
    }
    const fs::path candidate = it->path();
    if (IsStringPac(candidate) || (candidate.parent_path() == ch_folder &&
                                   LowerAscii(internal::PathToUtf8(candidate.extension())) == ".pac"))
      files.push_back(candidate);
  }
  return files;
}

std::map<uint32_t, WrestlerSource> BuildWrestlerSources(const std::vector<fs::path>& game_folders,
                                                         std::map<uint32_t, std::string>& names,
                                                         std::string* error_out = nullptr) {
  std::map<uint32_t, WrestlerSource> sources;
  names.clear();
  std::string local_error;
  for (const fs::path& game_folder : game_folders) {
    const fs::path pac_root = PacRoot(game_folder);
    std::vector<fs::path> files = FilesInPacTree(pac_root);
    std::vector<fs::path> string_files, character_files;
    for (const fs::path& file : files) {
      if (IsStringPac(file)) string_files.push_back(file);
      else character_files.push_back(file);
    }
    std::sort(string_files.begin(), string_files.end(), [](const fs::path& a, const fs::path& b) {
      const bool a_base = IsBaseStringPac(a), b_base = IsBaseStringPac(b);
      if (a_base != b_base) return a_base;
      return LowerAscii(internal::PathToUtf8(a.filename())) < LowerAscii(internal::PathToUtf8(b.filename()));
    });
    for (const fs::path& file : string_files) {
      Bytes raw;
      if (!ReadFile(file, raw, local_error)) continue;
      if (!VisitStringContainer(raw, 0, names, local_error)) {
        // One damaged optional DLC string pack must not hide names from the other installed packs.
        local_error.clear();
      }
    }
    struct Candidate {
      uint32_t id;
      int dlc;
      fs::path file;
    };
    std::vector<Candidate> candidates;
    for (const fs::path& file : character_files) {
      uint32_t id = 0;
      int dlc = -1;
      if (ParseCharacterFileName(file, id, dlc)) candidates.push_back({id, dlc, file});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
      if (a.id != b.id) return a.id < b.id;
      if ((a.dlc < 0) != (b.dlc < 0)) return a.dlc < 0;
      return a.dlc > b.dlc;
    });
    for (const Candidate& candidate : candidates) {
      if (sources.contains(candidate.id)) continue;
      sources.emplace(candidate.id, WrestlerSource{candidate.id, {}, candidate.file});
    }
  }
  for (auto it = sources.begin(); it != sources.end();) {
    const auto name = names.find(it->first);
    if (name == names.end()) {
      it = sources.erase(it);
    } else {
      it->second.name = name->second;
      ++it;
    }
  }
  if (error_out) *error_out = std::move(local_error);
  return sources;
}

std::string JoinPrefix(std::string prefix, std::string_view part) {
  const std::string clean = SafeToken(part);
  if (!prefix.empty()) prefix += '_';
  prefix += clean;
  return prefix;
}

bool IsTextureLeaf(uint32_t id) {
  return id == 10 || id == 12 || (id >= 13 && id <= 16);
}

struct ExportFiles {
  std::vector<std::string> leaves;
  std::vector<std::string> textures;
};

bool ExtractDdsBundle(const Bytes& bundle, const std::string& prefix, uint32_t leaf_id,
                      const fs::path& texture_folder, ExportFiles& outputs, std::string& error) {
  if (bundle.size() < 16) {
    error = "A wrestler texture bundle is truncated.";
    return false;
  }
  const uint32_t count = Le32(bundle.data());
  if (count == 0 || count > 4096 || uint64_t(count) * 32 + 16 > bundle.size()) {
    error = "A wrestler texture bundle has an invalid entry table.";
    return false;
  }
  const size_t table_end = 16 + size_t(count) * 32;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t record = 16 + size_t(i) * 32;
    const std::string texture_name = TrimField(bundle.data() + record, 16);
    const std::string format = LowerAscii(TrimField(bundle.data() + record + 16, 4));
    const uint32_t size = Le32(bundle.data() + record + 20);
    const uint32_t offset = Le32(bundle.data() + record + 24);
    if (format != "dds" || size < 128 || offset < table_end || offset > bundle.size() || size > bundle.size() - offset ||
        std::memcmp(bundle.data() + offset, "DDS ", 4) != 0 || Le32(bundle.data() + offset + 4) != 124) {
      error = "A wrestler texture bundle contains an unsupported or invalid DDS texture.";
      return false;
    }
    Bytes dds(bundle.begin() + static_cast<std::ptrdiff_t>(offset),
              bundle.begin() + static_cast<std::ptrdiff_t>(offset + size));
    const std::string filename = prefix + "_leaf-" + std::to_string(leaf_id) + "-" +
                                 SafeToken(texture_name.empty() ? "texture" : texture_name) + ".dds";
    if (!WriteFile(texture_folder / filename, dds, error)) return false;
    outputs.textures.push_back("textures/" + filename);
  }
  return true;
}

bool WriteCharacterLeaves(const Bytes& input, const std::string& prefix, const fs::path& root,
                          ExportFiles& outputs, size_t& leaf_count, const CancelFlag& cancel,
                          std::string& error, unsigned depth = 0) {
  if (cancel.load(std::memory_order_relaxed)) {
    error = "Export cancelled.";
    return false;
  }
  if (depth > 8) return true;
  if (HasMagic(input, "EPAC") || HasMagic(input, "EPK8")) {
    std::vector<PacEntry> entries;
    if (!ParseEpac(input, entries, error)) return false;
    for (const PacEntry& entry : entries) {
      const std::string child = JoinPrefix(JoinPrefix(prefix, entry.group), entry.name);
      if (!WriteCharacterLeaves(entry.data, child, root, outputs, leaf_count, cancel, error, depth + 1)) return false;
    }
    return true;
  }
  if (HasMagic(input, "PACH")) {
    std::vector<PachLeaf> leaves;
    if (!ParsePach(input, leaves, error)) return false;
    for (const PachLeaf& leaf : leaves) {
      if (cancel.load(std::memory_order_relaxed)) {
        error = "Export cancelled.";
        return false;
      }
      Bytes raw;
      if (HasMagic(leaf.data, "BPE ")) {
        if (!DecodeBpe(leaf.data, raw, error, &cancel)) return false;
      } else {
        raw = leaf.data;
      }
      const std::string filename = prefix + "_leaf-" + std::to_string(leaf.id) + ".bin";
      if (!WriteFile(root / "leaves" / filename, raw, error)) return false;
      outputs.leaves.push_back("leaves/" + filename);
      ++leaf_count;
      if (IsTextureLeaf(leaf.id) && !ExtractDdsBundle(raw, prefix, leaf.id, root / "textures", outputs, error))
        return false;
    }
    return true;
  }
  if (HasMagic(input, "BPE ")) {
    Bytes decoded;
    if (!DecodeBpe(input, decoded, error, &cancel)) return false;
    if (HasMagic(decoded, "EPAC") || HasMagic(decoded, "EPK8") || HasMagic(decoded, "PACH"))
      return WriteCharacterLeaves(decoded, prefix, root, outputs, leaf_count, cancel, error, depth + 1);
  }
  return true;
}

bool WriteText(const fs::path& path, const std::string& text, std::string& error) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    error = "An export manifest could not be created.";
    return false;
  }
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  file.flush();
  if (!file) {
    error = "An export manifest could not be written.";
    return false;
  }
  return true;
}

}  // namespace

namespace internal {

bool DecodeWweXCompress(const std::vector<uint8_t>& input, std::vector<uint8_t>& output,
                        std::string& error) {
  return DecodeXCompress(input, output, error);
}

bool DecodeWweBpe(const std::vector<uint8_t>& input, std::vector<uint8_t>& output,
                  std::string& error) {
  return DecodeBpe(input, output, error);
}

}  // namespace internal

std::vector<StockWrestler> LoadStockWrestlers(const std::vector<fs::path>& game_folders) {
  std::map<uint32_t, std::string> names;
  std::map<uint32_t, WrestlerSource> sources = BuildWrestlerSources(game_folders, names);
  std::vector<StockWrestler> wrestlers;
  wrestlers.reserve(sources.size());
  for (const auto& [id, source] : sources) wrestlers.push_back({id, source.name, "WWE 2K14"});
  std::sort(wrestlers.begin(), wrestlers.end(), [](const StockWrestler& a, const StockWrestler& b) {
    const std::string a_name = LowerAscii(a.name), b_name = LowerAscii(b.name);
    return a_name == b_name ? a.id < b.id : a_name < b_name;
  });
  return wrestlers;
}

Result ExportStockWrestlers(const std::vector<fs::path>& game_folders, const std::vector<uint32_t>& ids,
                            const fs::path& output_folder, const ProgressFn& progress,
                            const CancelFlag& cancel) {
  if (ids.empty()) return Result::Fail("Select at least one WWE 2K14 wrestler to export.");
  if (ids.size() > 300) return Result::Fail("Select fewer wrestlers and try the export again.");
  try {
    std::map<uint32_t, std::string> names;
    const std::map<uint32_t, WrestlerSource> available = BuildWrestlerSources(game_folders, names);
    std::vector<WrestlerSource> selected;
    std::set<uint32_t> seen;
    for (const uint32_t id : ids) {
      if (!seen.insert(id).second) continue;
      const auto source = available.find(id);
      if (source == available.end()) return Result::Fail("A selected wrestler is no longer available in the game folder.");
      selected.push_back(source->second);
    }
    if (selected.empty()) return Result::Fail("Select at least one WWE 2K14 wrestler to export.");
    std::sort(selected.begin(), selected.end(), [](const WrestlerSource& a, const WrestlerSource& b) {
      return a.id < b.id;
    });
    if (cancel.load(std::memory_order_relaxed)) return Result::Fail("Export cancelled.");
    std::error_code ec;
    const bool output_exists = fs::exists(output_folder, ec);
    if (ec) return Result::Fail("Choose a folder that the launcher can write to.");
    if (output_exists) {
      if (!fs::is_directory(output_folder, ec) || ec)
        return Result::Fail("Choose a folder that the launcher can write to.");
    } else if (!fs::create_directories(output_folder, ec) || ec) {
      return Result::Fail("Choose a folder that the launcher can write to.");
    }
    std::vector<std::string> folder_names;
    uint64_t total_bytes = 0;
    for (const WrestlerSource& wrestler : selected) {
      const std::string folder_name = SafeToken(wrestler.name) + " [" + std::to_string(wrestler.id) + "]";
      folder_names.push_back(folder_name);
      if (fs::exists(output_folder / folder_name, ec))
        return Result::Fail("The destination already contains an export for " + wrestler.name + ". Choose another folder.");
      ec.clear();
      const uint64_t size = fs::file_size(wrestler.file, ec);
      if (!ec) total_bytes += size;
      ec.clear();
    }
    if (cancel.load(std::memory_order_relaxed)) return Result::Fail("Export cancelled.");
    static std::atomic<uint64_t> stage_sequence{0};
    fs::path stage = output_folder / (".wwe2k14-export-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(stage_sequence.fetch_add(1, std::memory_order_relaxed)) + ".partial");
    if (!fs::create_directory(stage, ec) || ec) return Result::Fail("A temporary export folder could not be created.");
    std::vector<fs::path> committed;
    uint64_t completed_bytes = 0;
    for (size_t i = 0; i < selected.size(); ++i) {
      if (cancel.load(std::memory_order_relaxed)) {
        fs::remove_all(stage, ec);
        return Result::Fail("Export cancelled.");
      }
      const WrestlerSource& wrestler = selected[i];
      const fs::path wrestler_folder = stage / folder_names[i];
      if (!fs::create_directories(wrestler_folder / "leaves", ec) || ec ||
          !fs::create_directories(wrestler_folder / "textures", ec) || ec) {
        fs::remove_all(stage, ec);
        return Result::Fail("The export folder could not be prepared. Check free space and permissions.");
      }
      if (progress) progress(completed_bytes, total_bytes, "Reading " + wrestler.name + "…");
      Bytes input;
      std::string error;
      if (!ReadFile(wrestler.file, input, error)) {
        fs::remove_all(stage, ec);
        return Result::Fail(error);
      }
      const uint64_t source_size = input.size();
      if (HasMagic(input, "\x0F\xF5\x12\xED")) {
        Bytes decoded;
        const bool ok = DecodeXCompress(input, decoded, error, &cancel, [&](size_t chunk, size_t chunks) {
          if (progress && chunks)
            progress(completed_bytes + source_size * chunk / chunks, total_bytes,
                     "Reading " + wrestler.name + "…");
        });
        if (!ok) {
          fs::remove_all(stage, ec);
          return Result::Fail(error);
        }
        input = std::move(decoded);
      }
      if (!HasMagic(input, "EPAC") && !HasMagic(input, "EPK8")) {
        fs::remove_all(stage, ec);
        return Result::Fail("A selected wrestler's PAC file has an unsupported format.");
      }
      ExportFiles exported;
      size_t leaf_count = 0;
      if (!WriteCharacterLeaves(input, {}, wrestler_folder, exported, leaf_count, cancel, error)) {
        fs::remove_all(stage, ec);
        return Result::Fail(error);
      }
      if (leaf_count == 0) {
        fs::remove_all(stage, ec);
        return Result::Fail("No model leaves were found for " + wrestler.name + ".");
      }
      std::ostringstream name_text;
      name_text << "Name: " << wrestler.name << "\nID: " << wrestler.id << "\nGame: WWE 2K14\n";
      if (!WriteText(wrestler_folder / "name.txt", name_text.str(), error)) {
        fs::remove_all(stage, ec);
        return Result::Fail(error);
      }
      std::ostringstream manifest;
      manifest << "format\tWWE2K14 raw export v1\n"
               << "name\t" << wrestler.name << "\n"
               << "id\t" << wrestler.id << "\n"
               << "game\tWWE 2K14\n"
               << "source file\t" << internal::PathToUtf8(wrestler.file.filename()) << "\n"
               << "\nleaves\n";
      for (const std::string& path : exported.leaves) manifest << path << '\n';
      manifest << "\ntextures\n";
      for (const std::string& path : exported.textures) manifest << path << '\n';
      if (!WriteText(wrestler_folder / "manifest.tsv", manifest.str(), error)) {
        fs::remove_all(stage, ec);
        return Result::Fail(error);
      }
      completed_bytes += source_size;
      if (progress) progress(completed_bytes, total_bytes, "Prepared " + wrestler.name + "");
    }
    if (cancel.load(std::memory_order_relaxed)) {
      fs::remove_all(stage, ec);
      return Result::Fail("Export cancelled.");
    }
    for (size_t i = 0; i < selected.size(); ++i) {
      const fs::path staged = stage / folder_names[i];
      const fs::path destination = output_folder / folder_names[i];
      fs::rename(staged, destination, ec);
      if (ec) {
        for (const fs::path& path : committed) fs::remove_all(path, ec);
        fs::remove_all(stage, ec);
        return Result::Fail("An export folder could not be moved into the destination. Check its permissions.");
      }
      committed.push_back(destination);
    }
    fs::remove(stage, ec);
    if (progress) progress(total_bytes, total_bytes, "Export complete");
    return Result::Ok();
  } catch (const std::exception& exception) {
    return Result::Fail(std::string("The export could not be completed: ") + exception.what());
  } catch (...) {
    return Result::Fail("The export could not be completed.");
  }
}

}  // namespace wwe2k14::launcher
