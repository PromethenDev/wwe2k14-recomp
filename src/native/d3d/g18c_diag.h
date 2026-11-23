// Diagnostic for card G18 (attire composite); see g18c_diag.cpp. Lite trace by default, WWE2K14_D3D_G18C=1 full, =0 off.
#pragma once
#include <cstddef>
#include <cstdint>

struct PPCContext;

namespace wwe2k14::d3d {
bool G18cEnabled();
void G18cSetTexture(PPCContext& ctx, uint8_t* base, uint32_t frame);
void G18cReadback(uint32_t start, const uint8_t* bytes, size_t n);
}  // namespace wwe2k14::d3d
