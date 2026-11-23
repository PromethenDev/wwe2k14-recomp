#pragma once

// Q1b: pure, host-testable sampling-quality core.
//
// This header deliberately contains no game hooks, D3D types, backend types or
// engine state. It only proves properties of raw SPIR-V bytes provided by the
// caller, decides whether a texture may receive a conservative min-LOD clamp,
// tracks texture ranges that CPU write watches have touched, and records guest
// pages covered by resolves.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wwe2k14::d3d::quality {

// Exact-match mode parser: only "1" and "2" select a non-zero mode.
inline uint8_t ParseMode(const char* text) noexcept {
  if (text == nullptr || text[0] == '\0' || text[1] != '\0') {
    return 0;
  }
  if (text[0] == '1') {
    return 1;
  }
  if (text[0] == '2') {
    return 2;
  }
  return 0;
}

namespace detail {

inline uint32_t LoadLe32(const uint8_t* bytes) noexcept {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8u) |
         (static_cast<uint32_t>(bytes[2]) << 16u) |
         (static_cast<uint32_t>(bytes[3]) << 24u);
}

// True for core opcodes this proof recognises as non-image or as one of the
// explicitly handled image forms. Holes are unknown/reserved opcodes.
inline bool IsKnownCoreOpcode(uint32_t opcode) noexcept {
  return opcode <= 8u ||
         (opcode >= 10u && opcode <= 12u) ||
         (opcode >= 14u && opcode <= 17u) ||
         (opcode >= 19u && opcode <= 39u) ||
         (opcode >= 41u && opcode <= 46u) ||
         (opcode >= 48u && opcode <= 52u) ||
         (opcode >= 54u && opcode <= 57u) ||
         (opcode >= 59u && opcode <= 107u) ||
         (opcode >= 109u && opcode <= 124u) ||
         (opcode >= 126u && opcode <= 152u) ||
         (opcode >= 154u && opcode <= 205u) ||
         (opcode >= 207u && opcode <= 215u) ||
         (opcode >= 218u && opcode <= 221u) ||
         (opcode >= 224u && opcode <= 225u) ||
         (opcode >= 227u && opcode <= 242u) ||
         (opcode >= 245u && opcode <= 257u) ||
         (opcode >= 259u && opcode <= 304u) ||
         (opcode >= 305u && opcode <= 320u) ||
         (opcode >= 321u && opcode <= 366u) ||
         (opcode >= 400u && opcode <= 403u);
}

}  // namespace detail

// Reasons a sampling proof did not pass. `parsed` and `safe` are separate:
// a well-formed module can fail because its fetch form is not proven safe.
inline constexpr uint32_t kProofMalformed = 1u << 0;
inline constexpr uint32_t kProofUnsupportedHeader = 1u << 1;
inline constexpr uint32_t kProofEntryPoint = 1u << 2;
inline constexpr uint32_t kProofWrongStage = 1u << 3;
inline constexpr uint32_t kProofNoSamples = 1u << 4;
inline constexpr uint32_t kProofExplicitLod = 1u << 5;
inline constexpr uint32_t kProofBadExplicitForm = 1u << 6;
inline constexpr uint32_t kProofForbiddenImageOp = 1u << 7;
inline constexpr uint32_t kProofSparseImageOp = 1u << 8;
inline constexpr uint32_t kProofUnknownOpcode = 1u << 9;
inline constexpr uint32_t kProofBadImageForm = 1u << 10;
inline constexpr uint32_t kProofImageOperands = 1u << 11;

struct SamplingProof {
  bool parsed = false;
  bool safe = false;
  uint32_t samples = 0;
  uint32_t reasons = 0;
};

