/**
 * @file    gpu/shaders/shader_constants.h
 * @brief   Spec constant bits, named for host code. The values come from the
 *          recompiler's shader_common.h, which the host HLSL includes too.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

#define SHADER_COMMON_SPEC_CONSTANTS_ONLY
#include <shader_common.h>

namespace bd::gpu {
// The only two the REBLUE_RECOMP recompiler emits, the rest are Unleashed's.
inline constexpr u32 kSpecConstantR11G11B10Normal =
    SPEC_CONSTANT_R11G11B10_NORMAL;
inline constexpr u32 kSpecConstantAlphaTest = SPEC_CONSTANT_ALPHA_TEST;
inline constexpr u32 kSpecConstantDhen3nNormal = SPEC_CONSTANT_DHEN3N_NORMAL;
} // namespace bd::gpu
