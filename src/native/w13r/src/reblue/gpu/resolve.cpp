/**
 * @file    gpu/resolve.cpp
 * @brief   EDRAM resolve emulation: surface-to-texture copy, the lazy resolve
 *          alias, and the fullscreen composite chain seeding.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/gpu_census.h"
#include "gpu/frame.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#if !defined(REBLUE_D3D12)
#include <plume_vulkan.h>
#endif
#include <rex/cvar.h>

#include "core/profiling.h"

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/format.h"

// Full same-size, same-format resolves (Video::ResolveRegion) become the same deferred source link as
// ResolveRtToTexture's lazy alias: the texture samples the render target until the target is written
// again, when MaterializeOutboundLocked copies the snapshot. Off by default (picture check pending).
REXCVAR_DEFINE_BOOL(svr_readback_async, true, "SvR",
                    "Read-backs of targets that rested (SvrProcessDeferredReadbacks) and the mip "
                    "rebuild uploads after them without a GPU wait: the copy finishes kNumFrames "
                    "presents later (WWE '13: a 25 ms stall every 301 frames in matches)");
REXCVAR_DEFINE_BOOL(wwe13_resolve_alias, false, "WWE13",
                    "Avoid full same-size, same-format single-sample resolves by aliasing the render target until its next write.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(wwe13_resolve_alias_dedupe, false, "WWE13",
                    "With wwe13_resolve_alias: deferred copies of the same snapshot are made once, into "
                    "the newest destination, and the others sample that copy");
REXCVAR_DEFINE_BOOL(wwe13_depth_alias, false, "WWE13",
                    "D3D12: sample matching D24S8 depth resolves directly until the depth buffer is written again.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bd::gpu {

namespace {

inline const plume::RenderTexture *const kDepthFbKey =
    reinterpret_cast<const plume::RenderTexture *>(uintptr_t{1});

std::atomic<u64> g_resolve_copies_avoided{0};
std::atomic<u64> g_resolve_copies_done{0};
std::chrono::steady_clock::time_point g_resolve_log_time{};
std::atomic<u64> g_depth_copies_avoided{0};
std::atomic<u64> g_depth_copies_done{0};
std::atomic<u64> g_dedupe_copies_avoided{0};
u64 g_resolve_serial = 0;  // under the video state mutex
std::chrono::steady_clock::time_point g_depth_log_time{};

void LogResolveCopyStats() {
  const auto now = std::chrono::steady_clock::now();
  if (g_resolve_log_time.time_since_epoch().count() != 0 &&
      now - g_resolve_log_time < std::chrono::seconds(10))
    return;
  g_resolve_log_time = now;
  BD_INFO("[wwe13-resolve-alias] copies avoided={} done={} deduped={}",
          g_resolve_copies_avoided.load(std::memory_order_relaxed),
          g_resolve_copies_done.load(std::memory_order_relaxed),
          g_dedupe_copies_avoided.load(std::memory_order_relaxed));
}

void LogDepthCopyStats() {
  const auto now = std::chrono::steady_clock::now();
  if (g_depth_log_time.time_since_epoch().count() != 0 &&
      now - g_depth_log_time < std::chrono::seconds(10))
    return;
  g_depth_log_time = now;
  BD_INFO("[wwe13-depth-alias] copies avoided={} done={}",
          g_depth_copies_avoided.load(std::memory_order_relaxed),
          g_depth_copies_done.load(std::memory_order_relaxed));
}

GuestTexture *ResolveSourceForFlagsLocked(VideoState &s, u32 flags) {
  if (flags & kResolveDepthStencil)
    return s.depth_stencil;
  if (s.render_target)
    return s.render_target;
  return s.back_buffer_surface;
}

bool CanAliasDepthResolveLocked(const VideoState &s, const GuestTexture *src,
                                const GuestTexture *dst) {
#if defined(REBLUE_D3D12) && !defined(REBLUE_D3D11)
  return REXCVAR_GET(wwe13_depth_alias) && src && dst && src == s.depth_stencil &&
         src != dst && src->texture && dst->texture &&
         src->format == plume::RenderFormat::D24_UNORM_S8_UINT &&
         dst->format == plume::RenderFormat::R32_FLOAT &&
         src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
         src->width == dst->width && src->height == dst->height &&
         dst->resolveScale == 1.0f && dst->viewDimension !=
             plume::RenderTextureViewDimension::TEXTURE_CUBE &&
         dst->mipLevels <= 1 && src->descriptorIndex != kInvalidDescriptorIndex;
#else
  (void)s;
  (void)src;
  (void)dst;
  return false;
#endif
}

bool CanAliasResolveLocked(const GuestTexture *src, const GuestTexture *dst) {
  return src && dst && src != dst && src->texture && dst->texture &&
         src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
         src->width == dst->width && src->height == dst->height &&
         src->format == dst->format && dst->resolveScale == 1.0f &&
         dst->viewDimension !=
             plume::RenderTextureViewDimension::TEXTURE_CUBE &&
         dst->mipLevels <= 1 && src->descriptorIndex != kInvalidDescriptorIndex;
}

} // namespace

void DetachSourceSurfaceLocked(VideoState &s, GuestTexture *texture) {
  if (!texture || !texture->sourceSurface)
    return;
  GuestTexture *source = texture->sourceSurface;
  source->destinationTextures.erase(texture);
  texture->sourceSurface = nullptr;
}

bool CopySurfaceToTextureLocked(VideoState &s, GuestTexture *src,
                                GuestTexture *dst, const char *reason,
                                const plume::RenderRect *rect) {
  if (!dst || !dst->texture || !src || !src->texture || src == dst) {
    return false;
  }
  if (!s.ready)
    return false;
  // Textures that sample dst (alias dedupe) keep its current content.
  if (!dst->destinationTextures.empty())
    MaterializeOutboundLocked(s, dst);
  if (dst->sampleCount != plume::RenderSampleCount::COUNT_1) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      BD_WARN("{}: MSAA destination {}x{} rejected (resolve targets must be "
              "single-sample)",
              reason, dst->width, dst->height);
    }
    return false;
  }

  BeginCommandList(s);
  if (!s.command_list_open)
    return false;

  s.draw_framebuffer_bound = false;

  const bool dims_match =
      src->width == dst->width && src->height == dst->height;
  const bool format_match = src->format == dst->format;
  const float resolve_scale = dst->resolveScale;
  const bool needs_scale = (resolve_scale != 1.0f);
  const bool dst_is_cube =
      dst->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE;

  if (dims_match && format_match && !needs_scale &&
      src->sampleCount == plume::RenderSampleCount::COUNT_1) {
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture,
                                    plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(dst->texture,
                                    plume::RenderTextureLayout::COPY_DEST),
    };
    s.command_list->barriers(plume::RenderBarrierStage::COPY, pre,
                             std::size(pre));
    src->layout = plume::RenderTextureLayout::COPY_SOURCE;
    dst->layout = plume::RenderTextureLayout::COPY_DEST;
    if (dst_is_cube) {
      census::Mark(s, census::kResolveCopy, src, dst);
      s.command_list->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(
              dst->texture, dst->resolveLevel, dst->resolveFace),
          plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0));
    } else {
      census::Mark(s, census::kResolveCopy, src, dst);
      s.command_list->copyTexture(dst->texture, src->texture);
    }
  } else if (dst_is_cube) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      BD_WARN("{}: cube resolve dst needs convert/scale (src={}x{} fmt={} "
              "dst fmt={} scale={}), no per-face RTV, skipping",
              reason, src->width, src->height, static_cast<int>(src->format),
              static_cast<int>(dst->format), resolve_scale);
    }
    return false;
  } else {
    if (src->descriptorIndex == kInvalidDescriptorIndex) {
      BindTextureSRVLocked(s, src);
    }
    if (src->descriptorIndex == kInvalidDescriptorIndex) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        BD_ERROR("{}: source RT 0x{:x} not in bindless heap, can't "
                 "shader-resolve",
                 reason, reinterpret_cast<uintptr_t>(src));
      }
      return false;
    }

    const bool depth_dst = IsDepthFormat(dst->format);
    if (!depth_dst && !IsRenderTargetCapable(dst->format)) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        BD_ERROR("{}: dst format {} is not RT-capable, skipping shader "
                 "resolve (src={}x{} fmt={} dst={}x{} fmt={})",
                 reason, static_cast<int>(dst->format), src->width, src->height,
                 static_cast<int>(src->format), dst->width, dst->height,
                 static_cast<int>(dst->format));
      }
      return false;
    }

    const plume::RenderTexture *fb_key = depth_dst ? kDepthFbKey : nullptr;

    auto it = dst->framebuffers.find(fb_key);
    plume::RenderFramebuffer *dst_fb = nullptr;
    if (it != dst->framebuffers.end()) {
      dst_fb = it->second.get();
    } else {
      std::unique_ptr<plume::RenderFramebuffer> fb;
      if (depth_dst) {
        plume::RenderFramebufferDesc desc;
        desc.depthAttachment = dst->texture;
        desc.colorAttachmentsCount = 0;
        fb = s.device->createFramebuffer(desc);
      } else {
        const plume::RenderTexture *attachments[1] = {dst->texture};
        plume::RenderFramebufferDesc desc(attachments, 1);
        fb = s.device->createFramebuffer(desc);
      }
      if (!fb) {
        static std::atomic<u32> s_warn{0};
        if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
          BD_ERROR("{}: createFramebuffer failed (depth_dst={}), skipping "
                   "shader resolve (src={}x{} dst={}x{})",
                   reason, depth_dst ? 1 : 0, src->width, src->height,
                   dst->width, dst->height);
        }
        return false;
      }
      dst_fb = fb.get();
      dst->framebufferAttached = true;
      dst->framebuffers.emplace(fb_key, std::move(fb));
      s.framebuffer_owners.insert(dst);
    }

    const plume::RenderTextureLayout dst_write_layout =
        depth_dst ? plume::RenderTextureLayout::DEPTH_WRITE
                  : plume::RenderTextureLayout::COLOR_WRITE;
    const bool discard_dst =
        (dst->layout == plume::RenderTextureLayout::UNKNOWN);
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture,
                                    plume::RenderTextureLayout::SHADER_READ),
        plume::RenderTextureBarrier(dst->texture, dst_write_layout),
    };
    census::Mark(s, census::kResolveBarrier);
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, pre,
                             std::size(pre));
    src->layout = plume::RenderTextureLayout::SHADER_READ;
    dst->layout = dst_write_layout;
    if (discard_dst)
      s.command_list->discardTexture(dst->texture);

    s.command_list->setFramebuffer(dst_fb);
    // A source only padded past the destination (WWE '12's 1280x736 tiled scene surface, color
    // and depth, resolved into 1280x720 textures) is copied 1:1 like the Xbox resolve does, not
    // squeezed: the viewport spans the source and the scissor keeps the destination.
    float vp_w = static_cast<float>(dst->width);
    float vp_h = static_cast<float>(dst->height);
    if (src->sampleCount == plume::RenderSampleCount::COUNT_1 && src->width >= dst->width &&
        src->height >= dst->height && src->width < dst->width * 2u &&
        src->height < dst->height * 2u) {
      vp_w = static_cast<float>(src->width);
      vp_h = static_cast<float>(src->height);
    }
    s.command_list->setViewports(plume::RenderViewport(0.0f, 0.0f, vp_w, vp_h));
    // wwe13_resolve_rect_draw: only the resolved rect (1:1, same size, not a first write that
    // discarded the texture), as the Xbox resolve writes; else the whole destination.
    if (rect && !discard_dst && dims_match && !needs_scale &&
        src->sampleCount == plume::RenderSampleCount::COUNT_1)
      s.command_list->setScissors(*rect);
    else
      s.command_list->setScissors(plume::RenderRect(
          0, 0, static_cast<i32>(dst->width), static_cast<i32>(dst->height)));
    plume::RenderPipeline *pipeline = nullptr;
    if (src->sampleCount != plume::RenderSampleCount::COUNT_1) {
      pipeline = GetOrCreateResolveMSAAPipeline(s, dst->format,
                                                src->sampleCount, depth_dst);
    } else if (depth_dst) {
      pipeline = GetOrCreateCopyDepthPipeline(s, dst->format);
    } else {
      pipeline = GetOrCreateResolvePipeline(s, dst->format);
    }
    if (!pipeline) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        BD_ERROR("{}: createGraphicsPipeline failed for dst format {}", reason,
                 static_cast<int>(dst->format));
      }
      return false;
    }
    s.command_list->setPipeline(pipeline);
    const u32 descriptor_index = src->descriptorIndex;

    u32 box_ratio = 0u;
    if (!depth_dst && src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        dst->width != 0u && dst->height != 0u && src->width > dst->width &&
        src->height > dst->height && (src->width % dst->width) == 0u &&
        (src->height % dst->height) == 0u &&
        (src->width / dst->width) == (src->height / dst->height)) {
      box_ratio = src->width / dst->width;
    }

    struct CopyPushConstants {
      u32 resourceDescriptorIndex;
      u32 resourceDescriptorIndex2;
      float param0;
      float param1;
    } push{descriptor_index, 0u, depth_dst ? 1.0f : resolve_scale,
           static_cast<float>(box_ratio)};
#if defined(REBLUE_D3D11)
    D3D11BindHostSlot(descriptor_index, 0);
#endif
    s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &push,
                                             kCopyPushConstantByteOffset,
                                             sizeof(push));
    census::Mark(s, census::kResolveDraw, src, dst);
    s.command_list->drawInstanced(3, 1, 0, 0);

    s.command_list->setFramebuffer(nullptr);
    s.current_pso = nullptr;
    s.dirtyStates.pipelineState = true;
    s.draw_framebuffer_bound = false;
  }

  if (REXCVAR_GET(wwe13_resolve_alias)) {
    g_resolve_copies_done.fetch_add(1, std::memory_order_relaxed);
    LogResolveCopyStats();
  }
#if defined(REBLUE_D3D12) && !defined(REBLUE_D3D11)
  if (REXCVAR_GET(wwe13_depth_alias) && src == s.depth_stencil &&
      dst->format == plume::RenderFormat::R32_FLOAT) {
    g_depth_copies_done.fetch_add(1, std::memory_order_relaxed);
    LogDepthCopyStats();
  }
#endif
  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

namespace {

bool ClearColorTargetLocked(VideoState &s, GuestTexture *dst,
                            plume::RenderColor color) {
  if (dst && !dst->destinationTextures.empty())
    MaterializeOutboundLocked(s, dst);  // alias dedupe: keep their snapshot
  if (!dst || !dst->texture || !s.ready)
    return false;
  if (IsDepthFormat(dst->format) ||
      dst->sampleCount != plume::RenderSampleCount::COUNT_1)
    return false;
  // A texture created without colour-attachment usage (a format that isn't RT-capable, e.g.
  // R16G16_UNORM where the device lacks it) has no valid framebuffer: clearing it through one
  // crashed Intel's driver at match start (issues #8, #13; NVIDIA and AMD let it pass).
  if (!IsRenderTargetCapable(dst->format))
    return false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  s.draw_framebuffer_bound = false;

  plume::RenderFramebuffer *dst_fb = GetFramebuffer(s, dst, nullptr);
  if (!dst_fb)
    return false;

  plume::RenderTextureBarrier pre(dst->texture,
                                  plume::RenderTextureLayout::COLOR_WRITE);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &pre, 1);
  dst->layout = plume::RenderTextureLayout::COLOR_WRITE;
  s.command_list->setFramebuffer(dst_fb);
  s.command_list->setViewports(
      plume::RenderViewport(0.0f, 0.0f, static_cast<float>(dst->width),
                            static_cast<float>(dst->height)));
  s.command_list->setScissors(plume::RenderRect(
      0, 0, static_cast<i32>(dst->width), static_cast<i32>(dst->height)));
  census::Mark(s, census::kClear);
  s.command_list->clearColor(0, color);
  s.command_list->setFramebuffer(nullptr);
  s.draw_framebuffer_bound = false;

  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

} // namespace

void Video::ClearTexture(GuestTexture *texture, u32 color_argb) {
  if (!texture || !texture->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  ClearColorTargetLocked(s, texture, ArgbToRenderColor(color_argb));
}

bool MaterializeOutboundLocked(VideoState &s, GuestTexture *source,
                               bool aliasable_only) {
  if (!source || source->destinationTextures.empty())
    return false;
  bool recorded = false;
  std::vector<GuestTexture *> destinations(source->destinationTextures.begin(),
                                           source->destinationTextures.end());
  // wwe13_resolve_alias_dedupe: the one-to-one destinations all hold the same snapshot. Copy it
  // once, into the most recently resolved one (it is resolved into again last), and let the others
  // sample that copy until it is written (CopySurfaceToTextureLocked / TrackResolveSource /
  // ResolveRegion / uploads materialize them first).
  if (REXCVAR_GET(wwe13_resolve_alias_dedupe) && destinations.size() > 1) {
    GuestTexture *keep = nullptr;
    for (GuestTexture *dst : destinations) {
      if (!dst || dst == source || dst->sourceSurface != source || dst->pendingDestroy ||
          !dst->destinationTextures.empty() || !CanAliasResolveLocked(source, dst) ||
          dst->descriptorIndex == kInvalidDescriptorIndex)
        continue;
      if (!keep || dst->resolveSerial > keep->resolveSerial)
        keep = dst;
    }
    if (keep && CopySurfaceToTextureLocked(s, source, keep, "Materialize")) {
      recorded = true;
      DetachSourceSurfaceLocked(s, keep);
      for (GuestTexture *dst : destinations) {
        if (!dst || dst == keep || dst == source || dst->sourceSurface != source ||
            dst->pendingDestroy || !dst->destinationTextures.empty() ||
            !CanAliasResolveLocked(source, dst))
          continue;
        source->destinationTextures.erase(dst);
        dst->sourceSurface = keep;
        keep->destinationTextures.insert(dst);
        g_dedupe_copies_avoided.fetch_add(1, std::memory_order_relaxed);
      }
      BindTextureSRVLocked(s, keep);
    }
    destinations.assign(source->destinationTextures.begin(), source->destinationTextures.end());
  }
  for (GuestTexture *dst : destinations) {
    if (!dst || dst == source || dst->sourceSurface != source)
      continue;
    if (dst->pendingDestroy) {
      DetachSourceSurfaceLocked(s, dst);
      continue;
    }
    const bool one_to_one =
        source->width == dst->width && source->height == dst->height &&
        source->format == dst->format &&
        source->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        dst->resolveScale == 1.0f &&
        dst->viewDimension != plume::RenderTextureViewDimension::TEXTURE_CUBE;
    if (aliasable_only && !one_to_one)
      continue;
    if (CopySurfaceToTextureLocked(s, source, dst, "Materialize")) {
      recorded = true;
      DetachSourceSurfaceLocked(s, dst);
    }
  }
  return recorded;
}

void MaterializeInboundLocked(VideoState &s, GuestTexture *dst) {
  if (!dst || !dst->sourceSurface || dst->sourceSurface == dst)
    return;
  if (CopySurfaceToTextureLocked(s, dst->sourceSurface, dst, "Materialize")) {
    DetachSourceSurfaceLocked(s, dst);
  }
}

bool FullscreenChainClassLocked(const VideoState &s, const GuestTexture *t) {
  const GuestTexture *bb = s.back_buffer_surface;
  if (!t || !bb || t == bb || IsDepthFormat(t->format) || t->reflection)
    return false;
  constexpr u32 kDesignWidth = 1280u;
  const u32 min_full_width =
      bb->width > kDesignWidth ? kDesignWidth : bb->width;
  if (t->width < min_full_width)
    return false;
  const u64 a = u64(t->width) * bb->height;
  const u64 b = u64(t->height) * bb->width;
  const u64 diff = a > b ? a - b : b - a;
  return diff * 100ull <= b * 3ull;
}

void Video::TrackResolveSource(u32 flags, GuestTexture *dst, u32 level,
                               u32 face) {
  if (!dst || !dst->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  dst->resolveLevel = level;
  dst->resolveFace = face;
  const int exp_bias = static_cast<i32>(flags) >> kResolveExponentBiasShift;
  dst->resolveScale = static_cast<float>(std::ldexp(1.0, exp_bias));
  GuestTexture *src = ResolveSourceForFlagsLocked(s, flags);
  dst->resolveSerial = ++g_resolve_serial;
  if (!dst->destinationTextures.empty())
    MaterializeOutboundLocked(s, dst);  // its content is about to change
  if (!src || !src->texture || src == dst) {
    DetachSourceSurfaceLocked(s, dst);
    return;
  }
  if (dst->sourceSurface && dst->sourceSurface != src)
    dst->sourceSurface->destinationTextures.erase(dst);
  dst->sourceSurface = src;
  src->destinationTextures.insert(dst);
  BindTextureSRVLocked(s, src);
}

namespace {

void NoteTileContentLocked(VideoState &s, GuestTexture *dst) {
  if (!dst || !dst->texture || !FullscreenChainClassLocked(s, dst))
    return;
  s.fullscreen_chain_head = dst;
}

} // namespace

void Video::ResolveRtToTexture(GuestTexture *dst, const plume::RenderRect *rect) {
  BD_CPU_ZONE("ResolveRtToTexture");
  if (!dst || !dst->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  GuestTexture *src = dst->sourceSurface;
  if (!src || src == dst || !src->texture)
    return;

  if (CanAliasDepthResolveLocked(s, src, dst)) {
    g_depth_copies_avoided.fetch_add(1, std::memory_order_relaxed);
    LogDepthCopyStats();
    s.last_resolved_dst = dst;
    NoteTileContentLocked(s, dst);
    s.draw_framebuffer_bound = false;
    return;
  }

  if (CanAliasResolveLocked(src, dst)) {
    if (REXCVAR_GET(wwe13_resolve_alias)) {
      g_resolve_copies_avoided.fetch_add(1, std::memory_order_relaxed);
      LogResolveCopyStats();
    }
    s.last_resolved_dst = dst;
    NoteTileContentLocked(s, dst);
    s.draw_framebuffer_bound = false;
    return;
  }

  if (CopySurfaceToTextureLocked(s, src, dst, "Resolve", rect)) {
    s.last_resolved_dst = dst;
    NoteTileContentLocked(s, dst);
    DetachSourceSurfaceLocked(s, dst);
  }
}

bool Video::ResolveRegion(u32 flags, GuestTexture *dst, u32 level, u32 face, i32 left, i32 top,
                          i32 right, i32 bottom, i32 dst_x, i32 dst_y) {
  if (!dst || !dst->texture || level >= dst->mipLevels)
    return false;
  const int exp_bias = static_cast<i32>(flags) >> kResolveExponentBiasShift;
  if (exp_bias != 0)
    return false;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return false;
  GuestTexture *src = ResolveSourceForFlagsLocked(s, flags);
  if (src && src != dst) {
    dst->resolveSerial = ++g_resolve_serial;
    if (!dst->destinationTextures.empty())
      MaterializeOutboundLocked(s, dst);  // its content is about to change
  }
  if (!src || !src->texture || src == dst || src->format != dst->format ||
      src->sampleCount != plume::RenderSampleCount::COUNT_1 ||
      dst->sampleCount != plume::RenderSampleCount::COUNT_1)
    return false;
  const i32 level_w = i32(std::max(1u, dst->width >> level));
  const i32 level_h = i32(std::max(1u, dst->height >> level));
  left = std::clamp(left, 0, i32(src->width));
  top = std::clamp(top, 0, i32(src->height));
  right = std::clamp(right, left, i32(src->width));
  bottom = std::clamp(bottom, top, i32(src->height));
  if (dst_x < 0 || dst_y < 0 || dst_x >= level_w || dst_y >= level_h)
    return true;  // nothing lands in the destination
  right = std::min(right, left + (level_w - dst_x));
  bottom = std::min(bottom, top + (level_h - dst_y));
  if (right <= left || bottom <= top)
    return true;

  // A complete level-zero resolve is just a view of the source until its next
  // write. Partial regions and subresources retain their accumulated-copy semantics.
  const bool full_subresource =
      level == 0 && face == 0 &&
      dst->viewDimension != plume::RenderTextureViewDimension::TEXTURE_CUBE &&
      dst->mipLevels <= 1 && dst->width == src->width &&
      dst->height == src->height && left == 0 && top == 0 &&
      right == i32(src->width) && bottom == i32(src->height) && dst_x == 0 &&
      dst_y == 0;
  if (REXCVAR_GET(wwe13_resolve_alias) && full_subresource) {
    if (dst->sourceSurface && dst->sourceSurface != src)
      dst->sourceSurface->destinationTextures.erase(dst);
    dst->sourceSurface = src;
    src->destinationTextures.insert(dst);
    BindTextureSRVLocked(s, src);
    s.last_resolved_dst = dst;
    NoteTileContentLocked(s, dst);
    s.draw_framebuffer_bound = false;
    g_resolve_copies_avoided.fetch_add(1, std::memory_order_relaxed);
    LogResolveCopyStats();
    return true;
  }

  // A partial resolve into a texture that still samples an earlier full resolve through the alias
  // must first receive that snapshot, or the part not covered now would show stale texture memory.
  if (REXCVAR_GET(wwe13_resolve_alias) && dst->sourceSurface && dst->sourceSurface != dst)
    MaterializeInboundLocked(s, dst);
  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  s.draw_framebuffer_bound = false;
  plume::RenderTextureBarrier pre[2] = {
      plume::RenderTextureBarrier(src->texture, plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(dst->texture, plume::RenderTextureLayout::COPY_DEST),
  };
  s.command_list->barriers(plume::RenderBarrierStage::COPY, pre, std::size(pre));
  src->layout = plume::RenderTextureLayout::COPY_SOURCE;
  dst->layout = plume::RenderTextureLayout::COPY_DEST;
  const plume::RenderBox box(left, top, right, bottom);
  census::Mark(s, census::kResolveCopy, src, dst);
  s.command_list->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(dst->texture, level, face),
      plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0), u32(dst_x), u32(dst_y),
      0, &box);
  if (REXCVAR_GET(wwe13_resolve_alias)) {
    g_resolve_copies_done.fetch_add(1, std::memory_order_relaxed);
    LogResolveCopyStats();
  }
  s.last_resolved_dst = dst;
  NoteTileContentLocked(s, dst);
  // The texture now holds pieces of several resolves: never re-copy the whole source over it.
  DetachSourceSurfaceLocked(s, dst);
  return true;
}

bool Video::ReadbackTextureLevel(GuestTexture *src, u32 level, u32 face,
                                 std::vector<u8> &out, u32 &row_pitch) {
  if (!src || !src->texture || level >= src->mipLevels ||
      src->sampleCount != plume::RenderSampleCount::COUNT_1)
    return false;
  const u32 w = std::max(1u, src->width >> level);
  const u32 h = std::max(1u, src->height >> level);
  row_pitch = rex::align(w * 4u, 256u);
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return false;
  auto buffer = CreateHostBuffer(s.device.get(),
                                 plume::RenderBufferDesc::ReadbackBuffer(u64(row_pitch) * h),
                                 "resolve-readback");
  if (!buffer)
    return false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  s.draw_framebuffer_bound = false;
  plume::RenderTextureBarrier pre(src->texture, plume::RenderTextureLayout::COPY_SOURCE);
  s.command_list->barriers(plume::RenderBarrierStage::COPY, &pre, 1);
  src->layout = plume::RenderTextureLayout::COPY_SOURCE;
  census::Mark(s, census::kResolveCopy);
  s.command_list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(buffer.get(), src->format, w, h, 1,
                                                        row_pitch / 4u),
      plume::RenderTextureCopyLocation::Subresource(src->texture, level, face));
#if !defined(REBLUE_D3D12)
  // Make the copy visible to the CPU (a fence wait alone does not): a transfer -> host barrier,
  // then invalidate the mapped memory below. Without them AMD integrated GPUs (shared memory)
  // handed back stale bytes: Superstar Threads attires, compressed by the game from this
  // read-back, came out white on a Radeon 610M / 780M (issue #27), fine on NVIDIA.
  {
    VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(static_cast<plume::VulkanCommandList *>(s.command_list)->vk,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &to_host, 0, nullptr, 0, nullptr);
  }
#endif
  // The game reads the resolved memory right away (it waits for the GPU first): finish this
  // frame's work so far, then reopen the same list for the rest of the frame.
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  SubmitOpenListLocked(s);
  s.queue->waitForCommandFence(s.fences[cur].get());
  s.command_list_submitted[cur] = false;
  const void *mapped = buffer->map();
  if (!mapped)
    return false;
#if !defined(REBLUE_D3D12)
  {
    auto *vk_buffer = static_cast<plume::VulkanBuffer *>(buffer.get());
    vmaInvalidateAllocation(vk_buffer->device->allocator, vk_buffer->allocation, 0, VK_WHOLE_SIZE);
  }
#endif
  out.assign(static_cast<const u8 *>(mapped), static_cast<const u8 *>(mapped) + size_t(row_pitch) * h);
  buffer->unmap();
  return true;
}

void Video::ReadbackTextureLevel0Batch(const std::vector<GuestTexture *> &src,
                                       std::vector<std::vector<u8>> &out, std::vector<u32> &pitches) {
  out.assign(src.size(), {});
  pitches.assign(src.size(), 0);
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  std::vector<std::unique_ptr<plume::RenderBuffer>> buffers(src.size());
  std::vector<u32> heights(src.size(), 0);
  bool any = false;
  for (size_t i = 0; i < src.size(); ++i) {
    GuestTexture *t = src[i];
    if (!t || !t->texture || t->sampleCount != plume::RenderSampleCount::COUNT_1)
      continue;
    const u32 w = std::max(1u, t->width), h = std::max(1u, t->height);
    const u32 row_pitch = rex::align(w * 4u, 256u);
    buffers[i] = CreateHostBuffer(s.device.get(),
                                  plume::RenderBufferDesc::ReadbackBuffer(u64(row_pitch) * h),
                                  "resolve-readback-batch");
    if (!buffers[i])
      continue;
    BeginCommandList(s);
    if (!s.command_list_open)
      return;
    s.draw_framebuffer_bound = false;
    plume::RenderTextureBarrier pre(t->texture, plume::RenderTextureLayout::COPY_SOURCE);
    s.command_list->barriers(plume::RenderBarrierStage::COPY, &pre, 1);
    t->layout = plume::RenderTextureLayout::COPY_SOURCE;
    census::Mark(s, census::kResolveCopy);
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::PlacedFootprint(buffers[i].get(), t->format, w, h, 1,
                                                          row_pitch / 4u),
        plume::RenderTextureCopyLocation::Subresource(t->texture, 0, 0));
    pitches[i] = row_pitch;
    heights[i] = h;
    any = true;
  }
  if (!any)
    return;
#if !defined(REBLUE_D3D12)
  {  // as in ReadbackTextureLevel: transfer -> host visibility (issue #27 on AMD shared memory)
    VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(static_cast<plume::VulkanCommandList *>(s.command_list)->vk,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &to_host, 0, nullptr, 0, nullptr);
  }
#endif
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  SubmitOpenListLocked(s);
  s.queue->waitForCommandFence(s.fences[cur].get());
  s.command_list_submitted[cur] = false;
  for (size_t i = 0; i < src.size(); ++i) {
    if (!buffers[i])
      continue;
    const void *mapped = buffers[i]->map();
    if (!mapped)
      continue;
#if !defined(REBLUE_D3D12)
    auto *vk_buffer = static_cast<plume::VulkanBuffer *>(buffers[i].get());
    vmaInvalidateAllocation(vk_buffer->device->allocator, vk_buffer->allocation, 0, VK_WHOLE_SIZE);
#endif
    out[i].assign(static_cast<const u8 *>(mapped),
                  static_cast<const u8 *>(mapped) + size_t(pitches[i]) * heights[i]);
    buffers[i]->unmap();
  }
}

namespace {
struct AsyncReadback {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u64 done_at = 0;  // frames_advanced value from which the copy is complete
  u32 row_pitch = 0, height = 0;
};
std::unordered_map<u64, AsyncReadback> g_async_readbacks;  // under state().mutex
u64 g_async_next = 1;
}  // namespace

u64 Video::ReadbackTextureLevelBegin(GuestTexture *src, u32 level, u32 face) {
  if (!src || !src->texture || level >= src->mipLevels ||
      src->sampleCount != plume::RenderSampleCount::COUNT_1)
    return 0;
  const u32 w = std::max(1u, src->width >> level);
  const u32 h = std::max(1u, src->height >> level);
  const u32 row_pitch = rex::align(w * 4u, 256u);
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return 0;
  auto buffer = CreateHostBuffer(s.device.get(),
                                 plume::RenderBufferDesc::ReadbackBuffer(u64(row_pitch) * h),
                                 "resolve-readback-async");
  if (!buffer)
    return 0;
  BeginCommandList(s);
  if (!s.command_list_open)
    return 0;
  s.draw_framebuffer_bound = false;
  plume::RenderTextureBarrier pre(src->texture, plume::RenderTextureLayout::COPY_SOURCE);
  s.command_list->barriers(plume::RenderBarrierStage::COPY, &pre, 1);
  src->layout = plume::RenderTextureLayout::COPY_SOURCE;
  census::Mark(s, census::kResolveCopy);
  s.command_list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(buffer.get(), src->format, w, h, 1,
                                                        row_pitch / 4u),
      plume::RenderTextureCopyLocation::Subresource(src->texture, level, face));
#if !defined(REBLUE_D3D12)
  {  // as in ReadbackTextureLevel (issue #27: stale bytes on AMD integrated GPUs without it)
    VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(static_cast<plume::VulkanCommandList *>(s.command_list)->vk,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &to_host, 0, nullptr, 0, nullptr);
  }
#endif
  // Recorded into the open list of slot frame(); that slot is awaited when it is reused, i.e.
  // after kNumFrames more AdvanceAndWaitReused calls. No submit, no wait here.
  const u64 ticket = g_async_next++;
  g_async_readbacks[ticket] =
      AsyncReadback{std::move(buffer), s.frames_advanced + kNumFrames, row_pitch, h};
  return ticket;
}

bool Video::ReadbackTextureLevelPoll(u64 ticket, std::vector<u8> &out, u32 &row_pitch,
                                     bool &failed) {
  failed = false;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  const auto it = g_async_readbacks.find(ticket);
  if (it == g_async_readbacks.end()) {
    failed = true;
    return true;
  }
  if (s.frames_advanced < it->second.done_at)
    return false;
  AsyncReadback r = std::move(it->second);
  g_async_readbacks.erase(it);
  const void *mapped = r.buffer->map();
  if (!mapped) {
    failed = true;
    return true;
  }
#if !defined(REBLUE_D3D12)
  {
    auto *vk_buffer = static_cast<plume::VulkanBuffer *>(r.buffer.get());
    vmaInvalidateAllocation(vk_buffer->device->allocator, vk_buffer->allocation, 0, VK_WHOLE_SIZE);
  }
#endif
  row_pitch = r.row_pitch;
  out.assign(static_cast<const u8 *>(mapped),
             static_cast<const u8 *>(mapped) + size_t(r.row_pitch) * r.height);
  r.buffer->unmap();
  return true;
}

bool Video::UploadTextureLevel(GuestTexture *dst, u32 level, const u8 *rgba, u32 row_pitch,
                               u32 face) {
  if (!dst || !dst->texture || !rgba || level >= dst->mipLevels || (row_pitch & 255u))
    return false;
  const u32 w = std::max(1u, dst->width >> level);
  const u32 h = std::max(1u, dst->height >> level);
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return false;
  if (!dst->destinationTextures.empty())
    MaterializeOutboundLocked(s, dst);  // alias dedupe: keep their snapshot
  auto buffer = CreateHostBuffer(s.device.get(),
                                 plume::RenderBufferDesc::UploadBuffer(u64(row_pitch) * h),
                                 "level-upload");
  if (!buffer)
    return false;
  void *mapped = buffer->map();
  if (!mapped)
    return false;
  std::memcpy(mapped, rgba, size_t(row_pitch) * h);
  buffer->unmap();
  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  s.draw_framebuffer_bound = false;
  plume::RenderTextureBarrier pre(dst->texture, plume::RenderTextureLayout::COPY_DEST);
  s.command_list->barriers(plume::RenderBarrierStage::COPY, &pre, 1);
  dst->layout = plume::RenderTextureLayout::COPY_DEST;
  census::Mark(s, census::kUpload);
  s.command_list->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(dst->texture, level, face),
      plume::RenderTextureCopyLocation::PlacedFootprint(buffer.get(), dst->format, w, h, 1,
                                                        row_pitch / 4u));
  if (REXCVAR_GET(svr_readback_async)) {
    // The copy stays in this frame's list; the staging buffer is freed once this slot's fence
    // is awaited at its reuse (DrainSlot), so no GPU wait here.
    s.buffer_graveyard[Video::RetireSlot("level-upload")].push_back(std::move(buffer));
    return true;
  }
  // The staging buffer dies with this call: let the copy finish first.
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  SubmitOpenListLocked(s);
  s.queue->waitForCommandFence(s.fences[cur].get());
  s.command_list_submitted[cur] = false;
  return true;
}

bool Video::SeedTextureBaseLevel(GuestTexture *dst, GuestTexture *src) {
  if (!dst || !src || !dst->texture || !src->texture || dst == src ||
      dst->width != src->width || dst->height != src->height || dst->format != src->format)
    return false;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  s.draw_framebuffer_bound = false;
  plume::RenderTextureBarrier pre[2] = {
      plume::RenderTextureBarrier(src->texture, plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(dst->texture, plume::RenderTextureLayout::COPY_DEST),
  };
  s.command_list->barriers(plume::RenderBarrierStage::COPY, pre, std::size(pre));
  src->layout = plume::RenderTextureLayout::COPY_SOURCE;
  dst->layout = plume::RenderTextureLayout::COPY_DEST;
  census::Mark(s, census::kResolveCopy);
  s.command_list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(dst->texture, 0, 0),
                                    plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0));
  return true;
}

} // namespace bd::gpu
