/**
 * @file    gpu/hooks/draw.cpp
 * @brief   Guest draw calls, and the EDRAM resolve / tiling calls that bracket
 *          them.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <chrono>
#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/gpu_census.h"
#include "gpu/constant_buffers.h"
#include "gpu/cpu_zones.h"
#include "gpu/d3d.h"
#if defined(SVR_NATIVE_RENDERER)
#include "svr_draw_log.h"
#include "svr_geometry.h"

#if defined(SVR_NATIVE_RENDERER)
// Graphics option (0.8, like Kaiko's SvR 2011 port): the crowd off. WWE '12 draws its crowd as a few
// hundred small textured cards per frame (vertex shaders 5B5BA6E0BD55C5A2 and 89B81CEC1095FF3B,
// pixel shader F9438D84762BD6DF: 339 of the 669 scene draws in a match capture, 2026-10-17), from
// people it renders into 64x64 textures at load (vertex shader F578E8C06A67CD09), and the front
// rows as 3D people (vertex shaders B2F2C29A78FFD857 and 826B5DD16C1147DC, pixel shader
// 4DC2A77541B851D7: 96 draws). Off skips all of them: empty stands, a lot less CPU and GPU work.
bool Wwe12BeltProbe(u32 device_guest, u64 vs_hash, u32 count, u32 start);  // dev probe

REXCVAR_DEFINE_BOOL(wwe13_resolve_rect_draw, false, "WWE13",
                    "Format-converting resolves of a source rect draw only that rect (Xbox semantics), "
                    "not the whole destination texture");
REXCVAR_DEFINE_BOOL(wwe12_crowd, true, "WWE12",
                    "Draw the crowd. Off = empty stands, much faster on small PCs.");

// Graphics option (0.9): shadows off. WWE '12 renders its shadow map as an R32_FLOAT colour target
// with a depth buffer (1120x1120 in a match: 60 draws per frame, capture 2026-10-17); no other pass
// pairs that format with a depth buffer. Off skips those draws: the map keeps its clear (no
// shadow) and the CPU and GPU save the pass.
REXCVAR_DEFINE_BOOL(wwe12_shadows, true, "WWE12",
                    "Draw the shadows. Off = no shadows, faster on small PCs.");

REXCVAR_DEFINE_STRING(svr_skip_shaders, "", "SvR",
                      "Dev: comma list of 16-digit hex shader hashes (vertex or pixel): draws using one are skipped "
                      "(finds which passes the crowd or the shadows are made of)");

REXCVAR_DEFINE_BOOL(svr_edge_blur, false, "SvR",
                    "SvR 2010's own edge-blur filter (the console's anti-aliasing): off keeps the image "
                    "sharp; the higher internal resolution already smooths edges");
#endif
#include "svr_hook.h"
#include "svr_resources.h"
#endif
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/host_resource_heap.h"
#include "gpu/output.h"
#include "gpu/shaders/shader_cache.h"

namespace bd::gpu {
extern std::atomic<u64> g_live_ab_draws;  // svr_resources.cpp: draws per live A/B segment
}
// Replay CPU profile of the draw path (svr::replay sets g_draw_profile): ns per section.
namespace bd::gpu {
bool g_draw_profile = false;
std::atomic<u64> g_draw_prof_ns[7]{};  // geometry, pre, framebuffer, flush, record, draws, draws issued
}  // namespace bd::gpu

namespace {

namespace xe = rex::graphics::xenos;
inline u64 ProfNow() {
  return u64(std::chrono::steady_clock::now().time_since_epoch().count());
}
struct DrawProfSection {
  int idx;
  u64 t0;
  explicit DrawProfSection(int i) : idx(i), t0(bd::gpu::g_draw_profile ? ProfNow() : 0) {}
  void end() {
    if (t0) bd::gpu::g_draw_prof_ns[idx].fetch_add(ProfNow() - t0, std::memory_order_relaxed);
    t0 = 0;
  }
  ~DrawProfSection() { end(); }
};

struct DrawArgs {
  bool indexed = false;
  bool is_up = false;
  u32 vertexOrIndexCount = 0;
  i32 baseVertexIndex = 0;
  u32 startIndex = 0;
  u32 startVertex = 0;
};

plume::RenderPrimitiveTopology MapPrimitiveType(u32 prim) {
  switch (static_cast<xe::PrimitiveType>(prim)) {
  case xe::PrimitiveType::kPointList:
    return plume::RenderPrimitiveTopology::POINT_LIST;
  case xe::PrimitiveType::kLineList:
    return plume::RenderPrimitiveTopology::LINE_LIST;
  case xe::PrimitiveType::kLineStrip:
    return plume::RenderPrimitiveTopology::LINE_STRIP;
  case xe::PrimitiveType::kTriangleList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  case xe::PrimitiveType::kTriangleFan:
    return plume::RenderPrimitiveTopology::TRIANGLE_FAN;
  case xe::PrimitiveType::kTriangleStrip:
    return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  case xe::PrimitiveType::kQuadList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  default: {
    // Unmapped X360 primitive type (e.g. kRectangleList). Drawing as a
    // triangle list is almost certainly wrong, so warn rather than fail
    // silently.
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 4) {
      BD_WARN("MapPrimitiveType: unmapped X360 primitive type {} drawn as "
              "TRIANGLE_LIST - rect/unknown topology not implemented",
              prim);
    }
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  }
}

void DispatchDraw(u32 device_guest, u32 primitive_type, const char *name,
                  const DrawArgs &args = {}) {
  WWE12_CPU_ZONE(kDraws);
  bd::gpu::g_live_ab_draws.fetch_add(1, std::memory_order_relaxed);
  if (bd::gpu::g_draw_profile) bd::gpu::g_draw_prof_ns[5].fetch_add(1, std::memory_order_relaxed);
  DrawProfSection prof_pre(1);
#if defined(SVR_NATIVE_RENDERER)
  // A clear the Xbox did after resolving this surface's tiles, due now that it is drawn again.
  if (const auto *dev = bd::mem::at<const bd::gpu::D3DDevice>(device_guest)) {
    u32 clear_flags = 0, clear_color = 0, clear_stencil = 0;
    float clear_depth = 0.0f;
    if (bd::gpu::SvrTakePendingClear(u32(dev->renderTargetShadow[0]), clear_flags, clear_color,
                                     clear_depth, clear_stencil))
      bd::gpu::Video::Clear(clear_flags, clear_color, clear_depth, clear_stencil);
  }
#endif
#if defined(REXGLUE_ENABLE_PROFILING)
  // Per-draw pass/shader attribution, only formatted while a profiler is
  // connected (TRACY_ON_DEMAND).
  char zone_name[64] = "Draw";
  if (BD_PROFILER_CONNECTED()) {
    const auto *ps = bd::gpu::state().pixel_shader;
    const u64 ps_hash =
        (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
    std::snprintf(zone_name, sizeof(zone_name), "Draw pass=%u ps=%016llX",
                  bd::gpu::CurrentRenderPassId(),
                  static_cast<unsigned long long>(ps_hash));
  }
  // CPU zone only: per-draw GPU timestamps serialize the GPU and poison every
  // GPU number in the capture. Coarse GPU cost comes from the per-frame zones.
  BD_CPU_ZONE_DYN(zone_name);
#endif
  // One lock across the whole recording sequence: loader threads record texture
  // uploads and Present records under the same mutex, and the per-frame command
  // list they all write is single-producer.
  auto &s = bd::gpu::state();
  std::unique_lock<std::mutex> lock(s.mutex);
  bd::gpu::Video::OpenCommandListLocked();

#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrDrawLogDraw(device_guest, name, primitive_type, args.indexed,
                          args.vertexOrIndexCount,
                          args.indexed ? args.startIndex : args.startVertex,
                          args.baseVertexIndex);
  // Dev bisection switch: SVR_SKIP_DRAWS bitmask, 1 indexed, 2 DrawVertices, 4 BeginVertices (UP),
  // 8 any draw into a render target other than R8G8B8A8, 16 indexed with DHEN3N normals (lit
  // meshes), 32 indexed without, 64 unlit indexed skinned (real BLENDINDICES), 128 unlit indexed
  // static.
  {
    static const u32 skip_mask = [] {
      const char *env = std::getenv("SVR_SKIP_DRAWS");
      return env ? u32(std::strtoul(env, nullptr, 0)) : 0u;
    }();
    if (skip_mask) {
      const bool up = args.is_up;
      const auto *decl = s.pipelineState.vertexDeclaration;
      const bool lit = decl && decl->hasDhen3nNormal;
      bool skinned = false;
      if (decl) {
        for (u32 i = 0; i < decl->inputElementCount; ++i) {
          const auto &e = decl->inputElements[i];
          if (e.slotIndex != 15 && e.semanticName &&
              std::strcmp(e.semanticName, "BLENDINDICES") == 0)
            skinned = true;
        }
      }
      if (((skip_mask & 1) && args.indexed) || ((skip_mask & 2) && !args.indexed && !up) ||
          ((skip_mask & 4) && up) ||
          ((skip_mask & 8) && s.pipelineState.renderTargetFormat !=
                                  plume::RenderFormat::R8G8B8A8_UNORM) ||
          ((skip_mask & 16) && args.indexed && lit) ||
          ((skip_mask & 32) && args.indexed && !lit) ||
          ((skip_mask & 64) && args.indexed && !lit && skinned) ||
          ((skip_mask & 128) && args.indexed && !lit && !skinned))
        return;
    }
  }
  // SvR 2010's edge-blur post pass (pixel shader 0x73413A409D5E296B): a 3x3 Sobel edge test that
  // replaces every edge pixel with the average of its 9 neighbours. On the console that softened
  // jaggies at 720p; here its 1-pixel 720p offsets are 2+ pixels at the internal resolution, so it
  // blurs hair, tattoos, ropes and the crowd while the supersampling already smooths edges. Skipping
  // the draw leaves the scene as rendered (the pass only rewrites the scene with a blurred copy).
  if (s.vertex_shader && s.vertex_shader->shaderCacheEntry &&
      Wwe12BeltProbe(device_guest, s.vertex_shader->shaderCacheEntry->hash, args.vertexOrIndexCount,
                     args.indexed ? args.startIndex : args.startVertex))
    return;
  if (!REXCVAR_GET(wwe12_shadows) &&
      s.pipelineState.renderTargetFormat == plume::RenderFormat::R32_FLOAT &&
      s.pipelineState.depthStencilFormat != plume::RenderFormat::UNKNOWN)
    return;
  if (!REXCVAR_GET(wwe12_crowd) && s.vertex_shader && s.vertex_shader->shaderCacheEntry) {
    const u64 vs = s.vertex_shader->shaderCacheEntry->hash;
    if (vs == 0x5B5BA6E0BD55C5A2ull || vs == 0x89B81CEC1095FF3Bull || vs == 0xF578E8C06A67CD09ull ||
        vs == 0xB2F2C29A78FFD857ull || vs == 0x826B5DD16C1147DCull)
      return;
  }
  if (const std::string& skip = REXCVAR_GET(svr_skip_shaders); !skip.empty()) {
    static std::string parsed_from;
    static std::vector<u64> hashes;
    if (parsed_from != skip) {
      parsed_from = skip;
      hashes.clear();
      size_t pos = 0;
      while (pos < skip.size()) {
        const size_t end = skip.find(',', pos);
        const std::string item = skip.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!item.empty()) hashes.push_back(std::strtoull(item.c_str(), nullptr, 16));
        if (end == std::string::npos) break;
        pos = end + 1;
      }
    }
    const u64 vs = (s.vertex_shader && s.vertex_shader->shaderCacheEntry) ? s.vertex_shader->shaderCacheEntry->hash : 0;
    const u64 ps = (s.pixel_shader && s.pixel_shader->shaderCacheEntry) ? s.pixel_shader->shaderCacheEntry->hash : 0;
    for (u64 h : hashes)
      if (h && (h == vs || h == ps)) return;
  }
  if (!REXCVAR_GET(svr_edge_blur) && s.pixel_shader && s.pixel_shader->shaderCacheEntry &&
      s.pixel_shader->shaderCacheEntry->hash == 0x73413A409D5E296Bull)
    return;
  // Vulkan has no "no index buffer" state: an indexed draw with none bound, or reaching past the
  // end of the bound one, can hang the GPU. Such a draw is skipped (and counted in the log).
  if (args.indexed && primitive_type != 13) {
    const u32 index_size = s.index_view.format == plume::RenderFormat::R32_UINT ? 4u : 2u;
    const u64 end = (u64(args.startIndex) + args.vertexOrIndexCount) * index_size;
    if (s.index_view.buffer.ref == nullptr || end > s.index_view.size) {
      static std::atomic<u32> s_skipped{0};
      const u32 k = s_skipped.fetch_add(1, std::memory_order_relaxed);
      if (k < 16 || (k & (k - 1)) == 0)
        BD_WARN("[draw] SvR: skipped indexed draw #{} ({}): {} count={} startI={} ib size={}", k,
                s.index_view.buffer.ref ? "past end of index buffer" : "no index buffer", name,
                args.vertexOrIndexCount, args.startIndex, s.index_view.size);
      return;
    }
  }
#endif

#if defined(SVR_NATIVE_RENDERER)
  // SvR's SetViewport isn't hooked: the viewport the game's draw uses is the one its D3D keeps in
  // the device (floats at +0x3160; SetRenderTarget/SetDepthStencilSurface reset it to the surface).
  if (const auto *dev = bd::mem::at<const bd::gpu::D3DDevice>(device_guest)) {
    const auto &vp = dev->viewport;
    // The bound host target is this many times the guest's size.
    const float k = bd::gpu::SvrSurfaceScale(u32(dev->renderTargetShadow[0]));
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.x, float(vp.X) * k);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.y, float(vp.Y) * k);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.width,
                                         float(vp.Width) * k);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.height,
                                         float(vp.Height) * k);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.minDepth,
                                         float(vp.MinZ));
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.maxDepth,
                                         float(vp.MaxZ));
    s.dirtyStates.scissorRect = s.dirtyStates.scissorRect || s.dirtyStates.viewport;
  }
#endif

  // PSO key includes topology, so set it before any flush.
  bd::gpu::Video::SetDirtyValue<plume::RenderPrimitiveTopology>(
      s.dirtyStates.pipelineState, s.pipelineState.primitiveTopology,
      MapPrimitiveType(primitive_type));

  prof_pre.end();
  {
    DrawProfSection prof(2);
    if (!bd::gpu::Video::BindDrawFramebufferLocked()) {
      return;
    }
  }
  {
    DrawProfSection prof(3);
    if (!bd::gpu::Video::FlushRenderStateLocked(device_guest)) {
      return; // FlushRenderState logs its own reason
    }
  }
  DrawProfSection prof_record(4);
  if (bd::gpu::g_draw_profile) bd::gpu::g_draw_prof_ns[6].fetch_add(1, std::memory_order_relaxed);

  auto *cmd_list = s.command_list;
  bd::gpu::census::Mark(s, bd::gpu::census::kDraw, s.bound_fb_rt ? s.bound_fb_rt : s.bound_fb_ds);
  if (!cmd_list)
    return;

  if (primitive_type == 13) {
    u32 quads = args.vertexOrIndexCount / 4;
    const u32 max_quads = bd::gpu::Video::QuadlistMaxQuads();
    if (quads > max_quads)
      quads = max_quads;
    const auto *ib = bd::gpu::Video::QuadlistExpansionIBView();
    if (quads && ib) {
      const i32 base = args.indexed ? args.baseVertexIndex
                                    : static_cast<i32>(args.startVertex);
      cmd_list->setIndexBuffer(ib);
      cmd_list->drawIndexedInstanced(quads * 6, 1, 0, base, 0);
      // The expansion IB replaced the tracked guest IB on the command list, and
      // without re-dirtying the next indexed draw reads quad pattern indices.
      s.dirtyStates.indices = true;
    }
  } else if (args.indexed) {
    // No IB bound on an indexed draw: FlushRenderState skipped setIndexBuffer
    // and D3D12 fires EXECUTION WARNING #211, every index reads as 0,
    // geometry collapses, scene goes black.
    if (s.index_view.buffer.ref == nullptr) {
      static std::atomic<u32> s_no_ib{0};
      const u32 k = s_no_ib.fetch_add(1, std::memory_order_relaxed);
      if (k < 16) {
        BD_WARN("[draw] indexed draw with no IB bound: #{} {} count={} "
                "startI={} baseV={}",
                k, name, args.vertexOrIndexCount, args.startIndex,
                args.baseVertexIndex);
      }
    }
    cmd_list->drawIndexedInstanced(args.vertexOrIndexCount, 1, args.startIndex,
                                   args.baseVertexIndex, 0);
  } else {
    cmd_list->drawInstanced(args.vertexOrIndexCount, 1, args.startVertex, 0);
  }
}

// bdBuildQuadVertices assembles every glyph into this one buffer, and unlike
// bdPrim it divides each position by the 2D basis on the way in, so text
// arrives in the unit square while sprites arrive in canvas pixels.
constexpr u32 kTextQuadBatchEA = 0x82DBECF0;

// Which edges of a quad's UV rect the inset is allowed to move.
enum class UVEdges {
  All,      // every edge, for a batch whose cells are known to abut
  CellSeam, // only edges sitting on an interior texel boundary
};

void FitDesignCanvasVertices(u8 *verts, u32 primitiveType, u32 vertexCount,
                             u32 vertexStride, bool normalized);
void InsetQuadUVs(u8 *verts, u32 vertexCount, u32 vertexStride, UVEdges edges);
bool IsScreenSpriteQuad(u32 primitiveType, u32 vertexCount, u32 vertexStride);

bool UploadAndBindUpVertices(u32 primitiveType, u32 pVertexData,
                             u32 vertexCount, u32 vertexStride) {
  if (!pVertexData || !vertexCount || !vertexStride)
    return false;
  const u32 totalSize = vertexCount * vertexStride;
  auto alloc = bd::gpu::UploadGuestBytesByteSwap32(pVertexData, totalSize,
                                                   /*alignment=*/4);
  if (!alloc.memory)
    return false;
