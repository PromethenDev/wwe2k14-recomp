/**
 * @file    gpu/constant_buffers.h
 * @brief   Shader constant upload path.
 *
 *   VS f4 constants at device+0x700, PS f4 at device+0x1700, SharedConstants
 *   bound through the main RenderPipelineLayout's three root descriptors
 *   (b0/b1/b2, space4).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

#include <plume_render_interface.h>

namespace bd::gpu {

// Mirror of the SharedConstants cbuffer (b2, space4). Layout matches the
// packoffset(c<N>) emit in the recompiled shader prelude, so reordering or
// resizing means regenerating every shader in the DXIL cache.
struct SharedConstants {
  u32 texture2DIndices[16]{};   // c0..c3,  bytes 0..63
  u32 texture3DIndices[16]{};   // c4..c7,  bytes 64..127
  u32 textureCubeIndices[16]{}; // c8..c11, bytes 128..191
  u32 samplerIndices[16]{};     // c12..c15, bytes 192..255
  u32 booleansArr[8]{};         // c16, c17 (g_BooleansArr[2]) bytes 256..287
  u32 swappedTexcoords{};       // c18.x, byte 288
  float halfPixelOffsetX{};     // c18.y, byte 292
  float halfPixelOffsetY{};     // c18.z, byte 296
  float alphaThreshold{};       // c18.w, byte 300
  u32 swappedNormals{};         // c19.x, byte 304
  u32 swappedBinormals{};       // c19.y, byte 308
  u32 swappedTangents{};        // c19.z, byte 312
  u32 swappedBlendWeights{};    // c19.w, byte 316
  u32 swappedPositions{};       // c20.x, byte 320
  // Bit N set when TEXCOORD<N> is bound R16G16(B16A16)_UINT, and the shader's
  // sintTexcoord() sign-extends to recover the X360 raw-int-as-float vfetch.
  u32 sintTexcoords{}; // c20.y, byte 324
  // Sun shadow PCF tap scale (g_ShadowPcfScale). The kernel's tap offsets are
  // shader literals in 1/1024-of-the-map UV units, so their world footprint
  // grows with the bd_shadow_distance coverage box, so the recompiler
  // multiplies each by this to hold the penumbra constant in world space.
  // max(1/distance, 1024/dimension) never scales a tap below one map texel.
  float shadowPcfScale{1.0f}; // c20.z, byte 328
  // Guest pixels per host pixel of the bound target (g_InvRenderScale): recompiled pixel shaders
  // scale vPos by it, as host targets may be a multiple of the guest's size.
  float invRenderScale{1.0f}; // c20.w, byte 332
  // Half-pixel NDC nudge read only by the substituted 2D blit VS
  float blitHalfPixelOffsetX{}; // c21.x, byte 336
  float blitHalfPixelOffsetY{}; // c21.y, byte 340
  // 1 when the viewport's depth range is reversed (MinZ > MaxZ): wwe13_d3d11.exe (D3D11 refuses it)
  // and wwe13_d3d12.exe (svr_d3d12_depth_flip) swap the viewport and every vertex shader flips z
  // instead (g_SvrDepthFlip), as WWE '12 PC does.
  u32 depthFlip{};              // c21.z, byte 344
  u32 _pad_c21_w{};             // c21.w, byte 348
#if defined(SVR_NATIVE_RENDERER)
  // SvR samples fetch slots 16-31 too; the SvR XenosRecomp patch puts their indices after the
  // shared block (svrTextureIndexOffset / svrSamplerIndexOffset, byte 384).
  // g_ScreenSpace (c22): oPos.xy = oPos.xy * scale + offset * w in every vertex shader. Identity,
  // or window pixels to clip space for draws made with SvR's viewport transform off.
  float screenSpaceScaleX{1.0f};   // c22.x, byte 352
  float screenSpaceScaleY{1.0f};   // c22.y, byte 356
  float screenSpaceOffsetX{};      // c22.z, byte 360
  float screenSpaceOffsetY{};      // c22.w, byte 364
  u32 _pad_c23[4]{};               // bytes 368..383
  u32 texture2DIndicesHi[16]{};    // c24..c27, bytes 384..447
  u32 texture3DIndicesHi[16]{};    // c28..c31, bytes 448..511
  u32 textureCubeIndicesHi[16]{};  // c32..c35, bytes 512..575
  u32 samplerIndicesHi[16]{};      // c36..c39, bytes 576..639
#endif
};
#if defined(SVR_NATIVE_RENDERER)
static_assert(offsetof(SharedConstants, texture2DIndicesHi) == 384);
static_assert(offsetof(SharedConstants, samplerIndicesHi) == 576);
static_assert(sizeof(SharedConstants) == 640);
#else
static_assert(sizeof(SharedConstants) == 352);
#endif

// memory stays valid until ResetFrame().
struct ConstantAllocation {
  u8 *memory = nullptr;
  plume::RenderBufferReference ref{};
  u64 gpuAddress = 0;
  u32 size = 0;
};

bool TryInit();

// Rewind a frame slot's upload chunks. Caller must have awaited that slot's
// GPU fence
// (DrainSlot), never while in flight.
void ResetFrame(u32 slot);

// Byte-swap VS/PS float constants from guest device+0x700 / device+0x1700 into
// the upload heap.
ConstantAllocation UploadVertexShaderConstants(u32 device_guest);
// zero_dof: clear g_f4DOFParam/g_f4DOFParam2 (depth of field off, wwe12_dof).
ConstantAllocation UploadPixelShaderConstants(u32 device_guest, bool zero_dof = false);
// Blocks uploaded / reused since the last call (reuse: Android, see constant_buffers.cpp).
void ConstantBlockStats(u64 &uploaded, u64 &reused);

// Rebuilt from live guest state every draw: sampler fetch constants and bool
// constants come from unhooked recompiled code, so there is no dirty signal.
// size == 0 means byte-identical to what is already bound on this command list
// and the caller can skip the upload + root rebind.
ConstantAllocation UploadSharedConstants(u32 device_guest);

// Root bindings do not survive begin(), so this drops the 'shared CB still
// bound' reuse gate. Call on every command list reset.
void InvalidateSharedBinding();

#if defined(REBLUE_D3D11)
// wwe13_d3d11.exe (gpu/d3d11_slots.cpp): the bindless entries of the shared block bound to
// registers before a draw, and their swizzles written in their place.
void D3D11BindSlots(SharedConstants &c);
void D3D11BindHostSlot(u32 texture_index, u32 sampler_index);
void D3D11ForgetSlots();
#endif

// alignment defaults to 4 (the upload buffer is WRITE_COMBINE), and CBV callers
// pass 256.
ConstantAllocation UploadGuestBytesByteSwap32(u32 guest_va, u32 size,
                                              u32 alignment = 4);

// Verbatim, no swap, for texture pixel data.
ConstantAllocation UploadGuestBytes(u32 guest_va, u32 size, u32 alignment = 4);

// For pixel data already untiled to host staging.
ConstantAllocation UploadHostBytes(const void *host_data, u32 size,
                                   u32 alignment = 4);

// Upload ring space for the caller to fill (SvR geometry copies): same lifetime as the uploads
// above, so only bind it while UploadEpoch() still returns the value it was allocated under.
ConstantAllocation AllocateUploadBytes(u32 size, u32 alignment = 4);
u64 UploadEpoch();

} // namespace bd::gpu
