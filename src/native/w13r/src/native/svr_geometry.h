// SvR 2008: vertex and index buffers for the native renderer's draws.
//
// SvR's D3D (stock XDK 2.0.5632) keeps its buffers as its own guest objects:
//   D3DVertexBuffer (D3DDevice_CreateVertexBuffer sub_82235648): D3DResource + vertex fetch
//     constant: +0x18 = CPU address | 3, +0x1C = size in bytes (bits 2-25) | 0x10000000 | endian
//     (2 = 8in32).
//   D3DIndexBuffer (D3DDevice_CreateIndexBuffer sub_82235770): D3DResource (Common bit 31 =
//     INDEX32, bits 30-29 = endian: 1 = 8in16 for INDEX16, 2 = 8in32 for INDEX32), Address (+0x18,
//     CPU address), Size (+0x1C, exact bytes).
// The CPU addresses are in a physical alias (0xA0000000 / 0xC0000000 / 0xE0000000). The XDK turns
// them into GPU addresses where the GPU reads them: SetStreamSource (sub_8225A648) writes stream
// N's fetch constant into the device, DrawIndexedVertices (sub_825A4340) converts the index buffer
// address into its DRAW_INDX packet.
#pragma once

#include <rex/types.h>

namespace bd::gpu {

// The XDK's GPU_CONVERT_CPU_TO_GPU_ADDRESS: a CPU address in any physical alias, or an address that
// already is a GPU one, to the GPU (physical) address. The 0xE0000000 alias (4 KB pages) maps 4 KB
// further on, so TranslatePhysical(cpu_address) alone reads the wrong page there.
constexpr u32 SvrGpuAddress(u32 address) {
  return (address & 0x1FFFFFFFu) + ((((address >> 20) + 0x200u) & 0x1000u));
}

// Binds the vertex streams the current vertex declaration reads and, for an indexed draw, the index
// buffer, as SvR's D3D left them in the device for the draw about to run. Call before taking
// state().mutex (the Video setters take it).
void SvrBindDrawGeometry(u32 device_guest, bool indexed);

// The CPU rewrote [address, address + size) (CPU or GPU address): copies over it are compared with
// guest memory again at their next use, even within the same frame.
void SvrInvalidateGeometry(u32 address, u32 size);

}  // namespace bd::gpu
