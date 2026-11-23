/**
 * @file    gpu/hooks/shader.cpp
 * @brief   Guest hooks that create shader and vertex declaration objects,
 *          including BD's runtime-HLSL blit shaders.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <cstring>
#include <mutex>

#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/host_resource_heap.h"
#include "gpu/vertex_declaration.h"
#if defined(SVR_NATIVE_RENDERER)
#include "svr_hook.h"
#include "svr_resources.h"
#endif

#if defined(REBLUE_D3D12)
#else
#endif

namespace {

// Replaces the engine's declaration struct with a HostResourceHeap
// GuestVertexDeclaration so SetVertexDeclaration_hook can FromGuest it. The
// engine only stores and forwards the pointer, so the layout need not match.
bd::gpu::GuestVertexDeclaration *hcgCreateVertexDeclaration_hook(
    rex::MappedPtr<bd::gpu::GuestVertexElement> elements) {
  if (!elements)
    return nullptr;
  return bd::gpu::CreateVertexDeclaration(
      static_cast<bd::gpu::GuestVertexElement *>(elements));
}

bd::gpu::GuestShader *hcgCreateVertexShaderResource_hook(mapped_u32 function) {
  if (!function)
    return nullptr;
  auto *shader = bd::gpu::CreateShader(static_cast<const be_u32 *>(function),
                                       bd::gpu::ResourceType::VertexShader);
  return shader;
}

bd::gpu::GuestShader *hcgCreatePixelShaderResource_hook(mapped_u32 function) {
  if (!function)
    return nullptr;
  auto *shader = bd::gpu::CreateShader(static_cast<const be_u32 *>(function),
                                       bd::gpu::ResourceType::PixelShader);
  return shader;
}

} // namespace

#if defined(SVR_NATIVE_RENDERER) && defined(WWE12_NO_CREATE_SHADERS)
// WWE '12 builds its shader and declaration objects itself (no Create* calls): the renderer
// discovers them when first bound (SvrLookupShader / SvrLookupVertexDeclaration in
// src/native/svr_resources.cpp).
#elif defined(SVR_NATIVE_RENDERER)
// SvR: the game's Create functions make its real D3D objects; the renderer pairs each with a host
// shader / declaration (see src/native/svr_hook.h).
namespace {
void SvrVertexDeclarationCreated(PPCContext &args, u32 decl, u8 *) {
  bd::gpu::SvrOnVertexDeclarationCreated(decl, args.r3.u32);
}
void SvrVertexShaderCreated(PPCContext &args, u32 shader, u8 *) {
  bd::gpu::SvrOnShaderCreated(shader, args.r3.u32, false);
}
void SvrPixelShaderCreated(PPCContext &args, u32 shader, u8 *) {
  bd::gpu::SvrOnShaderCreated(shader, args.r3.u32, true);
}
}  // namespace
SVR_HOOK_RESULT(D3DDevice_CreateVertexDeclaration, SvrVertexDeclarationCreated);
SVR_HOOK_RESULT(D3DDevice_CreateVertexShader, SvrVertexShaderCreated);
SVR_HOOK_RESULT(D3DDevice_CreatePixelShader, SvrPixelShaderCreated);
#else
REX_HOOK(D3DDevice_CreateVertexDeclaration, hcgCreateVertexDeclaration_hook);
REX_HOOK(D3DDevice_CreateVertexShader, hcgCreateVertexShaderResource_hook);
REX_HOOK(D3DDevice_CreatePixelShader, hcgCreatePixelShaderResource_hook);
#endif
