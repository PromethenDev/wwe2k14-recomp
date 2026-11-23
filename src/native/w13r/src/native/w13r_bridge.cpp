// w13r_bridge.cpp - null guest GPU (ported from the WWE '13 fork's SDK 0.10 graphics/null, BSD-3 Tom Clay) and the
// renderer start-up from the fork's wwe13_app.h (iMrBurn, GPL-3.0), for 2K14's SDK 0.7.3. Decision 0008.
#include "w13r_bridge.h"

#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "gpu/device.h"
#include "gpu/imgui_overlay_drawer.h"

#if defined(WWE13_RUNTIME_SHADERS) && !defined(WWE13_BUILTIN_SPIRV)
bool Wwe13DxcUsable();  // wwe13_shader_cache.cpp
#endif
bool Wwe13TryNextBackend();  // wwe13_backend_chain.cpp

namespace {
using namespace rex::graphics;

class NullCommandProcessor final : public CommandProcessor {
 public:
  using CommandProcessor::CommandProcessor;
  void IssueSwap(uint32_t, uint32_t, uint32_t) override {}
  void TracePlaybackWroteMemory(uint32_t, uint32_t) override {}
  void RestoreEdramSnapshot(const void*) override {}

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}
  Shader* LoadShader(xenos::ShaderType, uint32_t, const uint32_t*, uint32_t) override { return nullptr; }
  bool IssueDraw(xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override { return true; }
  bool IssueCopy() override { return true; }
};

class NullGraphicsSystem final : public GraphicsSystem {
 public:
  std::string name() const override { return "Null"; }
  rex::X_STATUS Setup(rex::runtime::FunctionDispatcher* dispatcher, rex::system::KernelState* kernel_state,
                 rex::ui::WindowedAppContext* app_context, bool) override {
    REXLOG_INFO("[w13r] guest GPU: null (command stream for synchronization only; the native renderer draws)");
    return GraphicsSystem::Setup(dispatcher, kernel_state, app_context, false);  // no provider, no presenter
  }

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<NullCommandProcessor>(this, kernel_state_);
  }
};

[[noreturn]] void Fail(const wchar_t* message) {
#if defined(_WIN32)
  MessageBoxW(nullptr, message, L"WWE 2K14 PC", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
  std::_Exit(1);
}
}  // namespace

namespace w13r {
std::unique_ptr<rex::system::IGraphicsSystem> CreateNullGpu() { return std::make_unique<NullGraphicsSystem>(); }

void Start(rex::ui::Window* window) {
  // WWE2K14_W13R_GPU: the GPU's name as the driver lists it (e.g. "Intel(R) UHD Graphics 630"); empty = the renderer's
  // own choice (a discrete GPU first). Same as the svr_gpu_device setting, but settable from the launcher environment.
  if (const char* gpu = std::getenv("WWE2K14_W13R_GPU"); gpu && gpu[0]) rex::cvar::SetFlagByName("svr_gpu_device", gpu);
  // The launcher's picture size (WWE2K14_D3D_SCENE_SCALE: 480p 0.6, 720p 1.0, 1080p 1.5, 1440p 2.0, 4K 3.0) as the
  // renderer's internal resolution. Unset keeps the renderer's own automatic choice.
  if (const char* sc = std::getenv("WWE2K14_D3D_SCENE_SCALE"); sc && sc[0]) {
    const double v = std::strtod(sc, nullptr);
    const char* h = v <= 0 ? nullptr : v < 0.7 ? "480" : v < 1.2 ? "720" : v < 1.75 ? "1080" : v < 2.5 ? "1440" : "2160";
    if (h) {
      rex::cvar::SetFlagByName("svr_render_scale", h);
      REXLOG_INFO("[w13r] picture size from the launcher: scale {} -> {}p", sc, h);
    }
  }
  if (!bd::gpu::Video::CreateHostDevice(window)) {
    REXLOG_ERROR("Native renderer: could not create the host device ({} unavailable or too old)",
                 bd::gpu::Video::BackendName());
    if (Wwe13TryNextBackend()) {
      REXLOG_ERROR("Native renderer: graphics API unavailable, AUTO starts the next executable");
      rex::FlushLogging();
      std::_Exit(0);
    }
    rex::FlushLogging();
    Fail(L"The native renderer could not start: Vulkan 1.2, Direct3D 12 or Direct3D 11 does not work on this PC, "
         L"or the graphics driver is too old.\n\nUpdate the graphics driver or choose another renderer in the launcher.");
  }
#if defined(WWE13_RUNTIME_SHADERS) && !defined(WWE13_BUILTIN_SPIRV)
  if (!Wwe13DxcUsable()) {
    REXLOG_ERROR("Native renderer: shader compiler (dxcompiler.dll) missing or unusable");
    rex::FlushLogging();
    Fail(L"The native renderer could not start: dxcompiler.dll is missing or unusable (it must sit next to the "
         L"program).");
  }
#endif
  REXLOG_INFO("[w13r] host renderer up: {}", bd::gpu::Video::BackendName());
}

std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer() {
  return std::make_unique<bd::gpu::ImGuiOverlayDrawer>();
}

// As the WWE '13 fork's wwe13_app.h: Present runs on the game thread and ImGui on the UI thread, so the overlay is
// drawn synchronously on the UI thread into the presented image, only while a dialog is open.
void InstallOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& app_context) {
  if (!drawer) return;
  bd::gpu::Video::SetOverlayDrawHook([drawer, &app_context](plume::RenderCommandList* cmd, plume::RenderFramebuffer* fb,
                                                            uint32_t w, uint32_t h) {
    if (!drawer->HasDialogs()) return;
    app_context.CallInUIThreadSynchronous([drawer, cmd, fb, w, h] {
      bd::gpu::ReblueUIDrawContext ctx(w, h, cmd, fb);
      drawer->Draw(ctx);
    });
  });
  REXLOG_INFO("[w13r] in-game overlay installed (settings menu, FPS counter)");
}
}  // namespace w13r
