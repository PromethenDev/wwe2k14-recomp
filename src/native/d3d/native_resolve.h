// Native Resolve worker sub_82CB2320 (native_resolve_core.h, host test tools/nr/native-resolve-test.sh).
// WWE2K14_D3D_NATIVE_RESOLVE: 0 = guest body, 1 = native body (default on desktop), 2 = CHECK; _AB=N: mode 1 only in odd blocks
// of N frames. See native_resolve.cpp for the CHECK design.
#pragma once
#include <cstdint>

struct PPCContext;

namespace wwe2k14::d3d::native_resolve {
// The hook's body call: true = handled (native, or CHECK which also ran `original`); false = run the guest body.
bool Body(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame);
}  // namespace wwe2k14::d3d::native_resolve
