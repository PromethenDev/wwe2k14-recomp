// U9/astra1: host-only draw ownership handoff. No guest interpretation.
#pragma once
#include "nr_backend.h"
#include "nr_record_queue.h"
#include <cstdlib>

namespace nr::backend::draw_swap {
inline int ParseMode(const char* e) {
  return e && e[0] && !e[1] && (e[0] == '1' || e[0] == '2') ? e[0] - '0' : 0;
}
inline int Mode() {
  static const int mode = ParseMode(std::getenv("WWE2K14_NR_DRAW_SWAP"));
  return mode;
}
// Compare values, never C++ padding; float payloads are bit-compared (NaNs and signed zero included).
inline bool SameOutput(const Draw& a, const Draw& b) {
  const auto& a_attributes = a.Attributes();
  const auto& b_attributes = b.Attributes();
  const auto& a_buffers = a.Buffers();
  const auto& b_buffers = b.Buffers();
  const auto& a_indices = a.Indices();
  const auto& b_indices = b.Indices();
  const auto& a_textures = a.Textures();
  const auto& b_textures = b.Textures();
#define SAME(f) if (a.f != b.f) return false
#define BYTES(f) if (std::memcmp(&a.f, &b.f, sizeof(a.f))) return false
  SAME(color); SAME(depth); SAME(vs_key); SAME(ps_key); SAME(vs_spirv); SAME(ps_spirv);
  SAME(spec_constants); SAME(sample_shading); SAME(topology); SAME(cull); SAME(front_ccw); SAME(depth_clamp);
  SAME(index32); SAME(count);
  if (a_indices != b_indices) return false;
  BYTES(vs_constants); BYTES(ps_constants); BYTES(shared); BYTES(viewport); BYTES(scissor); BYTES(blend_constants);
  SAME(depth_test); SAME(depth_write); SAME(depth_compare); SAME(stencil); SAME(stencil_ref);
  SAME(stencil_read); SAME(stencil_write); SAME(ordinal); SAME(jobs_only);
  SAME(index_cache_id); SAME(index_cache_bytes); SAME(index_cache_upload); SAME(cache_flush);
  SAME(pre_ops); SAME(pre_job_hashes); SAME(pre_job_names);
  SAME(stencil_front.fail); SAME(stencil_front.depth_fail); SAME(stencil_front.pass); SAME(stencil_front.compare);
  SAME(stencil_back.fail); SAME(stencil_back.depth_fail); SAME(stencil_back.pass); SAME(stencil_back.compare);
  SAME(blend.enabled); SAME(blend.src); SAME(blend.dst); SAME(blend.op); SAME(blend.src_a);
  SAME(blend.dst_a); SAME(blend.op_a); SAME(blend.write_mask);
  if (a_attributes.size() != b_attributes.size() || a_buffers.size() != b_buffers.size() ||
      a_textures.size() != b_textures.size() || a.pre_jobs.size() != b.pre_jobs.size()) return false;
  // Closures cannot be compared by value. ExchangeAcquired preserves their original object identity;
  // SameOutput checks their ordered sidecars. The host stress test executes captured payloads exactly once.
  for (size_t i = 0; i < a.pre_jobs.size(); ++i)
    if (bool(a.pre_jobs[i]) != bool(b.pre_jobs[i])) return false;
  for (size_t i = 0; i < a_attributes.size(); ++i) {
    const auto& x = a_attributes[i]; const auto& y = b_attributes[i];
    if (x.location != y.location || x.format != y.format || x.binding != y.binding || x.offset_bytes != y.offset_bytes)
      return false;
  }
  for (size_t i = 0; i < a_buffers.size(); ++i) {
    const auto& x = a_buffers[i]; const auto& y = b_buffers[i];
    if (x.data != y.data || x.stride_bytes != y.stride_bytes || x.prefix_bytes != y.prefix_bytes ||
        x.cache_id != y.cache_id || x.cache_size != y.cache_size || x.cache_upload != y.cache_upload) return false;
  }
  for (size_t i = 0; i < a_textures.size(); ++i) {
    const auto& x = a_textures[i]; const auto& y = b_textures[i];
    if (x.slot != y.slot || x.dimension != y.dimension || x.texture != y.texture ||
        x.sampler.mag_linear != y.sampler.mag_linear || x.sampler.min_linear != y.sampler.min_linear ||
        x.sampler.mip_linear != y.sampler.mip_linear || x.sampler.address_u != y.sampler.address_u ||
        x.sampler.address_v != y.sampler.address_v || x.sampler.address_w != y.sampler.address_w ||
        x.sampler.border != y.sampler.border || x.sampler.max_anisotropy != y.sampler.max_anisotropy ||
        std::memcmp(&x.sampler.min_lod, &y.sampler.min_lod, sizeof(float)) ||
        std::memcmp(&x.sampler.max_lod, &y.sampler.max_lod, sizeof(float)) ||
        std::memcmp(&x.sampler.lod_bias, &y.sampler.lod_bias, sizeof(float))) return false;
  }
#undef SAME
#undef BYTES
  return true;
}
struct Result { Draw* submitted; bool swapped; bool checked; bool mismatch; };
// The caller already acquired `slot`; no publication occurs here. False preconditions keep the legacy path.
// CHECK compares against the actual legacy transfer, not a second implementation of it.
template <uint32_t N>
Result Handoff(RecordQueue<Draw, N>& queue, Draw* slot, std::unique_ptr<Draw>& owner, int mode) {
  if (!owner || !slot) return {slot, false, false, true};
  if (mode != 1 && mode != 2) return {slot, false, false, false};
  std::unique_ptr<Draw> legacy;
  if (mode == 2) {
    Draw source = *owner;
    legacy = std::make_unique<Draw>();
    legacy->TakeFrom(source);
    if (!SameOutput(*owner, *legacy)) return {slot, false, true, true};
  }
  Draw* original = owner.get();
  if (!queue.ExchangeAcquired(slot, owner)) return {slot, false, mode == 2, true};
  // The queue owns `original` without moving any Draw member, including non-comparable job closures.
  return {original, true, mode == 2, false};
}
}  // namespace nr::backend::draw_swap
