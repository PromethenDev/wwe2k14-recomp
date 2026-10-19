#pragma once

#include <cmath>

namespace wwe2k14::launcher::ui {

// SDL's window display scale includes both content scaling and pixel density. ImGui uses
// logical-window coordinates, so only the remaining content scale belongs in layout and fonts.
inline float ComputeUiScale(float display_scale, float pixel_density) {
  if (display_scale < 0.5f || !std::isfinite(display_scale)) display_scale = 1.0f;
  if (pixel_density < 0.5f || !std::isfinite(pixel_density)) pixel_density = 1.0f;
  const float scale = display_scale / pixel_density;
  return scale >= 0.5f && std::isfinite(scale) ? scale : 1.0f;
}

}  // namespace wwe2k14::launcher::ui