// A conservative sampling proof over raw SPIR-V bytes.
//
// It validates the header, instruction lengths, one single-stage entry point,
// function/end framing, and the shape of image instructions. It rejects:
//   - OpImageFetch/Read/Gather/DrefGather/DrefRead/DrefFetch, sparse image
//     opcodes, and unknown/reserved/extension opcodes;
//   - OpImageSampleExplicitLod unless it has exactly the Grad form;
//   - OpImageSampleImplicitLod with Lod or Grad operands, and malformed
//     optional-operand words.
// A vertex shader may have no image sample; a fragment shader must have at
// least one. This is not a general SPIR-V validator: type/ID dataflow and
// decorations are outside its scope.
inline SamplingProof ProveSpirvSampling(const uint8_t* bytes, size_t size,
                                        bool vertex_stage) noexcept {
  SamplingProof proof;

  if (bytes == nullptr || size < 20u || (size % 4u) != 0u) {
    proof.reasons |= kProofMalformed;
    return proof;
  }

  const size_t word_count = size / 4u;
  const uint32_t magic = detail::LoadLe32(bytes);
  const uint32_t version = detail::LoadLe32(bytes + 4);
  const uint32_t generator = detail::LoadLe32(bytes + 8);
  const uint32_t bound = detail::LoadLe32(bytes + 12);
  const uint32_t reserved = detail::LoadLe32(bytes + 16);
  const uint32_t major = version >> 16u;
  const uint32_t minor = (version >> 8u) & 0xffu;

  (void)generator;  // Tool identity; any value is valid SPIR-V.
  if (magic != 0x07230203u || major != 1u || minor > 6u ||
      (version & 0xffu) != 0u || reserved != 0u || bound == 0u ||
      bound > (1u << 22u)) {
    proof.reasons |= kProofUnsupportedHeader;
    return proof;
  }

  bool in_function = false;
  bool saw_function = false;
  uint32_t entry_points = 0;
  uint32_t entry_stage = 0;
  uint32_t entry_id = 0;
  std::array<uint32_t, 8> arithmetic_sets{};
  uint32_t arithmetic_set_count = 0;

  size_t offset = 5u;
  while (offset < word_count) {
    const uint32_t first = detail::LoadLe32(bytes + offset * 4u);
    const uint32_t opcode = first & 0xffffu;
    const uint32_t words = first >> 16u;
    if (words == 0u || static_cast<size_t>(words) > word_count - offset) {
      proof.reasons |= kProofMalformed;
      return proof;
    }

    const uint8_t* operands = bytes + (offset + 1u) * 4u;

    if (!detail::IsKnownCoreOpcode(opcode)) {
      proof.reasons |= kProofUnknownOpcode;
    } else if (opcode == 11u) {  // OpExtInstImport
      // GLSL.std.450 contains arithmetic, not image operations. Unknown
      // extended instruction sets are not a sampling proof.
      constexpr char name[] = "GLSL.std.450";
      if (words < 3u || size_t(words - 2u) * 4u < sizeof(name) ||
          std::memcmp(operands + 4u, name, sizeof(name)) != 0 ||
          arithmetic_set_count == arithmetic_sets.size()) {
        proof.reasons |= kProofUnknownOpcode;
      } else {
        arithmetic_sets[arithmetic_set_count++] = detail::LoadLe32(operands);
      }
    } else if (opcode == 12u) {  // OpExtInst
      bool trusted = false;
      if (words >= 5u) {
        const uint32_t set = detail::LoadLe32(operands + 8u);
        for (uint32_t n = 0; n < arithmetic_set_count; ++n) trusted |= arithmetic_sets[n] == set;
      }
      if (!trusted) proof.reasons |= kProofUnknownOpcode;
    } else if (opcode == 15u) {  // OpEntryPoint
      if (in_function || words < 4u) {
        proof.reasons |= kProofMalformed;
        return proof;
      }
      const uint32_t stage = detail::LoadLe32(operands);
      const uint32_t id = detail::LoadLe32(operands + 4u);
      if (id == 0u || id >= bound) {
        proof.reasons |= kProofMalformed;
        return proof;
      }
      ++entry_points;
      if (entry_points == 1u) {
        entry_stage = stage;
        entry_id = id;
      }
    } else if (opcode == 54u) {  // OpFunction
      if (in_function || words != 5u) {
        proof.reasons |= kProofMalformed;
        return proof;
      }
      const uint32_t id = detail::LoadLe32(operands + 4u);
      if (id == 0u || id >= bound) {
        proof.reasons |= kProofMalformed;
        return proof;
      }
      in_function = true;
      saw_function = true;
    } else if (opcode == 56u) {  // OpFunctionEnd
      if (!in_function || words != 1u) {
        proof.reasons |= kProofMalformed;
        return proof;
      }
      in_function = false;
    } else if (opcode == 87u || opcode == 88u ||
               (opcode >= 89u && opcode <= 94u) ||
               (opcode >= 305u && opcode <= 312u)) {
      ++proof.samples;
    }

    if ((opcode >= 86u && opcode <= 107u) && !in_function) {
      proof.reasons |= kProofMalformed;
      return proof;
    }
    // OpImageTexelPointer can feed image atomics: it is a texel-exact image
    // access too, even though its opcode is outside the sample/fetch range.
    if (opcode == 60u || (opcode >= 89u && opcode <= 99u)) {
      proof.reasons |= kProofForbiddenImageOp;
    } else if (opcode >= 305u && opcode <= 320u) {
      proof.reasons |= kProofSparseImageOp;
    } else if (opcode == 86u) {  // OpSampledImage
      if (words != 5u) proof.reasons |= kProofBadImageForm;
    } else if (opcode == 87u) {  // OpImageSampleImplicitLod
      if (words < 5u) {
        proof.reasons |= kProofBadImageForm;
      } else if (words == 5u) {
        // No optional image operands: safe.
      } else {
        const uint32_t mask = detail::LoadLe32(operands + 16u);
        constexpr uint32_t kAllowed =
            (1u << 0) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7);
        uint32_t extra = 0;
        bool bad = (mask & ~kAllowed) != 0u;
        if ((mask & (1u << 0)) != 0u) ++extra;   // Bias
        if ((mask & (1u << 1)) != 0u) {         // Lod: explicit level
          ++extra;
          proof.reasons |= kProofExplicitLod;
        }
        if ((mask & (1u << 2)) != 0u) {         // Grad: invalid here
          extra += 2u;
          bad = true;
        }
        if ((mask & (1u << 3)) != 0u) ++extra;  // ConstOffset
        if ((mask & (1u << 4)) != 0u) ++extra;  // Offset
        if ((mask & (1u << 5)) != 0u) {         // ConstOffsets: invalid here
          ++extra;
          bad = true;
        }
        if ((mask & (1u << 6)) != 0u) ++extra;  // Sample
        if ((mask & (1u << 7)) != 0u) ++extra;  // MinLod
        if (mask == 0u || bad || words != 6u + extra) {
          proof.reasons |= kProofImageOperands;
        }
      }
    } else if (opcode == 88u) {  // OpImageSampleExplicitLod
      if (words < 6u) {
        proof.reasons |= kProofBadExplicitForm;
      } else {
        const uint32_t mask = detail::LoadLe32(operands + 16u);
        if ((mask & (1u << 1)) != 0u) {
          proof.reasons |= kProofExplicitLod;
        }
        if (mask != (1u << 2) || words != 8u) {
          proof.reasons |= kProofBadExplicitForm;
        }
      }
    } else if (opcode >= 100u && opcode <= 107u) {
      uint32_t expected = 0;
      switch (opcode) {
        case 100u:  // OpImage
        case 101u:  // OpImageQueryFormat
        case 102u:  // OpImageQueryOrder
        case 104u:  // OpImageQuerySize
        case 106u:  // OpImageQueryLevels
        case 107u:  // OpImageQuerySamples
          expected = 4u;
          break;
        case 103u:  // OpImageQuerySizeLod
        case 105u:  // OpImageQueryLod
          expected = 5u;
          break;
        default:
          expected = 0u;
          break;
      }
      if (expected == 0u || words != expected) {
        proof.reasons |= kProofBadImageForm;
      }
    }

    offset += words;
  }

  if (offset != word_count || in_function || !saw_function ||
      entry_points == 0u) {
    proof.reasons |= kProofMalformed;
    return proof;
  }

  bool entry_defined = false;
  if (entry_points != 0u) {
    offset = 5u;
    while (offset < word_count) {
      const uint32_t first = detail::LoadLe32(bytes + offset * 4u);
      const uint32_t opcode = first & 0xffffu;
      const uint32_t words = first >> 16u;
      if (opcode == 54u && words == 5u &&
          detail::LoadLe32(bytes + (offset + 1u) * 4u + 4u) == entry_id) {
        entry_defined = true;
        break;
      }
      offset += words;
    }
  }
  if (!entry_defined) {
    proof.reasons |= kProofMalformed;
    return proof;
  }

  proof.parsed = true;
  if (entry_points > 1u) {
    proof.reasons |= kProofEntryPoint;
  }
  const uint32_t expected_stage = vertex_stage ? 0u : 4u;
  if (entry_stage != expected_stage) {
    proof.reasons |= kProofWrongStage;
  }
  if (!vertex_stage && proof.samples == 0u) {
    proof.reasons |= kProofNoSamples;
  }
  proof.safe = proof.reasons == 0u;
  return proof;
}

