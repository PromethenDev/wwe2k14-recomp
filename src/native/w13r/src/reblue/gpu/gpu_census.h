// WWE '13 PC: GPU time census (test-only diagnostic, D3D12 build, --wwe13_gpu_census=true).
//
// Splits each frame's GPU time into what the renderer records: draws (by render-target size), EDRAM
// resolve emulation (copies and the fullscreen composite draws), clears, texture uploads and the
// present pass. A GPU timestamp is written only when the kind of work changes, and the interval up to
// the next one is charged to the kind that started it; the totals are logged every 10 seconds as
// "[census]" lines. Off by default: the timestamps themselves cost GPU time, so census runs are for
// finding where the time goes, never for speed numbers.
#pragma once

#include <rex/types.h>

namespace bd::gpu {
struct VideoState;
struct GuestTexture;

namespace census {
enum Kind : u8 {
  kDraw,          // a guest draw (bucketed by the bound target's size)
  kResolveCopy,   // EDRAM resolve emulation: surface -> texture copy
  kResolveDraw,   // EDRAM resolve emulation: fullscreen draw (scaled / composite resolve)
  kClear,         // Clear() and framebuffer bind clears
  kUpload,        // texture uploads (guest memory -> host texture)
  kPresent,       // the final present pass
  kResolveBarrier,  // the layout transitions before a resolve draw (render target -> shader read)
  kOther,         // anything after a list starts before the first mark
  kKindCount
};

bool Enabled();
// Record that the work following this point on the open command list is of `kind`. `rt` (draws
// only) picks the size bucket; for resolves `rt` is the source and `dst` the destination.
void Mark(VideoState &s, Kind kind, const GuestTexture *rt = nullptr, const GuestTexture *dst = nullptr);
// The open command list is about to be ended (resolves its timestamps into the readback buffer).
void OnListEnd(VideoState &s);
// The frame slot's GPU work has finished (its fence was waited on): read and accumulate it.
void OnSlotRetired(VideoState &s, u32 slot);
// Once per presented frame.
void OnPresent();
// Log and reset the totals now, labelled; from then on only Flush logs (replay in-run A/B).
void Flush(const char *label);
}  // namespace census
}  // namespace bd::gpu
