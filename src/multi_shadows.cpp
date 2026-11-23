// "Multi-man shadows" option (card G70, owner 11-06; evidence docs/tasks/G70-gate-report.md, G70-option-report.md).
// WWE2K14_MULTI_SHADOWS=1: matches with 3 to 6 wrestlers get the same soft wrestler + rope shadows on the mat as a one-on-one, because
// the game's two plane-shadow creators are told the count is 2. It raises the picture and costs some speed, so the launcher decides.
// Off (no variable): this hook adds two comparisons per count call and changes nothing.

#include <cstdlib>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "multi_shadows_core.h"

extern "C" {
PPC_FUNC(__imp__sub_827E5228);
PPC_FUNC(sub_827D9178);
PPC_FUNC(__imp__sub_827F8040);
PPC_FUNC(sub_82250050);  // guest allocator (size)
PPC_FUNC(sub_827F0548);  // plane shadow task constructor (memory, 28, 0)
PPC_FUNC(sub_82C41608);  // adds a task to its owner (owner, task)
}

namespace {
bool OptionOn() {
  static const bool enabled = [] {
    const char* e = std::getenv("WWE2K14_MULTI_SHADOWS");
    return e && *e == '1';
  }();
  return enabled;
}
}  // namespace

// The creator used in matches. It stops on G[24] and G[19] itself (Ladder, TLC), so the hook below cannot help there: run the creator's own steps instead.
extern "C" PPC_FUNC(sub_827F8040) {
  namespace ms = wwe2k14::multishadows;
  if (OptionOn()) {
    const uint32_t owner = ctx.r3.u32;
    __imp__sub_827E5228(ctx, base);
    const int32_t real = ctx.r3.s32;
    sub_827D9178(ctx, base);
    const uint32_t g = ctx.r3.u32;
    const uint8_t g19 = PPC_LOAD_U8(g + 19), g24 = PPC_LOAD_U8(g + 24);
    if (ms::ForceWithFlags(real, g19 != 0, g24 != 0, true)) {
      REXLOG_INFO("[multi-shadows] flagged creator wrestlers={} G19={} G24={}", real, g19, g24);
      ctx.r3.u64 = 1456;
      sub_82250050(ctx, base);
      uint32_t task = 0;
      if (ctx.r3.u32) {
        ctx.r5.u64 = 0;
        ctx.r4.u64 = 28;
        sub_827F0548(ctx, base);
        task = ctx.r3.u32;
      }
      ctx.r4.u64 = task;
      ctx.r3.u64 = owner;
      sub_82C41608(ctx, base);
      return;
    }
    ctx.r3.u64 = owner;
  }
  __imp__sub_827F8040(ctx, base);
}

extern "C" PPC_FUNC(sub_827E5228) {  // wrestler count
  namespace ms = wwe2k14::multishadows;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_827E5228(ctx, base);
  if (!ms::IsCreatorCall(lr)) return;
  const bool enabled = OptionOn();
  if (!enabled) return;
  const int32_t real = ctx.r3.s32;
  sub_827D9178(ctx, base);  // the callers go on to read G[24] and G[19] themselves; these are read here only for the log
  const uint32_t g = ctx.r3.u32;
  const int32_t reported = ms::ReportedCount(lr, real, enabled);
  REXLOG_INFO("[multi-shadows] creator lr={:08X} wrestlers={} reports={} G19={} G24={}", lr, real, reported, PPC_LOAD_U8(g + 19),
              PPC_LOAD_U8(g + 24));
  ctx.r3.s64 = reported;
}
