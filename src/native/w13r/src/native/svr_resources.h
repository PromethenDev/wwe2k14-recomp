// SvR 2008: host resources for the game's own D3D textures and surfaces.
//
// With the null GPU, SvR's D3D library creates real guest resources (headers + physical memory)
// itself; the native renderer gives each a host counterpart the first time it's used. Surfaces
// become host render targets; resolve destinations (including the front buffer) become host
// textures the GPU writes; any other texture is mirrored from guest memory (native_texture_mirror).

#pragma once

#include <rex/types.h>

#include <vector>

namespace bd::gpu {

struct GuestTexture;
struct GuestShader;
struct GuestVertexDeclaration;

// Guest D3DSurface (render or depth target) -> host render target.
GuestTexture *SvrResolveSurface(u32 surface_va);
// Host pixels per guest pixel of a surface's host target: the render scale for screen surfaces
// (16:9, square, tiled), 1 for the game's work surfaces (wrestler skin composites), whose content
// is UV-mapped: upscaled, their patch seams showed as cracks on the skin.
float SvrSurfaceScale(u32 surface_va);
// Guest texture that the GPU writes (Resolve destination, front buffer) -> host texture.
// scale: host pixels per guest pixel for a target made now (0 = the render scale). A resolve
// passes its source surface's, so copies stay 1:1.
GuestTexture *SvrResolveTarget(u32 texture_va, float scale = 0.0f);

// Every GPU-written texture (resolve destination) that has a host texture now: its texture VA,
// host texture and scale (1 = 1x). For the recorder's start-of-recording snapshot.
struct SvrTargetInfo {
  u32 texture_va;
  GuestTexture *host;
  float scale;
};
void SvrListTargets(std::vector<SvrTargetInfo> &out);
// After a resolve into texture_va: also write the result into the texture's guest memory, as the
// Xbox resolve does, for the first resolves of a 1x target (the game reads custom attire
// composites back with the CPU to compress them). Per-frame targets stop after a few.
void SvrAfterResolve(u32 texture_va, GuestTexture *dst, u32 level, u32 face);
// Guest texture bound for sampling -> host texture (a resolve target, else a mirror).
GuestTexture *SvrResolveTexture(u32 texture_va);
// Called once per presented frame (mirror checks are per frame).
void SvrOnFrame();

// A resolve reaching past its source surface (predicated tiling): grow the host surfaces bound
// as color and depth to cover right x bottom (guest pixels) from their next bind on.
void SvrNoteTiledResolve(u32 color_va, u32 depth_va, u32 right, u32 bottom);

// Resolve with D3DRESOLVE_CLEARRENDERTARGET / CLEARDEPTHSTENCIL (0x100 / 0x200): the Xbox clears
// EDRAM after copying. Tile resolves all copy from the one full-size host surface, so the clear
// waits until the game draws into that surface again (SvrTakePendingClear before each draw). The
// clear depth/stencil (passed in f1, not seen by the hook) are the ones of the game's last Clear.
void SvrNoteResolveClear(u32 surface_va, u32 resolve_flags, u32 color_argb);
void SvrNoteGameClear(u32 surface_va, float depth, u32 stencil);
bool SvrTakePendingClear(u32 surface_va, u32 &clear_flags, u32 &color_argb, float &depth,
                         u32 &stencil);

// Internal resolution as a multiple of the guest's (1280x720): host render targets and resolve
// textures are this many times larger, and viewports/scissors are scaled to match. 0.5 (360p),
// 2/3 (480p), 0.75 (540p), 1, 1.5 (1080p), 2, 3 or 4. Set with svr_render_scale (a multiple 1-4
// or a height 360/480/540/720/1080/1440/2160/2880; 0 = from the window height: 2 at 1080p/1440p, 3 at 4K, 1 at 720p/800p). Fixed for the
// session once the first render target exists.
float SvrRenderScale();
// Frames presented so far, plus 1 (never 0); per-frame mirror checks compare against it.
u64 SvrFrameIndex();

struct GuestVertexElement;

// Shaders and vertex declarations stay the game's own D3D objects; each one the game creates is
// given a host counterpart (shared between objects with the same microcode / elements).
void SvrOnShaderCreated(u32 shader_va, u32 function_va, bool pixel);
// Same, from a copy of the shader container (recorder replay: the game's buffer may be gone by then).
void SvrRegisterShaderContainer(u32 shader_va, const void *container, bool pixel);
GuestShader *SvrLookupShader(u32 shader_va);
void SvrOnVertexDeclarationCreated(u32 decl_va, u32 elements_va);
// Same, from a copy of the elements (terminated by stream 0xFF; recorder replay).
void SvrRegisterVertexDeclaration(u32 decl_va, const GuestVertexElement *elements);
GuestVertexDeclaration *SvrLookupVertexDeclaration(u32 decl_va);

}  // namespace bd::gpu
