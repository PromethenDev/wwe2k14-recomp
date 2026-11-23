// W3d: opt-in title/match aspect switching at the verified game-image aspect readers.
// The pump initializes the mapped constant to 16:9; runtime mode transitions switch it before later reader calls.

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "native/d3d/wide_aspect.h"
#include "native/d3d/ultrawide_probe.h"

namespace {
constexpr uint32_t kGameAspectAddress = 0x8202B768;
constexpr uint32_t kNativeAspectBits = 0x3FE38E39;
std::mutex g_aspect_write_mutex;
std::atomic<uint8_t*> g_game_base{nullptr};

uint32_t WideAspectBits() {
  const float aspect = wwe2k14::d3d::wide_aspect::GameAspect(wwe2k14::d3d::wide_aspect::WideK());
  return std::bit_cast<uint32_t>(aspect);
}

bool IsKnownAspectBits(uint32_t bits) {
  return bits == kNativeAspectBits || bits == WideAspectBits();
}

wwe2k14::d3d::wide_aspect::AspectWriteResult WriteWideAspect(uint8_t* base, bool active) {
  using namespace wwe2k14::d3d::wide_aspect;
  if (!base) return {};
  std::lock_guard<std::mutex> lock(g_aspect_write_mutex);
  const uint32_t old_bits = PPC_LOAD_U32(kGameAspectAddress);
  const uint32_t new_bits = active ? WideAspectBits() : kNativeAspectBits;
  if (!IsKnownAspectBits(old_bits)) {
    MarkWideProjectionActive(false);
    REXLOG_WARN("[d3d-wide-aspect] left 0x{:08X} unchanged: found 0x{:08X}, expected 16:9 or 0x{:08X}",
                kGameAspectAddress, old_bits, WideAspectBits());
    return {};
  }
  if (old_bits != new_bits) PPC_STORE_U32(kGameAspectAddress, new_bits);
  MarkWideProjectionActive(active);
  return {true, old_bits, new_bits};
}

void EnsureWideAspect(uint8_t* base) {
  if (!base || !wwe2k14::d3d::wide_aspect::Enabled()) return;
  g_game_base.store(base, std::memory_order_release);

  static std::once_flag once;
  std::call_once(once, [base] {
    const uint32_t old_bits = PPC_LOAD_U32(kGameAspectAddress);
    if (!IsKnownAspectBits(old_bits)) {
      REXLOG_WARN("[d3d-wide-aspect] left 0x{:08X} unchanged after mapping: found 0x{:08X}, expected 16:9 or 0x{:08X}",
                  kGameAspectAddress, old_bits, WideAspectBits());
      return;
    }
    const auto result = WriteWideAspect(base, wwe2k14::d3d::wide_aspect::WideProjectionRequested());
    if (result.ready) {
      wwe2k14::d3d::wide_aspect::MarkAspectReady();
      const bool requested = wwe2k14::d3d::wide_aspect::WideProjectionRequested();
      if (requested != wwe2k14::d3d::wide_aspect::WideProjectionActive()) WriteWideAspect(base, requested);
    }
  });
}
}  // namespace

void wwe2k14::d3d::wide_aspect::ApplyGuestImagePatch(uint8_t* base) { EnsureWideAspect(base); }

wwe2k14::d3d::wide_aspect::AspectWriteResult wwe2k14::d3d::wide_aspect::SetWideProjectionActive(bool active) {
  g_wide_projection_requested.store(active, std::memory_order_release);
  uint8_t* base = g_game_base.load(std::memory_order_acquire);
  if (!Enabled() || !AspectReady() || !base) return {};
  return WriteWideAspect(base, active);
}

#define WWE2K14_WIDE_ASPECT_HOOK(address, reader)                                                        \
  extern "C" PPC_FUNC(__imp__sub_##address);                                                             \
  extern "C" PPC_FUNC(sub_##address) {                                                                   \
    if (wwe2k14::d3d::wide_probe::Enabled())                                                              \
      wwe2k14::d3d::wide_probe::CountAspectReader(wwe2k14::d3d::wide_probe::AspectReader::reader);       \
    EnsureWideAspect(base);                                                                               \
    __imp__sub_##address(ctx, base);                                                                      \
  }

// Every verified direct reader passes this boundary before its original body can load 0x8202B768.
WWE2K14_WIDE_ASPECT_HOOK(8276B670, k8276B670)
WWE2K14_WIDE_ASPECT_HOOK(827E9520, k827E9520)
WWE2K14_WIDE_ASPECT_HOOK(827E9AD8, k827E9AD8)
WWE2K14_WIDE_ASPECT_HOOK(827E9E40, k827E9E40)
WWE2K14_WIDE_ASPECT_HOOK(827EB9B8, k827EB9B8)
WWE2K14_WIDE_ASPECT_HOOK(82BB6F78, k82BB6F78)
WWE2K14_WIDE_ASPECT_HOOK(828469B0, k828469B0)

#undef WWE2K14_WIDE_ASPECT_HOOK
