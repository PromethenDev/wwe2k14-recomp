// Titantron guard (player report Discord 11-10, v1.5.2: WWE '13 titantron crash). Always on.
// "N:line" = generated/wwe2k14_recomp.N.cpp.
//
// Crash: sub_823F6C30 (13:34596) does lwz r10,36(r3) then lwz r8,24(r10) with no zero check; when *(r3+36) is 0 the
// second load reads guest 0x18 and faults. The game checks the same kind of +36 pointer for zero elsewhere, so an
// empty pointer is a normal case. Guard: r3 == 0 or *(r3+36) == 0 returns the function's own no-work result (r3 = 1,
// 13:34634) instead of a crash.
#include <cstdint>
#include <atomic>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
std::atomic<bool> g_logged{false};
}

extern "C" {
PPC_FUNC(__imp__sub_823F6C30);
PPC_FUNC(sub_823F6C30);
}

extern "C" PPC_FUNC(sub_823F6C30) {
  if (ctx.r3.u32 == 0 || PPC_LOAD_U32(ctx.r3.u32 + 36) == 0) {
    if (!g_logged.exchange(true))
      REXLOG_WARN("[titantron-guard] sub_823F6C30 called with an empty +36 pointer (r3 {:08X}, caller {:08X}): "
                  "no-work result 1 instead of a crash (Discord 11-10, v1.5.2)", ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_823F6C30(ctx, base);
}