#if !defined(SVR_NATIVE_RENDERER)
  // Blue Dragon's 2D canvas fit-ups (its text batch address, sprite UV insets); SvR's UI vertices
  // are drawn as the game wrote them.
  const bool text_batch = pVertexData == kTextQuadBatchEA;
  FitDesignCanvasVertices(alloc.memory, primitiveType, vertexCount,
                          vertexStride, text_batch);
  if (text_batch)
    InsetQuadUVs(alloc.memory, vertexCount, vertexStride, UVEdges::All);
  else if (IsScreenSpriteQuad(primitiveType, vertexCount, vertexStride))
    InsetQuadUVs(alloc.memory, vertexCount, vertexStride, UVEdges::CellSeam);
#endif
  bd::gpu::Video::SetVertexStream(0, alloc.ref, alloc.size, vertexStride);
  return true;
}

// Zooming an event sprite about an off-canvas pivot retreats the authored art
// inside the frame and lets the power-of-two padding take the edge, which X360
// TV overscan hid. Pull the sampled UV back until the art edge meets the
// frame edge. Sibling of bdMotionBlurQuadInsetHook in gpu/hooks/tweaks.cpp.
constexpr u32 kQuadVertices = 4;

// An overshoot an artist could hide had to fit inside title-safe overscan. Past
// that, the art fills its canvas and the UV means what it says.
constexpr float kOverscanFraction = 0.05f;

