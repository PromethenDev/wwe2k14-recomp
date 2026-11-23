// Pure frame-block scheduler for the V17b dirty-translation A/B lane.
#pragma once

#include <cstdint>

namespace wwe2k14::d3d::dtr::ab {

// Parse a bounded decimal block length. A null/empty/zero value disables the schedule.
inline uint32_t ParseBlockFrames(const char* text, bool* invalid) {
  if (invalid) *invalid = false;
  if (!text || !*text) return 0;
  uint64_t value = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') {
      if (invalid) *invalid = true;
      return 0;
    }
    value = value * 10 + uint64_t(*p - '0');
    if (value > 1000000) {
      if (invalid) *invalid = true;
      return 0;
    }
  }
  return uint32_t(value);
}

// A starts with the legacy path; B uses the memo. The schedule is valid only for base mode 1.
constexpr int ModeForFrame(int base_mode, uint32_t block_frames, uint32_t frame) {
  if (base_mode != 1 || !block_frames) return base_mode;
  return ((frame / block_frames) & 1u) ? 1 : 0;
}

}  // namespace wwe2k14::d3d::dtr::ab