// ---------------------------------------------------------------------------
// Conservative min-LOD decision.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kQualityNotKnown = 1u << 0;
inline constexpr uint32_t kQualityNotOrdinaryGuest = 1u << 1;
inline constexpr uint32_t kQualityNotSampledOnly = 1u << 2;
inline constexpr uint32_t kQualityRenderTarget = 1u << 3;
inline constexpr uint32_t kQualityResolveTarget = 1u << 4;
inline constexpr uint32_t kQualityCpuUpdated = 1u << 5;
inline constexpr uint32_t kQualityLateUpload = 1u << 6;
inline constexpr uint32_t kQualityTwoDimensional = 1u << 7;
inline constexpr uint32_t kQualityBadDimensions = 1u << 8;
inline constexpr uint32_t kQualityBaseLevel = 1u << 9;
inline constexpr uint32_t kQualityResidentLevels = 1u << 10;
inline constexpr uint32_t kQualityUnsafeSampling = 1u << 11;
inline constexpr uint32_t kQualityNonFinite = 1u << 12;
inline constexpr uint32_t kQualityInvertedRange = 1u << 13;
inline constexpr uint32_t kQualityRangeBelowOne = 1u << 14;

struct SamplingQualityInfo {
  bool known = false;
  bool ordinary_guest = false;
  bool sampled_only = false;
  bool render_target = false;
  bool resolve_target = false;
  bool cpu_updated = false;
  bool late_upload = false;
  bool two_d = false;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t base_level = 0;
  uint32_t resident_levels = 0;
  bool sampling_proof_safe = false;
};

