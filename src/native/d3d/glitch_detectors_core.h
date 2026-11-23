// Pure helpers for the sampled T2 glitch detectors. Kept separate for host-only tests.
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <system_error>
#include <unordered_set>

namespace wwe2k14::d3d::glitch::core {

inline uint32_t ParseFrameInterval(const char* value) {
  if (!value || !*value) return 0;
  uint32_t interval = 0;
  const char* end = value + std::strlen(value);
  const auto parsed = std::from_chars(value, end, interval, 10);
  return parsed.ec == std::errc{} && parsed.ptr == end ? interval : 0;
}

inline bool IsSampleFrame(uint32_t frame, uint32_t interval) {
  return interval != 0 && frame % interval == 0;
}

inline bool IsNonFiniteFloatBits(uint32_t bits) {
  return (bits & 0x7F800000u) == 0x7F800000u;
}

inline size_t FirstNonFiniteFloat(const void* bytes, size_t byte_count, uint32_t* bits_out = nullptr) {
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i + sizeof(uint32_t) <= byte_count; i += sizeof(uint32_t)) {
    uint32_t bits = 0;
    std::memcpy(&bits, data + i, sizeof(bits));
    if (IsNonFiniteFloatBits(bits)) {
      if (bits_out) *bits_out = bits;
      return i / sizeof(uint32_t);
    }
  }
  return byte_count / sizeof(uint32_t);
}

inline bool IsFloatConstantUsed(const uint64_t* usage_bitmap, size_t index) {
  return usage_bitmap && index < 256 && (usage_bitmap[index / 64] & (uint64_t(1) << (index % 64))) != 0;
}

// Visit the first non-finite component in each used float4 constant. The shader's bitmap marks registers, not
// components, so inspect all four words once a register is known to be read.
template <typename F>
inline void ForEachNonFiniteUsedFloatConstant(const void* bytes, size_t byte_count, const uint64_t* usage_bitmap,
                                              F&& callback) {
  if (!bytes || !usage_bitmap) return;
  const auto* data = static_cast<const uint8_t*>(bytes);
  const size_t register_count = byte_count / (4 * sizeof(uint32_t));
  for (size_t reg = 0; reg < register_count && reg < 256; ++reg) {
    if (!IsFloatConstantUsed(usage_bitmap, reg)) continue;
    for (size_t component = 0; component < 4; ++component) {
      uint32_t bits = 0;
      const size_t offset = (reg * 4 + component) * sizeof(uint32_t);
      std::memcpy(&bits, data + offset, sizeof(bits));
      if (!IsNonFiniteFloatBits(bits)) continue;
      callback(static_cast<uint32_t>(reg), static_cast<uint32_t>(component), bits);
      break;
    }
  }
}

struct UniqueKey {
  uint8_t kind = 0;
  uint64_t a = 0, b = 0, c = 0, d = 0;
  bool operator==(const UniqueKey& other) const {
    return kind == other.kind && a == other.a && b == other.b && c == other.c && d == other.d;
  }
};

inline UniqueKey NonFiniteConstantKey(uint8_t kind, uint64_t vs_key, uint64_t ps_key, uint8_t stage, uint32_t index) {
  return {kind, vs_key, ps_key, (uint64_t(stage) << 32) | index, 0};
}

struct UniqueKeyHash {
  size_t operator()(const UniqueKey& key) const {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t v : {uint64_t(key.kind), key.a, key.b, key.c, key.d}) {
      for (unsigned i = 0; i < 8; ++i) {
        h = (h ^ uint8_t(v)) * 1099511628211ull;
        v >>= 8;
      }
    }
    return static_cast<size_t>(h);
  }
};

class UniqueKeys {
 public:
  explicit UniqueKeys(size_t limit) : limit_(limit) {}
  bool Insert(const UniqueKey& key) {
    if (keys_.count(key)) return false;
    if (keys_.size() >= limit_) return false;
    keys_.insert(key);
    return true;
  }
  size_t size() const { return keys_.size(); }

 private:
  size_t limit_;
  std::unordered_set<UniqueKey, UniqueKeyHash> keys_;
};

}  // namespace wwe2k14::d3d::glitch::core
