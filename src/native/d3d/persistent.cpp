// Stage 2 step-0 stub (decision 0006): interface freeze only, owner package replaces this file. Everything is a
// no-op / "not implemented"; nothing runs unless WWE2K14_D3D_SHADOW is set (and nothing calls these yet).
#include "persistent.h"

namespace wwe2k14::d3d::persistent {
bool Active() { return false; }
void OnShaderGprCall(uint32_t) {}
}  // namespace wwe2k14::d3d::persistent
