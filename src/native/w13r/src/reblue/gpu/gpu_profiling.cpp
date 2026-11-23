/**
 * @file    gpu/gpu_profiling.cpp
 * @brief   Tracy D3D12 profiler context and command list plumbing.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/gpu_profiling.h"

#if defined(REXGLUE_ENABLE_PROFILING) && defined(REBLUE_D3D12)

namespace bd::gpu {

namespace {
TracyD3D12Ctx s_ctx = nullptr;
ID3D12GraphicsCommandList *s_cmd = nullptr;
} // namespace

void InitGPUProfiler(ID3D12Device *device, ID3D12CommandQueue *queue) {
  // Leaving s_ctx null when the profiler was never started is what keeps the
  // GPU zones and the per-frame collect off the un-started profiler.
  if (!TracyIsStarted)
    return;
  s_ctx = TracyD3D12Context(device, queue);
  TracyD3D12ContextName(s_ctx, "D3D12", 5);
}

TracyD3D12Ctx GpuProfilerCtx() { return s_ctx; }

void SetGPUProfilerCommandList(ID3D12GraphicsCommandList *cmd) { s_cmd = cmd; }

ID3D12GraphicsCommandList *GpuProfilerCommandList() { return s_cmd; }

} // namespace bd::gpu

#endif
