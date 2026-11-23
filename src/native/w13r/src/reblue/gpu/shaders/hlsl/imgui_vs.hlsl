// ImGui overlay vertex shader: pixel-space -> NDC via per-frame ortho root constant (b0, space2).
struct VS_IN {
  float2 pos : POSITION;
  float2 uv  : TEXCOORD0;
  float4 col : COLOR0;     // IA decodes R8G8B8A8_UNORM -> rgba (IM_COL32 order)
};

struct VS_OUT {
  float4 pos : SV_Position;
  float4 col : COLOR0;
  float2 uv  : TEXCOORD0;
};

// Without an explicit vk::push_constant attribute dxc's default Vulkan
// register mapping places a plain cbuffer at descriptor [set=space,
// binding=register] (here set 2, binding 0) instead of the push-constant
// range the runtime's pipeline layout actually declares in
// TryInitDeviceResources -> VUID-VkGraphicsPipelineCreateInfo-layout-07988 /
// VUID-vkCmdDrawIndexed-None-08600. vk::push_constant only attaches to a
// ConstantBuffer<T> global (dxc: "'push_constant' attribute only applies to
// global variables of struct type"), not a legacy cbuffer block, so the two
// branches below differ. DXIL/D3D12 (#else) is the original
// text, byte-for-byte, so its output is untouched.
#ifdef __spirv__
struct OrthoData {
  float2 uScale;
  float2 uTranslate;
};
[[vk::push_constant]] ConstantBuffer<OrthoData> gOrtho : register(b0, space2);
#define uScale gOrtho.uScale
#define uTranslate gOrtho.uTranslate
#else
cbuffer Ortho : register(b0, space2) {
  float2 uScale;
  float2 uTranslate;
};
#endif

VS_OUT main(VS_IN i) {
  VS_OUT o;
  o.pos = float4(i.pos * uScale + uTranslate, 0.0, 1.0);
  o.col = i.col;
  o.uv  = i.uv;
  return o;
}
