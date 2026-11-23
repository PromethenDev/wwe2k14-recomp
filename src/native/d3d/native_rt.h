// N2-RT: native SetRenderTarget tree; see native_rt.cpp and docs/tasks/N2-RT-native.md.
#pragma once
#include <cstdint>

struct PPCContext;

namespace wwe2k14::d3d::native_rt {
// The hook's body call for SetRenderTarget 82CAF5E8: true = handled (native, or CHECK which also ran `original`);
// false = mode 0, run the guest body.
// frame: game swap count, for the in-run A/B (WWE2K14_D3D_NATIVE_RT_AB=N: mode 1 only in odd blocks of N frames).
bool Body(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame);
// N2-DS: the same for SetDepthStencil 82CAF978 (WWE2K14_D3D_NATIVE_DS 0 default / 1 / 2 CHECK, _AB=N).
bool BodyDs(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame);
}  // namespace wwe2k14::d3d::native_rt