struct QualityDecision {
  bool eligible = false;
  bool changed = false;
  uint32_t reasons = 0;
  float min_lod = 0.0f;
  float max_lod = 0.0f;
  float lod_bias = 0.0f;
};

// Modes 0 and any non-1/2 mode are a no-op that preserves the three floats
// bit-for-bit. Modes 1 and 2 only raise a finite, valid min_lod below 1 to 1.
inline QualityDecision DecideSamplingQuality(uint8_t mode,
                                             const SamplingQualityInfo& info,
                                             float min_lod, float max_lod,
                                             float lod_bias) noexcept {
  QualityDecision decision;
  decision.min_lod = min_lod;
  decision.max_lod = max_lod;
  decision.lod_bias = lod_bias;

  if (mode != 1u && mode != 2u) {
    return decision;
  }

  if (!info.known) decision.reasons |= kQualityNotKnown;
  if (!info.ordinary_guest) decision.reasons |= kQualityNotOrdinaryGuest;
  if (!info.sampled_only) decision.reasons |= kQualityNotSampledOnly;
  if (info.render_target) decision.reasons |= kQualityRenderTarget;
  if (info.resolve_target) decision.reasons |= kQualityResolveTarget;
  if (info.cpu_updated) decision.reasons |= kQualityCpuUpdated;
  if (info.late_upload) decision.reasons |= kQualityLateUpload;
  if (!info.two_d) decision.reasons |= kQualityTwoDimensional;
  if (info.width < 256u || info.height < 256u) {
    decision.reasons |= kQualityBadDimensions;
  }
  if (info.base_level != 0u) decision.reasons |= kQualityBaseLevel;
  if (info.resident_levels < 2u) decision.reasons |= kQualityResidentLevels;
  if (!info.sampling_proof_safe) {
    decision.reasons |= kQualityUnsafeSampling;
  }
  if (!std::isfinite(min_lod) || !std::isfinite(max_lod) ||
      !std::isfinite(lod_bias)) {
    decision.reasons |= kQualityNonFinite;
  }
  if (decision.reasons != 0u) {
    return decision;
  }

  if (!(max_lod >= 1.0f)) {
    decision.reasons |= kQualityRangeBelowOne;
    return decision;
  }
  if (min_lod > max_lod) {
    decision.reasons |= kQualityInvertedRange;
    return decision;
  }

  decision.eligible = true;
  if (min_lod < 1.0f) {
    decision.min_lod = 1.0f;
    decision.changed = true;
  }
  return decision;
}

