// Card S3-68 (WWE2K14_D3D_DIRTY_TRANSLATE): dirty-group draw translation. Worker thread only.
//
// TranslateDraw (draw.cpp) reuses the outputs of a state group when none of its inputs changed since the outputs were
// computed: register inputs through the RegView dirty mask (regview.h TakeDirty: render-state registers -> one epoch,
// fetch slots -> one epoch each), every other input as an explicit key (identity keys, primitive, bound depth surface,
// scissor clamp width, cvars). Mode 1 uses a valid memo instead of recomputing; mode 2 ("check") always recomputes and
// compares the memo with the fresh result (picture = full path); mode 0 (default) leaves TranslateDraw unchanged.
// A memo is filled only by a draw that reached the end of the group (skip paths never fill one).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef WWE2K14_HAS_NR
#include "../nr/nr_backend.h"
#include "../nr/nr_shared.h"
#endif

namespace rex::graphics {
class Shader;
}

namespace wwe2k14::d3d {
class RegView;
}

namespace wwe2k14::d3d::dtr {

// 0 off (default), 1 reuse, 2 check. Read once.
int Mode();
// Lane-only optional A/B schedule; when configured, alternates 600-frame legacy/reuse blocks.
int ModeForFrame(uint32_t frame);

enum Group : uint32_t { kGShaders, kGNoop, kGVariant, kGState, kGLayout, kGSlots, kGSampler, kGCount };
// One memo lookup of group g: `hit` = a valid memo for the same inputs existed.
void Count(Group g, bool hit);
// Mode 2: the memo differed from the full path (first 20 logged with `what`).
void Mismatch(Group g, uint32_t seq, const std::string& what);
// Start of a draw translation: takes the RegView dirty mask and advances the epochs of the changed groups.
void BeginDraw(const RegView& rv);
uint64_t RsEpoch();
uint64_t SlotEpoch(uint32_t slot);
// Counter line `[d3d-dtr]` (called where draw.cpp logs its translate split, every 300 translated frames).
void LogLine(uint32_t frames, uint32_t frame);

#ifdef WWE2K14_HAS_NR
// Render-state outputs of TranslateDraw's consts+state block (everything there that is not per draw).
struct StateOut {
  uint32_t spec = 0, param_gen = 0;
  uint8_t alpha[8] = {};    // shared [528, 536): alpha ref, alpha func
  uint8_t ndc[36] = {};     // shared [544, 580): ndc scale, ndc offset, VTE flags
  uint8_t scales[16] = {};  // shared [816, 832): colour exponent scale per RT
  float viewport[6] = {};
  int32_t scissor[4] = {};
  bool depth_test = false, depth_write = false, stencil = false;
  uint32_t depth_compare = 0, stencil_ref = 0, stencil_read = 0, stencil_write = 0;
  nr::backend::StencilFace stencil_front, stencil_back;
  nr::backend::BlendTarget blend;
  void From(const nr::backend::Draw& d, uint32_t spec_in, uint32_t param_gen_in);
  void ApplyTo(nr::backend::Draw& d) const;  // every field above except spec / param_gen (the caller's locals)
  // Empty = equal, else the first differing field.
  std::string Diff(const StateOut& o) const;
};

struct ShaderMemo {
  bool valid = false;
  uint64_t vfnv = 0, pfnv = 0;
  bool has_ps = false;
  // (A) identity lookups (pure of the keys once they succeeded) + noop / unsupported checks passed in noop_epoch.
  const rex::graphics::Shader* vsh = nullptr;
  const rex::graphics::Shader* psh = nullptr;
  const nr::shared::ShaderEntry* vs = nullptr;
  const nr::shared::ShaderEntry* ps = nullptr;
  uint64_t noop_epoch = ~0ull;
  // (B) pixel shader variant (float24 / early-Z / depth-only, set B): valid in var_epoch for var_depth_bound/var_cvars.
  uint64_t var_epoch = ~0ull;
  bool var_depth_bound = false;
  uint8_t var_cvars = 0;
  const nr::shared::ShaderEntry* ps_use = nullptr;
  uint64_t d_vs_key = 0, d_ps_key = 0;
  const std::vector<uint8_t>* vs_spirv = nullptr;
  const std::vector<uint8_t>* ps_spirv = nullptr;
  bool sample_shading = false;
  std::string ids;
  // (E) render state: valid in st_epoch for (st_clamp_w, st_prim, st_ps_use, st_cvars).
  uint64_t st_epoch = ~0ull;
  uint32_t st_clamp_w = 0, st_prim = 0;
  bool st_ps_use = false;
  uint8_t st_cvars = 0;
  StateOut st;
};
// Direct-mapped slot for the key (may hold another key: compare vfnv/pfnv/has_ps; Reset() on refill).
ShaderMemo& ShaderSlot(uint64_t vfnv, uint64_t pfnv, bool has_ps);
void ResetShaderMemo(ShaderMemo& m, uint64_t vfnv, uint64_t pfnv, bool has_ps);

// (D) vertex layout of a VS contract entry (pure of vs->vfetch): unique fetches in first-use order (= binding order).
struct LayoutFetch {
  uint32_t fetch = 0, stride_bytes = 0, extent = 0;
};
struct LayoutMemo {
  bool valid = false;
  const nr::shared::ShaderEntry* vs = nullptr;
  std::vector<LayoutFetch> fetches;
  std::vector<nr::backend::VertexAttribute> attrs;
};
LayoutMemo& LayoutSlot(const nr::shared::ShaderEntry* vs);
bool SameAttributes(const std::vector<nr::backend::VertexAttribute>& a, const std::vector<nr::backend::VertexAttribute>& b);

// (T) texture slots + shader-contract dimensions sampled by (VS, PS used), in slot order (pure of the two entries).
struct SlotsMemo {
  bool valid = false;
  const nr::shared::ShaderEntry* vs = nullptr;
  const nr::shared::ShaderEntry* ps = nullptr;
  std::vector<std::pair<uint32_t, uint32_t>> slots;  // (slot, dimension)
};
SlotsMemo& SlotsSlot(const nr::shared::ShaderEntry* vs, const nr::shared::ShaderEntry* ps);

// Sampler decoded from fetch slot s (valid while the slot's epoch and the SDK shader objects are unchanged).
struct SamplerMemo {
  bool valid = false;
  uint64_t epoch = 0;
  const rex::graphics::Shader* vsh = nullptr;
  const rex::graphics::Shader* psh = nullptr;
  nr::backend::SamplerDesc sampler;
};
SamplerMemo& SamplerSlot(uint32_t slot);
bool SameSampler(const nr::backend::SamplerDesc& a, const nr::backend::SamplerDesc& b);
#endif

}  // namespace wwe2k14::d3d::dtr
