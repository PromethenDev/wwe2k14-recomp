// Frame-block A/B schedule used only by the V17c lane candidate.
#pragma once

#include <cstdint>

namespace wwe2k14::d3d::v17c_ab {

namespace schedule {
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

// Phase A is the legacy path; phase B enables all three V17c worker levers.
constexpr int ModeForFrame(uint32_t block_frames, uint32_t frame) {
  return block_frames ? int((frame / block_frames) & 1u) : -1;
}
}  // namespace schedule

// -1 when WWE2K14_D3D_V17C_AB is absent, invalid, or missing one of the three base opt-ins.
bool Enabled();
// Returns -1 when disabled, otherwise the phase for this frame and logs each block once.
int ModeForFrame(uint32_t frame);

}  // namespace wwe2k14::d3d::v17c_ab
