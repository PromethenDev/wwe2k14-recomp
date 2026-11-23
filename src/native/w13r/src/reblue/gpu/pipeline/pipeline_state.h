/**
 * @file    gpu/pipeline/pipeline_state.h
 * @brief   Hashable draw pipeline state + per-draw dirty flags (the host
 *          renderer's mirror of the guest device's draw intent).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>
#include <type_traits>

#include <plume_render_interface.h>

namespace bd::gpu {

struct GuestShader;
struct GuestVertexDeclaration;

// pack(1): PipelineState is hashed by raw bytes (XXH3_64bits over sizeof),
// so padding would leak undefined bytes into the hash.
#pragma pack(push, 1)
struct PipelineState {
  GuestShader *vertexShader = nullptr;
  GuestShader *pixelShader = nullptr;
  GuestVertexDeclaration *vertexDeclaration = nullptr;
  bool instancing = false;
  bool zEnable = true;
  bool zWriteEnable = true;
  bool stencilEnable = false;
  bool stencilTwoSided = false;
  plume::RenderBlend srcBlend = plume::RenderBlend::ONE;
  plume::RenderBlend destBlend = plume::RenderBlend::ZERO;
  plume::RenderCullMode cullMode = plume::RenderCullMode::NONE;
  plume::RenderFillMode fillMode = plume::RenderFillMode::SOLID;
  plume::RenderFrontFace frontFace = plume::RenderFrontFace::CLOCKWISE;
  plume::RenderComparisonFunction zFunc = plume::RenderComparisonFunction::LESS;
  plume::RenderComparisonFunction stencilFunc =
      plume::RenderComparisonFunction::ALWAYS;
  plume::RenderStencilOp stencilFail = plume::RenderStencilOp::KEEP;
  plume::RenderStencilOp stencilZFail = plume::RenderStencilOp::KEEP;
  plume::RenderStencilOp stencilPass = plume::RenderStencilOp::KEEP;
  plume::RenderComparisonFunction stencilFuncCCW =
      plume::RenderComparisonFunction::ALWAYS;
  plume::RenderStencilOp stencilFailCCW = plume::RenderStencilOp::KEEP;
  plume::RenderStencilOp stencilZFailCCW = plume::RenderStencilOp::KEEP;
  plume::RenderStencilOp stencilPassCCW = plume::RenderStencilOp::KEEP;
  u8 stencilMask = 0xFFu;
  u8 stencilWriteMask = 0xFFu;
  u8 stencilRef = 0;
  bool alphaBlendEnable = false;
  plume::RenderBlendOperation blendOp = plume::RenderBlendOperation::ADD;
  float slopeScaledDepthBias = 0.0f;
  i32 depthBias = 0;
  plume::RenderBlend srcBlendAlpha = plume::RenderBlend::ONE;
  plume::RenderBlend destBlendAlpha = plume::RenderBlend::ZERO;
  plume::RenderBlendOperation blendOpAlpha = plume::RenderBlendOperation::ADD;
  u32 colorWriteEnable = static_cast<u32>(plume::RenderColorWriteEnable::ALL);
  plume::RenderPrimitiveTopology primitiveTopology =
      plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  u8 vertexStrides[16]{};
  plume::RenderFormat renderTargetFormat{};
  plume::RenderFormat depthStencilFormat{};
  plume::RenderSampleCounts sampleCount = plume::RenderSampleCount::COUNT_1;
  bool enableAlphaToCoverage = false;
  u32 specConstants = 0;
  // When set, the pipeline cache swaps the guest PS for occlusion_count_ps.
  // Hashed so the counting variant is its own PSO. Off for normal draws.
  bool occlusionCounting = false;
  // Dead field, always false, kept only to hold the PSO raw-byte hash / CSV
  // schema stable. Drop on the next cache regen.
  bool enhancedDOF = false;
};
#pragma pack(pop)

// PipelineState is hashed by raw bytes (XXH3 over sizeof) and embedded by value
// into the PSO cache, so it must stay a trivially copyable POD. A sizeof
// change tripwire lives next to the hash in the pipeline cache. sizeof/padding
// drift is itself safe (the on-disk capture is a field-named CSV plus a
// designated-initializer header, not a byte image), but any field
// add/remove/reorder must be matched in kCSVHeader/CsvRow and in the cache
// and template generators, then the generated caches regenerated.
static_assert(std::is_trivially_copyable_v<PipelineState>,
              "PipelineState must stay trivially copyable (raw-byte hashed).");

struct DirtyStates {
  bool renderTargetAndDepthStencil;
  bool viewport;
  bool pipelineState;
  bool depthBias;
  bool scissorRect;
  bool vertexShaderConstants;
  u8 vertexStreamFirst;
  u8 vertexStreamLast;
  bool indices;
  bool pixelShaderConstants;

  explicit DirtyStates(bool value)
      : renderTargetAndDepthStencil(value), viewport(value),
        pipelineState(value), depthBias(value), scissorRect(value),
        vertexShaderConstants(value), vertexStreamFirst(value ? 0 : 255),
        vertexStreamLast(value ? 15 : 0), indices(value),
        pixelShaderConstants(value) {}
};

} // namespace bd::gpu
