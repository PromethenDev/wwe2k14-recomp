// GitHub #80 diagnostic (2026-11-15; Qwen N-29/N-30, lines checked by Claude): after a custom-song entrance the victory music
// plays dry until restart. The game's music object (singleton) keeps +852 = "XMP request outstanding / custom music active"
// (getter sub_82AF9D90, 70.cpp:24659), set by play sub_82AF9DB8 (24746-24748) and by stop sub_82AFA400 (25686-25690), cleared
// only by the poll sub_82AFA228 (25414) when the overlapped result is not 996 (ERROR_IO_INCOMPLETE). Log-only: one line per
// play / stop / poll call and whenever the getter's answer changes, so a custom-song session shows whether +852 ever clears.
#include <atomic>
#include <cstdint>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

extern "C" {
PPC_FUNC(__imp__sub_82AF9DB8);
PPC_FUNC(__imp__sub_82AFA400);
PPC_FUNC(__imp__sub_82AFA228);
PPC_FUNC(__imp__sub_82AF9D90);
}

namespace {
std::atomic<uint32_t> g_lines{0};
std::atomic<int32_t> g_last_get{-1};
bool Budget() { return g_lines.fetch_add(1, std::memory_order_relaxed) < 300; }
void Log(const char* what, uint32_t obj, uint32_t result, uint8_t* base) {
  if (!obj || !Budget()) return;
  REXLOG_INFO("[xmp-music] {} obj={:08X} result={} f852={} f856={:08X} f860={}", what, obj, result,
              PPC_LOAD_U32(obj + 852), PPC_LOAD_U32(obj + 856), PPC_LOAD_U32(obj + 860));
}
}  // namespace

extern "C" PPC_FUNC(sub_82AF9DB8) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_82AF9DB8(ctx, base);
  Log("play", obj, ctx.r3.u32, base);
}
extern "C" PPC_FUNC(sub_82AFA400) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_82AFA400(ctx, base);
  Log("stop", obj, ctx.r3.u32, base);
}
extern "C" PPC_FUNC(sub_82AFA228) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t before = obj ? PPC_LOAD_U32(obj + 852) : 0;
  __imp__sub_82AFA228(ctx, base);
  if (obj && PPC_LOAD_U32(obj + 852) != before) Log("poll cleared", obj, ctx.r3.u32, base);
}
extern "C" PPC_FUNC(sub_82AF9D90) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_82AF9D90(ctx, base);
  const int32_t now = ctx.r3.s32;
  if (g_last_get.exchange(now, std::memory_order_relaxed) != now) Log("active?", obj, uint32_t(now), base);
}
