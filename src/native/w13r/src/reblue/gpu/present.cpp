/**
 * @file    gpu/present.cpp
 * @brief   End of frame: the deferred clear, the swapchain
 * acquire/blit/present, and the pre-Runtime overlay-only present the installer
 * uses.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/gpu_census.h"
#include "gpu/frame.h"
#include "gpu/cpu_zones.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#if defined(__ANDROID__)
#include <time.h>

#include <android/native_window.h>
#endif

#include <plume_render_interface.h>

#include <rex/cvar.h>

#include "gpu/gpu_profiling.h"

#include "core/logging.h"
#include "engine/engine.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/output.h"
#include "gpu/settings.h"
#include "platform/platform.h"

bool Wwe12LoadTurboSkipPresent();  // native/wwe12_load_turbo.cpp
#if defined(SVR_NATIVE_RENDERER)
void SvrGuestGammaRamp(float& gamma, float& scale);  // native/wwe13_gamma.cpp: brightness
unsigned SvrFxaaLevel();  // native/wwe13_gamma.cpp: svr_fxaa
#endif

// Present pass with the default-settings dead code stripped (wwe13_present_lite): FXAA off
// (svr_fxaa = 0) and the X360 sRGB decode unreachable in the native renderer leave the full
// gamma_correction_ps with an unused Fxaa() (8 extra samples) and an unused decode branch. The
// lite variant keeps the same sample, pow and scale, so the picture is byte-for-byte identical.
#if !defined(REBLUE_D3D11) && !defined(REBLUE_D3D12) && defined(_WIN32)
#include <plume_vulkan.h>  // VulkanDxgiPresentActive
REXCVAR_DECLARE(int32_t, wwe13_vk_dxgi_fallback_test);  // device.cpp
#define WWE13_DXGI_FALLBACK 1
#endif
REXCVAR_DEFINE_BOOL(wwe13_present_lite, false, "WWE13",
                    "Cheaper present pass: FXAA-free / no-sRGB-decode pixel shader (svr_fxaa = 0)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bd::gpu {

namespace {

void ApplyVsync(VideoState &s) {
  if (s.swap_chain) {
    s.swap_chain->setVsyncEnabled(Settings::Get().Vsync());
  }
}

constexpr i32 kIdleFPS = 30;

#if defined(WWE13_DXGI_FALLBACK)
u32 s_dxgi_acquire_fails = 0;  // present thread only (under s.mutex)
u32 s_dxgi_test_presents = 0;
#endif

void PaceFrame(bool idle = false) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  // w13r (2K14): this limiter is OFF by default - 2K14 paces its frames on the guest vblank already, and the two
  // schedules beat (desk4/desk5 11-22, UHD 630 480p WrestleMania demo: 46.0 -> 50.2 fps with it off).
  // WWE2K14_W13R_PACE=1 turns it back on.
  static const bool pace_on = [] {
    const char *v = std::getenv("WWE2K14_W13R_PACE");
    return v && v[0] == '1';
  }();
  if (!pace_on) return;
  i32 fps = bd::engine::Settings::Get().FPSLimit();
  if (bd::engine::SofdecPlayer::Playing())
    fps = 30;
  if (idle && (fps <= 0 || fps > kIdleFPS))
    fps = kIdleFPS;
  if (fps <= 0) {
    next = {};
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(1000.0 / fps));
  const auto now = Clock::now();
  if (next.time_since_epoch().count() != 0 && now < next) {
    std::this_thread::sleep_until(next);
    next += period;
  } else {
    next = now + period;
  }
}

void AbandonFrame(VideoState &s, std::unique_lock<std::mutex> &lock) {
  s.frame_present_committed = true;
  if (!s.command_list_open)
    return;
  SubmitOpenListLocked(s);
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

#if defined(__ANDROID__) && !defined(REBLUE_D3D12)
// Android gives the activity a new window after it went to the background (and on the Retroid
// Pocket 6 at start, portrait panel turned to landscape: GitHub issue #21). The swap chain keeps
// the old one: presents still succeed into a window nobody shows, or vkCreateSwapchainKHR answers
// NATIVE_WINDOW_IN_USE / SURFACE_LOST (all seen on the emulator). Black screen with the game
// running behind. So the window the swap chain was built on is watched, and the swap chain built
// again, surface included, when SDL hands another one.
// SDL keeps handing the old pointer after Android destroyed that window (onNativeSurfaceDestroyed
// releases it without clearing the property): we hold our own reference on the swap chain's
// window, so it is never freed under us, and a new window always has another address.
plume::RenderWindow g_swap_window = nullptr;

void HoldSwapWindow(plume::RenderWindow window) {
  if (window)
    ANativeWindow_acquire(window);
  if (g_swap_window)
    ANativeWindow_release(g_swap_window);
  g_swap_window = window;
}

// The SDL window if it is not the swap chain's, checked 4 times a second; else null.
plume::RenderWindow NewAndroidWindow() {
  static std::chrono::steady_clock::time_point s_last{};
  const auto now = std::chrono::steady_clock::now();
  if (now - s_last < std::chrono::milliseconds(250))
    return nullptr;
  s_last = now;
  plume::RenderWindow window{};
  if (!bd::platform::GetNativeRenderWindow(nullptr, window) || window == g_swap_window)
    return nullptr;
  return window;
}

// The old swap chain goes first: Android lets only one swap chain hold a window.
bool RebuildOnWindow(VideoState &s, plume::RenderWindow window) {
  ANativeWindow_acquire(window);  // kept alive while we build on it
  s.framebuffers.clear();
  s.swap_chain.reset();
  plume::RenderSwapChainDesc desc(window, kSwapChainFormat, kNumFrames + 1, false, kNumFrames);
  s.swap_chain = s.queue->createSwapChain(desc);
  if (s.swap_chain)
    s.swap_chain->resize();
  const bool ok = s.swap_chain && !s.swap_chain->isEmpty() && BuildFramebuffers(s) &&
                  BuildPresentSemaphores(s);
  HoldSwapWindow(window);  // even on failure: watch for the next window, not this one again
  ANativeWindow_release(window);
  if (!ok) {
    BD_ERROR("Swap chain rebuilt on the new Android window: failed");
    s.framebuffers.clear();
    return false;
  }
  BD_INFO("Swap chain rebuilt on the new Android window ({}x{})", s.swap_chain->getWidth(),
          s.swap_chain->getHeight());
  return true;
}
#endif

// Resizes the swap chain to the window (fences already waited, framebuffers cleared).
bool ResizeSwapChain(VideoState &s) {
#if defined(__ANDROID__) && !defined(REBLUE_D3D12)
  // Android, app in the background: the window is gone and every try fails. Once a second; the
  // new window, when the app comes back, is caught by NewAndroidWindow.
  static std::chrono::steady_clock::time_point s_last_fail{};
  const auto now = std::chrono::steady_clock::now();
  if (now - s_last_fail < std::chrono::seconds(1))
    return false;
  if (s.swap_chain && s.swap_chain->resize() && BuildFramebuffers(s) && BuildPresentSemaphores(s))
    return true;
  s_last_fail = now;
  return false;
#else
  if (s.swap_chain && s.swap_chain->resize() && BuildFramebuffers(s) && BuildPresentSemaphores(s))
    return true;
  if (s.swap_chain && s.swap_chain->getWidth() && s.swap_chain->getHeight())  // else minimized
    BD_ERROR("Swap chain resize failed");
  return false;
#endif
}

// new_window (Android): build the swap chain again on that window instead of resizing it.
#if defined(WWE13_DXGI_FALLBACK)
// Submits the open list and waits every frame in flight (the GPU is idle afterwards).
void WaitAllFrames(VideoState &s) {
  SubmitOpenListLocked(s);
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }
}
#endif

void RebuildSwapChain(VideoState &s, plume::RenderWindow new_window = nullptr) {
  SubmitOpenListLocked(s);
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }
  s.framebuffers.clear();
#if defined(__ANDROID__) && !defined(REBLUE_D3D12)
  if (new_window ? !RebuildOnWindow(s, new_window) : !ResizeSwapChain(s))
    return;
#else
  (void)new_window;
  if (!ResizeSwapChain(s))
    return;
#endif
  if (static_cast<AspectMode>(Settings::Get().AspectRatio()) ==
      AspectMode::Auto)
    Output::Recompute();
}

GuestTexture *SelectPresentSource(VideoState &s, GuestTexture *frontBuffer) {
  GuestTexture *last_rt = s.last_drawn_rt;
  GuestTexture *rt = (s.back_buffer_surface && last_rt == s.back_buffer_surface)
                         ? s.back_buffer_surface
                     : (frontBuffer && frontBuffer->texture) ? frontBuffer
                     : last_rt                               ? last_rt
                     : s.last_resolved_dst ? s.last_resolved_dst
                                           : s.back_buffer_surface;
  if (rt && rt->sourceSurface && rt->sourceSurface != rt &&
      rt->sourceSurface->texture &&
      rt->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      rt->sourceSurface->descriptorIndex != kInvalidDescriptorIndex) {
    rt = rt->sourceSurface;
  }
  return rt;
}

void RecordPresentPass(VideoState &s, GuestTexture *rt,
                       plume::RenderTexture *back,
                       plume::RenderFramebuffer *back_fb) {
  BD_GPU_ZONE("RecordPresentPass");
  census::Mark(s, census::kPresent);
  if (rt->layout != plume::RenderTextureLayout::SHADER_READ) {
    const bool needs_discard =
        (rt->layout == plume::RenderTextureLayout::UNKNOWN);
    if (needs_discard) {
      s.command_list->barriers(
          plume::RenderBarrierStage::GRAPHICS,
          plume::RenderTextureBarrier(rt->texture,
                                      plume::RenderTextureLayout::COLOR_WRITE));
      s.command_list->discardTexture(rt->texture);
    }
    s.command_list->barriers(
        plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(rt->texture,
                                    plume::RenderTextureLayout::SHADER_READ));
    rt->layout = plume::RenderTextureLayout::SHADER_READ;
  }

  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();
  const u32 gamma_src_desc = rt->descriptorIndex;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);

  const double present_aspect =
      (bd::engine::SofdecPlayer::Playing() && !Output::StretchToFill())
          ? kDesignCanvasAspect
          : Output::RenderAspect();
  u32 fit_w = swap_w, fit_h = swap_h;
  i32 off_x = 0, off_y = 0;
  Output::ComputeFit(swap_w, swap_h, present_aspect, fit_w, fit_h, off_x,
                          off_y);
  if (fit_w != swap_w || fit_h != swap_h) {
    s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  }
  s.command_list->setViewports(plume::RenderViewport(
      static_cast<float>(off_x), static_cast<float>(off_y),
      static_cast<float>(fit_w), static_cast<float>(fit_h)));
  s.command_list->setScissors(
      plume::RenderRect(off_x, off_y, off_x + static_cast<i32>(fit_w),
                        off_y + static_cast<i32>(fit_h)));
#if defined(SVR_NATIVE_RENDERER)
  // SvR's front buffer is already display-encoded (Blue Dragon's is linear and relies on the
  // pow(x, 0.5) scanout ramp its bdBuildGammaRampLUT hook captures; SvR has neither), and the
  // playable build shows it as written. WWE 2K15's brightness option reshapes the scanout ramp:
  // apply it as scale * x^gamma (native/wwe13_gamma.cpp; the shader takes a negative display
  // correction as that scale).
  constexpr float kPresentGamma = 1.0f;
  float ramp_scale = 1.0f;
  SvrGuestGammaRamp(s.guest_gamma, ramp_scale);
  const float kPresentDisplayCorrection = -ramp_scale;
  const u32 fxaa_level = SvrFxaaLevel();
#else
  constexpr u32 fxaa_level = 0;
  constexpr float kPresentGamma = 1.0f;
  constexpr float kPresentDisplayCorrection = 1.0f;
#endif
  // The lite variant assumes the native-renderer scale path (Param1 < 0) and no FXAA; pick it only
  // then. It still applies pow for a non-identity ramp, so the picture is identical to the full
  // shader whenever svr_fxaa = 0.
#if defined(SVR_NATIVE_RENDERER)
  const bool present_lite = REXCVAR_GET(wwe13_present_lite) && fxaa_level == 0;
#else
  const bool present_lite = false;
#endif
  s.command_list->setPipeline(present_lite ? s.gamma_correction_lite_pipeline.get()
                                           : s.gamma_correction_pipeline.get());
  struct PresentPushConstants {
    u32 descriptor_index;
    u32 descriptor_index_2;
    float gamma;
    float display_correction;
  } pc{gamma_src_desc, fxaa_level, s.guest_gamma * kPresentGamma,
       kPresentDisplayCorrection};
#if defined(REBLUE_D3D11)
  D3D11BindHostSlot(gamma_src_desc, 0);
#endif
  s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                           kCopyPushConstantByteOffset,
                                           sizeof(pc));
  s.command_list->drawInstanced(3, 1, 0, 0);

  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));

  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }

  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
}

} // namespace

#if defined(__ANDROID__)
void NoteSwapChainWindow(plume::RenderWindow window) {
#if !defined(REBLUE_D3D12)
  HoldSwapWindow(window);
#else
  (void)window;
#endif
}
#endif

void Video::Clear(u32 flags, u32 color_argb, float depth, u32 stencil,
                  const plume::RenderRect *rects, u32 rect_count) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.frame_present_committed = false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  GuestTexture *rt = nullptr;
  GuestTexture *ds = nullptr;
  ResolveEffectiveTargets(s, rt, ds);
  const bool clear_color = (flags & kClearTarget) != 0 && rt != nullptr;
  const bool clear_depth = (flags & kClearZBuffer) != 0 && ds != nullptr;
  const bool clear_stencil = (flags & kClearStencil) != 0 && ds != nullptr;
  if (!clear_color && !clear_depth && !clear_stencil)
    return;
  if (!BindDrawFramebufferLocked(clear_color))
    return;
  // A partial clear stays partial (a whole-target clear there wipes the rest of a texture the
  // game builds in pieces). Rects are clamped to the framebuffer; none left = nothing to clear.
  plume::RenderRect clamped[8];
  u32 clamped_count = 0;
  if (rect_count) {
    const GuestTexture *fb = clear_color ? rt : ds;
    for (u32 i = 0; i < rect_count && clamped_count < 8; ++i) {
      plume::RenderRect r = rects[i];
      r.left = std::max(r.left, 0);
      r.top = std::max(r.top, 0);
      r.right = std::min(r.right, i32(fb->width));
      r.bottom = std::min(r.bottom, i32(fb->height));
      if (r.left < r.right && r.top < r.bottom)
        clamped[clamped_count++] = r;
    }
    if (!clamped_count)
      return;
  }
  const plume::RenderRect *clear_rects = clamped_count ? clamped : nullptr;
  census::Mark(s, census::kClear);
  if (clear_color)
    s.command_list->clearColor(0, ArgbToRenderColor(color_argb), clear_rects, clamped_count);
  if (clear_depth || clear_stencil) {
    s.command_list->clearDepthStencil(clear_depth, clear_stencil, depth,
                                      stencil, clear_rects, clamped_count);
  }
}

void Video::RequestResize() {
  state().resize_requested.store(true, std::memory_order_release);
}

// Frame cost statistics: every 10 s, one [perf] line. "work" is the render thread's time between
// two presents minus its waits (GPU fence, swap chain acquire/present, frame pacing): the CPU
// cost of recording a frame. "gpu wait" growing means the GPU is the limit; "work" near the
// frame interval means the CPU is.
struct FrameStats {
  using Clock = std::chrono::steady_clock;
  Clock::time_point window_start{}, last_present{};
  double wait_ms = 0;  // waits since the last present
  double sum_interval = 0, max_interval = 0, sum_work = 0, max_work = 0, sum_gpu = 0,
         max_gpu = 0, sum_swap = 0;
  u32 frames = 0, hitches = 0;
  double window_cpu_ms = -1;  // render thread CPU time at window start (Android), -1 = unknown
  double window_process_ms = -1;
};

// Android: the render thread's own CPU time. "work" can include time spent waiting for the
// game's next frame; render CPU near "work" means recording frames is the limit, far below it
// means the game's logic is.
// The whole process's CPU time, beside it, tells how many cores the game keeps busy.
double CpuMs(bool whole_process) {
#if defined(__ANDROID__)
  timespec ts{};
  if (clock_gettime(whole_process ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#else
  (void)whole_process;
#endif
  return -1;
}
FrameStats g_frame_stats;
extern std::atomic<u64> g_geometry_hashed_bytes, g_geometry_sampled;  // svr_geometry.cpp
extern std::atomic<u64> g_texture_hashed_bytes, g_texture_sampled;    // svr_resources.cpp

double MsSince(FrameStats::Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(FrameStats::Clock::now() - t).count();
}

#if defined(__ANDROID__)
}  // namespace bd::gpu
void Wwe12PerfHintReport(double work_ms);  // android/wwe12_perf_hint.cpp
namespace bd::gpu {
#endif

std::atomic<u64> g_present_wait_ns{0};  // all present waits so far (renderq_timeline.cpp)
void FrameStatsPresented(double gpu_wait_ms, double swap_ms) {
  auto &fs = g_frame_stats;
  const auto now = FrameStats::Clock::now();
  if (fs.last_present.time_since_epoch().count() != 0) {
    const double interval = std::chrono::duration<double, std::milli>(now - fs.last_present).count();
    const double work = std::max(0.0, interval - fs.wait_ms);
#if defined(__ANDROID__)
    Wwe12PerfHintReport(work);
#endif
    fs.sum_interval += interval;
    fs.max_interval = std::max(fs.max_interval, interval);
    fs.sum_work += work;
    fs.max_work = std::max(fs.max_work, work);
    fs.sum_gpu += gpu_wait_ms;
    fs.max_gpu = std::max(fs.max_gpu, gpu_wait_ms);
    fs.sum_swap += swap_ms;
    fs.hitches += interval > 25.0;
    ++fs.frames;
  } else {
    fs.window_start = now;
    fs.window_cpu_ms = CpuMs(false);
    fs.window_process_ms = CpuMs(true);
  }
  fs.last_present = now;
  fs.wait_ms = 0;
  if (std::chrono::duration<double>(now - fs.window_start).count() >= 10.0 && fs.frames) {
    const double n = fs.frames;
    // Video memory in use / what the driver allows us, to spot a leak in players' logs.
    const Video::VideoMemory vram = Video::MemoryUsage();
    BD_INFO("[perf] {} frames: interval {:.1f} ms (max {:.1f}) | work {:.1f} ms (max {:.1f}) | "
            "gpu wait {:.1f} ms (max {:.1f}) | swap/vsync {:.1f} ms | hitches >25ms {} | "
            "vram {} / {} MB",
            fs.frames, fs.sum_interval / n, fs.max_interval, fs.sum_work / n, fs.max_work,
            fs.sum_gpu / n, fs.max_gpu, fs.sum_swap / n, fs.hitches, vram.used >> 20,
            vram.budget >> 20);
    // Change checks of the game's memory (geometry and texture copies): MB hashed in full per
    // frame, and copies checked by sample instead.
    BD_INFO("[perf] hashed per frame: geometry {:.2f} MB ({:.0f} sampled), textures {:.2f} MB "
            "({:.0f} sampled)",
            g_geometry_hashed_bytes.exchange(0) / n / 1048576.0, g_geometry_sampled.exchange(0) / n,
            g_texture_hashed_bytes.exchange(0) / n / 1048576.0, g_texture_sampled.exchange(0) / n);
    const double cpu_now = CpuMs(false), process_now = CpuMs(true);
    u64 blocks_uploaded = 0, blocks_reused = 0;
    ConstantBlockStats(blocks_uploaded, blocks_reused);
    if (cpu_now >= 0 && fs.window_cpu_ms >= 0)
      BD_INFO("[perf] cpu per frame: render thread {:.1f} ms (work {:.1f} ms) | whole game "
              "{:.1f} ms | constant blocks {:.0f} uploaded, {:.0f} reused",
              (cpu_now - fs.window_cpu_ms) / n, fs.sum_work / n,
              (process_now - fs.window_process_ms) / n, blocks_uploaded / n, blocks_reused / n);
#if defined(__ANDROID__)
    {
      using namespace zones;
      const auto ms = [&](Id id) { return g_ns[id].exchange(0, std::memory_order_relaxed) / n / 1e6; };
      const double draws = ms(kDraws), constants = ms(kConstants), geometry = ms(kGeometry),
                   textures = ms(kTextures), present = ms(kPresent);
      // The rest of the render thread's time is the game's own code (recompiled).
      BD_INFO("[perf] render thread split per frame: draws {:.1f} ms (constants {:.1f} of it) | "
              "textures {:.1f} ms | geometry {:.1f} ms | present {:.1f} ms (waits included)",
              draws, constants, textures, geometry, present);
    }
#endif
    fs = FrameStats{};
    fs.window_start = now;
    fs.last_present = now;
    fs.window_cpu_ms = cpu_now;
    fs.window_process_ms = process_now;
  }
}

void Video::Present(GuestTexture *frontBuffer) {
  const auto present_entry_w13r = FrameStats::Clock::now();  // w13r breakdown
  WWE12_CPU_ZONE(kPresent);
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire)) {
    return;
  }
  if (s.frame_present_committed) {
    return;
  }
  // WWE '12 turbo loading screens (native/wwe12_load_turbo.cpp): most frames are not shown.
  if (Wwe12LoadTurboSkipPresent()) {
    AbandonFrame(s, lock);
    return;
  }

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
#if defined(__ANDROID__) && !defined(REBLUE_D3D12)
  if (plume::RenderWindow window = NewAndroidWindow())
    RebuildSwapChain(s, window);
  else
#endif
  if (!s.swap_chain || s.swap_chain->needsResize() || resize_requested) {
    RebuildSwapChain(s);
  }

  if (s.framebuffers.empty()) {
#if defined(WWE13_DXGI_FALLBACK)
    // The DXGI chain could not be resized (window or display change) and the window is not
    // minimized: after 3 such frames use the Vulkan swap chain instead of staying black.
    if (plume::VulkanDxgiPresentActive && s.swap_chain && s.swap_chain->getWidth() &&
        s.swap_chain->getHeight() && ++s_dxgi_acquire_fails >= 3) {
      s_dxgi_acquire_fails = 0;
      WaitAllFrames(s);
      if (FallBackToVulkanSwapChain(s, "resize failed"))
        BuildFramebuffers(s) && BuildPresentSemaphores(s);
    }
#endif
    AbandonFrame(s, lock);
    if (lock.owns_lock())
      lock.unlock();
    PaceFrame(true);
    return;
  }

  u32 texture_index = 0;
  const auto acquire_start = FrameStats::Clock::now();
  {
    BD_CPU_ZONE("AcquireTexture");
    if (!s.swap_chain->acquireTexture(
            s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)].get(),
            &texture_index)) {
      RebuildSwapChain(s);
      if (s.framebuffers.empty() ||
          !s.swap_chain->acquireTexture(
              s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)]
                  .get(),
              &texture_index)) {
        CheckDeviceRemoved("swapchain acquire");
#if defined(WWE13_DXGI_FALLBACK)
        // 3 frames in a row without an image on the DXGI path (e.g. a driver that imports the
        // shared textures but fails the keyed-mutex submit): use the Vulkan swap chain instead.
        // RebuildSwapChain above waited every frame fence, so nothing is in flight.
        if (plume::VulkanDxgiPresentActive && ++s_dxgi_acquire_fails >= 3) {
          s_dxgi_acquire_fails = 0;
          if (FallBackToVulkanSwapChain(s, "3 frames without an image"))
            BuildFramebuffers(s) && BuildPresentSemaphores(s);
        }
#endif
        AbandonFrame(s, lock);
        return;
      }
    }
  }
#if defined(WWE13_DXGI_FALLBACK)
  s_dxgi_acquire_fails = 0;
  if (REXCVAR_GET(wwe13_vk_dxgi_fallback_test) == 2 && plume::VulkanDxgiPresentActive &&
      ++s_dxgi_test_presents == 600) {
    // Test only: same switch as above, mid-game (fences waited first, like RebuildSwapChain).
    WaitAllFrames(s);
    if (FallBackToVulkanSwapChain(s, "test"))
      BuildFramebuffers(s) && BuildPresentSemaphores(s);
    AbandonFrame(s, lock);
    return;
  }
#endif

  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();
  double swap_ms = MsSince(acquire_start);
  const double acquire_ms_w13r = swap_ms;  // w13r breakdown

  GuestTexture *rt = SelectPresentSource(s, frontBuffer);

  const bool have_rt_blit =
      rt && rt->texture && rt->descriptorIndex != kInvalidDescriptorIndex &&
      rt->sampleCount == plume::RenderSampleCount::COUNT_1;
  if (!have_rt_blit) {
    static std::atomic<u32> s_log{0};
    const u32 n = s_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 5) {
      BD_ERROR("Present #{} skipped: no drawable RT", n);
    }
    AbandonFrame(s, lock);
    return;
  }

  BeginCommandList(s);
  RecordPresentPass(s, rt, back, back_fb);

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  census::OnListEnd(s);
  s.command_lists[cur]->end();
  s.command_list_open = false;

  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  {
    BD_CPU_ZONE("Submit");
    s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                                 s.fences[cur].get());
  }
  s.command_list_submitted[cur] = true;
  census::OnPresent();
  ApplyVsync(s);
  double present_call_ms = 0;
  {
    BD_CPU_ZONE("PresentSwap");
    const auto present_start = FrameStats::Clock::now();
    if (!s.swap_chain->present(texture_index, signals, 1)) {
      if (!CheckDeviceRemoved("swapchain present"))
        s.resize_requested.store(true, std::memory_order_release);
    }
    present_call_ms = MsSince(present_start);
    swap_ms += present_call_ms;
  }
  double gpu_wait_ms = 0;
  double pace_ms = 0;
  {
    BD_CPU_ZONE("WaitFence");
    const auto fence_start = FrameStats::Clock::now();
    AdvanceAndWaitReused(s);
    gpu_wait_ms = MsSince(fence_start);
  }
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  s.frame_present_committed = true;
  BD_FRAME_MARK();
  Video::SyncBackBufferSizeLocked();
  lock.unlock();
  {
    BD_CPU_ZONE("DrainSlot");
    DrainSlot(s, reclaimed);
  }
  {
    BD_CPU_ZONE("PaceFrame");
    const auto pace_start = FrameStats::Clock::now();
    PaceFrame();
    pace_ms = MsSince(pace_start);
    swap_ms += pace_ms;
  }
  {  // w13r (2K14): where the present time goes, every 600 frames (present call / GPU fence / limiter sleep).
    static double sum_present = 0, sum_fence = 0, sum_pace = 0, sum_acquire = 0, sum_total = 0;
    static u32 n = 0;
    sum_present += present_call_ms;
    sum_fence += gpu_wait_ms;
    sum_pace += pace_ms;
    sum_acquire += acquire_ms_w13r;
    sum_total += std::chrono::duration<double, std::milli>(FrameStats::Clock::now() - present_entry_w13r).count();
    if (++n == 600) {
      BD_INFO("[w13r-present] 600 frames: whole Present {:.2f} ms = acquire {:.2f} + present call {:.2f} + GPU fence {:.2f} "
              "+ limiter {:.2f} + rest (avg/frame)",
              sum_total / n, sum_acquire / n, sum_present / n, sum_fence / n, sum_pace / n);
      sum_present = sum_fence = sum_pace = sum_acquire = sum_total = 0;
      n = 0;
    }
  }
  g_frame_stats.wait_ms += gpu_wait_ms + swap_ms;
  g_present_wait_ns.fetch_add(u64((gpu_wait_ms + swap_ms) * 1e6), std::memory_order_relaxed);
  FrameStatsPresented(gpu_wait_ms, swap_ms);
}

void Video::SkipPresent() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire) ||
      s.frame_present_committed) {
    return;
  }
  AbandonFrame(s, lock);
}

void Video::PresentOverlayFrame() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.swap_chain || s.ready)
    return;

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (s.command_list_submitted[i]) {
        s.queue->waitForCommandFence(s.fences[i].get());
        s.command_list_submitted[i] = false;
      }
    }
    s.framebuffers.clear();
    ResizeSwapChain(s);
  }
  if (s.framebuffers.empty())
    return;

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  u32 texture_index = 0;
  if (!s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(),
                                    &texture_index)) {
    s.resize_requested.store(true, std::memory_order_release);
    return;
  }
  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();
  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();

  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);
  s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));
  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }
  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));

  census::OnListEnd(s);
  s.command_lists[cur]->end();
  s.command_list_open = false;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  if (!s.swap_chain->present(texture_index, signals, 1)) {
    CheckDeviceRemoved("swapchain present (overlay)");
  }
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

} // namespace bd::gpu
