#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

namespace wwe2k14::d3d::wide_core {

inline constexpr float kOff = 0.0f;
inline constexpr float kMinEnabled = 1.001f;

inline bool ParsePositiveNumber(std::string_view text, double* value) {
  if (text.empty()) return false;
  double parsed = 0.0;
  double fraction_scale = 0.1;
  bool fraction = false, digit_seen = false;
  for (char c : text) {
    if (c == '.' && !fraction) {
      fraction = true;
      continue;
    }
    if (c < '0' || c > '9') return false;
    digit_seen = true;
    const double digit = double(c - '0');
    if (fraction) {
      parsed += digit * fraction_scale;
      fraction_scale *= 0.1;
    } else {
      parsed = parsed * 10.0 + digit;
    }
    if (!std::isfinite(parsed)) return false;
  }
  if (!digit_seen || !(parsed > 0.0)) return false;
  *value = parsed;
  return true;
}

inline float ParseK(const char* value) {
  if (!value || !*value) return kOff;
  const std::string_view text(value);
  const size_t colon = text.find(':');
  double k = 0.0;
  if (colon == std::string_view::npos) {
    if (!ParsePositiveNumber(text, &k)) return kOff;
  } else {
    double width = 0.0, height = 0.0;
    if (text.find(':', colon + 1) != std::string_view::npos ||
        !ParsePositiveNumber(text.substr(0, colon), &width) ||
        !ParsePositiveNumber(text.substr(colon + 1), &height)) {
      return kOff;
    }
    const double aspect = width / height;
    constexpr double kMaxAspect = 4.0 * (16.0 / 9.0);
    k = aspect >= kMaxAspect ? 4.0 : aspect / (16.0 / 9.0);
  }
  if (!std::isfinite(k) || k <= 1.001) return kOff;
  return float(std::clamp(k, 1.0, 4.0));
}

inline bool IsEnabled(float k) { return std::isfinite(k) && k > kMinEnabled; }

inline int32_t TargetImageDimension(int32_t color_image, int32_t depth_image, int32_t fallback) {
  if (color_image > 0) return color_image;
  if (depth_image > 0) return depth_image;
  return std::max(fallback, 0);
}

inline float ImageCenterNdc(int32_t image_width, int32_t viewport_x, int32_t viewport_width) {
  if (image_width <= 0 || viewport_width <= 0) return 0.0f;
  const double image_center = double(image_width) * 0.5;
  return float(2.0 * (image_center - double(viewport_x)) / double(viewport_width) - 1.0);
}

struct NdcX {
  float scale;
  float offset;
};

inline NdcX SqueezeNdc(float scale, float offset, float k, float center_ndc = 0.0f) {
  if (!IsEnabled(k)) return {scale, offset};
  return {scale / k, offset / k + center_ndc * (1.0f - 1.0f / k)};
}

struct PixelRect {
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
};

inline bool CoversImage(PixelRect rect, int32_t image_width, int32_t image_height) {
  if (image_width <= 0 || image_height <= 0 || rect.width <= 0 || rect.height <= 0) return false;
  const int64_t right = int64_t(rect.x) + rect.width;
  const int64_t bottom = int64_t(rect.y) + rect.height;
  return rect.x <= 0 && rect.y <= 0 && right >= image_width && bottom >= image_height;
}

inline bool IsFullscreenPass(bool depth_test_enabled, bool small_draw, PixelRect viewport, PixelRect scissor,
                             int32_t image_width, int32_t image_height) {
  return !depth_test_enabled && small_draw && CoversImage(viewport, image_width, image_height) &&
         CoversImage(scissor, image_width, image_height);
}

// A HUD quad uses the same full-image viewport/scissor as a post pass, but follows a full-image draw on the same
// bound guest surface. The object id and event order are already available on the worker; no vertex decode or GPU work.
inline bool IsHudDraw(bool depth_test_enabled, bool small_draw, bool screen_space, bool has_color_target,
                      uint32_t target_object, bool after_fullscreen_pass, uint32_t fullscreen_target_object) {
  return !depth_test_enabled && small_draw && !screen_space && has_color_target && target_object != 0 &&
         after_fullscreen_pass && target_object == fullscreen_target_object;
}

struct FrameModeHysteresis {
  static constexpr uint32_t kSceneDrawThreshold = 50;
  // W3d: title-scene windows averaged at most 293 eligible draws/frame; match windows averaged at least 1,245.
  // Keep this higher threshold exclusive to WIDE_ASPECT because the title has real 3D draws but must stay 16:9.
  static constexpr uint32_t kAspectSceneDrawThreshold = 500;
  static constexpr uint32_t kAspectRateWindowFrames = 15;
  static constexpr uint32_t kStableFrames = 30;

  explicit FrameModeHysteresis(bool start_letterboxed = true) : letterboxed(start_letterboxed) {}

