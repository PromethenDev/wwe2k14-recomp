/**
 * @file    gpu/physical_buffers.h
 * @brief   Registry bridging engine-owned D3DVertexBuffer/D3DIndexBuffer struct
 *          VAs (asset-loaded geometry that bypasses CreateVertex/IndexBuffer)
 * to host GuestBuffers. Owns its own mutex, separate from VideoState.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <memory>

#include <rex/types.h>

namespace plume {
struct RenderBuffer;
}

namespace bd::gpu {

struct GuestBuffer;
enum class ResourceType : u32;

// For an engine-owned D3DIndexBuffer / D3DVertexBuffer struct VA that missed
// HostResourceHeap::FromGuest. Bootstrap rebuilds and registers a host
// GuestBuffer from a fully initialized X360 struct, for buffers no
// creation-time registration ever saw (asset-loaded meshes patched by an
// unhooked loader).
GuestBuffer *FindPhysicalBufferByStruct(u32 struct_va);
GuestBuffer *AdoptPhysicalBuffer(u32 struct_va,
                                               ResourceType rtype);

// FindPhysicalBufferByStruct, then AdoptPhysicalBuffer. Used by
// every draw/lock hook that binds a physical VB/IB by its engine-owned VA.
GuestBuffer *ResolveGuestBufferVa(u32 va, ResourceType rtype);

// Free the plume buffers retired by the registry's stale-reuse refresh. Runs
// from the per-slot post-fence drain, so no in-flight list references them.
void DrainBufferGraveyard(u32 slot);

// Parks a host buffer until the current frame slot's fence; DrainBufferGraveyard then scrubs any
// binding naming it and frees it. SvR geometry copies (src/native/svr_geometry.cpp) retire here.
void RetirePhysicalBuffer(std::unique_ptr<plume::RenderBuffer> buffer);

} // namespace bd::gpu
