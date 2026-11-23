// ImGui overlay pixel shader (bindless). reblue_compile_shader passes -all-resources-bound
// + HV 2021 so the unbounded arrays compile under SM6.0.
Texture2D    gTextures[] : register(t0, space0);
SamplerState gSamplers[] : register(s0, space1);

// See the vertex shader's Ortho cbuffer for why this needs vk::push_constant (and
// a struct, not a legacy cbuffer) on Vulkan. dxc gives every stage's
// push-constant block its own Offset space starting at 0, but the runtime's
// pipeline layout places this PIXEL range right
// after the VERTEX Ortho range in the single shared push-constant address
// space (byte 16, after Ortho's 16 bytes) so the two ranges do not overlap
// (VUID-vkCmdPushConstants-offset-01796 fires if a PIXEL-only push touches
// bytes a VERTEX range also claims). vk::offset(16) matches that placement, and
// DXIL/D3D12 (#else) is the original text, byte-for-byte, so its independent
// root-constant range at b1,space2 is untouched.
#ifdef __spirv__
struct SlotsData {
  [[vk::offset(16)]] uint uTexSlot;
  uint uSampSlot;
};
[[vk::push_constant]] ConstantBuffer<SlotsData> gSlots : register(b1, space2);
#define uTexSlot gSlots.uTexSlot
#define uSampSlot gSlots.uSampSlot
#else
cbuffer Slots : register(b1, space2) {
  uint uTexSlot;
  uint uSampSlot;
};
#endif

struct PS_IN {
  float4 pos : SV_Position;
  float4 col : COLOR0;
  float2 uv  : TEXCOORD0;
};

float4 main(PS_IN i) : SV_Target {
  // uTexSlot/uSampSlot uniform per draw (push constant) -> no NonUniformResourceIndex.
  // LOD 0: ImGui textures are single-mip.
  float4 texel = gTextures[uTexSlot].SampleLevel(gSamplers[uSampSlot], i.uv, 0.0);
  return texel * i.col;
}
