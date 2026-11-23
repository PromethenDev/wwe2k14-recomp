#include "sampling_quality.h"
#ifdef WWE2K14_HAS_NR
#include "../nr/nr_shared.h"

namespace wwe2k14::d3d::quality {
namespace {
using Entry = nr::shared::ShaderEntry;
struct Summary { bool safe = false; uint32_t reasons = 0, samples = 0, variants = 0; };

Summary Scan(const Entry& e, bool vertex) {
  Summary r;
  r.safe = !e.spirv.empty();
  for (const auto* v : {&e.spirv, &e.spirv_f24, &e.spirv_f24c,
                        &e.spirv_b, &e.spirv_f24_b, &e.spirv_f24c_b}) {
    if (v->empty()) continue;
    const SamplingProof p = ProveSpirvSampling(v->data(), v->size(), vertex);
    ++r.variants;
    r.samples += p.samples;
    r.reasons |= p.reasons;
    r.safe &= p.safe;
  }
  return r;
}

bool Contains(const Entry& e, const std::vector<uint8_t>* active) {
  if (!active || active->empty()) return false;
  for (const auto* v : {&e.spirv, &e.spirv_f24, &e.spirv_f24c,
                        &e.spirv_b, &e.spirv_f24_b, &e.spirv_f24c_b})
    if (active == v) return true;
  return false;
}

bool Stage(const Entry& e, bool vertex, bool check, bool* mismatch, uint32_t* reasons) {
  if (!e.quality_proof_state) {
    const Summary r = Scan(e, vertex);
    e.quality_proof_state = r.safe ? 1 : 2;
    e.quality_proof_reasons = r.reasons;
    e.quality_proof_samples = r.samples;
    e.quality_proof_variants = r.variants;
  }
  if (check) {
    const Summary fresh = Scan(e, vertex);
    if ((e.quality_proof_state == 1) != fresh.safe || e.quality_proof_reasons != fresh.reasons ||
        e.quality_proof_samples != fresh.samples || e.quality_proof_variants != fresh.variants) {
      *mismatch = true;
      return false;
    }
  }
  *reasons |= e.quality_proof_reasons;
  return e.quality_proof_state == 1;
}
}  // namespace

bool BindingSamplingSafe(const Entry* vs, const Entry* ps, uint32_t slot,
                         const std::vector<uint8_t>* active_vs, const std::vector<uint8_t>* active_ps,
                         bool check, bool* cache_mismatch, uint32_t* proof_reasons) {
  bool mismatch = false;
  uint32_t reasons = 0;
  // Conservative: vertex-only texture use and replaced/native library PS are
  // unproved. The PS must positively bind this slot in the loaded contract.
  bool safe = vs && ps && ps->texture_slots.count(slot) && Contains(*vs, active_vs) && Contains(*ps, active_ps);
  if (safe) {
    const bool v = Stage(*vs, true, check, &mismatch, &reasons);
    const bool p = Stage(*ps, false, check, &mismatch, &reasons);
    safe = v && p;
  }
  if (cache_mismatch) *cache_mismatch = mismatch;
  if (proof_reasons) *proof_reasons = reasons;
  return safe && !mismatch;
}
}  // namespace wwe2k14::d3d::quality
#else
namespace wwe2k14::d3d::quality {
bool BindingSamplingSafe(const nr::shared::ShaderEntry*, const nr::shared::ShaderEntry*, uint32_t,
                         const std::vector<uint8_t>*, const std::vector<uint8_t>*, bool,
                         bool* mismatch, uint32_t* reasons) {
  if (mismatch) *mismatch = false;
  if (reasons) *reasons = kProofUnknownOpcode;
  return false;
}
}
#endif
