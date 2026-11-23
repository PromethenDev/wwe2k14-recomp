// Native Clear tree sub_82CB8BA8 -> 82CB8A70 -> 82CB83F0 (native_clear_core.h, host test tools/nr/native-clear-test.sh:
// 50,000 random states 0 mismatches, 8 planted bugs caught). WWE2K14_D3D_NATIVE_CLEAR: 0 = guest body,
// 1 = native body (default on desktop); _AB=N: native only in odd blocks of N frames. No in-game CHECK: the save/restore CHECK of the Resolve
// worker froze the game (it clobbers other threads' device updates), so acceptance = host test + smoke + in-run A/B.
#pragma once
#include <cstdint>

struct PPCContext;

namespace wwe2k14::d3d::native_clear {
bool Body(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame);
}  // namespace wwe2k14::d3d::native_clear
