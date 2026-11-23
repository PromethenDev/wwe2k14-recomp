// Optional weak-PC visual reductions. Defaults preserve the full picture.
#pragma once

#include <cstdint>
#include <cstring>

namespace wwe2k14::d3d::weakpc {

inline constexpr uint64_t kCrowdPixelShader = 0xF5C9EF1CFA228C6Eull;
inline constexpr uint64_t kF1ShadowConsumerLinear = 0x246729150A48CB87ull;
inline constexpr uint64_t kF1ShadowConsumerPoint = 0xF514A1BD539FEEADull;
inline constexpr uint64_t kShadowPassShaders[] = {0xACF4C56B7294FFFCull, 0xE17C541F03AF4646ull,
                                                   0x7A0DFD92C5D0ADF3ull, 0x2FC3888D607DEB97ull};

inline int ParseCrowdMode(const char* value) {
  if (value && std::strcmp(value, "0") == 0) return 0;
  if (value && std::strcmp(value, "2") == 0) return 2;
  return 1;
}

inline bool ParseShadowsEnabled(const char* value) { return !value || std::strcmp(value, "0") != 0; }

struct CrowdDrawCadence {
  uint32_t frame = ~uint32_t{0};
  uint32_t draws_in_frame = 0;
};

// Mode 0 removes every draw with the verified crowd PS. Mode 2 retains the first, third, ... such draw each frame.
inline bool DropCrowdDraw(int mode, uint64_t ps_key, uint32_t frame, CrowdDrawCadence* cadence) {
  if (ps_key != kCrowdPixelShader) return false;
  if (mode == 0) return true;
  if (mode != 2 || !cadence) return false;
  if (cadence->frame != frame) {
    cadence->frame = frame;
    cadence->draws_in_frame = 0;
  }
  return (cadence->draws_in_frame++ & 1u) != 0;
}

// Fixed16 target class from U1's skip test; `fixed16_format` is true only for RGBA16S or RG16S.
inline bool IsFixed16ShadowTarget(uint32_t width, uint32_t height, bool depth, bool fixed16_format) {
  return !depth && width == 560 && height == 512 && fixed16_format;
}

inline bool IsShadowPassPixelShader(uint64_t ps_key) {
  for (uint64_t key : kShadowPassShaders)
    if (key == ps_key) return true;
  return false;
}

// Receiver bindings are matched only on evidence-backed D3D fetch base/slot keys. F1's fixed16 consumers are
// 1FA00000 at slot 0 with the two keys above; G35 records 1FA00000 and 1F600000 at slot 12 and 1F800000 at slot 0.
// G35's receiver PS key was not captured, so those bindings intentionally use the observed base+slot pair only.
inline bool IsShadowInput(uint64_t ps_key, uint32_t slot, uint32_t fetch_base) {
  if (slot == 0 && fetch_base == 0x1FA00000u &&
      (ps_key == kF1ShadowConsumerLinear || ps_key == kF1ShadowConsumerPoint))
    return true;
  if (slot == 12 && (fetch_base == 0x1FA00000u || fetch_base == 0x1F600000u)) return true;
  if (slot == 0 && fetch_base == 0x1F800000u) return true;
  return false;
}

}  // namespace wwe2k14::d3d::weakpc
