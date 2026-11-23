#include "copy_common.hlsli"

Texture2D<float> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

float main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Depth
{
    // Sample (not Load) so dim-mismatched depth resolves (672x720 -> 1280x720) stretch instead of pasting 1:1.
    return g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
        .Sample(g_SamplerDescriptorHeap[0], texCoord);
}