// ---------------------------------------------------------------------------
// Write-watch history.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kHistoryCpuDirty = 1u << 0;
inline constexpr uint32_t kHistoryGeneration = 1u << 1;
inline constexpr uint32_t kHistoryRange = 1u << 2;
inline constexpr uint32_t kHistoryOverflow = 1u << 3;
inline constexpr uint32_t kHistoryInvalidRange = 1u << 4;
inline constexpr uint32_t kHistoryDirtyBeforeObservation = 1u << 5;

struct RangeEndpoints {
  uint32_t base_begin = 0;
  uint32_t base_end = 0;
  uint32_t mip_begin = 0;
  uint32_t mip_end = 0;
};

inline bool operator==(const RangeEndpoints& a,
                        const RangeEndpoints& b) noexcept {
  return a.base_begin == b.base_begin && a.base_end == b.base_end &&
         a.mip_begin == b.mip_begin && a.mip_end == b.mip_end;
}

inline bool operator!=(const RangeEndpoints& a,
                       const RangeEndpoints& b) noexcept {
  return !(a == b);
}

inline bool RangeEndpointsValid(const RangeEndpoints& range) noexcept {
  return range.base_begin <= range.base_end &&
         range.mip_begin <= range.mip_end;
}

enum class RangeObservation : uint8_t {
  kFirst,
  kStable,
  kExcluded,
  kOverflow,
};

struct RangeObservationResult {
  RangeObservation state = RangeObservation::kExcluded;
  uint32_t reasons = 0;
  uint64_t first_generation = 0;
  bool observed = false;
};

struct RangeRecord {
  RangeEndpoints range{};
  uint64_t first_generation = 0;
  uint32_t exclusion = 0;
  bool observed = false;
};

// Bounded history keyed by texture identity. A record keeps only the first
// observed endpoint set and write generation. Any later generation change
// excludes even if the bytes are identical; the original generation is never
// refreshed. Dirty watch reports before the first observation create an
// excluded placeholder. Capacity overflow is globally fail-closed.
class RangeWriteHistory {
 public:
  static constexpr uint32_t kCapacity = 4096u;

  RangeObservationResult Observe(uint64_t key, const RangeEndpoints& range,
                                 uint64_t generation) noexcept {
    RangeObservationResult result;
    Entry* entry = FindEntry(key);
    if (entry == nullptr) {
      if (overflow_ || size_ == kCapacity) {
        overflow_ = true;
        result.state = RangeObservation::kOverflow;
        result.reasons = kHistoryOverflow;
        return result;
      }
      entry = InsertEntry(key);
      entry->key = key;
      entry->range = range;
      entry->first_generation = generation;
      entry->observed = true;
      if (!RangeEndpointsValid(range)) {
        entry->exclusion |= kHistoryInvalidRange;
      }
      result.first_generation = generation;
      result.observed = true;
      result.reasons = entry->exclusion;
      result.state = entry->exclusion == 0u ? RangeObservation::kFirst
                                           : RangeObservation::kExcluded;
      return result;
    }

    result.observed = entry->observed;
    if (!entry->observed) {
      // A dirty watch preceded any observation. Record the first observation
      // for diagnostics, but keep the sticky exclusion.
      entry->range = range;
      entry->first_generation = generation;
      entry->observed = true;
      if (!RangeEndpointsValid(range)) {
        entry->exclusion |= kHistoryInvalidRange;
      }
      result.observed = true;
    }
    result.first_generation = entry->first_generation;

    if (overflow_) {
      entry->exclusion |= kHistoryOverflow;
    }
    if (entry->exclusion != 0u) {
      result.state = RangeObservation::kExcluded;
      result.reasons = entry->exclusion;
      return result;
    }

    if (entry->range != range) entry->exclusion |= kHistoryRange;
    if (entry->first_generation != generation) {
      entry->exclusion |= kHistoryGeneration;
    }
    if (entry->exclusion != 0u) {
      result.state = RangeObservation::kExcluded;
      result.reasons = entry->exclusion;
      return result;
    }

    result.state = RangeObservation::kStable;
    result.reasons = 0u;
    return result;
  }

