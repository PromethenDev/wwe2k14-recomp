/**
 * @file    gpu/shaders/shader_cache.h
 * @brief   Compiled shader cache symbols emitted by XenosRecomp into
 *          generated/shader_cache.cpp. Referenced by unqualified name from
 *          the generated cpp, so this must stay in the global namespace with
 *          the emitter's exact field order and types. The build-time
 *          reblue_prelink compiles it without the rex SDK on its include
 *          path, hence <cstdint> rather than the rex aliases.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace bd::gpu {
struct GuestShader;
}

struct ShaderCacheEntry {
  const uint64_t hash;
  const uint32_t dxilOffset;
  const uint32_t dxilSize;
  const uint32_t spirvOffset;
  const uint32_t spirvSize;
  const uint32_t specConstantsMask;
  bd::gpu::GuestShader *guestShader;
};

#if defined(WWE13_RUNTIME_SHADERS)
// WWE 2K15: no shader cache is linked into the executable (it would be game-derived data). The
// player's own shaders are converted at run time and kept beside the exe
// (src/native/wwe13_shader_cache.cpp); these replace the generated tables below.
ShaderCacheEntry *Wwe13FindShaderEntry(uint64_t hash);
// True when dxcompiler.dll loads and gives a compiler instance (checked at startup: without it
// the game's shaders can't be converted and the picture stays black).
bool Wwe13DxcUsable();
// The smol-v SPIR-V of an entry (entry->spirvSize bytes).
const uint8_t *Wwe13ShaderSpirv(const ShaderCacheEntry *entry);
// A container the cache doesn't have yet: converted in the background; when it is ready, `shader`
// gets its entry (until then the renderer skips the draws that need it).
void Wwe13RequestShader(uint64_t hash, const uint8_t *container, uint32_t size,
                          bd::gpu::GuestShader *shader);
// Blocks until `shader` has its entry (its conversion finished) or failed, at most timeout_ms.
// Draws that build textures (attire composition, damage maps) run once: skipping them while their
// shader converts left wrestlers white for good.
void Wwe13WaitShader(bd::gpu::GuestShader *shader, uint32_t timeout_ms);
#endif

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
