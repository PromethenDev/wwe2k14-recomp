// D3D swap snapshot queue used by the external-present path. Worker thread. Never presents or writes guest memory.
#pragma once

#include <cstdint>
#include <string>

#include "shadow_types.h"
#include "wide_core.h"

namespace wwe2k14::d3d {

// Worker thread, in event order, for kSwap. Finds the exact front-buffer image (resolves::Last(), checked against the
// Swap's r4 texture = e.args[0]) and queues it by swap ordinal for the CP thread to take.
void OnSwap(const EvHeader& e);

// CP thread, after the worker swap event has passed GateCpSwapStatus. Returns translated WIDE draw counts for `ord`.
wide_core::FrameDrawStats TakeWideSwapStats(uint64_t ord);

// CP thread, holding BackendLock, after GateCpSwapStatus(ord)
// returned 0. Hands over the D3D path's front-buffer snapshot of guest swap `ord` (an id the caller now owns and must
// release) and its size; nb::kNone when that frame is not presentable (no exact front-buffer resolve / no extent).
// Card S3-19: *swizzle = the front-buffer fetch swizzle from D3D device state (12 bits), -1 when not captured.
uint32_t TakePresentFrame(uint64_t ord, uint32_t* w, uint32_t* h, bool* swap_rb = nullptr, int32_t* swizzle = nullptr,
                          std::string* info = nullptr,  // info: card S3-44 FLASH_TRACE description of the swap
                          uint32_t* game_frame = nullptr);  // RR2: the swap event's game frame
// S3-2 (WWE2K14_NR_DRAW_OFF=1, needs PresentFromD3d): nr stops drawing; the D3D path alone produces the picture.
bool NrDrawOff();
// Card S3-8: CP thread, before a PM4_INTERRUPT is dispatched (nr Observer::OnBeforeInterrupt), draw-off only.
void OnBeforeGpuInterrupt();
// Card S3-8: CP thread, at each guest swap (nr Observer::OnBeforeSwap): next guest frame's interrupts start at index 0.
void OnGpuInterruptFrameEnd();
bool PresentFromD3d();

}  // namespace wwe2k14::d3d
