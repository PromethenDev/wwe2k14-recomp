// N4 step 2c: native shader flush sub_82CC1E48 (native_flush_core.h); see native_flush.cpp.
#pragma once
#include <cstdint>

struct PPCContext;

namespace wwe2k14::d3d::native_flush {
// The hook's body call for 82CC1E48: true = handled (native, or CHECK which also ran `original`); false = run the guest body.
// WWE2K14_D3D_NATIVE_FLUSH 0 guest / 1 native (desktop default) / 2 CHECK; WWE2K14_D3D_NATIVE_FLUSH_AB=N: mode 1 in odd blocks of N frames.
bool Body(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame);

// CHECK recording of the guest flush's hooked callees (ShaderConstLoad, VS upload, ring reserve): one object at the top of
// each of those hooks. Inert (one thread-local load) unless this thread is inside a CHECK run of the guest flush.
struct RecScope {
  RecScope(uint32_t addr, PPCContext& ctx, uint8_t* base) {
    if (addr) Begin(addr, ctx, base);
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
}  // namespace wwe2k14::d3d::native_flush