// bdPrimPushVertex2D's layout. bdDrawRectPrimitive shares the stride but puts
// the color where u sits here, and submits nothing but QUADLIST or LINESTRIP,
// so the primitive type is what tells the two apart.
struct ScreenSpriteVertex {
  be_f32 x;
  be_f32 y;
  be_f32 u;
  be_f32 v;
  be_u32 color;
};
static_assert(sizeof(ScreenSpriteVertex) == 0x14);

// Stride tells the prim system's three vertex layouts apart. The two 2D ones
// lead with a canvas position, the 3D one with a world position.
constexpr u32 kPrimVertex2DDualTexStride = 0x1C;

bool Is2DPrimStride(u32 stride) {
  return stride == sizeof(ScreenSpriteVertex) ||
         stride == kPrimVertex2DDualTexStride;
}

// bdPrimPushVertex2D writes x,y,u,v,color and always submits a strip, while
// bdDrawRectPrimitive shares the stride but puts the color where u sits and
// submits QUADLIST or LINESTRIP. Matching all three tells the two apart.
bool IsScreenSpriteQuad(u32 primitiveType, u32 vertexCount, u32 vertexStride) {
  return static_cast<xe::PrimitiveType>(primitiveType) ==
             xe::PrimitiveType::kTriangleStrip &&
         vertexCount == kQuadVertices &&
         vertexStride == sizeof(ScreenSpriteVertex);
}

