/**
 * @file    gpu/shaders/linked_shader_cache.h
 * @brief   Build-time pre-linked spec constant shader variants, emitted by
 *          reblue_prelink into generated/linked_shader_cache.cpp. Referenced
 *          by unqualified name from the generated cpp, so this must stay in
 *          the global namespace.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

struct LinkedShaderCacheEntry {
  const u64 hash;          // == ShaderCacheEntry::hash
  const u32 specConstants; // masked g_SpecConstants() value
  const u32 dxilOffset;    // into the decompressed linked cache
  const u32 dxilSize;
};

// Sorted by (hash, specConstants) for binary search.
extern LinkedShaderCacheEntry g_linkedShaderCacheEntries[];
extern const size_t g_linkedShaderCacheEntryCount;

// miniz (deflate) compressed concatenation of the linked DXIL blobs.
extern const u8 g_compressedLinkedDxilCache[];
extern const size_t g_linkedDxilCacheCompressedSize;
extern const size_t g_linkedDxilCacheDecompressedSize;
