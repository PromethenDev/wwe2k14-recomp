// GPU->CPU coherency for render-to-texture data the game's CPU code reads (bug-hunt round 3).
//
// Symptom: the Create-A-Superstar "menu screen pose" portrait saved into 00CustomSuperStar.cas was noise
// (the character-select tile showed a corrupted image and "overall 76" next to it). The game renders the portrait
// on the GPU, resolves it into guest memory and then untiles it on the CPU with the XDK function XGUntileSurface.
// The recompiled GPU keeps resolve results in its own buffer (readback_resolve=none), so the CPU untiled stale
// memory. This is the same defect WWE '13 hit with its retro-attire textures;
// the fix is the same: read back only the tiled source surface of each untile instead of every resolve
// (readback_resolve=full halves the in-ring frame rate).
//
// sub_82CFD800 is the 2K14 copy of WWE '13's sub_82BE19E8 (identical prologue and body; neighbours line up at a
// constant +0x11BE18 offset). XGUntileSurface(dest=r3, row_pitch=r4, point=r5, source=r6, width=r7, height=r8,
// rect=r9, texel_pitch=r10); the tiled source spans align32(width) * align32(height) * texel_pitch bytes.
//
// Kill switch: WWE2K14_UNTILE_COHERENCY=0.

#include <cstdint>
#include <cstdlib>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

extern "C" PPC_FUNC(__imp__sub_82CFD800);
extern "C" PPC_FUNC(sub_82CFD800) {
  static const bool enabled = [] {
    const char* v = std::getenv("WWE2K14_UNTILE_COHERENCY");
    return !(v && v[0] == '0');
  }();
  if (enabled) {
    const uint32_t source = ctx.r6.u32;
    const uint64_t size =
        uint64_t((ctx.r7.u32 + 31) & ~31u) * ((ctx.r8.u32 + 31) & ~31u) * ctx.r10.u32;
    if (size && size <= 64u * 1024 * 1024) {
      auto* kernel_state = REX_KERNEL_STATE();
      auto* graphics = kernel_state ? static_cast<rex::graphics::GraphicsSystem*>(
                                          kernel_state->emulator()->graphics_system())
                                    : nullptr;
      if (graphics && graphics->command_processor()) {
        graphics->command_processor()->MakeGpuWritesVisibleToCpu(source, uint32_t(size));
        static int logged = 0;
        if (logged < 8) {
          ++logged;
          REXLOG_INFO("[untile-coherency] XGUntileSurface source={:08X} {}x{} texel={} bytes={}", source,
                      ctx.r7.u32, ctx.r8.u32, ctx.r10.u32, size);
        }
      }
    }
  }
  __imp__sub_82CFD800(ctx, base);
}
