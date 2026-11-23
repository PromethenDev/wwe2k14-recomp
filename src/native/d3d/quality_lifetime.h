// Q1tex R2: CPU-only lifetime registry for sampled-texture quality metadata.
//
// The registry is keyed by the backend texture handle. Handles are owned by the
// texture layer and must be erased in BackendRelease before the backend can
// reuse them; otherwise a later texture with a reused id would inherit the old
// residency facts. The live set is bounded, and overflow is sticky: once a
// new-handle insertion fails, every later new handle fails too (updates to
// stored handles still work). The caller therefore fails closed globally on
// overflowed(). Records already stored stay inspectable through Find/records(),
// but being inspectable does not authorize sampling.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "sampling_quality_core.h"

namespace wwe2k14::d3d::quality {

struct QualityUploadRecord {
  SamplingQualityInfo info;
  std::array<uint32_t, 4> range{};
};

class QualityUploadRegistry {
 public:
  static constexpr std::size_t kCapacity = 8192;

  // Inserts a new handle or replaces the record of an existing handle.
  // Existing handles can be updated at capacity. A new handle at capacity
  // fails and makes overflow sticky; no entry is evicted.
  bool Put(uint32_t handle, const QualityUploadRecord& record) {
    auto it = records_.find(handle);
    if (it != records_.end()) {
      it->second = record;
      return true;
    }
    if (overflow_ || records_.size() >= kCapacity) {
      overflow_ = true;
      return false;
    }
    records_.emplace(handle, record);
    if (records_.size() > peak_) peak_ = records_.size();
    return true;
  }

  const QualityUploadRecord* Find(uint32_t handle) const {
    const auto it = records_.find(handle);
    return it == records_.end() ? nullptr : &it->second;
  }

  bool Erase(uint32_t handle) { return records_.erase(handle) != 0; }

  std::size_t size() const { return records_.size(); }
  std::size_t peak() const { return peak_; }
  bool overflowed() const { return overflow_; }

  // Read-only iteration for diagnostics (for example, orphaned handles).
  const std::unordered_map<uint32_t, QualityUploadRecord>& records() const { return records_; }

 private:
  std::unordered_map<uint32_t, QualityUploadRecord> records_;
  std::size_t peak_ = 0;
  bool overflow_ = false;
};

// Diagnostics-only classification of the residency facts, shared with the
// host tests. Unknown metadata is never treated as a single-level texture.
enum class MetadataLevelFacts : uint8_t { kUnknown, kSingleLevel, kMultiLevel };

inline MetadataLevelFacts ClassifyMetadataLevels(const SamplingQualityInfo& info) noexcept {
  // Zero recorded complete levels is not evidence of one actual level.
  if (!info.known || info.resident_levels == 0) return MetadataLevelFacts::kUnknown;
  return info.resident_levels > 1 ? MetadataLevelFacts::kMultiLevel : MetadataLevelFacts::kSingleLevel;
}

enum class FetchLevelFacts : uint8_t { kInvalid, kSingleLevel, kMultiLevel };

inline FetchLevelFacts ClassifyFetchLevels(bool valid, uint32_t claimed_levels) noexcept {
  if (!valid || claimed_levels == 0) return FetchLevelFacts::kInvalid;
  return claimed_levels > 1 ? FetchLevelFacts::kMultiLevel : FetchLevelFacts::kSingleLevel;
}

}  // namespace wwe2k14::d3d::quality
