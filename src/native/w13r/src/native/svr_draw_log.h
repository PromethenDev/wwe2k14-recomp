// Native renderer dev aid: log every draw, clear and resolve of one frame (SVR_DRAW_LOG=<frame>).
#pragma once

#include <rex/types.h>

namespace bd::gpu {

bool SvrDrawLogActive();
void SvrDrawLogDraw(u32 device_guest, const char *name, u32 primitive, bool indexed, u32 count,
                    u32 start, i32 base);
void SvrDrawLogResolve(u32 device_guest, u32 flags, u32 source_rect_va, u32 dest_va,
                       u32 dest_point_va);
void SvrDrawLogClear(u32 device_guest, u32 count, u32 rects_va, u32 flags, u32 color);
// BeginVertices data (big-endian floats as the game wrote them), first vertices only.
void SvrDrawLogVertices(u32 data_va, u32 count, u32 stride);

}  // namespace bd::gpu