  // Returns true only when the applied mode changes. The caller supplies eligible scene draws in aspect mode,
  // squeezed scene draws otherwise, plus the mode-specific threshold; non-scene frames letterbox.
  bool Update(uint32_t scene_draws, uint32_t scene_draw_threshold = kSceneDrawThreshold) {
    const bool desired_letterboxed = scene_draws < scene_draw_threshold;
    if (desired_letterboxed == letterboxed) {
      pending_frames = 0;
      return false;
    }
    if (++pending_frames < kStableFrames) return false;
    letterboxed = desired_letterboxed;
    pending_frames = 0;
    return true;
  }

  // Aspect mode smooths per-frame counts before applying the same 30-frame mode hysteresis. Legacy squeeze mode
  // continues to call Update() directly, preserving its exact behavior.
  bool UpdateAspect(uint32_t scene_draws) {
    aspect_draw_sum -= aspect_draw_history[aspect_draw_cursor];
    aspect_draw_history[aspect_draw_cursor] = scene_draws;
    aspect_draw_sum += scene_draws;
    aspect_draw_cursor = (aspect_draw_cursor + 1) % kAspectRateWindowFrames;
    if (aspect_draw_samples < kAspectRateWindowFrames) ++aspect_draw_samples;
    if (aspect_draw_samples < kAspectRateWindowFrames) return false;
    const uint32_t average_draws = uint32_t(aspect_draw_sum / kAspectRateWindowFrames);
    return Update(average_draws, kAspectSceneDrawThreshold);
  }

  bool letterboxed = true;
  uint32_t pending_frames = 0;

 private:
  std::array<uint32_t, kAspectRateWindowFrames> aspect_draw_history{};
  uint64_t aspect_draw_sum = 0;
  uint32_t aspect_draw_cursor = 0;
  uint32_t aspect_draw_samples = 0;
};

struct FrameDrawStats {
  uint32_t squeezed = 0;
  uint32_t aspect_scene_draws = 0;
  uint32_t hud_squeezed = 0;
  uint32_t skipped_screen_space = 0;
  uint32_t skipped_fullscreen = 0;
  uint32_t skipped_no_target = 0;
  uint32_t skipped_no_depth = 0;
};

struct ScissorRect {
  int32_t x;
  int32_t width;
};

inline int32_t RoundPixel(double value, bool down) {
  const double rounded = down ? std::floor(value) : std::ceil(value);
  if (rounded <= double(std::numeric_limits<int32_t>::min())) return std::numeric_limits<int32_t>::min();
  if (rounded >= double(std::numeric_limits<int32_t>::max())) return std::numeric_limits<int32_t>::max();
  return int32_t(rounded);
}

inline ScissorRect SqueezeScissor(int32_t x, int32_t width, int32_t target_width, float k) {
  if (!IsEnabled(k) || width <= 0 || target_width <= 0) return {x, width};
  const double center = double(target_width) * 0.5;
  const double left = center + (double(x) - center) / k;
  const double right = center + (double(x) + double(width) - center) / k;
  // An edge on the target border stays there: the squeezed draw reaches past the old 16:9 view (Hor+), and clipping
  // a full-width scissor to the centre 1/k would cut that extra view away again.
  const int32_t new_left = x <= 0 ? x : RoundPixel(left, true);
  const int32_t new_right = x + width >= target_width ? x + width : RoundPixel(right, false);
  const int64_t new_width = int64_t(new_right) - int64_t(new_left);
  return {new_left, int32_t(std::min<int64_t>(new_width, std::numeric_limits<int32_t>::max()))};
}

enum class SkipReason : uint8_t { kNone, kScreenSpace, kFullscreenPass, kNoSceneTarget, kNoDepthTest };

inline SkipReason ClassifyDraw(bool screen_space, bool is_fullscreen_pass, bool has_scene_targets,
                               bool depth_test_enabled) {
  if (screen_space) return SkipReason::kScreenSpace;
  if (is_fullscreen_pass) return SkipReason::kFullscreenPass;
  if (!has_scene_targets) return SkipReason::kNoSceneTarget;
  if (!depth_test_enabled) return SkipReason::kNoDepthTest;
  return SkipReason::kNone;
}

inline bool ShouldSqueeze(bool screen_space, bool is_fullscreen_pass, bool has_scene_targets,
                          bool depth_test_enabled) {
  return ClassifyDraw(screen_space, is_fullscreen_pass, has_scene_targets, depth_test_enabled) == SkipReason::kNone;
}

inline bool ShouldApplyGpuSqueeze(SkipReason skip_reason, bool is_hud, bool game_aspect_enabled) {
  return is_hud || (!game_aspect_enabled && skip_reason == SkipReason::kNone);
}

inline uint32_t SceneDrawCountForFrameMode(const FrameDrawStats& stats, bool game_aspect_enabled) {
  return game_aspect_enabled ? stats.aspect_scene_draws : stats.squeezed;
}

}  // namespace wwe2k14::d3d::wide_core