  RangeObservationResult MarkDirty(uint64_t key) noexcept {
    RangeObservationResult result;
    Entry* entry = FindEntry(key);
    if (entry == nullptr) {
      if (overflow_ || size_ == kCapacity) {
        overflow_ = true;
        result.state = RangeObservation::kOverflow;
        result.reasons = kHistoryOverflow;
        return result;
      }
      entry = InsertEntry(key);
      entry->key = key;
      entry->exclusion = kHistoryDirtyBeforeObservation;
      result.state = RangeObservation::kExcluded;
      result.reasons = entry->exclusion;
      return result;
    }

    entry->exclusion |= kHistoryCpuDirty;
    if (overflow_) entry->exclusion |= kHistoryOverflow;
    result.observed = entry->observed;
    result.first_generation = entry->first_generation;
    result.state = RangeObservation::kExcluded;
    result.reasons = entry->exclusion;
    return result;
  }

  // Marks every tracked, observed record whose base or mip half-open range
  // intersects [begin, end) on either half. Unknown keys cannot be matched;
  // use MarkDirty(key) when the watch fires before the first observation.
  uint32_t MarkDirtyRange(uint32_t begin, uint32_t end) noexcept {
    if (end <= begin) return 0u;
    uint32_t marked = 0u;
    for (uint32_t i = 0u; i < kSlots; ++i) {
      Entry& entry = entries_[i];
      if (!entry.occupied || !entry.observed) continue;
      const auto overlaps = [begin, end](uint32_t first,
                                         uint32_t last) noexcept {
        return first < last && begin < last && first < end;
      };
      if (overlaps(entry.range.base_begin, entry.range.base_end) ||
          overlaps(entry.range.mip_begin, entry.range.mip_end)) {
        entry.exclusion |= kHistoryCpuDirty;
        ++marked;
      }
    }
    return marked;
  }

  bool IsExcluded(uint64_t key) const noexcept {
    if (overflow_) return true;
    const Entry* entry = FindEntry(key);
    return entry != nullptr && entry->exclusion != 0u;
  }

  bool Find(uint64_t key, RangeRecord* out) const noexcept {
    const Entry* entry = FindEntry(key);
    if (entry == nullptr) return false;
    if (out != nullptr) {
      out->range = entry->range;
      out->first_generation = entry->first_generation;
      out->exclusion = entry->exclusion;
      out->observed = entry->observed;
    }
    return true;
  }

  uint32_t size() const noexcept { return size_; }
  bool overflowed() const noexcept { return overflow_; }

 private:
  struct Entry {
    uint64_t key = 0;
    RangeEndpoints range{};
    uint64_t first_generation = 0;
    uint32_t exclusion = 0;
    bool observed = false;
    bool occupied = false;
  };

  static constexpr uint32_t kSlots = kCapacity * 2u;
  static uint32_t Hash(uint64_t key) noexcept {
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9ull;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebull;
    return uint32_t(key ^ (key >> 31)) & (kSlots - 1u);
  }

  Entry* FindEntry(uint64_t key) noexcept {
    for (uint32_t n = 0u, i = Hash(key); n < kSlots; ++n, i = (i + 1u) & (kSlots - 1u)) {
      if (!entries_[i].occupied) return nullptr;
      if (entries_[i].key == key) return &entries_[i];
    }
    return nullptr;
  }

  const Entry* FindEntry(uint64_t key) const noexcept {
    for (uint32_t n = 0u, i = Hash(key); n < kSlots; ++n, i = (i + 1u) & (kSlots - 1u)) {
      if (!entries_[i].occupied) return nullptr;
      if (entries_[i].key == key) return &entries_[i];
    }
    return nullptr;
  }

