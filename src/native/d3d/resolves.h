// WP6 (decision 0006 stage 2): Resolve -> host copy into a host image representing the destination texture.
// Frozen interface (+ additive part). Worker thread, under the backend lock the worker already holds.
//
// Semantics (RB_COPY registers are the authority, as in hardware): the copy source is the shadow image bound for
// RB_COPY_CONTROL.copy_src_select (4 = depth, else colour target 0..3); the rectangle is ResolveDesc::src_rect
// {left, top, right, bottom} in surface pixels (empty = whole image), aligned to 8 like the SDK; its origin is also the
// destination coordinate relative to RB_COPY_DEST_BASE (so per-tile resolves of a predicated-tiling scene compose a
// full destination image: the host image registered for WP8 covers the union of the tile rects of one epoch, and the
// ResolveDesc passed to RegisterResolveImage carries that union rect in src_rect). ResolveDesc::dst_point is only counted (card S3-17b parked).
// Clear-with-resolve (control bits 8/9) is applied to the resolved rect after the copy, deferred until the next draw
// bind / clear / SetRT / tiling begin / intersecting resolve (see targets.h).
#pragma once

#include <cstdint>
#include <string>

#include "device_state.h"
#include "shadow_types.h"

namespace wwe2k14::d3d::resolves {

void OnResolve(const EvHeader& e, const DeviceState& s, const ResolveDesc& r);

struct ResolveRecord { uint32_t seq, packet_addr; ResolveDesc desc; uint32_t snapshot; uint64_t hash; };
// Most recent resolve (for WP10 / compare); null if none.
const ResolveRecord* Last();

// ---------------------------------------------------------------- additive part
// Output hash definition (WP10 / compare must compute the same on nr's side): XXH3-64 (seed = w << 32 | h) over the
// tightly packed RGBA8 rows of the resolved rect as nr::backend::ReadColor returns them (MSAA already averaged).
uint64_t HashRGBA8(const uint8_t* rgba, uint32_t w, uint32_t h);

// kRGBA4 = an RGBA8 source copied to a 4:4:4:4 (2-byte) destination ColorFormat; kNoop = a resolve whose clamped
// rect is empty (nr: no-op, nr_frontend.cpp:3473-3477). Appended so existing by_class indices keep their meaning.
enum class Class : uint8_t { kNone, kRGBA8, kK8, kFixed16, kDepthR32F, kClearOnly, kUnsupported, kRGBA4, kNoop };

struct ResolveExtra {
  uint32_t seq = 0, packet_addr = 0xFFFFFFFFu;
  Class cls = Class::kNone;
  uint32_t dest_base = 0, extent_start = 0, extent_length = 0;  // physical; RB_COPY_DEST_BASE / SDK-style extent
  uint32_t dest_pitch = 0, dest_info = 0;                       // rb_copy[2], rb_copy[3]
  int32_t rect[4] = {0, 0, 0, 0};                               // this resolve's rect (aligned, clamped), x0 y0 x1 y1
  int32_t union_rect[4] = {0, 0, 0, 0};                         // rect of the image registered for WP8
  uint32_t width = 0, height = 0;                               // of `rect`
  uint8_t samples = 1;
  bool hashed = false;       // ResolveRecord::hash valid (sampled, RGBA8 class only)
  bool registered = false;   // RegisterResolveImage was called for this resolve
  bool merged = false;       // merged into the previous tile image of the same predicated-tiling epoch
  uint32_t tiling_epoch = 0;
  bool flushed_clear = false;  // card S3-44 diagnostic: a pending deferred clear on the source was applied before the copy
};
// Records of the last 128 resolves, found by the event's packet_addr (the paired CP copy packet) or the last one.
const ResolveRecord* FindByPacket(uint32_t packet_addr, const ResolveExtra** extra);
const ResolveExtra* LastExtra();
// Card S3-44 diagnostic (FLASH_TRACE): this frame's resolves as text (cleared by the call; worker thread).
std::string TakeFrameResolves();

// One "[d3d-shadow] resolve ..." line (counts per class, unsupported reasons).
void LogStats(const char* why);
// S5-1 opt-in resolve/copy census, called once per worker frame.
void CensusFrameEnd(uint64_t frame);

}  // namespace wwe2k14::d3d::resolves
