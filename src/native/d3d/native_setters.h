// F1 (docs/PLAN-FULLY-NATIVE.md): native bodies replacing guest D3D setters. WWE2K14_D3D_NATIVE_CONST:
// 0 (default) old path; 1 native body; 2 CHECK: original body runs, its bytes are compared with the native result.
// WWE2K14_D3D_NATIVE_CONST_AB=<frames> (mode 1): even blocks old path, odd blocks native (tools/nr/cost-ab.py).
#pragma once
#include <cstdint>

struct PPCContext;

namespace wwe2k14::d3d::native_setters {
using GuestBody = void (*)(PPCContext&, uint8_t*);
// True when the call was fully handled. False = run the normal hook path (off, A arm, foreign thread, unusual args).
bool TryConst(PPCContext& ctx, uint8_t* base, bool ps, uint32_t frame, GuestBody original);
// F1 step 2: WWE2K14_D3D_NATIVE_INTERP=1 (+ _AB=<frames>): RenderCmdInterp sub_82BB8C00 skips the hook layer. The body is the
// original; it has no capture role and is tag-trusted, so capture is unchanged (verify with WWE2K14_D3D_TAG_SCAN=check).
// The pump guard and the freeze-report ring node stay. Calls on another thread keep the normal path.
bool InterpFast(uint32_t frame);
// F1 step 3: WWE2K14_D3D_NATIVE_STATE=0|1|2 (+ _AB=<frames>, mode 1): SetVertexDecl, SetIndices, SetStreamSource. A call that
// needs the fence-record path (it writes to the ring) runs the original. CHECK: original runs; the whole device block and
// the outside stores must equal the native prediction.
// F1 step 4: WWE2K14_D3D_NATIVE_TEX=0|1|2 (+ _AB): SetTexture, same rules.
enum class StateSetter { kDecl, kIndices, kStream, kTexture };
bool TryState(PPCContext& ctx, uint8_t* base, StateSetter which, uint32_t frame, GuestBody original);
}  // namespace wwe2k14::d3d::native_setters
