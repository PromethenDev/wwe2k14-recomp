// Card U2c: pure decision logic of the crowd draw grouping (no GPU calls), shared by nr_backend.cpp and the host test
// src/native/nr/tests/crowd-group-test.cpp.
#pragma once

#include "nr_backend.h"

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <vector>

namespace nr::backend::crowd {

enum CrowdWhy : int { kCwJoined, kCwGeometry, kCwTexture, kCwPsConst, kCwShared, kCwPass, kCwState, kCwOtherDraw, kCwFull, kCwCount };

inline bool CrowdGeometryCached(const Draw& d) {
  if (d.Buffers().empty() || d.index_cache_id == 0 || d.index_cache_upload || d.cache_flush || !d.Indices().empty()) return false;
  for (const VertexBuffer& vb : d.Buffers())
    if (vb.cache_id == 0 || vb.cache_upload || !vb.data.empty() || vb.prefix_bytes != 0) return false;
  return true;
}
inline bool SameSampler(const SamplerDesc& a, const SamplerDesc& b) {
  return a.mag_linear == b.mag_linear && a.min_linear == b.min_linear && a.mip_linear == b.mip_linear &&
         a.address_u == b.address_u && a.address_v == b.address_v && a.address_w == b.address_w && a.border == b.border &&
         a.max_anisotropy == b.max_anisotropy && a.min_lod == b.min_lod && a.max_lod == b.max_lod && a.lod_bias == b.lod_bias;
}
inline bool SameStencil(const StencilFace& a, const StencilFace& b) {
  return a.fail == b.fail && a.depth_fail == b.depth_fail && a.pass == b.pass && a.compare == b.compare;
}
// Flat crowd cards: one 4-index strip per card, translated VS 2ED176C29EDABB20 reads only constants and PS F1CC033418E7B30F is
// fetch * interpolant. PS F1CC also feeds other geometry through other VS keys, so both keys must match.
inline constexpr uint64_t kCardPsKey = 0xF1CC033418E7B30Full, kCardVsKey = 0x2ED176C29EDABB20ull;
inline bool IsCrowdCard(uint64_t ps_key, uint64_t vs_key) { return ps_key == kCardPsKey && vs_key == kCardVsKey; }
// Entrance crowd (WWE2K14_D3D_CROWD_CARDS=2, on top of the cards above): flat cards PS E65E / VS DB37, 3D figures PS 166A / VS BB50.
// Both VS read the member matrix at c76-c78 through the pushed constants, like the match crowd VS.
inline constexpr uint64_t kEntranceCardPsKey = 0xE65EA09E867B698Dull, kEntranceCardVsKey = 0xDB379786A443B517ull;
inline constexpr uint64_t kEntranceFigurePsKey = 0x166AF9EA7AE1C50Cull, kEntranceFigureVsKey = 0xBB50ECEC8F7BAB26ull;
inline bool IsEntranceCrowd(uint64_t ps_key, uint64_t vs_key) {
  return (ps_key == kEntranceCardPsKey && vs_key == kEntranceCardVsKey) ||
         (ps_key == kEntranceFigurePsKey && vs_key == kEntranceFigureVsKey);
}

// Every Draw field the body reads, except vs_constants (and the diagnostic ordinal). First difference wins.
inline CrowdWhy CrowdCompare(const Draw& a, const Draw& b, bool ignore_ps_key = false, bool full_constants = false) {
  const auto& a_buffers = a.Buffers();
  const auto& b_buffers = b.Buffers();
  const auto& a_indices = a.Indices();
  const auto& b_indices = b.Indices();
  const auto& a_attributes = a.Attributes();
  const auto& b_attributes = b.Attributes();
  const auto& a_textures = a.Textures();
  const auto& b_textures = b.Textures();
  if (a.color != b.color || a.depth != b.depth) return kCwPass;
  if (a_buffers.size() != b_buffers.size() || a.index_cache_id != b.index_cache_id ||
      a.index_cache_bytes != b.index_cache_bytes || a.count != b.count || a.index32 != b.index32 || a.topology != b.topology)
    return kCwGeometry;
  // A member never carries cache stores / flushes (those run inside the draw that owns them: the run's first draw may).
  if (b.cache_flush || b.index_cache_upload) return kCwGeometry;
  for (size_t i = 0; i < a_buffers.size(); ++i) {
    const VertexBuffer &x = a_buffers[i], &y = b_buffers[i];
    if (x.cache_id != y.cache_id || x.cache_size != y.cache_size || x.stride_bytes != y.stride_bytes) return kCwGeometry;
    if (x.direct_src != y.direct_src || x.direct_bytes != y.direct_bytes) return kCwGeometry;
    if (y.cache_upload) return kCwGeometry;
    if (x.cache_id == 0) {  // not in the vertex cache (first sighting / dynamic): the member must carry the very same bytes
      if (x.prefix_bytes != y.prefix_bytes || x.data.size() != y.data.size() ||
          (!x.data.empty() && std::memcmp(x.data.data(), y.data.data(), x.data.size()) != 0))
        return kCwGeometry;
    }
  }
  if (a.index_cache_id == 0 &&
      (a_indices.size() != b_indices.size() ||
       (!a_indices.empty() && std::memcmp(a_indices.data(), b_indices.data(), a_indices.size()) != 0)))
    return kCwGeometry;
  if (a_attributes.size() != b_attributes.size()) return kCwGeometry;
  for (size_t i = 0; i < a_attributes.size(); ++i) {
    const VertexAttribute &x = a_attributes[i], &y = b_attributes[i];
    if (x.location != y.location || x.format != y.format || x.binding != y.binding || x.offset_bytes != y.offset_bytes)
      return kCwGeometry;
  }
  if (a_textures.size() != b_textures.size()) return kCwTexture;
  for (size_t i = 0; i < a_textures.size(); ++i) {
    const TextureBinding &x = a_textures[i], &y = b_textures[i];
    if (x.slot != y.slot || x.dimension != y.dimension || x.texture != y.texture || !SameSampler(x.sampler, y.sampler))
      return kCwTexture;
  }
  const uint8_t* a_ps = full_constants && a.full_float_constants ? a.full_float_constants->ps : a.ps_constants;
  const uint8_t* b_ps = full_constants && b.full_float_constants ? b.full_float_constants->ps : b.ps_constants;
  if (std::memcmp(a_ps, b_ps, sizeof(a.ps_constants)) != 0) return kCwPsConst;
  if (std::memcmp(a.shared, b.shared, sizeof(a.shared)) != 0) return kCwShared;
  if (a.vs_key != b.vs_key || (!ignore_ps_key && a.ps_key != b.ps_key) || a.vs_spirv != b.vs_spirv || a.ps_spirv != b.ps_spirv ||
      a.spec_constants != b.spec_constants || a.sample_shading != b.sample_shading || a.cull != b.cull ||
      a.front_ccw != b.front_ccw || a.depth_clamp != b.depth_clamp || a.depth_test != b.depth_test ||
      a.depth_write != b.depth_write || a.depth_compare != b.depth_compare || a.stencil != b.stencil ||
      a.stencil_ref != b.stencil_ref || a.stencil_read != b.stencil_read || a.stencil_write != b.stencil_write ||
      !SameStencil(a.stencil_front, b.stencil_front) || !SameStencil(a.stencil_back, b.stencil_back) ||
      a.blend.enabled != b.blend.enabled || a.blend.src != b.blend.src || a.blend.dst != b.blend.dst ||
      a.blend.op != b.blend.op || a.blend.src_a != b.blend.src_a || a.blend.dst_a != b.blend.dst_a ||
      a.blend.op_a != b.blend.op_a || a.blend.write_mask != b.blend.write_mask ||
      std::memcmp(a.blend_constants, b.blend_constants, sizeof(a.blend_constants)) != 0 ||
      std::memcmp(a.viewport, b.viewport, sizeof(a.viewport)) != 0 || std::memcmp(a.scissor, b.scissor, sizeof(a.scissor)) != 0)
    return kCwState;
  return kCwJoined;
}

// The pending run: `first` is a copy of the run's first draw; `vs` holds one 4096-byte vertex-constant block per member.
struct CrowdGroup {
  Draw first;
  std::vector<uint8_t> vs;
  uint32_t n = 0;
  // Offers draw `d` to the pending run. kCwJoined: appended (n grew). Otherwise the reason it does not belong; the run is
  // unchanged and the caller must run it before handling `d`.
  CrowdWhy TryJoin(const Draw& d, bool crowd_ps, uint32_t max_instances) {
    if (!n) return kCwOtherDraw;
    if (!crowd_ps) return kCwOtherDraw;
    if (n >= max_instances) return kCwFull;
    const CrowdWhy why = CrowdCompare(first, d);
    if (why != kCwJoined) return why;
    vs.insert(vs.end(), d.vs_constants, d.vs_constants + 4096);
    ++n;
    return kCwJoined;
  }
  void Start(const Draw& d) {
    first = d;
    vs.assign(d.vs_constants, d.vs_constants + 4096);
    n = 1;
  }
};

// Ring packing: instance i's vertex-constant block sits `stride` bytes (kConstBytes, the ring slot size) after instance 0's,
// which is where the VS variant reads it (base + i * stride + register * 16).
inline void PackVsBlocks(uint8_t* slot0_vs, size_t stride, const uint8_t* blocks, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) std::memcpy(slot0_vs + size_t(i) * stride, blocks + size_t(i) * 4096, 4096);
}
// Number of instances whose block in the ring equals its source block.
inline uint32_t VerifyVsBlocks(const uint8_t* slot0_vs, size_t stride, const uint8_t* blocks, uint32_t n) {
  uint32_t ok = 0;
  for (uint32_t i = 0; i < n; ++i) ok += std::memcmp(slot0_vs + size_t(i) * stride, blocks + size_t(i) * 4096, 4096) == 0;
  return ok;
}

