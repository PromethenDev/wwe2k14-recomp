#pragma once

#include <cstdint>

namespace wwe2k14::d3d::efa4 {

// Mode 1 stays native; mode 2 repeats 600 native frames followed by 600 translated frames.
constexpr bool NativeActive(uint32_t mode, uint64_t frame) {
  if (mode == 1) return true;
  if (mode != 2 || frame == 0) return false;
  return ((frame - 1) % 1200) < 600;
}

}  // namespace wwe2k14::d3d::efa4
