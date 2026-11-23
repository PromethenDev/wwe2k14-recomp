#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

static const uint kMaxBoxTaps = 8u;

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint ratio = (uint)(g_PushConstants.Param1 + 0.5);
    float3 rgb;
    float a;
    if (ratio >= 2u)
    {
        uint taps = min(ratio, kMaxBoxTaps);
        int2 base = int2(position.xy) * (int)ratio;
        float4 acc = 0.0;
        [loop] for (uint y = 0u; y < taps; ++y)
        {
            int oy = (int)((y * ratio + ratio / 2u) / taps);
            [loop] for (uint x = 0u; x < taps; ++x)
            {
                int ox = (int)((x * ratio + ratio / 2u) / taps);
                acc += tex.Load(int3(base.x + ox, base.y + oy, 0));
            }
        }
        rgb = acc.rgb / (float)(taps * taps);
        a = acc.a / (float)(taps * taps);
    }
    else
    {
        float4 s = tex.Sample(g_SamplerDescriptorHeap[0], texCoord);
        rgb = s.rgb;
        a = s.a;
    }
    return float4(rgb * g_PushConstants.Param0, a);
}