// uv runs linearly from uv_lo at pos_lo to uv_hi at pos_hi. Yields the uv_hi
// that puts art_uv on the frame edge, or nothing when the quad already keeps
// the padding off screen.
std::optional<float> InsetSampledSpan(float pos_lo, float pos_hi, float uv_lo,
                                      float uv_hi, float frame_extent,
                                      float art_uv) {
  const float span = pos_hi - pos_lo;
  const float reach = frame_extent - pos_lo;
  if (span <= 0.0f || uv_hi <= uv_lo || uv_lo >= art_uv)
    return std::nullopt;
  // Only a quad covering the whole frame can put padding on screen.
  if (pos_lo > 0.0f || pos_hi < frame_extent)
    return std::nullopt;
  const float at_edge = uv_lo + reach * (uv_hi - uv_lo) / span;
  if (at_edge <= art_uv || at_edge > art_uv * (1.0f + kOverscanFraction))
    return std::nullopt;
  return uv_lo + (art_uv - uv_lo) * span / reach;
}

void InsetOverscanScreenSprite(u32 pVertexData, u32 primitiveType,
                               u32 vertexCount, u32 vertexStride) {
  if (!IsScreenSpriteQuad(primitiveType, vertexCount, vertexStride))
    return;

  const auto *tex = bd::gpu::state().textures[0];
  if (!tex)
    return;
  const auto tex_w = static_cast<float>(tex->width);
  const auto tex_h = static_cast<float>(tex->height);
  if (tex_w <= bd::gpu::kDesignCanvasWidth ||
      tex_h <= bd::gpu::kDesignCanvasHeight)
    return;

  auto *v = bd::mem::at<ScreenSpriteVertex>(pVertexData);
  if (!v)
    return;

  // Strip order is top-left, bottom-left, top-right, bottom-right, so u is
  // shared down each column and v across each row.
  if (auto inset_u = InsetSampledSpan(v[0].x, v[2].x, v[0].u, v[2].u,
                                      bd::gpu::kDesignCanvasWidth,
                                      bd::gpu::kDesignCanvasWidth / tex_w)) {
    v[2].u = *inset_u;
    v[3].u = *inset_u;
  }
  if (auto inset_v = InsetSampledSpan(v[0].y, v[1].y, v[0].v, v[1].v,
                                      bd::gpu::kDesignCanvasHeight,
                                      bd::gpu::kDesignCanvasHeight / tex_h)) {
    v[1].v = *inset_v;
    v[3].v = *inset_v;
  }
}

