#include "../wide_core.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace wwe2k14::d3d::wide_core;

namespace {
bool Near(float a, float b, float epsilon = 0.00001f) { return std::fabs(a - b) <= epsilon; }
}  // namespace

int main() {
  assert(ParseK(nullptr) == kOff);
  assert(ParseK("") == kOff);
  assert(ParseK("16:9") == kOff);
  assert(ParseK("1.001") == kOff);
  assert(ParseK("not-a-ratio") == kOff);
  assert(ParseK("21::9") == kOff);
  assert(ParseK("1.333x") == kOff);
  assert(Near(ParseK("21:9"), 1.3125f));
  assert(Near(ParseK("32:9"), 2.0f));
  assert(Near(ParseK("2560:1080"), 4.0f / 3.0f));
  assert(Near(ParseK("3440:1440"), 1.34375f));
  assert(Near(ParseK("1.333"), 1.333f));
  assert(Near(ParseK("1.002"), 1.002f));
  assert(ParseK("80:9") == 4.0f);
  assert(ParseK("8") == 4.0f);

  const float k = ParseK("21:9");
  const NdcX edges = SqueezeNdc(1.0f, 0.0f, k);
  assert(Near(-1.0f * edges.scale, -1.0f / k));
  assert(Near(1.0f * edges.scale, 1.0f / k));
  const NdcX centred = SqueezeNdc(0.5f, -0.25f, 2.0f);
  assert(Near(0.5f * centred.scale + centred.offset, 0.0f));
  const NdcX translated = SqueezeNdc(0.75f, -0.125f, 1.34375f);
  const float before = -0.4f * 0.75f + 0.75f * -0.125f;
  const float after = -0.4f * translated.scale + 0.75f * translated.offset;
  assert(Near(after, before / 1.34375f));

  constexpr int32_t tile_pitch = 320, scene_image_width = 1280;
  const int32_t image_width = TargetImageDimension(scene_image_width, 0, tile_pitch);
  assert(image_width == scene_image_width);
  const ScissorRect scene_scissor = SqueezeScissor(160, 80, image_width, 2.0f);
  const ScissorRect tile_centred_scissor = SqueezeScissor(160, 80, tile_pitch, 2.0f);
  assert(scene_scissor.x == 400 && scene_scissor.width == 40);
  assert(tile_centred_scissor.x == 160 && tile_centred_scissor.width == 40);

  const float centred_viewport = ImageCenterNdc(scene_image_width, 0, scene_image_width);
  assert(Near(centred_viewport, 0.0f));
  const NdcX scene_centered = SqueezeNdc(1.0f, 0.0f, 2.0f, centred_viewport);
  assert(Near(1.0f * scene_centered.scale + scene_centered.offset, 0.5f));
  const float offset_viewport = ImageCenterNdc(scene_image_width, 0, 640);
  assert(Near(offset_viewport, 1.0f));
  const NdcX offset_centered = SqueezeNdc(1.0f, 0.0f, 2.0f, offset_viewport);
  assert(Near(1.0f * offset_centered.scale + offset_centered.offset, 1.0f));

  const PixelRect full_rect{0, 0, 1280, 720};
  const PixelRect short_scissor{0, 0, 1279, 720};
  assert(IsFullscreenPass(false, true, full_rect, full_rect, 1280, 720));
  assert(!IsFullscreenPass(true, true, full_rect, full_rect, 1280, 720));
  assert(!IsFullscreenPass(false, false, full_rect, full_rect, 1280, 720));
  assert(!IsFullscreenPass(false, true, full_rect, short_scissor, 1280, 720));

  assert(IsHudDraw(false, true, false, true, 0x1234, true, 0x1234));
  assert(!IsHudDraw(false, true, false, true, 0x1234, false, 0x1234));
  assert(!IsHudDraw(false, true, false, true, 0x1234, true, 0x5678));
  assert(!IsHudDraw(true, true, false, true, 0x1234, true, 0x1234));
  assert(!IsHudDraw(false, false, false, true, 0x1234, true, 0x1234));
  assert(!IsHudDraw(false, true, true, true, 0x1234, true, 0x1234));
  assert(!IsHudDraw(false, true, false, false, 0x1234, true, 0x1234));

  FrameModeHysteresis mode(true);
  assert(!mode.Update(FrameModeHysteresis::kSceneDrawThreshold - 1));
  for (uint32_t i = 0; i + 1 < FrameModeHysteresis::kStableFrames; ++i)
    assert(!mode.Update(FrameModeHysteresis::kSceneDrawThreshold));
  assert(mode.letterboxed);
  assert(mode.Update(FrameModeHysteresis::kSceneDrawThreshold));
  assert(!mode.letterboxed);
  for (uint32_t i = 0; i + 1 < FrameModeHysteresis::kStableFrames; ++i)
    assert(!mode.Update(FrameModeHysteresis::kSceneDrawThreshold - 1));
  assert(!mode.letterboxed);
  assert(mode.Update(FrameModeHysteresis::kSceneDrawThreshold - 1));
  assert(mode.letterboxed);

  assert(FrameModeHysteresis::kSceneDrawThreshold == 50);
  assert(FrameModeHysteresis::kAspectSceneDrawThreshold == 500);
  FrameModeHysteresis aspect_mode(true);
  for (uint32_t i = 0; i < FrameModeHysteresis::kAspectRateWindowFrames * 2; ++i)
    assert(!aspect_mode.UpdateAspect(293));
  assert(aspect_mode.letterboxed);  // observed title-window mean stays below the aspect-mode threshold
  for (uint32_t i = 0; i < 14; ++i) assert(!aspect_mode.UpdateAspect(1000));
  for (uint32_t i = 0; i < FrameModeHysteresis::kStableFrames; ++i) assert(!aspect_mode.UpdateAspect(293));
  assert(aspect_mode.letterboxed);  // a short title-scene draw burst does not flip the presenter
  for (uint32_t i = 0; i + 1 < FrameModeHysteresis::kStableFrames + 3; ++i)
    assert(!aspect_mode.UpdateAspect(1245));
  assert(aspect_mode.letterboxed);
  assert(aspect_mode.UpdateAspect(1245));
  assert(!aspect_mode.letterboxed);
  for (uint32_t i = 0; i < FrameModeHysteresis::kAspectRateWindowFrames + FrameModeHysteresis::kStableFrames; ++i)
    aspect_mode.UpdateAspect(293);
  assert(aspect_mode.letterboxed);

  FrameModeHysteresis noisy_mode(true);
  for (uint32_t i = 0; i < 20; ++i) assert(!noisy_mode.Update(FrameModeHysteresis::kSceneDrawThreshold));
  assert(!noisy_mode.Update(FrameModeHysteresis::kSceneDrawThreshold - 1));  // interrupts the pending transition
  for (uint32_t i = 0; i + 1 < FrameModeHysteresis::kStableFrames; ++i)
    assert(!noisy_mode.Update(FrameModeHysteresis::kSceneDrawThreshold));
  assert(noisy_mode.letterboxed);

  const ScissorRect full = SqueezeScissor(0, 101, 101, 2.0f);
  assert(full.x == 0 && full.width == 101);
  const ScissorRect left_edge = SqueezeScissor(0, 30, 101, 2.0f);
  assert(left_edge.x == 0 && left_edge.width == 41);
  const ScissorRect right_edge = SqueezeScissor(70, 31, 101, 2.0f);
  assert(right_edge.x == 60 && right_edge.width == 41);
  const ScissorRect partial = SqueezeScissor(30, 20, 101, 2.0f);
  assert(partial.x == 40 && partial.width == 11);
  assert(ShouldSqueeze(false, false, true, true));
  assert(!ShouldSqueeze(true, false, true, true));
  assert(!ShouldSqueeze(false, true, true, true));
  assert(!ShouldSqueeze(false, false, false, true));
  assert(!ShouldSqueeze(false, false, true, false));
  assert(ClassifyDraw(false, false, true, true) == SkipReason::kNone);
  assert(ClassifyDraw(true, false, true, true) == SkipReason::kScreenSpace);
  assert(ClassifyDraw(false, true, true, false) == SkipReason::kFullscreenPass);
  assert(ClassifyDraw(false, false, false, true) == SkipReason::kNoSceneTarget);
  assert(ClassifyDraw(false, false, true, false) == SkipReason::kNoDepthTest);
  assert(ShouldApplyGpuSqueeze(SkipReason::kNone, false, false));
  assert(!ShouldApplyGpuSqueeze(SkipReason::kNone, false, true));
  assert(ShouldApplyGpuSqueeze(SkipReason::kNone, true, true));
  assert(!ShouldApplyGpuSqueeze(SkipReason::kFullscreenPass, false, true));
  assert(!ShouldApplyGpuSqueeze(SkipReason::kScreenSpace, false, true));
  FrameDrawStats aspect_stats{};
  aspect_stats.aspect_scene_draws = FrameModeHysteresis::kSceneDrawThreshold + 2;
  assert(SceneDrawCountForFrameMode(aspect_stats, true) == FrameModeHysteresis::kSceneDrawThreshold + 2);
  assert(SceneDrawCountForFrameMode(aspect_stats, false) == 0);

  const float raw_scale = std::bit_cast<float>(0x7FC12345u);
  const float raw_offset = -0.125f;
  const NdcX unchanged = SqueezeNdc(raw_scale, raw_offset, kOff);
  assert(std::bit_cast<uint32_t>(unchanged.scale) == std::bit_cast<uint32_t>(raw_scale));
  assert(std::bit_cast<uint32_t>(unchanged.offset) == std::bit_cast<uint32_t>(raw_offset));
  const ScissorRect unchanged_scissor = SqueezeScissor(-4, 38, 100, kOff);
  assert(unchanged_scissor.x == -4 && unchanged_scissor.width == 38);

  std::puts("wide-core-test: PASS");
  return 0;
}
