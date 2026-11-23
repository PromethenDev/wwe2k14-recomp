// From WWE '12 PC (wwe12_d3d11.exe), used as is by wwe13_d3d11.exe: Direct3D 11 has no descriptor heaps. The game's shaders, converted
// by tools/convert_shaders_d3d11.py, read fetch slot N from a register of its own (2D tN, cube
// t16+N, 3D t32+N, sampler sN; N = slot & 15, the pixel shader's slots 0-15, the vertex
// shader's 16-31). Before each draw the bindless entries the shared constants name go to those
// registers (plume::D3D11SetSlotTexture / D3D11SetSlotSampler, only what changed), and the
// constants get each slot's swizzle where the indices were (D3D11 views have none).

#if defined(REBLUE_D3D11)

#include <cstring>

#include <plume_d3d11.h>

#include "gpu/constant_buffers.h"
#include "gpu/device.h"

namespace bd::gpu {

namespace {

constexpr u32 kStageVertex = 1, kStagePixel = 2;

// What each register of each stage holds on the list being recorded (UINT32_MAX: unknown).
struct Bound {
  u32 textures[2][48];
  u32 samplers[2][16];
  const plume::RenderCommandList *list = nullptr;
};
Bound g_bound;

void Reset(const plume::RenderCommandList *list) {
  std::memset(g_bound.textures, 0xFF, sizeof(g_bound.textures));
  std::memset(g_bound.samplers, 0xFF, sizeof(g_bound.samplers));
  g_bound.list = list;
}

void Texture(VideoState &s, u32 stage, u32 reg, u32 index) {
  u32 &bound = g_bound.textures[stage == kStagePixel][reg];
  if (bound == index)
    return;
  bound = index;
  plume::D3D11SetSlotTexture(s.command_list, stage, reg, s.texture_descriptor_set.get(), index);
}

void Sampler(VideoState &s, u32 stage, u32 reg, u32 index) {
  u32 &bound = g_bound.samplers[stage == kStagePixel][reg];
  if (bound == index)
    return;
  bound = index;
  plume::D3D11SetSlotSampler(s.command_list, stage, reg, s.sampler_descriptor_set.get(), index);
}

} // namespace

void D3D11ForgetSlots() { g_bound.list = nullptr; }

void D3D11BindSlots(SharedConstants &c) {
  auto &s = state();
  if (!s.command_list || !s.texture_descriptor_set || !s.sampler_descriptor_set)
    return;
  if (g_bound.list != s.command_list)
    Reset(s.command_list);
  const auto *set = s.texture_descriptor_set.get();
  for (u32 half = 0; half < 2; ++half) {
    // Slots 0-15 are the pixel shader's, 16-31 the vertex shader's.
    const u32 stage = half ? kStageVertex : kStagePixel;
    u32 *tex2d = half ? c.texture2DIndicesHi : c.texture2DIndices;
    u32 *tex3d = half ? c.texture3DIndicesHi : c.texture3DIndices;
    u32 *cube = half ? c.textureCubeIndicesHi : c.textureCubeIndices;
    u32 *samplers = half ? c.samplerIndicesHi : c.samplerIndices;
    for (u32 i = 0; i < 16; ++i) {
      Texture(s, stage, i, tex2d[i]);
      Texture(s, stage, 16 + i, cube[i]);
      Texture(s, stage, 32 + i, tex3d[i]);
      Sampler(s, stage, i, samplers[i]);
      tex2d[i] = plume::D3D11SlotSwizzle(set, tex2d[i]);
      cube[i] = plume::D3D11SlotSwizzle(set, cube[i]);
      tex3d[i] = plume::D3D11SlotSwizzle(set, tex3d[i]);
    }
  }
}

// The port's own passes (copy, gamma, ImGui) read one texture at t0 (and s0).
void D3D11BindHostSlot(u32 texture_index, u32 sampler_index) {
  auto &s = state();
  if (!s.command_list || !s.texture_descriptor_set || !s.sampler_descriptor_set)
    return;
  if (g_bound.list != s.command_list)
    Reset(s.command_list);
  Texture(s, kStagePixel, 0, texture_index);
  Sampler(s, kStagePixel, 0, sampler_index);
}

} // namespace bd::gpu

#endif
