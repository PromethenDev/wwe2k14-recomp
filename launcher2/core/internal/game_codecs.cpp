#include "internal/game_codecs.h"
#include "launcher_core.h"

#include <lzx.h>
#include <mspack.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

namespace wwe2k14::launcher {
namespace {
using Bytes = std::vector<uint8_t>;
constexpr uint64_t kMaxDecodedBytes = 512ull * 1024 * 1024;
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
}  // namespace wwe2k14::launcher