// Guest UV rects land on exact texel boundaries, so a fraction of a texel of
// slack is float noise rather than an authored position.
constexpr float kTexelBoundaryTolerance = 0.01f;

// An atlas packs its cells flush against each other, so a UV edge on a texel
// boundary with texture on both sides is a cell seam. An edge at 0 or 1 has no
// neighbor to reach across, and CLAMP already returns the border texel there.
bool OnCellSeam(float uv, float extent) {
  if (uv <= 0.0f || uv >= 1.0f)
    return false;
  const float texel = uv * extent;
  return std::abs(texel - std::round(texel)) < kTexelBoundaryTolerance;
}

// At any scale but 1:1 a bilinear tap at a quad edge reaches past it, so an
// edge sitting on a cell seam returns the neighbor cell: the next glyph's ink,
// the orange copy of a gauge behind the gray one, the far half of a nine slice.
// Pull the edge half a source texel inward, which keeps every tap inside its
// own cell at any scale and still puts the 1:1 case on texel centers.
//
// UVEdges::All moves both edges whether or not they measure onto a boundary,
// for the glyph batch, whose cells are known to abut. CellSeam moves only the
// edges that do. That is what leaves a quad spanning a whole texture, the frame
// composite above all, sampling its own texel grid untouched.
//
// Applied to the uploaded copy, after the byte swap, so guest memory is
// untouched.
void InsetQuadUVs(u8 *verts, u32 vertexCount, u32 vertexStride, UVEdges edges) {
  if (vertexStride != sizeof(ScreenSpriteVertex))
    return;
  const auto *tex = bd::gpu::state().textures[0];
  if (!tex || !tex->width || !tex->height)
    return;
  const float extent[2] = {static_cast<float>(tex->width),
                           static_cast<float>(tex->height)};

  for (u32 base = 0; base + kQuadVertices <= vertexCount;
       base += kQuadVertices) {
    auto uv = [verts, vertexStride, base](u32 i) {
      return reinterpret_cast<float *>(verts +
                                       (base + i) * size_t{vertexStride}) +
             2;
    };
    for (u32 axis = 0; axis < 2; ++axis) {
      const float inset = 0.5f / extent[axis];
      float lo = uv(0)[axis], hi = lo;
      for (u32 i = 1; i < kQuadVertices; ++i) {
        lo = std::min(lo, uv(i)[axis]);
        hi = std::max(hi, uv(i)[axis]);
      }
      if (hi - lo <= 2.0f * inset)
        continue;
      const bool move_lo =
          edges == UVEdges::All || OnCellSeam(lo, extent[axis]);
      const bool move_hi =
          edges == UVEdges::All || OnCellSeam(hi, extent[axis]);
      // bdBuildQuadVertices copies the rect's two u and two v values verbatim
      // into the corners, so comparing against the extremes recovers which
      // corner each vertex is regardless of winding or mirroring.
      for (u32 i = 0; i < kQuadVertices; ++i) {
        float &c = uv(i)[axis];
        if (move_lo && c == lo)
          c = lo + inset;
        else if (move_hi && c == hi)
          c = hi - inset;
      }
    }
  }
}

// Authored extents this far apart still count as the same canvas edge.
constexpr float kCanvasEdgeTolerance = 8.0f;

bool IsOnePrimitive(u32 primitiveType, u32 vertexCount) {
  switch (static_cast<xe::PrimitiveType>(primitiveType)) {
  case xe::PrimitiveType::kTriangleStrip:
  case xe::PrimitiveType::kTriangleFan:
  case xe::PrimitiveType::kLineStrip:
    return true;
  default:
    return vertexCount <= 4;
  }
}

