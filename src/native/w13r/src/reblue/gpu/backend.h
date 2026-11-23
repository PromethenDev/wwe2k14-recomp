/**
 * @file    gpu/backend.h
 * @brief   The constants that differ between the D3D12 and Vulkan backends.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Kept out of gpu/gpu.h: including this is what makes a TU backend-specific.
 */
#pragma once

#if defined(REBLUE_COMMON_TU)
#error "backend-conditional: move this TU to reblue_backend_only in src/CMakeLists.txt"
#endif

#include <rex/types.h>

#include <plume_render_interface.h>

namespace bd::gpu {

// REBLUE_D3D12 builds are D3D12 only, every other build Vulkan only. g_vulkan
// is constexpr so the other backend's code folds away entirely. The build emits
// only the active backend's host-shader blobs (see cmake/shaders.cmake).
#if defined(REBLUE_D3D11)
// wwe13_d3d11.exe also defines REBLUE_D3D12 (same layouts); its shaders are DXBC (as WWE '12 PC).
inline constexpr bool g_vulkan = false;
inline constexpr plume::RenderShaderFormat kHostShaderFormat =
    plume::RenderShaderFormat::DXBC;
#define REBLUE_BLOB_SYMBOL(name) g_##name##_dxbc
#elif defined(REBLUE_D3D12)
inline constexpr bool g_vulkan = false;
inline constexpr plume::RenderShaderFormat kHostShaderFormat =
    plume::RenderShaderFormat::DXIL;
#define REBLUE_BLOB_SYMBOL(name) g_##name##_dxil
#else
inline constexpr bool g_vulkan = true;
inline constexpr plume::RenderShaderFormat kHostShaderFormat =
    plume::RenderShaderFormat::SPIRV;
#define REBLUE_BLOB_SYMBOL(name) g_##name##_spirv
#endif

// MoltenVK-specific renderer behavior must not leak into native Vulkan builds.
// CMake defines REBLUE_MVK only for the Apple Vulkan target.
#if defined(REBLUE_MVK)
inline constexpr bool g_mvk = true;
static_assert(g_vulkan);
#else
inline constexpr bool g_mvk = false;
#endif

// Swap chain (screen) format. Android surfaces offer RGBA8 only (WWE '12 Android port).
#if defined(__ANDROID__)
inline constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::R8G8B8A8_UNORM;
#else
inline constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::B8G8R8A8_UNORM;
#endif

#define REBLUE_SHADER_BLOB(name)                                               \
  REBLUE_BLOB_SYMBOL(name), sizeof(REBLUE_BLOB_SYMBOL(name))

// One shared VERTEX|PIXEL push constant range, since VUID 00292 lets a stage
// appear in only one: guest addresses at [0,24), copy helper block at [24,40).
#if defined(REBLUE_D3D12)
inline constexpr u32 kCopyPushConstantRangeIndex = 0;
inline constexpr u32 kCopyPushConstantByteOffset = 0;
#else
inline constexpr u32 kGuestPushConstantRangeIndex = 0;
inline constexpr u32 kCopyPushConstantRangeIndex = 0;
inline constexpr u32 kCopyPushConstantByteOffset = 24;
inline constexpr u32 kOcclusionDescriptorSetIndex = 4;
#endif

} // namespace bd::gpu
