// Card G18b (G18d section): pure sizing rule for a depth/stencil shadow image bound together with colour target 0.
// A Vulkan framebuffer takes its extent from colour 0 and the render area = framebuffer (plume_vulkan.cpp:2544-2546,
// :3477), so every attachment must be at least the colour image's size (VUID-VkFramebufferCreateInfo-flags-04533/04534).
// Host-only testable (tests/depth-cover-test.cpp).
#pragma once

#include <algorithm>
#include <cstdint>

namespace wwe2k14::d3d::depth_cover {

struct Plan {
  uint32_t width = 0;     // depth image width to use (>= the current width)
  uint32_t need_h = 0;    // minimum rows to request (>= the colour height when covering)
  bool recreate = false;  // an existing image must be replaced (its content is not carried over)
  bool cover = false;     // the declared extent is exceeded on purpose (EnsureHeight's never-grow rule is bypassed)
};

// depth_w / depth_h: the slot's current image (depth_h = 0 = no image yet); need_h: the rows Ensure would request
// without the rule; color_w / color_h: the bound colour 0 image (0 = no colour bound / no image).
inline Plan PlanCover(uint32_t depth_w, uint32_t depth_h, uint32_t need_h, uint32_t color_w, uint32_t color_h) {
  Plan p;
  p.width = depth_w;
  p.need_h = need_h;
  if (!color_w || !color_h) return p;
  // Height only (2026-10-28 UHD 630 A/B): widening the tiled scene's 320/640-pitch tile depth to the 1280-wide scene
  // colour doubled long frame gaps (89 vs 21-28 per 7 min, r142d-uhd vs -off/-nocover). The AMD white-skin case (G18:
  // 2080x720 depth under 2080x1024 colour) only needs rows. Narrow depth stays as before (FB_DEPTH_FIT still drops it
  // for draws that do not use depth; [g18d-fbcheck] counts the rest).
  const bool short_need = color_h > need_h;
  const bool short_image = depth_h && color_h > depth_h;
  if (!short_need && !short_image) return p;
  p.cover = true;
  p.need_h = std::max({need_h, color_h, depth_h});
  // A taller image is a replacement of the existing one (content is not carried over).
  p.recreate = depth_h && short_image;
  return p;
}

}  // namespace wwe2k14::d3d::depth_cover