// Scales one 2D draw about the canvas center. It has to be the geometry rather
// than a shrunk viewport: the rasterizer clips to the NDC box before the
// viewport transform, so a backdrop reaching for the surface edge would be cut
// off at exactly the rect it was trying to escape.
//
// Applied to the uploaded copy, so guest memory is untouched. 'normalized' says
// the draw arrived already divided by the 2D basis, as the text batch is.
void FitDesignCanvasVertices(u8 *verts, u32 primitiveType, u32 vertexCount,
                             u32 vertexStride, bool normalized) {
  if (!verts || !bd::gpu::Video::DesignCanvasDrain() ||
      !Is2DPrimStride(vertexStride) || !vertexCount)
    return;
  const float kx = bd::gpu::Output::DesignScaleX();
  const float ky = bd::gpu::Output::DesignScaleY();
  if (kx == 1.0f && ky == 1.0f)
    return;

  const float canvas_x = normalized ? 1.0f : bd::gpu::kDesignCanvasWidth;
  const float canvas_y = normalized ? 1.0f : bd::gpu::kDesignCanvasHeight;
  const float tol_x = kCanvasEdgeTolerance * canvas_x / bd::gpu::kDesignCanvasWidth;
  const float tol_y =
      kCanvasEdgeTolerance * canvas_y / bd::gpu::kDesignCanvasHeight;

  // Uploaded already byte-swapped, so position is two host floats at the front
  // of each vertex.
  auto pos = [verts, vertexStride](u32 i) {
    return reinterpret_cast<float *>(verts + i * vertexStride);
  };

  float min_x = 999999.0f, min_y = 999999.0f;
  float max_x = -999999.0f, max_y = -999999.0f;
  for (u32 i = 0; i < vertexCount; ++i) {
    const float *p = pos(i);
    min_x = std::min(min_x, p[0]);
    min_y = std::min(min_y, p[1]);
    max_x = std::max(max_x, p[0]);
    max_y = std::max(max_y, p[1]);
  }

  // Covering the canvas, not matching it: a menu's ground is often authored
  // well past the edges, and scaling one of those inward opens a gap at the
  // very edge it was oversized to reach.
  const bool one = IsOnePrimitive(primitiveType, vertexCount);
  const bool covers_x = one && min_x <= tol_x && max_x >= canvas_x - tol_x;
  const bool covers_y = one && min_y <= tol_y && max_y >= canvas_y - tol_y;
  const bool flush_x = one && (min_x <= tol_x || max_x >= canvas_x - tol_x);
  const bool flush_y = one && (min_y <= tol_y || max_y >= canvas_y - tol_y);
  const bool spans_x = covers_x || (covers_y && flush_x);
  const bool spans_y = covers_y || (covers_x && flush_y);
  const float scale_x = spans_x ? 1.0f : kx;
  const float scale_y = spans_y ? 1.0f : ky;
  if (scale_x == 1.0f && scale_y == 1.0f)
    return;

  const float mid_x = canvas_x * 0.5f;
  const float mid_y = canvas_y * 0.5f;
  for (u32 i = 0; i < vertexCount; ++i) {
    float *p = pos(i);
    p[0] = mid_x + (p[0] - mid_x) * scale_x;
    p[1] = mid_y + (p[1] - mid_y) * scale_y;
  }
}

u32 D3DDevice_DrawVerticesUP_hook(u32 device_guest, u32 primitiveType,
                                  u32 vertexCount, u32 pVertexData,
                                  u32 vertexStride) {
  InsetOverscanScreenSprite(pVertexData, primitiveType, vertexCount,
                            vertexStride);
  const bool ok =
      UploadAndBindUpVertices(primitiveType, pVertexData, vertexCount,
                              vertexStride);
  DrawArgs args{};
  args.is_up = ok;
  args.vertexOrIndexCount = vertexCount;
  args.startVertex = 0;
  DispatchDraw(device_guest, primitiveType, "DrawVerticesUP", args);
  return 0;
}

// The streaming draw-up pair. Their recompiled bodies drive the PM4 ring buffer
// reblue stubs out, so an unhooked BeginVertices spins forever in
// D3DDevice_RingBufferFlush. EndVertices copies synchronously, so the grow-only
// scratch is free again for the next BeginVertices.
struct {
  u32 va = 0;       // guest VA of the scratch buffer, 0 until first use
  u32 capacity = 0; // bytes currently allocated
} g_begin_vertices_scratch;

struct {
  u32 device = 0;
  u32 primitive_type = 0;
  u32 vertex_count = 0;
  u32 stride = 0;
  u32 data_va = 0; // 0 when nothing is pending to draw
} g_begin_vertices_pending;

u32 D3DDevice_BeginVertices_hook(u32 device_guest, u32 primitiveType,
                                 u32 vertexCount, u32 vertexStride) {
  g_begin_vertices_pending = {};
  const u64 size =
      static_cast<u64>(vertexCount) * static_cast<u64>(vertexStride);
  // Nothing to draw: return null so the caller skips its memcpy + EndVertices
  // (Visual__DrawVerticesUP gates both on a non-null BeginVertices result).
  if (size == 0 || size > 0xFFFFFFFFull)
    return 0;

  auto *memory = REX_KERNEL_MEMORY();
  if (g_begin_vertices_scratch.capacity < size) {
    // Safe to free the old block here: EndVertices copies the bytes out
    // synchronously, so nothing references the previous scratch by now.
    const u32 new_capacity = static_cast<u32>(size);
    const u32 va = memory->SystemHeapAlloc(new_capacity, 0x20);
    if (!va)
      return 0;
    if (g_begin_vertices_scratch.va)
      memory->SystemHeapFree(g_begin_vertices_scratch.va);
    g_begin_vertices_scratch.va = va;
    g_begin_vertices_scratch.capacity = new_capacity;
  }

  g_begin_vertices_pending.device = device_guest;
  g_begin_vertices_pending.primitive_type = primitiveType;
  g_begin_vertices_pending.vertex_count = vertexCount;
  g_begin_vertices_pending.stride = vertexStride;
  g_begin_vertices_pending.data_va = g_begin_vertices_scratch.va;
  return g_begin_vertices_scratch.va;
}

u32 D3DDevice_EndVertices_hook(u32 /*device_guest*/) {
  const auto p = g_begin_vertices_pending;
  g_begin_vertices_pending = {}; // consume: a stray EndVertices must not redraw
  if (!p.data_va || !p.vertex_count)
    return 0;

#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrDrawLogVertices(p.data_va, p.vertex_count, p.stride);
#endif
  const bool ok = UploadAndBindUpVertices(p.primitive_type, p.data_va,
                                         p.vertex_count, p.stride);
  DrawArgs args{};
  args.is_up = ok;
  args.vertexOrIndexCount = p.vertex_count;
  args.startVertex = 0;
  DispatchDraw(p.device, p.primitive_type, "BeginVertices", args);
  return 0;
}

