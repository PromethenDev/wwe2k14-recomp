// TEST-ONLY Universe card probe (KOTR groundwork, 2026-11-12). sub_82B8FB88 (74:69013) fills the 6 participant slots of
// one card match record (r4): u16 wrestler ids at r4+336 + 12*i, 512 = empty (Qwen Q-card-01 + Claude check). This logs
// each call (record address, args, caller) and the ids after it returns, so the card store and its stride can be read
// from one Universe run. WWE2K14_TEST_UNIV_CARD=1. Public builds: compiled out.
#include <cstdint>
#include <cstdlib>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

extern "C" {
PPC_FUNC(__imp__sub_82B8FB88);
PPC_FUNC(sub_82B8FB88);
}

#if defined(WWE2K14_NO_TEST_TOOLS)
extern "C" PPC_FUNC(sub_82B8FB88) { __imp__sub_82B8FB88(ctx, base); }
#else
namespace {
bool On() {
  static const bool on = [] { const char* e = std::getenv("WWE2K14_TEST_UNIV_CARD"); return e && *e == '1'; }();
  return on;
}
}  // namespace

extern "C" PPC_FUNC(sub_82B8FB88) {
  if (!On()) { __imp__sub_82B8FB88(ctx, base); return; }
  const uint32_t lr = static_cast<uint32_t>(ctx.lr), r3 = ctx.r3.u32, rec = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32,
                 r7 = ctx.r7.u32;
  __imp__sub_82B8FB88(ctx, base);
  uint32_t ids[6] = {};
  for (int i = 0; i < 6; ++i) ids[i] = rec >= 0x10000 ? PPC_LOAD_U16(rec + 336 + 12 * i) : 0;
  uint32_t head[4] = {};
  for (int i = 0; i < 4; ++i) head[i] = rec >= 0x10000 ? PPC_LOAD_U32(rec + 4 * i) : 0;
  REXLOG_WARN("[univ-card] fill rec={:08X} r3={:08X} r5={:08X} r6={} r7={:08X} from={:08X} -> ret={} ids={} {} {} {} {} {} "
              "head={:08X} {:08X} {:08X} {:08X}",
              rec, r3, r5, r6, r7, lr, ctx.r3.u32, ids[0], ids[1], ids[2], ids[3], ids[4], ids[5], head[0], head[1], head[2],
              head[3]);
}
#endif
