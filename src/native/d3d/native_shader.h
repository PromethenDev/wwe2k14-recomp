// N4: native SetPixelShader 82CB42A0 / SetVertexShader 82CB44A8, ShaderConstLoad, VS upload; see native_shader.cpp.
#pragma once
#include <cstdint>

struct PPCContext;

namespace wwe2k14::d3d::native_shader {
// The hook's body call: true = handled (native, or CHECK which also ran `original`); false = run the guest body.
// WWE2K14_D3D_NATIVE_SHADER 0 guest / 1 native (desktop default) / 2 CHECK; _AB=N in-run A/B.
bool Body(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame, bool vs);
// N4 step 2a: ShaderConstLoad 82CC1C58 (WWE2K14_D3D_NATIVE_CONSTLOAD 0 default / 1 native / 2 CHECK; _AB=N).
bool BodyConstLoad(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame);
// N4 step 3: register-run writer 82CC0D18 (WWE2K14_D3D_NATIVE_REGEMIT 0 / 1 native (desktop default) / 2 CHECK; _AB=N).
bool BodyRegEmit(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame);
// N4 step 2b: inline VS upload tree 82CC1B00 (WWE2K14_D3D_NATIVE_VSUPLOAD 0 default / 1 native / 2 CHECK; _AB=N).
bool BodyVSUpload(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame);
}  // namespace wwe2k14::d3d::native_shader