// CHECK verdict logic (card U2c). `pre` = target before the group, `inst` = scratch image after the instanced draw, `sep` = after the
// separate draws; all raw host bytes of equal size, `px` pixels. A pass needs painted_px > 0 (separate draws changed the image) and
// zero differing bytes between the two renderings (a pixel counts as painted if EITHER rendering changed it, so a draw that paints in only one
// way is a mismatch, never inconclusive); an unpainted image is inconclusive, never a pass.
struct PictureResult {
  enum Verdict { kPass, kMismatch, kInconclusive } verdict = kInconclusive;
  uint64_t diff_bytes = 0, painted_px = 0;
  uint32_t max_diff = 0;
  size_t first_diff = SIZE_MAX;
};
inline PictureResult ComparePictures(const std::vector<uint8_t>& pre, const std::vector<uint8_t>& inst,
                                     const std::vector<uint8_t>& sep, size_t px) {
  PictureResult r;
  if (inst.size() != sep.size() || pre.size() != sep.size() || px == 0 || sep.size() % px) return r;
  const size_t bpp = sep.size() / px;
  for (size_t i = 0; i < sep.size(); ++i) {
    if (inst[i] != sep[i]) {
      ++r.diff_bytes;
      r.max_diff = std::max<uint32_t>(r.max_diff, inst[i] > sep[i] ? inst[i] - sep[i] : sep[i] - inst[i]);
      if (r.first_diff == SIZE_MAX) r.first_diff = i;
    }
  }
  for (size_t i = 0; i + bpp <= sep.size(); i += bpp) r.painted_px += (std::memcmp(&sep[i], &pre[i], bpp) != 0) || (std::memcmp(&inst[i], &pre[i], bpp) != 0);
  r.verdict = r.painted_px == 0 ? PictureResult::kInconclusive : (r.diff_bytes ? PictureResult::kMismatch : PictureResult::kPass);
  return r;
}

}  // namespace nr::backend::crowd
