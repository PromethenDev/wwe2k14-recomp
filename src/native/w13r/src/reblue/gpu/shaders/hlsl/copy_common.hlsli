#pragma once

struct PushConstants
{
    // Primary bindless SRV slot (every helper PS reads this).
#ifdef __spirv__
    // Vulkan: block sits at [24,40) of the shared push range.
    [[vk::offset(24)]]
#endif
    uint  ResourceDescriptorIndex;
    // Secondary bindless SRV slot. Single-source passes ignore it.
    uint  ResourceDescriptorIndex2;
    // Per-pass float params. Param0 = gamma for the present pass.
    float Param0;
    float Param1;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants : register(b3, space4);