// All five params must be marshaled: a 4-arg signature drops the real
// IndexCount in r7 and submits zero count draws.
u32 D3DDevice_DrawIndexedVertices_hook(u32 device_guest, u32 primitiveType,
                                       u32 baseVertexIndex, u32 startIndex,
                                       u32 indexCount) {
#if defined(SVR_NATIVE_RENDERER)
  // Streams and indices exactly as SvR's D3D left them in the device for this draw (its own
  // SetStreamSource/SetIndices are not observed any more).
  {
    DrawProfSection prof(0);
    bd::gpu::SvrBindDrawGeometry(device_guest, /*indexed=*/true);
  }
#endif
  DrawArgs args{};
  args.indexed = true;
  args.vertexOrIndexCount = indexCount;
  args.baseVertexIndex = static_cast<i32>(baseVertexIndex);
  args.startIndex = startIndex;
  DispatchDraw(device_guest, primitiveType, "DrawIndexedVertices", args);
  return 0;
}

u32 D3DDevice_Resolve_hook(u32 device_guest, u32 Flags, u32 pSourceRect,
                           u32 pDestTexture, u32 pDestPoint, u32 DestLevel,
                           u32 DestSliceOrFace, u32 pClearColor,
                           u32 /*ClearZHi*/, u32 /*ClearZLo*/,
                           u32 /*ClearStencil*/, u32 /*pParameters*/) {
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrDrawLogResolve(device_guest, Flags, pSourceRect, pDestTexture, pDestPoint);
  float source_scale = 0.0f;
  if (const auto *dev = bd::mem::at<const bd::gpu::D3DDevice>(device_guest))
    source_scale = bd::gpu::SvrSurfaceScale(u32(dev->renderTargetShadow[0]));
  if (Flags & 0x300u) {
    u32 argb = 0;
    if (const auto *c = bd::mem::try_at<const be_f32>(pClearColor)) {
      const auto pack = [](float v) -> u32 {
        return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
      };
      argb = (pack(c[3]) << 24) | (pack(c[0]) << 16) | (pack(c[1]) << 8) | pack(c[2]);
    }
    if (const auto *dev = bd::mem::at<const bd::gpu::D3DDevice>(device_guest))
      bd::gpu::SvrNoteResolveClear(u32(dev->renderTargetShadow[0]), Flags, argb);
  }
  if (const auto *r = bd::mem::try_at<const bd::gpu::D3DRect>(pSourceRect)) {
    if (const auto *dev = bd::mem::at<const bd::gpu::D3DDevice>(device_guest))
      bd::gpu::SvrNoteTiledResolve(u32(dev->renderTargetShadow[0]), u32(dev->depthStencilShadow),
                                   u32(r->right), u32(r->bottom));
  }
  auto *dst = bd::gpu::SvrResolveTarget(pDestTexture, source_scale);
#else
  auto *dst =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(pDestTexture);
#endif
  if (!dst) {
    static std::atomic<u32> s_miss{0};
    const u32 n = s_miss.fetch_add(1, std::memory_order_relaxed);
    if (n < 32) {
      // WWE '12 initializes its 1x1 k_4_4_4_4 textures (sampled by every 3D draw) with one
      // resolve at load: log everything that resolve carries.
      std::string detail;
#if defined(SVR_NATIVE_RENDERER)
      if (const auto *r = bd::mem::try_at<const bd::gpu::D3DRect>(pSourceRect))
        detail += fmt::format(" rect {},{}-{},{}", i32(r->left), i32(r->top), i32(r->right),
                              i32(r->bottom));
      if (const auto *p = bd::mem::try_at<const be_i32>(pDestPoint))
        detail += fmt::format(" point {},{}", i32(p[0]), i32(p[1]));
      if (const auto *c = bd::mem::try_at<const be_f32>(pClearColor))
        detail += fmt::format(" clearcolor {},{},{},{}", float(c[0]), float(c[1]), float(c[2]),
                              float(c[3]));
      if (const auto *dev = bd::mem::at<const bd::gpu::D3DDevice>(device_guest))
        detail += fmt::format(" rt 0x{:08X}", u32(dev->renderTargetShadow[0]));
      for (u32 d = 0; d < 6; ++d)
        detail += fmt::format("{}{:08X}", d ? "." : " fetch ",
                              bd::mem::load<u32>(pDestTexture + 0x1Cu + d * 4u));
#endif
      BD_WARN("D3DDevice_Resolve: destination guest VA 0x{:08X} not a "
              "host texture (flags 0x{:X} level {}{})",
              pDestTexture, Flags, DestLevel, detail);
    }
    return 0;
  }
  plume::RenderRect rect_draw{};
  bool use_rect = false;
#if defined(SVR_NATIVE_RENDERER)
  // Copy the source rect (tiles, partial updates of a texture's mip levels; the whole
  // destination when there is none) to the dest point, unscaled, as the Xbox resolve does. A
  // padded source (the 1280x736 scene surface into a 1280x720 texture) must not be squeezed.
  {
    const float scale = source_scale > 0.0f ? source_scale : bd::gpu::SvrRenderScale();
    const auto sc = [scale](i32 v) { return i32(float(v) * scale + 0.5f); };
    i32 left = 0, top = 0, right = 0, bottom = 0;
    if (const auto *r = bd::mem::try_at<const bd::gpu::D3DRect>(pSourceRect)) {
      left = sc(i32(r->left));
      top = sc(i32(r->top));
      right = sc(i32(r->right));
      bottom = sc(i32(r->bottom));
    } else {
      right = i32(std::max(1u, dst->width >> DestLevel));
      bottom = i32(std::max(1u, dst->height >> DestLevel));
    }
    i32 dx = 0, dy = 0;
    if (const auto *p = bd::mem::try_at<const be_i32>(pDestPoint)) {
      dx = sc(i32(p[0]));
      dy = sc(i32(p[1]));
    }
    if (bd::gpu::Video::ResolveRegion(Flags, dst, DestLevel, DestSliceOrFace, left, top, right,
                                      bottom, dx, dy)) {
      bd::gpu::SvrAfterResolve(pDestTexture, dst, DestLevel, DestSliceOrFace);
      return 0;
    }
    // wwe13_resolve_rect_draw: a format-converting resolve of a source rect that lands at the
    // same place (the 512x512 RGBA16F atlas tiles) draws only that rect, not the whole texture.
    if (REXCVAR_GET(wwe13_resolve_rect_draw) && !(Flags & 0x4u) && DestLevel == 0 &&
        DestSliceOrFace == 0 && bd::mem::try_at<const bd::gpu::D3DRect>(pSourceRect) &&
        dx == left && dy == top && right > left && bottom > top) {
      rect_draw = plume::RenderRect(left, top, right, bottom);
      use_rect = true;
    }
  }
#endif
  bd::gpu::Video::TrackResolveSource(Flags, dst, DestLevel, DestSliceOrFace);
  bd::gpu::Video::ResolveRtToTexture(dst, use_rect ? &rect_draw : nullptr);
  return 0;
}

