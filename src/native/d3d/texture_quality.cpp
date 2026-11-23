#include "texture_quality.h"
#include "quality_lifetime.h"
#include "sampling_quality.h"
#include "textures.h"
#include "../nr/nr_backend.h"
#include <array>
#include <bit>
#include <cstdlib>
#include <rex/logging.h>

namespace wwe2k14::d3d::texq {
namespace {
struct Counters {
  uint64_t eligible = 0, clamped = 0, checks = 0, mismatches = 0, explicit_lod = 0;
  std::array<uint64_t, 15> excluded{};
  uint64_t bindings = 0;
  std::array<uint64_t, 15> first{};
  std::array<uint64_t, 3> guest_levels{};
  std::array<uint64_t, 3> resident_levels{};
  uint32_t swaps = 0;
} counters;

// CHECK-mode fetch evidence is bounded per PROCESS (never per 120-swap window).
std::array<std::array<uint32_t, 6>, 8> fetch_seen_single{};
std::array<std::array<uint32_t, 6>, 8> fetch_seen_multi{};
uint32_t fetch_seen_single_count = 0, fetch_seen_multi_count = 0;

bool ReferenceEligible(const quality::SamplingQualityInfo& i, float min, float max, float bias) {
  return i.known && i.ordinary_guest && i.sampled_only && !i.render_target && !i.resolve_target &&
         !i.cpu_updated && !i.late_upload && i.two_d && i.width >= 256 && i.height >= 256 &&
         i.base_level == 0 && i.resident_levels > 1 && i.sampling_proof_safe &&
         std::isfinite(min) && std::isfinite(max) && std::isfinite(bias) && max >= (min < 1.0f ? 1.0f : min);
}
bool Same(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
}  // namespace

uint8_t Mode() {
  static const uint8_t mode = quality::ParseMode(std::getenv("WWE2K14_D3D_TEX_QUALITY"));
  return mode;
}

void Apply(const DeviceState& state, uint32_t handle, uint32_t slot,
           const nr::shared::ShaderEntry* vs,
           const nr::shared::ShaderEntry* ps, const std::vector<uint8_t>* active_vs,
           const std::vector<uint8_t>* active_ps, nr::backend::SamplerDesc& sampler) {
  const uint8_t mode = Mode();
  if (!mode) return;
  ++counters.bindings;
  const auto fetch = textures::GetSamplingQualityFetchInfo(state, slot);
  auto info = textures::GetSamplingQualityInfo(handle);
  ++counters.guest_levels[static_cast<std::size_t>(
      quality::ClassifyFetchLevels(fetch.valid, fetch.claimed_levels))];
  ++counters.resident_levels[static_cast<std::size_t>(quality::ClassifyMetadataLevels(info))];
  if (mode == 2) {
    const auto kind = quality::ClassifyFetchLevels(fetch.valid, fetch.claimed_levels);
    if (kind != quality::FetchLevelFacts::kInvalid) {
      const bool single = kind == quality::FetchLevelFacts::kSingleLevel;
      auto& seen = single ? fetch_seen_single : fetch_seen_multi;
      uint32_t& count = single ? fetch_seen_single_count : fetch_seen_multi_count;
      bool duplicate = false;
      for (uint32_t i = 0; i < count; ++i)
        if (seen[i] == fetch.words) {
          duplicate = true;
          break;
        }
      if (!duplicate && count < static_cast<uint32_t>(seen.size())) {
        seen[count++] = fetch.words;
        const auto& w = fetch.words;
        REXLOG_INFO("[nr-texq-fetch] slot={} handle={} words={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
                    "decoded={}x{} min={} max={} first={} claimed={} metadata_known={} resident={} base={:08X}",
                    slot, handle, w[0], w[1], w[2], w[3], w[4], w[5], fetch.width, fetch.height,
                    fetch.min_level, fetch.max_level, fetch.first_level, fetch.claimed_levels,
                    info.known ? 1 : 0, info.resident_levels, fetch.base);
      }
    }
  }
  bool mismatch = false;
  uint32_t proof_reasons = 0;
  info.sampling_proof_safe = quality::BindingSamplingSafe(vs, ps, slot, active_vs, active_ps,
                                                        mode == 2, &mismatch, &proof_reasons);
  auto d = quality::DecideSamplingQuality(mode, info, sampler.min_lod, sampler.max_lod, sampler.lod_bias);
  if (mode == 2) {
    ++counters.checks;
    // Fresh texture watch/provenance plus a fresh translated-shader scan, and
    // an independently written reference decision, not comparison to itself.
    auto fresh = textures::GetSamplingQualityInfo(handle);
    bool fresh_bad = false;
    fresh.sampling_proof_safe = quality::BindingSamplingSafe(vs, ps, slot, active_vs, active_ps, true, &fresh_bad);
    const bool eligible = ReferenceEligible(fresh, sampler.min_lod, sampler.max_lod, sampler.lod_bias);
    const float expected = eligible && sampler.min_lod < 1.0f ? 1.0f : sampler.min_lod;
    mismatch |= fresh_bad || d.eligible != eligible || !Same(d.min_lod, expected) ||
                !Same(d.max_lod, sampler.max_lod) || !Same(d.lod_bias, sampler.lod_bias);
    if (mismatch) {
      ++counters.mismatches;
      d.eligible = d.changed = false;
      d.reasons |= quality::kQualityNotKnown;
    }
  }
  if (d.reasons) {
    const uint32_t reason_bit = std::countr_zero(d.reasons);
    if (reason_bit < static_cast<uint32_t>(counters.first.size())) ++counters.first[reason_bit];
  }
  if (proof_reasons & quality::kProofExplicitLod) ++counters.explicit_lod;
  for (uint32_t bit = 0; bit < counters.excluded.size(); ++bit)
    if (d.reasons & (1u << bit)) ++counters.excluded[bit];
  if (d.eligible) ++counters.eligible;
  if (d.changed) {
    sampler.min_lod = d.min_lod;
    ++counters.clamped;
  }
}

void FrameEnd() {
  if (!Mode() || ++counters.swaps != 120) return;
  const auto& x = counters.excluded;
  REXLOG_INFO("[nr-texq] mode={} swaps=120 eligible/f={:.2f} clamped/f={:.2f} "
              "excluded{{rt,resolve,cpu,1mip,explicit-lod,unknown,sampling}}/f="
              "{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f} checks={} mismatches={}",
              Mode(), counters.eligible / 120.0, counters.clamped / 120.0,
              x[3] / 120.0, x[4] / 120.0, x[5] / 120.0, x[10] / 120.0,
              counters.explicit_lod / 120.0, x[0] / 120.0, x[11] / 120.0,
              counters.checks, counters.mismatches);
  const auto meta = textures::GetSamplingQualityMetadataStats();
  const auto& g = counters.guest_levels;
  const auto& r = counters.resident_levels;
  const auto& fb = counters.first;
  REXLOG_INFO("[nr-texq-facts] bindings/f={:.2f} "
              "guest{{invalid,1,multi}}/f={:.2f},{:.2f},{:.2f} "
              "resident{{unknown,1,multi}}/f={:.2f},{:.2f},{:.2f} "
              "first{{unknown,source,sampled,rt,resolve,cpu,late,dim,size,base,mips,sampling,finite,inverted,below}}/f="
              "{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f} "
              "metadata{{live,peak,invalid_causes,orphans,history_records,history_overflow}}="
              "{},{},{},{},{},{}",
              counters.bindings / 120.0, g[0] / 120.0, g[1] / 120.0, g[2] / 120.0,
              r[0] / 120.0, r[1] / 120.0, r[2] / 120.0,
              fb[0] / 120.0, fb[1] / 120.0, fb[2] / 120.0, fb[3] / 120.0, fb[4] / 120.0,
              fb[5] / 120.0, fb[6] / 120.0, fb[7] / 120.0, fb[8] / 120.0, fb[9] / 120.0,
              fb[10] / 120.0, fb[11] / 120.0, fb[12] / 120.0, fb[13] / 120.0, fb[14] / 120.0,
              meta.live, meta.peak, meta.invalid_causes, meta.orphan_records,
              meta.history_records, meta.history_overflow);
  counters = {};
}
}  // namespace wwe2k14::d3d::texq
