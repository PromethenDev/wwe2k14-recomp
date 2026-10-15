// TEST-ONLY "unlock everything" switch (WWE2K14_UNLOCK_ALL=1, default OFF).
//
// All profile unlock data lives in one block of the in-memory save profile. sub_827DAAA0 is its trivial getter
// (lis r11,0x8377; lwz r11,-0xbf0(r11); addis r3,r11,7; addi r3,r3,0x5e44; blr), i.e. block = *(0x8376F410) +
// 0x75E44; 27 guest functions call it, including the 30 Years of WrestleMania timeline menu (sub_82851D08, the
// per-slot icon chooser, copies 0xe38 bytes of the block and reads three 80-word arrays at +0x64 / +0x1a4 / +0x2e4,
// picking the TLicon_WMxx _unlock/_lock/_comp/_clear image) and the Unlockables screen (sub_82879A10 / sub_8287B8F0:
// 256 words at +0x524, index = item id, 0 = locked, 1 = unlocked/new, 2 = seen).
//
// With the switch on, every call of the getter first marks the 30 Years slots (80 = 5 eras x 16) available and
// unlocked and all Unlockables items unlocked in the live profile memory. Completion words (+0x2e4) are left alone.
//
// Eras 2-5 of the 30 Years carousel are NOT gated by those arrays (forcing +0x64/+0x1a4, and +0x2e4 as a test, left
// them locked: logs/u3-unlock, logs/u6-era). The carousel init sub_82877550 fills its per-era flags (obj+396) from the
// generic unlock query sub_822BCC48(id) with ids {196 (era 1, always open), 166, 167, 168, 169}; the draw code
// sub_82877F60 shows the LOCKED widget when the flag is 0. With the switch on, the query answers "unlocked" for 166-169
// only (no save data is written by this part).
// CAVEAT: the values live in the real in-memory profile, so a later game save writes them to SaveData.Dat. Use a
// copy of the userdata folder for testing.

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <tuple>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
// Diagnostic (WWE2K14_UNLOCK_TRACE=1, default OFF): log each distinct caller of the unlock-block getter and each
// distinct (caller, lock id, result) of the menu lock test sub_8283ED38 (r4 = lock id, returns 1 = locked, 0 =
// unlocked), to find which code gates the 30 Years era carousel (the per-slot arrays above do not).
bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_UNLOCK_TRACE");
    return v && v[0] == '1';
  }();
  return on;
}
std::mutex g_trace_mutex;
}  // namespace

extern "C" PPC_FUNC(__imp__sub_8283ED38);
extern "C" PPC_FUNC(sub_8283ED38) {
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint32_t lock_id = ctx.r4.u32;
  __imp__sub_8283ED38(ctx, base);
  if (!TraceOn()) return;
  static std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
  std::lock_guard<std::mutex> lock(g_trace_mutex);
  if (seen.emplace(caller, lock_id, ctx.r3.u32).second)
    REXLOG_INFO("[unlock-trace] lock-test caller=0x{:08X} id={} result={}", caller, lock_id, ctx.r3.u32);
}

static bool UnlockAllOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_UNLOCK_ALL");
    const bool on = v && v[0] == '1';
    if (on) REXLOG_INFO("[unlock-all] WWE2K14_UNLOCK_ALL=1: 30 Years eras/slots + Unlockables forced unlocked (test only)");
    return on;
  }();
  return on;
}

extern "C" PPC_FUNC(__imp__sub_822BCC48);
extern "C" PPC_FUNC(sub_822BCC48) {
  const uint32_t id = ctx.r3.u32;
  if (UnlockAllOn() && id >= 166 && id <= 169) {  // 30 Years eras 2-5
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_822BCC48(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_827DAAA0);
extern "C" PPC_FUNC(sub_827DAAA0) {
  if (TraceOn()) {
    static std::set<uint32_t> seen;
    const uint32_t caller = static_cast<uint32_t>(ctx.lr);
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    if (seen.insert(caller).second) REXLOG_INFO("[unlock-trace] getter caller=0x{:08X}", caller);
  }
  __imp__sub_827DAAA0(ctx, base);
  if (!UnlockAllOn()) return;
  const uint32_t blk = ctx.r3.u32;
  if (blk < 0x82000000u) return;  // profile base pointer not set yet (block would be 0x75E44)
  for (uint32_t i = 0; i < 80; ++i) {
    PPC_STORE_U32(blk + 0x64 + 4 * i, 1);   // unlocked
    PPC_STORE_U32(blk + 0x1a4 + 4 * i, 1);  // available
  }
  for (uint32_t i = 0; i < 256; ++i) {
    if (PPC_LOAD_U32(blk + 0x524 + 4 * i) == 0) PPC_STORE_U32(blk + 0x524 + 4 * i, 1);
  }
}
