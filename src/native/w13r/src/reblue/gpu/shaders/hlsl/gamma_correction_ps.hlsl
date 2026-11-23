#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

float FxaaLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// Edge smoothing on the finished frame (the classic FXAA: blend along the local edge direction),
// for the native renderer, which has no MSAA on the game's own render targets. Level 1 is the
// light setting, 2 the strong one (longer edges, lower threshold), like the Xenos path's
// swap_post_effect fxaa / fxaa_extreme.
float3 Fxaa(Texture2D<float4> tex, SamplerState smp, float2 uv, uint level)
{
    uint w, h;
    tex.GetDimensions(w, h);
    const float2 rcp = 1.0 / float2(w, h);
    const float span = level >= 2 ? 16.0 : 8.0;
    const float reduce_mul = level >= 2 ? 1.0 / 16.0 : 1.0 / 8.0;
    const float reduce_min = 1.0 / 128.0;

    const float3 rgbM = tex.SampleLevel(smp, uv, 0).rgb;
    const float lNW = FxaaLuma(tex.SampleLevel(smp, uv + float2(-1, -1) * rcp, 0).rgb);
    const float lNE = FxaaLuma(tex.SampleLevel(smp, uv + float2(1, -1) * rcp, 0).rgb);
    const float lSW = FxaaLuma(tex.SampleLevel(smp, uv + float2(-1, 1) * rcp, 0).rgb);
    const float lSE = FxaaLuma(tex.SampleLevel(smp, uv + float2(1, 1) * rcp, 0).rgb);
    const float lM = FxaaLuma(rgbM);
    const float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    const float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    const float dir_reduce = max((lNW + lNE + lSW + lSE) * 0.25 * reduce_mul, reduce_min);
    const float rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
    dir = clamp(dir * rcp_dir_min, -span, span) * rcp;

    const float3 rgbA = 0.5 * (tex.SampleLevel(smp, uv + dir * (1.0 / 3.0 - 0.5), 0).rgb +
                               tex.SampleLevel(smp, uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
    const float3 rgbB = rgbA * 0.5 + 0.25 * (tex.SampleLevel(smp, uv + dir * -0.5, 0).rgb +
                                             tex.SampleLevel(smp, uv + dir * 0.5, 0).rgb);
    const float lB = FxaaLuma(rgbB);
    return (lB < lMin || lB > lMax) ? rgbA : rgbB;
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> source = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    float4 sampled = source.Sample(g_SamplerDescriptorHeap[0], texCoord);
    // The present pass leaves the second descriptor slot unused: it carries the FXAA level.
    if (g_PushConstants.ResourceDescriptorIndex2 != 0)
        sampled.rgb = Fxaa(source, g_SamplerDescriptorHeap[0], texCoord,
                           g_PushConstants.ResourceDescriptorIndex2);
    // max() guards pow() against NaN on negative HDR values (source RT may be R16F).
    float3 ramp = pow(max(sampled.rgb, 0.0), g_PushConstants.Param0);
    // A negative Param1 is a scale instead (no display correction): WWE 2K15's brightness ramp
    // is scale * x^gamma (native/wwe13_gamma.cpp).
    if (g_PushConstants.Param1 < 0.0)
        return float4(saturate(ramp * -g_PushConstants.Param1), 1.0);
    // X360 scanout: the system reshapes the app ramp for the display before upload
    // (D3D::GammaRampCorrected 0x8246F538): for display-gamma type 2 (TV) the
    // hardware LUT is Rec709_encode(sRGB_decode(app_ramp)). Xenia measurably
    // displays the UNCORRECTED front buffer (its DC LUT stays linear), so
    // Param1 (display correction strength) blends between the two references.
    float3 lin = lerp(ramp / 12.92, pow((ramp + 0.055) / 1.055, 2.4),
                      step(0.04045, ramp));
    float3 corrected = lerp(lin * 4.5, 1.099 * pow(lin, 0.45) - 0.099,
                            step(0.018, lin));
    sampled.rgb = lerp(ramp, corrected, g_PushConstants.Param1);
    return float4(sampled.rgb, 1.0);
}
