// Crowd decompile step C1 (docs/NOW.md, 11-19): TEST-ONLY diagnostic WWE2K14_D3D_CROWD_CALLERS=1 (default off; compiled to
// no-ops in public builds). At DrawIndexedVertices, when the resident vertex shader is the perspective crowd VS EB1F, the guest
// call chain (LR at entry + 5 saved LRs walked from the stack back chain) is counted, split by the r6 argument size class, and
// the top chains are logged every 20 s as "[crowd-callers]". Purpose: locate the game's own crowd code (its per-member loop and
// mesh choice). Remove when settled.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace wwe2k14::d3d::crowd_callers {

bool On();
void Pre(const PPCContext& ctx, const uint8_t* base);
void Post();

struct Scope {
  bool active = false;
  Scope(bool is_draw, const PPCContext& ctx, const uint8_t* base) {
    if (is_draw && On()) {
      active = true;
      Pre(ctx, base);
    }
  }
  ~Scope() {
    if (active) Post();
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

}  // namespace wwe2k14::d3d::crowd_callers