  Entry* InsertEntry(uint64_t key) noexcept {
    uint32_t i = Hash(key);
    while (entries_[i].occupied) i = (i + 1u) & (kSlots - 1u);
    entries_[i].occupied = true;
    entries_[i].key = key;
    ++size_;
    return &entries_[i];
  }

  std::array<Entry, kSlots> entries_{};
  uint32_t size_ = 0u;
  bool overflow_ = false;
};

// ---------------------------------------------------------------------------
// Resolve-page bitmap.
// ---------------------------------------------------------------------------

// Guest pages touched by resolve/copy work. The bitmap is fixed and has no
// unmark operation: once a 4 KiB page is covered by a resolve it stays marked
// for the process lifetime.
class ResolvePageMap {
 public:
  static constexpr uint32_t kPageShift = 12u;
  static constexpr uint32_t kPageBytes = 1u << kPageShift;
  static constexpr uint32_t kPageCount = 131072u;
  static constexpr uint32_t kGuestBytes = kPageCount * kPageBytes;
  static constexpr uint32_t kWordCount = kPageCount / 32u;

  static_assert(kGuestBytes == 0x20000000u,
                "resolve bitmap must cover exactly 512 MiB");
  static_assert(kWordCount == 4096u,
                "resolve bitmap must have exactly 4096 words");

  bool MarkRange(uint32_t begin, uint32_t size) noexcept {
    const uint64_t end64 = static_cast<uint64_t>(begin) +
                           static_cast<uint64_t>(size);
    if (end64 > kGuestBytes) return false;
    if (size == 0u) return true;
    const uint32_t end = static_cast<uint32_t>(end64);
    const uint32_t first = begin >> kPageShift;
    const uint32_t last = (end - 1u) >> kPageShift;
    for (uint32_t page = first; page <= last; ++page) {
      bits_[page >> 5u] |= 1u << (page & 31u);
    }
    return true;
  }

  // Marks both the base and mip half-open ranges. All endpoints are checked
  // before any page is marked.
  bool MarkEndpoints(const RangeEndpoints& range) noexcept {
    if (!RangeEndpointsValid(range)) return false;
    if (range.base_begin >= kGuestBytes || range.mip_begin >= kGuestBytes ||
        range.base_end > kGuestBytes || range.mip_end > kGuestBytes) {
      return false;
    }
    if (!MarkRange(range.base_begin, range.base_end - range.base_begin)) {
      return false;
    }
    return MarkRange(range.mip_begin, range.mip_end - range.mip_begin);
  }

  bool Overlaps(uint32_t begin, uint32_t size) const noexcept {
    if (size == 0u || begin >= kGuestBytes) return false;
    uint64_t end64 = static_cast<uint64_t>(begin) + static_cast<uint64_t>(size);
    if (end64 > kGuestBytes) end64 = kGuestBytes;
    const uint32_t end = static_cast<uint32_t>(end64);
    if (end <= begin) return false;
    const uint32_t first = begin >> kPageShift;
    const uint32_t last = (end - 1u) >> kPageShift;
    for (uint32_t page = first; page <= last; ++page) {
      if ((bits_[page >> 5u] & (1u << (page & 31u))) != 0u) return true;
    }
    return false;
  }

  bool OverlapsEndpoints(const RangeEndpoints& range) const noexcept {
    if (!RangeEndpointsValid(range)) return true;  // fail closed
    return Overlaps(range.base_begin, range.base_end - range.base_begin) ||
           Overlaps(range.mip_begin, range.mip_end - range.mip_begin);
  }

  bool IsPageMarked(uint32_t page) const noexcept {
    if (page >= kPageCount) return false;
    return (bits_[page >> 5u] & (1u << (page & 31u))) != 0u;
  }

  uint32_t marked_pages() const noexcept {
    uint32_t total = 0u;
    for (uint32_t word : bits_) {
      total += static_cast<uint32_t>(std::popcount(word));
    }
    return total;
  }

 private:
  std::array<uint32_t, kWordCount> bits_{};
};

}  // namespace wwe2k14::d3d::quality
