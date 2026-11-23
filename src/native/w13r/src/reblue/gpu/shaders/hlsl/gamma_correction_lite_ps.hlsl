#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

// Present pass with the pieces the default settings never use stripped out (wwe13_present_lite):
// no FXAA (svr_fxaa = 0 leaves the full shader's Fxaa() and its eight extra samples dead) and no
// X360 sRGB-decode path (the native renderer's display correction Param1 is always negative, so
// only the scale * x^gamma branch is taken). When the ramp is identity (gamma 1, scale 1, i.e.
// Param1 == -1) the pow is skipped too. Chosen on the CPU only under SVR_NATIVE_RENDERER with
// svr_fxaa == 0, where the full shader would produce this exact output.
float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> source = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    float4 sampled = source.Sample(g_SamplerDescriptorHeap[0], texCoord);
    // max() guards pow() against NaN on negative HDR values, as in the full shader.
    float3 ramp = (g_PushConstants.Param0 == 1.0 && g_PushConstants.Param1 == -1.0)
                      ? max(sampled.rgb, 0.0)
                      : pow(max(sampled.rgb, 0.0), g_PushConstants.Param0);
    return float4(saturate(ramp * -g_PushConstants.Param1), 1.0);
}
