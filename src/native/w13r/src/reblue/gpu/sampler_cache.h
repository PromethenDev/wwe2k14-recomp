/**
 * @file    gpu/sampler_cache.h
 * @brief   Per-draw sampler decode + bindless sampler heap cache.
 *
 *   One bindless entry per unique sampler configuration, decoded from the X360
 *   GPUTEXTURE_FETCH_CONSTANT at D3DDevice::fetchConstants[N].
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

namespace bd::gpu {

// Decode sampler bits from a 6-dword X360 fetch constant (host endianness).
plume::RenderSamplerDesc DecodeFromFetch(const u32 fc[6]);

// Returns the cached sampler's slot in sampler_descriptor_set, or 0 (reserved
// default slot) on miss or heap full. Caller holds state().mutex (the miss
// path allocates from the bindless sampler heap).
u32 ResolveSlotLocked(const plume::RenderSamplerDesc &desc);

} // namespace bd::gpu
