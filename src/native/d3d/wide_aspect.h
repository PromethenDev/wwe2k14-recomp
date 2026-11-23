#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "wide_core.h"

namespace wwe2k14::d3d::wide_aspect {

inline std::atomic<bool> g_aspect_ready{false};
inline std::atomic<bool> g_wide_projection_requested{false};
inline std::atomic<bool> g_wide_projection_active{false};

inline float WideK() {
  static const float wide_k = [] {
#if defined(_WIN32)
    char* value = nullptr;
    size_t value_size = 0;
    const int status = _dupenv_s(&value, &value_size, "WWE2K14_D3D_WIDE");
    const float parsed = status == 0 ? wide_core::ParseK(value) : wide_core::kOff;
    std::free(value);
    return parsed;
#else
    return wide_core::ParseK(std::getenv("WWE2K14_D3D_WIDE"));
#endif
  }();
  return wide_k;
}

// The game-image projection patch is strictly opt-in and only applies alongside WIDE.
inline bool Enabled() {
  static const bool enabled = [] {
#if defined(_WIN32)
    char* aspect_mode = nullptr;
    size_t aspect_mode_size = 0;
    const int status = _dupenv_s(&aspect_mode, &aspect_mode_size, "WWE2K14_D3D_WIDE_ASPECT");
    const bool requested = status == 0 && aspect_mode && std::strcmp(aspect_mode, "1") == 0;
    std::free(aspect_mode);
#else
    const char* aspect_mode = std::getenv("WWE2K14_D3D_WIDE_ASPECT");
#endif
#if defined(_WIN32)
    if (!requested) return false;
#else
    if (!aspect_mode || std::strcmp(aspect_mode, "1") != 0) return false;
#endif
    return wide_core::IsEnabled(WideK());
  }();
  return enabled;
}

inline float GameAspect(float wide_k) { return wide_k * (16.0f / 9.0f); }

inline bool AspectReady() { return g_aspect_ready.load(std::memory_order_acquire); }

inline bool WideProjectionActive() {
  return AspectReady() && g_wide_projection_active.load(std::memory_order_acquire);
}

inline bool WideProjectionRequested() {
  return g_wide_projection_requested.load(std::memory_order_acquire);
}

inline void MarkAspectReady() { g_aspect_ready.store(true, std::memory_order_release); }

inline void MarkWideProjectionActive(bool active) {
  g_wide_projection_active.store(active, std::memory_order_release);
}

struct AspectWriteResult {
  bool ready = false;
  uint32_t old_bits = 0;
  uint32_t new_bits = 0;
};

// Called by the existing guest render-command pump hook, after image mapping and before queued draw commands run.
void ApplyGuestImagePatch(uint8_t* base);

// Changes the mapped guest projection aspect when the draw-count mode detector enters/leaves a match scene.
AspectWriteResult SetWideProjectionActive(bool active);

}  // namespace wwe2k14::d3d::wide_aspect
