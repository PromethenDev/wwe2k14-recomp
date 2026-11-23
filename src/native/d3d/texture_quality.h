#pragma once
#include <cstdint>
#include <vector>
namespace nr::shared { struct ShaderEntry; }
namespace nr::backend { struct SamplerDesc; }
namespace wwe2k14::d3d { struct DeviceState; }
namespace wwe2k14::d3d::texq {
uint8_t Mode();
void Apply(const DeviceState& state, uint32_t handle, uint32_t slot, const nr::shared::ShaderEntry* vs,
           const nr::shared::ShaderEntry* ps, const std::vector<uint8_t>* active_vs,
           const std::vector<uint8_t>* active_ps, nr::backend::SamplerDesc& sampler);
void FrameEnd();
}
