// w13r_2k14_glue.cpp - the few symbols a w13r build needs from outside the renderer (decision 0008).
//  * 2K14 code outside src/native (ultrawide_probe.cpp) uses two helpers of the shipped renderer (src/native/d3d,
//    left out of a w13r build): same range rule as src/native/d3d/capture.cpp, guest base from the kernel state.
//  * The WWE '13 renderer calls two WWE '13-port functions (present counter, SDK 0.10 file I/O stats): no-ops here.
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/system/kernel_state.h>

namespace wwe2k14::d3d {
bool GuestRangeOk(uint32_t addr, uint64_t len) {
  if (addr < 0x1000) return false;
  if (len == 0) return true;
  if (uint64_t(addr) + len > 0x100000000ull) return false;
  if (addr >= 0xA0000000u) return uint64_t(addr & 0x1FFFFFFFu) + len <= 0x20000000ull;
  return true;
}
const uint8_t* ShadowGuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
}
// FPS counter (src/wwe2k14_app.h FpsOverlay): guest Swaps so far and the longest gap since the last call.
static std::atomic<uint64_t> g_swaps{0}, g_max_gap_ns{0}, g_last_ns{0};
void GameFrameStats(uint64_t* swaps, uint64_t* max_gap_ns) {
  if (swaps) *swaps = g_swaps.load(std::memory_order_relaxed);
  if (max_gap_ns) *max_gap_ns = g_max_gap_ns.exchange(0, std::memory_order_relaxed);
}
}  // namespace wwe2k14::d3d

namespace w13r {
void NoteGuestSwap() {  // recorder.cpp TestShot, once per D3DDevice_Swap
  const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count());
  const uint64_t last = wwe2k14::d3d::g_last_ns.exchange(now, std::memory_order_relaxed);
  if (last) {
    const uint64_t gap = now - last;
    uint64_t cur = wwe2k14::d3d::g_max_gap_ns.load(std::memory_order_relaxed);
    while (gap > cur && !wwe2k14::d3d::g_max_gap_ns.compare_exchange_weak(cur, gap, std::memory_order_relaxed)) {}
  }
  wwe2k14::d3d::g_swaps.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace w13r

void Wwe13OnGuestPresent(uint8_t*) {}  // WWE '13 freeze watchdog counter; 2K14 has its own (timeline)

namespace rex::kernel::xboxkrnl {
void GetFileIoStats(uint64_t* bytes, uint64_t* calls, uint64_t* nanoseconds) {  // SDK 0.10 only: report zero
  if (bytes) *bytes = 0;
  if (calls) *calls = 0;
  if (nanoseconds) *nanoseconds = 0;
}
}  // namespace rex::kernel::xboxkrnl
