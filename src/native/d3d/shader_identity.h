// WP5-impl (decision 0006 stage 2): exact microcode identity of the VS/PS the GPU executes, from D3D state only.
// VS code is captured after the hooked inline writer validates its emitted packet; PS code is read from the bound
// pixel-shader object's record when the guarded flush-entry predicate says the guest emits a PS pointer load.
// Observe only; needs WWE2K14_D3D_OBSERVE=1 (default off). When no identity is known, queries return false.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "shadow_types.h"

#ifndef WWE2K14_PS_LOAD_PREDICATE_ONLY
#include <rex/ppc/context.h>
namespace rex::graphics {
class Shader;
}
#endif

namespace wwe2k14::d3d {

namespace ps_load {
constexpr uint32_t kPsDirty = 0x00100000u;

// The guest's normal ShaderFlush PS-pointer-load branch: entry dirty bit set and both bound shader pointers non-null.
// Inputs are the entry r4 mask and entry device-state bindings, captured by the ShaderFlush hook.
inline bool ShouldLoad(uint32_t entry_mask, uint32_t vertex_shader, uint32_t pixel_shader) {
  return (entry_mask & kPsDirty) != 0 && vertex_shader != 0 && pixel_shader != 0;
}
}  // namespace ps_load

#ifndef WWE2K14_PS_LOAD_PREDICATE_ONLY

// Stage numbering of this API: 0 = vertex, 1 = pixel.
bool ShaderIdentityEnabled();

// Game thread. `device` = r3 and `entry_mask` = r4 at ShaderFlush entry; `entry_sp` is retained for the hooked VS-tail
// writer analysis. The PS-load decision uses the entry mask and entry VS/PS bindings only.
void ShaderIdentityPreFlush(const uint8_t* guest_base, uint32_t device, uint32_t entry_mask, uint32_t entry_sp);
// After the original returned. Records the identity of every VS/PS the flush uploaded.
void ShaderIdentityOnFlush(const uint8_t* guest_base, uint32_t device);
// Convenience wrapper around Pre + original + OnFlush.
void ShaderIdentityHookedFlush(PPCContext& ctx, uint8_t* base, PPCFunc* original);
// Hook wrapper for the inline VS writer sub_82CC1B00: original + exact capture of the code it appended.
void ShaderIdentityHookedVSUpload(PPCContext& ctx, uint8_t* base, PPCFunc* original);
// Hook wrapper for sub_82CC1C58: marks that the flush entered its VS block, to distinguish pointer from inline loads.
void ShaderIdentityHookedConstLoad(PPCContext& ctx, uint8_t* base, PPCFunc* original);

// Identity of the shader the CP executes for `stage` (0 VS, 1 PS): the last upload/load for that stage in execution
// order, independent of the currently bound object. False means unavailable; never a fallback.
bool CurrentShaderKey(uint32_t stage, ShaderKey* out);
// Previously captured microcode as host-order dwords. Pointer stable for process lifetime; nullptr if not captured.
const std::vector<uint32_t>* ShaderBytes(uint64_t fnv, uint32_t stage);
// SDK shader analyzed from the previously captured microcode; nullptr if not captured/analyzable.
const rex::graphics::Shader* ShaderObject(uint64_t fnv, uint32_t stage);

// RR4 (record-then-replay): every captured shader (stage, key, guest-order code), under the table lock.
void ShaderForEachRaw(const std::function<void(uint32_t stage, uint64_t fnv, const uint8_t* raw, uint32_t bytes)>& cb);
// RR4 offline replay: adds recorded microcode to the table exactly as a live capture does (Intern + analysis).
void ShaderReplayIntern(uint32_t stage, const uint8_t* raw, uint32_t bytes);

// FNV-1a-64 over host-order dwords serialized big-endian.
uint64_t ShaderFnv1a64(const uint32_t* host_dwords, size_t count);

// Cross-check against the nr frontend.
void ShaderIdentityCompare(bool captured_valid, const ShaderKey& captured, uint32_t stage, uint64_t nr_fnv);
void ShaderIdentityCompareDwords(bool captured_valid, const ShaderKey& captured, uint32_t stage,
                                 const uint32_t* sdk_host_dwords, size_t count);
void ShaderIdentitySummary(const char* why);

#endif  // WWE2K14_PS_LOAD_PREDICATE_ONLY

}  // namespace wwe2k14::d3d
