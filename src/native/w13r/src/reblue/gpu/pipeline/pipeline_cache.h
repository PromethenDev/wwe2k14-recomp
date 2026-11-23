/**
 * @file    gpu/pipeline/pipeline_cache.h
 * @brief   XXH3-keyed cache of native pipelines created from
 *          PipelineState.
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

#include "gpu/pipeline/pipeline_state.h"

namespace bd::gpu {

// Zero dead fields so functionally identical pipelines hash identically. A miss
// only inflates the cache, never breaks correctness.
void SanitizePipelineState(PipelineState &state);

// Caller must Sanitize first.
u64 HashPipelineState(const PipelineState &state);

} // namespace bd::gpu

namespace bd::gpu {

// Returns a native pipeline matching 'state' (Sanitize first), nullptr on
// failure. out_created is set true on a cache miss (built on the calling
// thread), so the recorder can flag draws whose PSO was not precached.
plume::RenderPipeline *GetOrCreatePipeline(const PipelineState &state,
                                           bool *out_created = nullptr);

size_t PipelineCacheSize();

} // namespace bd::gpu
