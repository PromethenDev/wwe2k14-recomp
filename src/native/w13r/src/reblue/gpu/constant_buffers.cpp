/**
 * @file    gpu/constant_buffers.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/constant_buffers.h"
#include "gpu/cpu_zones.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include <plume_render_interface_builders.h>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/hooks/tweaks.h"
#include "gpu/sampler_cache.h"
#include "gpu/settings.h"
#include "gpu/shaders/shader_cache.h"
#if defined(SVR_NATIVE_RENDERER)
#include <cmath>
#include <rex/cvar.h>
#include "svr_resources.h"
REXCVAR_DECLARE(bool, svr_match_guest_mips);
#endif
#if defined(REBLUE_D3D12) && !defined(REBLUE_D3D11) && defined(SVR_NATIVE_RENDERER)
REXCVAR_DEFINE_BOOL(svr_d3d12_depth_flip, true, "SvR",
                    "D3D12: swap a reversed viewport depth range (MinZ > MaxZ) and give z' = w - z in "
                    "the vertex shaders, as D3D11 does (old Intel drivers, UHD 620: wrestlers and "
                    "crowd half see-through in WWE '12 PC). false = pass the reversed range to the driver.");
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace bd::gpu {

namespace {

constexpr u32 kCBVAlignment = 256;

constexpr u32 kUploadChunkSize = 16 * 1024 * 1024;

// 256 vector4f registers = 4 KiB. Shaders reference the full window, so the
// upload spans the whole range every flush.
constexpr u32 kConstantRegisterCount = 256;
constexpr u32 kConstantBlockBytes = kConstantRegisterCount * 16;

struct UploadChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *mapped = nullptr;
  u64 gpuBase = 0;
};

// One upload chunk list per in-flight frame slot, rewound only after that
// slot's GPU fence is awaited, so the GPU never reads bytes the CPU has
// overwritten.
struct FrameUpload {
  std::vector<UploadChunk> chunks;
  u32 chunkIndex = 0;
  u32 chunkOffset = 0;
  u32 peakChunkCount = 0;
};

// DecodeFromFetch + ResolveSlotLocked (mutex + hash lookup) run per bound slot
// on EVERY draw, and the fetch constants almost never change between draws, so
// a 24-byte compare replaces them on the hot path. Sampler heap slots are never
// reclaimed, so a cached index stays valid until device teardown.
struct SamplerSlotCache {
  u8 fc[sizeof(DeviceFetchConstant)]{};
  u32 sampler = 0;
  i32 aniso = -1;
  float lodBias = 0.0f;
  bool clamp3d = false;
  bool valid = false;
};

struct UploadState {
  FrameUpload frames[kNumFrames];
  u32 cursor = 0;
  u64 epoch = 1;  // bumped by every ResetFrame
  SharedConstants shared{};
  SharedConstants lastUploaded{};
  SamplerSlotCache samplerSlots[kTextureSlots];
  float shadowPcfScale = 1.0f;
  bool sharedBound = false;
  bool ready = false;
};

UploadState &upload_state() {
  static UploadState s;
  return s;
}

void RecomputeShadowPcfScale(UploadState &s) {
  const f64 dist = std::clamp(ShadowCoverageScale(), 1.0, 4.0);
  const f64 dim = std::max(512, Settings::Get().ShadowDimension());
  s.shadowPcfScale = static_cast<float>(std::max(1.0 / dist, 1024.0 / dim));
}

bool CreateChunk(UploadChunk &chunk) {
  auto *device = bd::gpu::Video::HostDevice();
  if (!device)
    return false;
  chunk.buffer = bd::gpu::CreateHostBuffer(
      device,
      plume::RenderBufferDesc::UploadBuffer(
          kUploadChunkSize, plume::RenderBufferFlag::CONSTANT |
                                plume::RenderBufferFlag::VERTEX |
                                plume::RenderBufferFlag::INDEX |
                                plume::RenderBufferFlag::DEVICE_ADDRESSABLE),
      "cb-upload-chunk");
  if (!chunk.buffer) {
    BD_ERROR("constant_buffers: createBuffer({} MiB chunk) failed",
             kUploadChunkSize / (1024 * 1024));
    return false;
  }
  chunk.mapped = reinterpret_cast<u8 *>(chunk.buffer->map());
  if (!chunk.mapped) {
    BD_ERROR("constant_buffers: RenderBuffer::map() returned null");
    chunk.buffer.reset();
    return false;
  }
  chunk.gpuBase = chunk.buffer->getDeviceAddress();
  return true;
}

ConstantAllocation Allocate(UploadState &s, u32 size, u32 alignment) {
  if (!s.ready)
    return {};
  if (size > kUploadChunkSize) {
    BD_ERROR("constant_buffers: single allocation {} exceeds chunk size {}",
             size, kUploadChunkSize);
    return {};
  }
  FrameUpload &up = s.frames[s.cursor];
  u32 off = (up.chunkOffset + alignment - 1) & ~(alignment - 1);
  if (off + size > kUploadChunkSize) {
    ++up.chunkIndex;
    off = 0;
  }
  if (up.chunks.size() <= up.chunkIndex) {
    up.chunks.resize(up.chunkIndex + 1);
  }
  auto &chunk = up.chunks[up.chunkIndex];
  if (!chunk.buffer) {
    if (!CreateChunk(chunk))
      return {};
    const u32 total = up.chunkIndex + 1;
    if (total > up.peakChunkCount) {
      up.peakChunkCount = total;
      BD_DEBUG("constant_buffers: slot {} grew to {} chunk(s) ({} MiB total)",
               s.cursor, total, total * (kUploadChunkSize / (1024 * 1024)));
    }
  }
  up.chunkOffset = off + size;
  ConstantAllocation a;
  a.memory = chunk.mapped + off;
  a.ref = plume::RenderBufferReference(chunk.buffer.get(), off);
  a.gpuAddress = chunk.gpuBase + off;
  a.size = size;
  return a;
}

// kFlushNaN=true also flushes NaN -> +0 in the same pass. Xenos float ALU obeys
// the X360/D3D9 "multiply by zero yields zero" rule (0*NaN=0), so BD's
// degenerate constants (e.g. the bloom/glare 0/0 weight normalization when
// intensity is zero) are harmless on hardware. Our recompiled D3D12 shaders use
// strict IEEE (NaN*0=NaN), so a NaN constant propagates and blackens the
// post-fx composite. No BD shader reinterprets a float constant register as
// int, so the flush cannot corrupt int-encoded data. Branchless (cmov) and
// fused into the byte swap so it adds no extra memory pass.
template <bool kFlushNaN>
void CopyByteSwap32Impl(u8 *dst, u32 guest_va, u32 size) {
  const auto *src = bd::mem::at<const u32>(guest_va);
  if (!src) {
    std::memset(dst, 0, size);
    return;
  }
  const u32 count = size / sizeof(u32);
  auto *out = reinterpret_cast<u32 *>(dst);
  u32 i = 0;
#if defined(__x86_64__) || defined(_M_X64)
  const __m128i abs_mask = _mm_set1_epi32(0x7FFFFFFF);
  const __m128i inf_bits = _mm_set1_epi32(0x7F800000);
  for (; i + 4 <= count; i += 4) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
    v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
    v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    v = _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    if constexpr (kFlushNaN) {
      const __m128i is_nan =
          _mm_cmpgt_epi32(_mm_and_si128(v, abs_mask), inf_bits);
      v = _mm_andnot_si128(is_nan, v);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i *>(out + i), v);
  }
#elif defined(__aarch64__)
  // Android phones: same as above with NEON (4 KiB per draw, hundreds of draws per frame).
  const uint32x4_t abs_mask = vdupq_n_u32(0x7FFFFFFFu);
  const uint32x4_t inf_bits = vdupq_n_u32(0x7F800000u);
  for (; i + 4 <= count; i += 4) {
    uint32x4_t v = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(reinterpret_cast<const uint8_t *>(src + i))));
    if constexpr (kFlushNaN)
      v = vbicq_u32(v, vcgtq_u32(vandq_u32(v, abs_mask), inf_bits));
    vst1q_u32(out + i, v);
  }
#endif
  for (; i < count; ++i) {
#if defined(_MSC_VER)
    const u32 v = _byteswap_ulong(src[i]);
#else
    const u32 v = __builtin_bswap32(src[i]);
#endif
    if constexpr (kFlushNaN) {
      // NaN iff exponent all-1 and mantissa != 0, i.e. |bits| > +Inf bits.
      out[i] = (v & 0x7FFFFFFFu) > 0x7F800000u ? 0u : v;
    } else {
      out[i] = v;
    }
  }
}

void CopyByteSwap32(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<false>(dst, guest_va, size);
}

void CopyByteSwap32FlushNaN(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<true>(dst, guest_va, size);
}

} // namespace

bool TryInit() {
  auto &s = upload_state();
  if (s.ready)
    return true;
  s.cursor = 0;
  for (auto &up : s.frames) {
    up.chunkIndex = 0;
    up.chunkOffset = 0;
    up.peakChunkCount = 0;
  }
  for (auto &slot : s.samplerSlots)
    slot.valid = false;
  s.sharedBound = false;
  RecomputeShadowPcfScale(s);
  s.ready = true;
  return true;
}

void ResetFrame(u32 slot) {
  auto &s = upload_state();
  if (!s.ready)
    return;
  s.cursor = slot;
  ++s.epoch;
  FrameUpload &up = s.frames[slot];
  up.chunkIndex = 0;
  up.chunkOffset = 0;
  RecomputeShadowPcfScale(s);
}

void InvalidateSharedBinding() { upload_state().sharedBound = false; }

// c50.xy is BD's NDC->UV half-scale (0.5 on hw). bd_blur_ps reconstructs its
// sample UV as uv = c50.xy*(ndc+1), so it MUST be 0.5. The guest derives it
// from sceneDim/1280x720*0.5, letting output res and supersampling leak in, and
// the pass oversamples until the source collapses into the top-left. bd_blur_ps
// is the ONLY pixel shader reading c50 as this screen->UV scale (verified
// across all 17 c50-using PS). The rest own c50 as material data, so a blanket
// pin would corrupt them (bd_lightshaft_ps's g_vLightShaftDiffuse -> gray
// god rays). bdCameraRefractionUvScaleHook pins device reg50 at the
// bdCameraRender writers, and this catches blur draws those writers miss.
constexpr u32 kScreenUVScaleRegByteOffset = 50 * 16;
constexpr u64 kBDBlurPSHash = 0xD94E164866C3B9BCull;
void PinScreenUVScaleReg(u8 *block) {
  auto *ps = bd::gpu::state().pipelineState.pixelShader;
  const u64 h = (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
  if (h == kBDBlurPSHash) {
    auto *reg = reinterpret_cast<float *>(block + kScreenUVScaleRegByteOffset);
    reg[0] = 0.5f;
    reg[1] = 0.5f;
  }
}

// The last block uploaded per stage, to reuse it while the game's constants do not change.
// Draws in a row often share a material or an object (the same 4 KiB), and every upload writes
// 4 KiB into write-combined GPU memory: 44 % of the render thread on the Android emulator in a
// match. Reused only within the frame it was written for (its chunk is rewound after that), and
// only for the same raw guest bytes and the same edits (key), so the GPU reads the same values.
// PC: off by default until measured there; svr_reuse_constant_blocks=true turns it on for an A/B run
// (the [perf] line counts blocks uploaded and reused).
#if defined(__ANDROID__)
REXCVAR_DEFINE_BOOL(svr_reuse_constant_blocks, true, "SvR",
                    "Reuse the previous constant block when the game's bytes are unchanged");
#else
REXCVAR_DEFINE_BOOL(svr_reuse_constant_blocks, false, "SvR",
                    "Reuse the previous constant block when the game's bytes are unchanged");
#endif
REXCVAR_DECLARE(bool, wwe13_draw_fast_path);  // svr_resources.cpp: pixel-shader block only
struct StageBlockCache {
  u8 raw[kConstantBlockBytes];
  ConstantAllocation alloc;
  u64 epoch = 0;
  u64 key = 0;
};
std::atomic<u64> g_constant_blocks_uploaded{0}, g_constant_blocks_reused{0};

// Returns the cached block if `raw` and `key` match the last upload of this frame.
bool ReuseBlock(StageBlockCache &cache, const UploadState &s, const u8 *raw, u64 key,
                ConstantAllocation &out, bool enabled) {
  if (!enabled || !raw || cache.epoch != s.epoch || cache.key != key ||
      std::memcmp(cache.raw, raw, kConstantBlockBytes) != 0)
    return false;
  out = cache.alloc;
  g_constant_blocks_reused.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void RememberBlock(StageBlockCache &cache, const UploadState &s, const u8 *raw, u64 key,
                   const ConstantAllocation &alloc, bool enabled) {
  g_constant_blocks_uploaded.fetch_add(1, std::memory_order_relaxed);
  if (!enabled || !raw)
    return;
  std::memcpy(cache.raw, raw, kConstantBlockBytes);
  cache.alloc = alloc;
  cache.epoch = s.epoch;
  cache.key = key;
}

ConstantAllocation UploadVertexShaderConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadVSConstants");
  WWE12_CPU_ZONE(kConstants);
  auto &s = upload_state();
  if (!device_guest)
    return {};
  static StageBlockCache cache;
  const u32 src_va = device_guest + offsetof(D3DDevice, vsFloatConstants);
  const auto *raw = bd::mem::at<const u8>(src_va);
  // The vertex block changes on nearly every draw (world matrices): only the old option tries it.
  const bool reuse = REXCVAR_GET(svr_reuse_constant_blocks);
  ConstantAllocation alloc;
  if (ReuseBlock(cache, s, raw, 0, alloc, reuse))
    return alloc;
  alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32FlushNaN(alloc.memory, src_va, kConstantBlockBytes);
  RememberBlock(cache, s, raw, 0, alloc, reuse);
  return alloc;
}

ConstantAllocation UploadPixelShaderConstants(u32 device_guest, bool zero_dof) {
  BD_CPU_ZONE("UploadPSConstants");
  WWE12_CPU_ZONE(kConstants);
  auto &s = upload_state();
  if (!device_guest)
    return {};
  static StageBlockCache cache;
  const u32 src_va = device_guest + offsetof(D3DDevice, psFloatConstants);
  const auto *raw = bd::mem::at<const u8>(src_va);
  // The edits made after the copy depend on the pixel shader (blur pin) and on zero_dof.
  auto *ps = bd::gpu::state().pipelineState.pixelShader;
  const u64 key = ((ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0) ^ (zero_dof ? 1 : 0);
  const bool reuse = REXCVAR_GET(svr_reuse_constant_blocks) || REXCVAR_GET(wwe13_draw_fast_path);
  ConstantAllocation alloc;
  if (ReuseBlock(cache, s, raw, key, alloc, reuse))
    return alloc;
  alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32FlushNaN(alloc.memory, src_va, kConstantBlockBytes);
  PinScreenUVScaleReg(alloc.memory);
  if (zero_dof)
    std::memset(alloc.memory + 20 * 16, 0, 2 * 16);  // g_f4DOFParam, g_f4DOFParam2
  RememberBlock(cache, s, raw, key, alloc, reuse);
  return alloc;
}

void ConstantBlockStats(u64 &uploaded, u64 &reused) {
  uploaded = g_constant_blocks_uploaded.exchange(0, std::memory_order_relaxed);
  reused = g_constant_blocks_reused.exchange(0, std::memory_order_relaxed);
}

// Per-slot entries of the shared block: slots 0-15 in the base arrays, SvR's 16-31 after them.
#if defined(SVR_NATIVE_RENDERER)
static u32 &Tex2DIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.texture2DIndices[i] : c.texture2DIndicesHi[i - 16];
}
static u32 &Tex3DIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.texture3DIndices[i] : c.texture3DIndicesHi[i - 16];
}
static u32 &TexCubeIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.textureCubeIndices[i] : c.textureCubeIndicesHi[i - 16];
}
static u32 &SamplerIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.samplerIndices[i] : c.samplerIndicesHi[i - 16];
}
#else
static u32 &Tex2DIndex(SharedConstants &c, u32 i) { return c.texture2DIndices[i]; }
static u32 &Tex3DIndex(SharedConstants &c, u32 i) { return c.texture3DIndices[i]; }
static u32 &TexCubeIndex(SharedConstants &c, u32 i) { return c.textureCubeIndices[i]; }
static u32 &SamplerIndex(SharedConstants &c, u32 i) { return c.samplerIndices[i]; }
#endif

ConstantAllocation UploadSharedConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadSharedConstants");
  WWE12_CPU_ZONE(kConstants);
  auto &s = upload_state();
  if (!s.ready)
    return {};
  // bd_anisotropy participates in DecodeFromFetch's output, so a live change
  // must miss the per-slot cache so stale sampler indices are re-resolved.
  const i32 aniso_now = Settings::Get().Anisotropy();

  // vs.textures is authoritative: our SetTexture hook replaces BD's recompiled
  // body, so the engine's per-slot bound-texture shadow (device+0x2FF0+slot*4)
  // and GPU texture fetch constants (device+0x400+slot*0x18) are never written.
  // Video::SetTexture mirrors Xenos semantics (a null bind is ignored, since on
  // hardware it does not rebuild the fetch constant), so vs.textures holds the
  // last real texture per slot, exactly what the GPU would still be sampling.
  auto &vs = bd::gpu::state();
  const auto *device_p = bd::mem::at<const D3DDevice>(device_guest);
  // Rendering above the guest's 720p, the GPU would pick sharper mip levels than the game ever
  // used (and WWE '12's eyes and teeth turn black from the base level): bias by log2 of the
  // bound target's scale, so each texture samples the mips it does on the Xbox.
  float lod_bias = 0.0f;
#if defined(SVR_NATIVE_RENDERER)
  if (device_p && REXCVAR_GET(svr_match_guest_mips)) {
    const float k = SvrSurfaceScale(u32(device_p->renderTargetShadow[0]));
    if (k > 1.0f)
      lod_bias = std::log2(k);
  }
#endif
  for (u32 i = 0; i < bd::gpu::kTextureSlots; ++i) {
    SamplerIndex(s.shared, i) = 0;
    Tex2DIndex(s.shared, i) = bd::gpu::kNullTexture2DDescriptorIndex;
    Tex3DIndex(s.shared, i) = bd::gpu::kNullTexture3DDescriptorIndex;
    TexCubeIndex(s.shared, i) = bd::gpu::kNullTextureCubeDescriptorIndex;

    bd::gpu::GuestTexture *tex = vs.textures[i];
    if (tex && tex->sourceSurface && tex->sourceSurface->texture &&
        tex->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        tex->sourceSurface != vs.render_target &&
        tex->sourceSurface != vs.depth_stencil &&
        tex->sourceSurface->descriptorIndex !=
            bd::gpu::kInvalidDescriptorIndex) {
      tex = tex->sourceSurface;
    }
    if (tex && tex->descriptorIndex != bd::gpu::kInvalidDescriptorIndex) {
      switch (tex->viewDimension) {
      case plume::RenderTextureViewDimension::TEXTURE_3D:
        Tex3DIndex(s.shared, i) = tex->descriptorIndex;
        // X360: a 2D fetch on a 3D resource reads slice 0, so publish the
        // volume as its slice-0 2D view too so tfetch2D samples the base layer.
        if (tex->companion2D && tex->companion2D->descriptorIndex !=
                                    bd::gpu::kInvalidDescriptorIndex) {
          Tex2DIndex(s.shared, i) = tex->companion2D->descriptorIndex;
        }
        break;
      case plume::RenderTextureViewDimension::TEXTURE_CUBE:
        TexCubeIndex(s.shared, i) = tex->descriptorIndex;
        break;
      case plume::RenderTextureViewDimension::TEXTURE_2D:
      case plume::RenderTextureViewDimension::UNKNOWN:
      default:
        Tex2DIndex(s.shared, i) = tex->descriptorIndex;
        // BD static reflection cubes bind as a 2D atlas yet the water/glass
        // shader cube-fetches the slot, so publish the sliced TextureCube
        // companion so tfetchCube resolves a real cube, not the null cube.
        if (tex->companionCube && tex->companionCube->descriptorIndex !=
                                      bd::gpu::kInvalidDescriptorIndex) {
          TexCubeIndex(s.shared, i) = tex->companionCube->descriptorIndex;
        }
        break;
      }

      // X360 stores sampler state in fetchConstants[N].dword[*]. The
      // SetSamplerState_* setters are unhooked and run their recompiled bodies,
      // so the address mode bits there are valid.
      if (device_p) {
        const auto &fc_be = device_p->fetchConstants[i];
        const bool clamp3d =
            tex->viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D;
        auto &sc = s.samplerSlots[i];
        if (sc.valid && sc.clamp3d == clamp3d && sc.aniso == aniso_now && sc.lodBias == lod_bias &&
            std::memcmp(sc.fc, &fc_be, sizeof(sc.fc)) == 0) {
          SamplerIndex(s.shared, i) = sc.sampler;
        } else {
          const u32 fc[6] = {
              u32(fc_be.dword[0]), u32(fc_be.dword[1]), u32(fc_be.dword[2]),
              u32(fc_be.dword[3]), u32(fc_be.dword[4]), u32(fc_be.dword[5]),
          };
          auto desc = DecodeFromFetch(fc);

          // Shell fur volumes encode shell depth in W, and X360-default WRAP
          // wraps a z=0 fetch's second tap to the tip slice and halves density,
          // so force CLAMP to keep both taps on the dense base slice.
          if (clamp3d) {
            desc.addressW = plume::RenderTextureAddressMode::CLAMP;
          }
          desc.mipLODBias = lod_bias;
          const u32 resolved = ResolveSlotLocked(desc);
          std::memcpy(sc.fc, &fc_be, sizeof(sc.fc));
          sc.sampler = resolved;
          sc.aniso = aniso_now;
          sc.lodBias = lod_bias;
          sc.clamp3d = clamp3d;
          sc.valid = true;
          SamplerIndex(s.shared, i) = resolved;
        }
      }
    }
  }

  // Shader bool constants: VS at device+0x2700, PS at device+0x2710, 4 BE
  // dwords each. Shaders branch on BOOL_BIT(n) of a 256-bit register file
  // (VS 0..127, PS 128..255).
  if (device_guest) {
    const auto *device = bd::mem::at<const D3DDevice>(device_guest);
    for (u32 i = 0; i < 4; ++i) {
      s.shared.booleansArr[i] = device ? u32(device->vsBoolConstants[i]) : 0u;
      s.shared.booleansArr[4 + i] =
          device ? u32(device->psBoolConstants[i]) : 0u;
    }
  }

  s.shared.alphaThreshold = bd::gpu::Video::AlphaThreshold();
  s.shared.invRenderScale = 1.0f;
#if defined(SVR_NATIVE_RENDERER)
  if (device_p)
    s.shared.invRenderScale =
        1.0f / std::max(1.0f, SvrSurfaceScale(u32(device_p->renderTargetShadow[0])));
#endif
  s.shared.halfPixelOffsetX = 0.0f;
  s.shared.halfPixelOffsetY = 0.0f;
#if defined(SVR_NATIVE_RENDERER)
  // Host clip space is Vulkan's (pixel centres at +0.5; re:Blue's viewport puts NDC +1 at the top).
  s.shared.screenSpaceScaleX = 1.0f;
  s.shared.screenSpaceScaleY = 1.0f;
  s.shared.screenSpaceOffsetX = 0.0f;
  s.shared.screenSpaceOffsetY = 0.0f;
  // In guest pixels: the game's own viewport (host targets may be scaled up, the conversion to
  // clip space is the same).
  const float gx = device_p ? float(device_p->viewport.X) : 0.0f;
  const float gy = device_p ? float(device_p->viewport.Y) : 0.0f;
  const float gw = device_p ? float(device_p->viewport.Width) : 0.0f;
  const float gh = device_p ? float(device_p->viewport.Height) : 0.0f;
  if (device_guest && gw > 0.0f && gh > 0.0f) {
    const float w = gw;
    const float h = gh;
    // PA_CL_VTE_CNTL (device+0x294C, SetRenderState_ViewportEnable sub_8223EC88: 0x43F on,
    // 0x400 off). With the viewport transform off the vertex shader outputs window pixels.
    const u32 vte = bd::mem::load<u32>(device_guest + 0x294C);
    // PA_SU_VTX_CNTL.pix_center (device+0x29C0 bit 0, SetRenderState_HalfPixelOffset
    // sub_8223F040): clear means Direct3D 9 pixel centres, on integer coordinates.
    const bool d3d9_centres = (bd::mem::load<u32>(device_guest + 0x29C0) & 1u) == 0;
    const float centre = d3d9_centres ? 0.5f : 0.0f;
    if ((vte & 0x3Fu) == 0) {
      s.shared.screenSpaceScaleX = 2.0f / w;
      s.shared.screenSpaceScaleY = -2.0f / h;
      s.shared.screenSpaceOffsetX = (centre - gx) * 2.0f / w - 1.0f;
      s.shared.screenSpaceOffsetY = 1.0f - (centre - gy) * 2.0f / h;
    } else if (d3d9_centres) {
      // Half a pixel right and down, so D3D9-centred geometry samples and covers as on Xenos.
      s.shared.halfPixelOffsetX = 1.0f / w;
      s.shared.halfPixelOffsetY = -1.0f / h;
    }
  }
#endif
  s.shared.swappedTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTexcoords : 0u;
  s.shared.swappedNormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedNormals : 0u;
  s.shared.swappedBinormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBinormals : 0u;
  s.shared.swappedTangents =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTangents : 0u;
  s.shared.swappedBlendWeights =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBlendWeights : 0u;
  s.shared.swappedPositions =
      vs.vertex_declaration ? vs.vertex_declaration->swappedPositions : 0u;
  s.shared.sintTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->sintTexcoords : 0u;

  s.shared.shadowPcfScale = s.shadowPcfScale;
#if defined(REBLUE_D3D11)
  s.shared.depthFlip = vs.viewport.minDepth > vs.viewport.maxDepth ? 1u : 0u;
#elif defined(REBLUE_D3D12) && defined(SVR_NATIVE_RENDERER)
  s.shared.depthFlip = REXCVAR_GET(svr_d3d12_depth_flip) &&
                               vs.viewport.minDepth > vs.viewport.maxDepth
                           ? 1u
                           : 0u;
#endif

  // Viewport extent, not the render target's: the NDC->pixel mapping this
  // cancels is the viewport's. +x/-y = half a pixel right and down.
  s.shared.blitHalfPixelOffsetX =
      vs.viewport.width > 0.0f ? 1.0f / vs.viewport.width : 0.0f;
  s.shared.blitHalfPixelOffsetY =
      vs.viewport.height > 0.0f ? -1.0f / vs.viewport.height : 0.0f;

  // Byte-identical to the block already bound on this command list: the live
  // CBV is still correct, skip the upload and let the caller skip the rebind.
  // SharedConstants padding is zero-initialized and never written, so memcmp
  // is deterministic.
#if defined(REBLUE_D3D11)
  D3D11BindSlots(s.shared);
#endif
  if (s.sharedBound &&
      std::memcmp(&s.shared, &s.lastUploaded, sizeof(SharedConstants)) == 0) {
    return {};
  }

  auto alloc = Allocate(s, sizeof(SharedConstants), kCBVAlignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, &s.shared, sizeof(SharedConstants));
  s.lastUploaded = s.shared;
  s.sharedBound = true;
  return alloc;
}

ConstantAllocation UploadGuestBytesByteSwap32(u32 guest_va, u32 size,
                                              u32 alignment) {
  BD_CPU_ZONE("UploadGuestBytesByteSwap32");
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32(alloc.memory, guest_va, size);
  return alloc;
}

ConstantAllocation UploadGuestBytes(u32 guest_va, u32 size, u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  const auto *src = bd::mem::at<const u8>(guest_va);
  if (!src) {
    BD_WARN("constant_buffers: UploadGuestBytes translate failed for "
            "guest_va={:#x}",
            guest_va);
    return {};
  }
  std::memcpy(alloc.memory, src, size);
  return alloc;
}

ConstantAllocation UploadHostBytes(const void *host_data, u32 size,
                                   u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !host_data || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, host_data, size);
  return alloc;
}

ConstantAllocation AllocateUploadBytes(u32 size, u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !size)
    return {};
  return Allocate(s, size, alignment);
}

u64 UploadEpoch() { return upload_state().epoch; }

} // namespace bd::gpu
