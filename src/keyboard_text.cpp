// Keyboard typing #53: terminate the title's one-character UTF-8 buffer.
//
// The name-entry character handler sub_82944108 converts the typed code unit with
// sub_82C937E0(65001, 0, &wc, 1, stack+80, 4, 0, 0) (WideCharToMultiByte, cchWideChar = 1, so no NUL is written)
// and passes stack+80 to the insert routine sub_82933468, which copies it with sub_82C37E30(dst, 4, src)
// (strcpy_s: fails when strlen(src) >= 4). stack+80 is never cleared by the handler, so the copy depends on what
// earlier calls left in that stack slot. On the console the XAM keystroke call runs on the guest stack; here it is a
// host function, so the slot keeps other guest data and typed letters were dropped (field emptied, nothing inserted).
// After a successful conversion from that one call site, write the NUL the copy expects (the buffer holds 4 bytes).
// Verified in game 2026-10-26 (kbd-check3g): before the fix the bytes after the character were FF FF FF.
// WWE2K14_KEYBOARD_TEXT=1 also logs the bytes before the fix and the insert result.
//
// GH168: while an input-gated dialog is up, sub_829443B8 (manager +1816 != 0) calls the screen vtable slot +220,
// sub_82944638, which drains keystrokes with flags = 2 and inserts nothing. That drain still stamps the SDK's
// LastKeyboardTextPollNanoseconds(), so KeyboardTextPollingActive() never expires and the A-Z/0-9 pad binds stay
// suppressed. Release the poll clock after this drain; a real typing poll in sub_829443B8 stamps it again.

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <rex/input/mnk/keyboard_text_mapping.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
constexpr uint32_t kCharHandlerConvertReturn = 0x82944250;  // lr of the bl 0x82c937e0 in sub_82944108
constexpr uint32_t kCharHandlerInsertReturn = 0x82944284;   // lr of the bl 0x82933468 in sub_82944108

bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_KEYBOARD_TEXT");
    return v && v[0] == '1' && v[1] == '\0';
  }();
  return on;
}
}  // namespace

extern "C" PPC_FUNC(__imp__sub_82C937E0);
extern "C" PPC_FUNC(sub_82C937E0) {
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint32_t out = ctx.r7.u32;
  const uint32_t out_size = ctx.r8.u32;
  __imp__sub_82C937E0(ctx, base);
  if (caller != kCharHandlerConvertReturn) return;
  const int32_t written = ctx.r3.s32;
  if (TraceOn()) {
    REXLOG_INFO("[keyboard-text] convert written={} bytes={:02X} {:02X} {:02X} {:02X}", written,
                PPC_LOAD_U8(out + 0), PPC_LOAD_U8(out + 1), PPC_LOAD_U8(out + 2), PPC_LOAD_U8(out + 3));
  }
  if (written > 0 && static_cast<uint32_t>(written) < out_size) PPC_STORE_U8(out + written, 0);
}

extern "C" PPC_FUNC(__imp__sub_82933468);
extern "C" PPC_FUNC(sub_82933468) {
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint32_t text = ctx.r4.u32;
  const uint8_t b0 = caller == kCharHandlerInsertReturn ? PPC_LOAD_U8(text) : 0;
  __imp__sub_82933468(ctx, base);
  if (caller == kCharHandlerInsertReturn && TraceOn())
    REXLOG_INFO("[keyboard-text] insert first_byte={:02X} result={}", b0, ctx.r3.s32);
}

// GH168: the input-gated screen callback (vtable +220) drains keystrokes with flags = 2 and inserts nothing. Let the
// original run unchanged, then release the SDK poll clock so its 500 ms bind-suppression window does not restart.
extern "C" PPC_FUNC(__imp__sub_82944638);
extern "C" PPC_FUNC(sub_82944638) {
  __imp__sub_82944638(ctx, base);
  if (rex::input::mnk::KeyboardTextEnabled())
    rex::input::mnk::LastKeyboardTextPollNanoseconds().store(0, std::memory_order_release);
}
