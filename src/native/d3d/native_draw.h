// N3: native draw entry sub_82CB52D0 (DrawIndexedVertices); see native_draw.cpp and docs/tasks/N3-draw-entry-start.md.
#pragma once
#include <cstdint>

struct PPCContext;

namespace wwe2k14::d3d::native_draw {
// The hook's body call for 82CB52D0: true = handled (native, or CHECK which also ran `original`); false = run the guest body.
// WWE2K14_D3D_NATIVE_DRAW 0 guest / 1 native (desktop default) / 2 CHECK; WWE2K14_D3D_NATIVE_DRAW_AB=N: mode 1 only in odd blocks of N frames.
bool Body(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame);

// CHECK recording of the guest body's direct callees: one object at the top of each callee's hook/override. Inert (one
// thread-local load) unless this thread is inside a CHECK run of the guest draw body.
struct RecScope {
  RecScope(uint32_t addr, PPCContext& ctx, uint8_t* base) {
    if (addr) Begin(addr, ctx, base);  // addr is a constant 0 in hooks that are not draw callees: no code there
  }
  ~RecScope() {
    if (ctx_) End();
  }
  void Begin(uint32_t addr, PPCContext& ctx, uint8_t* base);
  void End();
  PPCContext* ctx_ = nullptr;
  uint8_t* base_ = nullptr;
  bool top_ = false;
};
}  // namespace wwe2k14::d3d::native_draw
