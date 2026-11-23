/**
 * @file    gpu/byte_swap.h
 * @brief   Per-element byte swap for guest big-endian buffer uploads.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/memory/utils.h>
#include <rex/types.h>

namespace bd::gpu {

// Byte-swaps 'bytes' worth of elements from src (guest big-endian) into dst
// (host little-endian). element_size == 2 swaps per u16 (index buffers), and
// any other value swaps per u32 (vertex buffers, dwords).
inline void ByteSwapElements(void *dst, const void *src, u32 bytes,
                             u32 element_size) {
  if (element_size == 2) {
    rex::memory::copy_and_swap(static_cast<u16 *>(dst),
                               static_cast<const u16 *>(src), bytes / 2);
  } else {
    rex::memory::copy_and_swap(static_cast<u32 *>(dst),
                               static_cast<const u32 *>(src), bytes / 4);
  }
}

} // namespace bd::gpu
