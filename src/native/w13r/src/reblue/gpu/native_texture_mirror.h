/**
 * @file  gpu/native_texture_mirror.h
 * @brief   Registry and builder for BD native (bdAllocRenderBuffer) content
 *          textures, keyed by guest VA. Native textures are engine-allocated
 *          outside any hooked D3D create API, so HostResourceHeap has no
 *          GuestTexture for them. The bdAllocRenderBuffer hook decodes the
 *          Xenos fetch constant, untiles the DXT data and registers a host BC
 *          mirror here. LoadTexture__vf03 evicts on free.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rex/types.h>

#include "gpu/resources.h"

namespace bd::gpu {

// nullptr when format/dimension unsupported or the build fails. name_va is the
// guest VA of the asset basename (bdAllocRenderBuffer arg0), logged on
// rejection to identify which content textures reblue cannot mirror, or 0 if
// unknown. Call only at guest texture creation: a cached entry at the same VA
// is treated as stale (prior occupant freed via an unhooked path) and rebuilt.
GuestTexture *GetOrCreateNativeMirror(u32 guest_va, u32 name_va = 0);

#if defined(SVR_NATIVE_RENDERER)
// Writes RGBA8 texels (host order, row_pitch bytes per row) into the guest memory of the base
// level of the texture whose header is at guest_va, in its Xbox layout (tiling, endian):
// what an Xbox resolve leaves in memory. k_8_8_8_8 and k_4_4_4_4 only. out_range (optional)
// gets the physical byte range written, or why nothing was (format, size).
bool WriteGuestTextureBase(u32 guest_va, const u8 *rgba, u32 row_pitch, u32 width, u32 height,
                           std::string *out_range = nullptr);
// Debug: the raw bytes of that base level as they are in memory, and its tiled row pitch in
// blocks (0 when linear).
bool ReadGuestTextureBaseRaw(u32 guest_va, std::vector<u8> &raw, u32 &pitch_blocks);
#endif

// Resolves a guest VA to its host GuestTexture (HostResourceHeap object or
// native mirror), or nullptr if neither.
GuestTexture *ResolveGuestTexture(u32 guest_va);

// Queues the host GuestTexture for deferred teardown.
void EvictNativeTexture(u32 guest_va);

// The guest bytes a texture's mirror is built from: its fetch constant (6 dwords) and the base
// level plus stored mips. False when the D3D object doesn't hold a texture fetch constant.
struct NativeMirrorSource {
  u32 fetch[6] = {};
  const u8 *base = nullptr;
  size_t base_size = 0;
  const u8 *mips = nullptr;
  size_t mips_size = 0;
};
bool GetNativeMirrorSource(u32 guest_va, NativeMirrorSource &out);

// One live native texture: its D3DTexture VA and the allocation's sequence
// number. The sequence is what a caller tracking per-instance state keys on,
// since a freed texture's VA can be reused by the next allocation.
struct NativeTextureRef {
  u32 va = 0;
  u64 seq = 0;
};

// The live native textures whose asset name matches, compared as a lowercased
// basename with no directory or extension, so both the full path a texlist
// request logs and the bare stem a preload names find the same entry. Guest
// thread, like everything else here.
std::vector<NativeTextureRef> NativeTexturesByName(std::string_view name);

// Rewrites a live native texture's tiled payload from a whole blob file image
// (the bdAllocRenderBuffer non-DDS layout) and rebuilds the host mirror, so
// every prompt already on screen shows the new texels on its next draw. The
// blob must carry the same dimensions and format the live texture was created
// with, a mismatch is refused, since the physical allocation cannot change
// size underneath the engine.
bool NativeTextureReplace(u32 guest_va, const u8 *blob, size_t size);

// Mip 0's uv sub-rect outside the flat field its corner block carries, for the
// views that fit a whole guest texture into a frame.
struct TextureContent {
  float u0 = 0.0f;
  float v0 = 0.0f;
  float u1 = 1.0f;
  float v1 = 1.0f;

  float Width() const { return u1 - u0; }
  float Height() const { return v1 - v0; }

  // Whole blocks compared against the corner block, since a flat field encodes
  // to the same bytes everywhere. The whole texture back when nothing differs.
  static TextureContent Scan(u32 guest_va);
};

// Free mirrors evicted while frame slot 'slot' was recording. Drained
// post-fence so the descriptor write + free cannot race an in-flight command
// list.
void DrainEvictedNativeTextures(u32 slot);

} // namespace bd::gpu
