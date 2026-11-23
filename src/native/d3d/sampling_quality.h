// Shared positive sampling/provenance proof for optional Q quality settings.
#pragma once
#include "sampling_quality_core.h"
#include <vector>

namespace nr::shared { struct ShaderEntry; }

namespace wwe2k14::d3d::quality {
// All nonempty translated variants, including the actual selected vectors,
// must be proved. No HLSL absence / shader-key heuristic is used as proof.
bool BindingSamplingSafe(const nr::shared::ShaderEntry* vs, const nr::shared::ShaderEntry* ps,
                         uint32_t slot, const std::vector<uint8_t>* active_vs,
                         const std::vector<uint8_t>* active_ps, bool check,
                         bool* cache_mismatch, uint32_t* proof_reasons = nullptr);
}
