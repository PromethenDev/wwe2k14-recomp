/**
 * @file    gpu/shaders/shader_linker.h
 * @brief   Runtime DXC link step for spec constant shaders. Shaders in the
 *          DXIL cache that carry a non-zero specConstantsMask ship as DXIL
 *          libraries with an unresolved g_SpecConstants() export. D3D12
 *          cannot use a library as a vs or ps directly, so it must first be
 *          linked against a concrete g_SpecConstants() implementation.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>
#include <vector>

namespace bd::gpu {

// Link a spec constant DXIL library against a concrete g_SpecConstants() value.
// isPixelShader picks the target profile (ps_6_0 vs vs_6_0). Empty on failure.
std::vector<u8> LinkSpecConstant(const u8 *libraryDxil, u32 libraryDxilSize,
                                 bool isPixelShader, u32 specConstants);

} // namespace bd::gpu