u32 D3DDevice_BeginTiling_hook(u32 /*device_guest*/, u32 /*Flags*/,
                               u32 /*Count*/, u32 /*pTileRects*/,
                               mapped_f32 pClearColor, f64 ClearZ,
                               u32 /*z_gpr_slot*/, u32 ClearStencil) {
  u32 color_argb = 0;
  if (const be_f32 *color_vec = pClearColor) {
    const auto pack = [](float v) -> u32 {
      return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    color_argb = (pack(color_vec[3]) << 24) | (pack(color_vec[0]) << 16) |
                 (pack(color_vec[1]) << 8) | (pack(color_vec[2]) << 0);
  }
  bd::gpu::Video::Clear(bd::gpu::kClearAll, color_argb, float(ClearZ),
                        ClearStencil);
  return 0;
}

u32 D3DDevice_EndTiling_hook(u32 /*device_guest*/, u32 ResolveFlags,
                             u32 /*pResolveRects*/, u32 pDestTexture,
                             u32 /*pClearColor*/, u32 /*ClearZ*/,
                             u32 /*ClearStencil*/, u32 /*pParameters*/) {
  if (!pDestTexture) {
    return 0;
  }
#if defined(SVR_NATIVE_RENDERER)
  auto *dst = bd::gpu::SvrResolveTarget(pDestTexture);
#else
  auto *dst =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(pDestTexture);
#endif
  if (!dst) {
    static std::atomic<u32> s_miss{0};
    const u32 n = s_miss.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
      BD_WARN("D3DDevice_EndTiling: destination guest VA 0x{:08X} not a "
              "host texture",
              pDestTexture);
    }
    return 0;
  }
  bd::gpu::Video::TrackResolveSource(ResolveFlags, dst);
  bd::gpu::Video::ResolveRtToTexture(dst);
  return 0;
}

u32 D3DDevice_DrawVertices_hook(u32 device_guest, u32 primitiveType,
                                u32 startVertex, u32 vertexCount) {
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrBindDrawGeometry(device_guest, /*indexed=*/false);
#endif
  DrawArgs args{};
  args.vertexOrIndexCount = vertexCount;
  args.startVertex = startVertex;
  DispatchDraw(device_guest, primitiveType, "DrawVertices", args);
  return 0;
}

#if defined(SVR_NATIVE_RENDERER)
// SvR: the game's BeginVertices hands out space in its own command buffer; the vertices it writes
// there are drawn when EndVertices is reached, before the game's EndVertices submits them.
void SvrBeginVerticesObserved(PPCContext &args, u32 data_va, u8 *) {
  g_begin_vertices_pending = {};
  if (!data_va)
    return;
  g_begin_vertices_pending.device = args.r3.u32;
  g_begin_vertices_pending.primitive_type = args.r4.u32;
  g_begin_vertices_pending.vertex_count = args.r5.u32;
  g_begin_vertices_pending.stride = args.r6.u32;
  g_begin_vertices_pending.data_va = data_va;
}
#endif

} // namespace

#if defined(SVR_NATIVE_RENDERER)
// SvR: the renderer draws from the state the game's draw call is about to consume, then the game's
// own D3D function runs (see src/native/svr_hook.h).
#if !defined(SVR_NO_DRAW_VERTICES)  // SvR 2009 links no DrawVertices (wwe12_d3d_names.h maps it)
SVR_HOOK_BEFORE(D3DDevice_DrawVertices, D3DDevice_DrawVertices_hook);
#endif
SVR_HOOK_BEFORE(D3DDevice_DrawIndexedVertices, D3DDevice_DrawIndexedVertices_hook);
#if !defined(WWE12_NO_BEGIN_VERTICES)  // WWE '12 uses no BeginVertices/EndVertices
SVR_HOOK_RESULT(D3DDevice_BeginVertices, SvrBeginVerticesObserved);
SVR_HOOK_BEFORE(D3DDevice_EndVertices, D3DDevice_EndVertices_hook);
#endif
SVR_HOOK_AFTER(D3DDevice_Resolve, D3DDevice_Resolve_hook);
#else
REX_HOOK(D3DDevice_DrawVertices, D3DDevice_DrawVertices_hook);
REX_HOOK(D3DDevice_DrawVerticesUP, D3DDevice_DrawVerticesUP_hook);
REX_HOOK(D3DDevice_DrawIndexedVertices, D3DDevice_DrawIndexedVertices_hook);
REX_HOOK(D3DDevice_BeginVertices, D3DDevice_BeginVertices_hook);
REX_HOOK(D3DDevice_EndVertices, D3DDevice_EndVertices_hook);
REX_HOOK(D3DDevice_Resolve, D3DDevice_Resolve_hook);
REX_HOOK(D3DDevice_BeginTiling, D3DDevice_BeginTiling_hook);
REX_HOOK(D3DDevice_EndTiling, D3DDevice_EndTiling_hook);
#endif
