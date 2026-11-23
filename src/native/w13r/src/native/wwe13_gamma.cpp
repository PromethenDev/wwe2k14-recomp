/**
 * @file        native/wwe13_gamma.cpp
 * @brief       The game's brightness setting for the native renderer's present pass.
 *
 * WWE 2K15's brightness option reshapes the display gamma ramp the game uploads through its D3D
 * library (DC_LUT registers). The Xenos path applies that ramp at swap; the native renderer's
 * guest GPU is the null backend, whose shared CommandProcessor still records the 256-entry table,
 * so the present pass reads it from there (issue #7: brightness stuck at maximum; it also made
 * the crowd look about twice as bright as on the Xenos path). The ramps the game builds are close
 * to scale * x^gamma (default setting: 0.86 * x^1.3), which the present shader applies.
 */

#include <cmath>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

namespace {
// The table accessor is protected; a pointer to it taken through a derived class is a plain
// CommandProcessor member pointer.
struct GammaTableAccess : rex::graphics::CommandProcessor {
  using rex::graphics::CommandProcessor::gamma_ramp_256_entry_table;
};
}  // namespace

// Fits the game's ramp as scale * x^gamma (least squares in log space over the mid inputs; the
// darkest entries are mostly rounding). Identity (1, 1) when there is no table yet.
void SvrGuestGammaRamp(float& gamma, float& scale) {
  gamma = 1.0f;
  scale = 1.0f;
  auto* runtime = rex::Runtime::instance();
  auto* graphics =
      runtime ? static_cast<rex::graphics::GraphicsSystem*>(runtime->graphics_system()) : nullptr;
  auto* processor = graphics ? graphics->command_processor() : nullptr;
  if (!processor)
    return;
  const auto* table = (processor->*&GammaTableAccess::gamma_ramp_256_entry_table)();
  double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  int n = 0;
  for (int i = 32; i <= 255; i += 16) {
    const auto& entry = table[i];
    const double y =
        (entry.color_10_red + entry.color_10_green + entry.color_10_blue) / (3.0 * 1023.0);
    if (y <= 0.0)
      continue;
    const double lx = std::log(i / 255.0), ly = std::log(y);
    sx += lx, sy += ly, sxx += lx * lx, sxy += lx * ly;
    ++n;
  }
  const double det = n * sxx - sx * sx;
  if (n < 4 || det <= 0.0)
    return;
  const double g = (n * sxy - sx * sy) / det;
  const double a = std::exp((sy - g * sx) / n);
  if (g < 0.2 || g > 5.0 || a < 0.1 || a > 4.0)
    return;
  gamma = float(g);
  scale = float(a);
  static float s_gamma = 0.0f, s_scale = 0.0f;
  if (std::fabs(gamma - s_gamma) > 0.01f || std::fabs(scale - s_scale) > 0.01f) {
    s_gamma = gamma, s_scale = scale;
    REXLOG_INFO("Native present: game brightness ramp {:.3f} * x^{:.3f} (ramp 32/64/128/192/255 "
                "-> {}/{}/{}/{}/{} of 1023)",
                scale, gamma, table[32].color_10_green, table[64].color_10_green,
                table[128].color_10_green, table[192].color_10_green, table[255].color_10_green);
  }
}

// Edge smoothing (FXAA) in the present pass: the native renderer draws the game's own render
// targets without MSAA. The launcher's anti-aliasing choice maps to it (0 off, 1, 2 strong).
REXCVAR_DEFINE_INT32(svr_fxaa, 0, "SvR", "Edge smoothing (FXAA) on the final image: 0 = off, 1 = on, 2 = strong")
    .range(0, 2);

unsigned SvrFxaaLevel() { return unsigned(REXCVAR_GET(svr_fxaa)); }
