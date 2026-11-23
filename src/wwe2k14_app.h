
// wwe2k14 - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/diagnostics/freeze_logger.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/graphics/flags.h>
#include <rex/rex_app.h>
#include <functional>
#include <rex/ui/keybinds.h>  // F11 bind (also needed without nr: w13r build)
#include <rex/ui/window.h>
namespace wwe2k14::test_env {
void Apply();  // src/test_env.cpp (lane all-1104): TEST-ENV.txt overrides on PC test builds
void StartExitTimer(std::function<void()> request_close);  // WWE2K14_TEST_EXIT_AFTER (test builds; no-op in public)
}
namespace wwe2k14::cool {
void Apply();  // src/cool_mode.cpp (lane all-1104): WWE2K14_ECOQOS=1, efficiency-class CPU work
}
#if defined(REX_UWP)
namespace wwe2k14::uwp_env {
void Apply();  // src/uwp_env.cpp (card C3)
}
#endif
#if REX_HAS_D3D12 && REX_HAS_VULKAN
#include <cstring>
#include <rex/graphics/vulkan/graphics_system.h>  // card C3: backend choice in dual-backend test builds
#endif

#include "shader_cache_seed.h"
#ifdef WWE2K14_W13R
#include "native/w13r/src/native/w13r_bridge.h"  // decision 0008: WWE '13 v1.6 renderer build
#endif
#include "timeline.h"

#ifdef WWE2K14_HAS_NR
#include <cstring>
#include <map>
#include <string_view>
#include <vector>

#if !defined(REX_UWP)
#include <SDL3/SDL_messagebox.h>
#endif
#include <vulkan/vulkan.h>

#include <rex/filesystem.h>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/graphics_system.h>
#include <rex/runtime.h>

#include "native/nr/nr_backend.h"
#include "native/nr/nr_shared.h"  // card RT1: SetRuntimeShaderCacheDir
#include "native/nr/nr_startup.h"  // card G56: pre-guest shared-device refusal
#include "native/nr/nr_constants32.h"  // card G56 B2: pre-guest constants ABI validation
#include "native/nr/nr_constants32_install.h"  // card G56b: complete shaders32 inspector
#include "native/nr/nr_constants32_startup.h"  // card G56b: lazy automatic/manual choice
#include "native/d3d/capture.h"
#include "native/d3d/hooks.h"  // GameFrameStats (FPS counter)
#include "native/d3d/rr_play.h"  // RR2: WWE2K14_RR_PLAY offline replay (exits before the runtime starts)
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <utility>
#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <atomic>
#include <cmath>
#include <thread>
#include "settings_menu.h"  // G1 in-game settings menu (F1, Back+Start)
#include "settings_menu_test.h"
#include "launcher_ini_update.h"
#include "overlay_input.h"
#include "native/d3d/live_options.h"
namespace wwe2k14::live_game {  // src/present_interval.cpp
bool LowPowerOn();
void SetLowPower(bool on);
}  // namespace wwe2k14::live_game
REXCVAR_DECLARE(bool, debug_ui);
#endif
#if defined(WWE2K14_W13R) && !defined(WWE2K14_HAS_NR)  // overlay UI pieces the shipped-renderer block above includes
#include <chrono>
#include <functional>
#include <memory>
#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <atomic>
#include <cmath>
#include <thread>
#include "settings_menu.h"
#include "settings_menu_test.h"
#include "launcher_ini_update.h"
#include "overlay_input.h"
namespace wwe2k14::live_game {  // src/present_interval.cpp
bool LowPowerOn();
void SetLowPower(bool on);
}  // namespace wwe2k14::live_game
namespace wwe2k14::d3d {
void GameFrameStats(uint64_t* swaps, uint64_t* max_gap_ns);  // w13r: native/w13r_2k14_glue.cpp
}
REXCVAR_DECLARE(bool, debug_ui);
#endif

namespace wwe2k14 {
// Installs the guest-state dump used by the always-on freeze report
// (defined in src/freeze_guest_state.cpp).
void RegisterFreezeGuestState();
}  // namespace wwe2k14

namespace wwe2k14 {
bool AutopilotEnabled();
void KeepDisplayAwake(bool on);  // src/keep_awake.cpp (GitHub #49; Windows only, no-op elsewhere)
std::unique_ptr<rex::system::IInputSystem> CreateAutopilotInputSystem(bool tool_mode);
}  // namespace wwe2k14

class Wwe2k14App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  // Defined after NativeStartupBoundaryApp so native-selected runs can be wrapped in
  // the startup-refusal boundary without changing the non-native path.
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

  // Always-on freeze logger: a 1 Hz watchdog plus a lock-free kernel-event ring
  // that writes a report into the player's logs folder if the game stops
  // presenting. Default on; WWE2K14_FREEZE_LOG=0 disables it.
  void OnPostSetup() override {
    // Card S3-73: the native D3D layer's load-stall detector requests a report through this file (set before the
    // watchdog thread starts, so nothing reads the environment concurrently).
    if (!std::getenv("WWE2K14_FREEZE_DUMP_NOW")) {
      const std::string log_file = rex::cvar::GetFlagByName("log_file");
      const auto dir = log_file.empty() ? std::filesystem::temp_directory_path()
                                        : std::filesystem::path(log_file).parent_path();
      const std::string request = (dir / ".wwe2k14-stall-request").string();
#ifdef _WIN32
      _putenv_s("WWE2K14_FREEZE_DUMP_NOW", request.c_str());
#else
      setenv("WWE2K14_FREEZE_DUMP_NOW", request.c_str(), 0);
#endif
    }
    rex::diagnostics::freeze_logger::Start();
    // Card LOG1: session timeline next to the main log (never the temp folder). WWE2K14_TIMELINE=0 disables it.
    {
      const std::string log_file = rex::cvar::GetFlagByName("log_file");
      std::filesystem::path timeline_dir = std::filesystem::path(log_file).parent_path();
      if (log_file.empty() || timeline_dir.empty()) timeline_dir = rex::filesystem::GetExecutableFolder() / "logs";
      const auto settings_file = rex::filesystem::GetExecutableFolder() / "wwe2k14-enhanced.ini";  // the launcher's
      wwe2k14::timeline::Start(timeline_dir.string().c_str(), settings_file.string().c_str());
    }
    // GitHub #58: F11 toggles fullscreen/windowed from whatever mode the launcher started in. F11 is not a mapped
    // keyboard-as-controller key, so it never reaches the game as a button. UI thread; SetFullscreen is deferred
    // to the platform loop, so the swapchain is recreated by the window code, not mid-frame here.
    rex::ui::RegisterBind("bind_toggle_fullscreen", "F11", "Toggle fullscreen / windowed", [this] {
      if (auto* w = window()) {
        const bool fs = !w->IsFullscreen();
        w->SetFullscreen(fs);
        REXLOG_INFO("[wwe2k14] fullscreen {} (F11)", fs ? "on" : "off");
      }
    });
    fullscreen_bind_registered_ = true;
    // Guest-state section of the freeze report (pump object / device / globals).
    wwe2k14::RegisterFreezeGuestState();
  }

  // Owner 2026-10-13 (same as WWE 13 d155c24): the mouse pointer hides after 4 s without mouse movement and comes
  // back when the mouse moves. Runs once the window exists (OnPostSetup is too early). WWE2K14_SHOW_CURSOR=1 keeps it.
  void OnPreLaunchModule() override {
    const char* show = std::getenv("WWE2K14_SHOW_CURSOR");
    if (auto* w = window(); w && !(show && show[0] == '1')) {
      w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
    }
    wwe2k14::KeepDisplayAwake(true);
    if (auto* w = window()) w->AddListener(&timeline_close_listener_);
#if !defined(REX_UWP)
    // ARC-TESTER (lane crowdc1, 11-11): test-only timed close through the player's own close path (no-op unless
    // WWE2K14_TEST_EXIT_AFTER is set; public builds compile test_env.cpp without it).
    wwe2k14::test_env::StartExitTimer([this] {
      app_context().CallInUIThread([this] {
        if (auto* w = window()) w->RequestClose();
      });
    });
#endif
#ifdef WWE2K14_W13R
    w13r::Start(window());  // host renderer on the game window, before the game starts drawing
    w13r::InstallOverlay(imgui_drawer(), app_context());  // settings menu / FPS counter drawn into the frames
#endif
#ifdef WWE2K14_HAS_NR
    if (auto* w = window()) w->AddListener(&close_listener_);  // the D3D crash filter learns about the window close
#endif
  }

  // Float24 depth: the game saves the scene depth (D24FS8) to a texture and restores it with a
  // per-tile quad before drawing characters (reversed depth, GEQUAL). On the default host
  // render-target path the restored depth comes back too near, so wrestlers far from the camera
  // (start of entrances) fail the depth test and vanish. Converting float24 in the pixel shader
  // fixes it (docs/BUGHUNT-2K14.md, 2026-10-11). WWE2K14_DEPTH_FLOAT24_PS=0 restores the old path.
#ifdef WWE2K14_W13R
  // w13r: the SDK's detached overlay mode (null GPU, no presenter) draws the ImGui overlay through the renderer.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override { return w13r::CreateOverlayDrawer(); }
#endif
  void OnPreSetup(rex::RuntimeConfig& config) override {
#if defined(REX_UWP)
    wwe2k14::uwp_env::Apply();  // card C3: Xbox settings -> WWE2K14_* environment, before anything reads it
#else
    wwe2k14::test_env::Apply();  // TEST-ENV.txt next to the exe (test A/B), before anything reads the environment
    wwe2k14::cool::Apply();      // WWE2K14_ECOQOS=1 (TEST-ENV.txt): plugged-in laptops behave like battery
#endif
#if REX_HAS_D3D12 && REX_HAS_VULKAN
    // Card C3 (test builds with an SDK that has both backends): the SDK's D3D12 backend only together with the native
    // renderer's D3D12 backend (WWE2K14_NR_BACKEND=d3d12); otherwise the shipped Vulkan backend, as before.
    {
      const char* be = std::getenv("WWE2K14_NR_BACKEND");
      if (!(be && std::strcmp(be, "d3d12") == 0)) {
        config.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
      }
      REXLOG_INFO("[wwe2k14] SDK graphics backend: {}", be && std::strcmp(be, "d3d12") == 0 ? "D3D12" : "Vulkan");
    }
#endif
#ifdef WWE2K14_W13R
    // w13r: the game's D3D command stream runs on a null guest GPU; the WWE '13 renderer draws from the D3D hooks.
    config.graphics = w13r::CreateNullGpu();
#endif
    // Card S3-16 (test harness, default off): scripted pad for unattended UHD entrance runs.
    if (wwe2k14::AutopilotEnabled()) config.input_factory = wwe2k14::CreateAutopilotInputSystem;
#if defined(WWE2K14_HAS_NR) || defined(WWE2K14_W13R)
    // G1: same SDK drivers behind a gate (Back+Start = settings menu; the game gets no pad while it is open).
    else config.input_factory = wwe2k14::overlay_input::CreateInputSystem;
#endif
    const char* v = std::getenv("WWE2K14_DEPTH_FLOAT24_PS");
    if (!(v && v[0] == '0')) REXCVAR_SET(depth_float24_convert_in_pixel_shader, true);
    // Guest vblank on the real display's vblank when it runs at a multiple of 60 Hz (Windows); see vsync_host_display
    // in the SDK graphics system. WWE 13 owner A/B 2026-09-26: entrances far better. WWE2K14_HOST_VSYNC=0 keeps the timer.
    const char* host_vsync = std::getenv("WWE2K14_HOST_VSYNC");
    rex::cvar::SetFlagByName("vsync_host_display", host_vsync && host_vsync[0] == '0' ? "false" : "true");
    REXLOG_INFO("[wwe2k14] vsync_host_display={} (WWE2K14_HOST_VSYNC), late_flip env WWE2K14_LATE_FLIP",
                rex::cvar::GetFlagByName("vsync_host_display"));
    // First run only: install the shader/pipeline cache shipped next to the exe (shader-cache/),
    // so pipelines are built at boot instead of stuttering mid-entrance.
    wwe2k14::SeedShaderCache(user_data_root_);
#ifdef WWE2K14_HAS_NR
    ConfigureNativeRenderer();
    wwe2k14::d3d::rr::RunReplayIfRequested();  // RR2: returns only when WWE2K14_RR_PLAY is unset
#endif
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    user_data_root_ = paths.user_data_root;
  }

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
#ifdef WWE2K14_HAS_NR
  // Card G56: after Runtime::Setup has created the graphics device, but before the XEX is
  // loaded or the window exists, refuse the opted-in Vulkan shared-device path if the device
  // cannot satisfy the renderer's core feature gate. The boundary wrapper turns this dedicated
  // exception into one blocking message and normal failed-startup teardown.
  void OnLoadXexImage(std::string& xex_image) override {
    (void)xex_image;
    const char* mode = std::getenv("WWE2K14_NR");
    if (!nr::startup::NativeModeSelected(mode)) return;
    // Card G56b: own the folder selection and the exact-"1" manual flag for this startup only.
    // The runtime reads the same variables (native/d3d/runtime.cpp:76-80), so every check below
    // must agree with the values the backend will actually see.
    const std::string primary =
        nr_constants32::PrimaryFolder(std::getenv(nr_constants32::kPrimaryFolderSwitch));
    const std::string secondary =
        nr_constants32::SecondaryFolder(std::getenv(nr_constants32::kSecondaryFolderSwitch));
    const char* const flag_env = std::getenv(nr_constants32::kSwitch);
    const std::string flag_value = flag_env ? flag_env : "";
    const char* const original_flag = flag_env ? flag_value.c_str() : nullptr;
    const bool manual_force = nr_constants32::SwitchRequested(original_flag);

    const char* const direct = std::getenv("WWE2K14_NR_DIRECT_PRESENT");
    const bool direct_present = nr::startup::DirectPresentSelected(direct);
    const bool d3d12_backend = nr::backend::BackendWantsDxil();
    if (nr::startup::ShouldSkipVulkanStartupGates(d3d12_backend, direct_present)) {
      // The D3D12 backend obtains and validates its shared device in the existing runtime path.
      // Do not feed its startup through the Vulkan feature probe or SPIR-V constants32 chooser.
      REXLOG_INFO("[wwe2k14] D3D12 direct-present selected; "
                  "Vulkan shared-device and constants32 startup gates skipped.");
      return;
    }
    if (!direct_present) {
      // The non-shared backend creates its own device and loads the primary folder itself. Validate
      // exactly that legacy selection before the guest starts, and leave a sibling install alone.
      const nr_constants32::Selection direct_selection =
          nr_constants32::ReadSelection(primary, secondary, original_flag);
      if (!direct_selection.ok) {
        REXLOG_ERROR("[wwe2k14] constants ABI: {}", direct_selection.error);
        throw nr::startup::NativeStartupRefusal(nr::startup::RefusalMessage());
      }
      return;
    }
    // The SDK sets up the Vulkan context and shared device on the command-processor thread
    // after starting it. Probe from that thread and copy the result back; reading SDK state
    // directly on this UI thread can race the context setup.
    auto* runtime = this->runtime();
    auto* graphics = runtime
                         ? static_cast<rex::graphics::GraphicsSystem*>(runtime->graphics_system())
                         : nullptr;
    auto* command_processor = graphics ? graphics->command_processor() : nullptr;
    if (!command_processor) {
      REXLOG_ERROR("[wwe2k14] the graphics command processor is unavailable.");
      throw nr::startup::NativeStartupRefusal(nr::startup::RefusalMessage());
    }

    struct ProbeOutcome {
      bool acquired = false;
      rex::graphics::NrVulkanDevice device{};
      std::string device_name;
    };
    // The chip name appears only in the owner-facing missing-feature message, so query it only
    // where that message can be reached: a forced ABI, a missing Int64, or a failed acquisition.
    // The query runs on the same command-processor thread as the device probe; a missing SDK
    // getter leaves the name empty rather than falling back to global Vulkan or enumeration.
    auto probe_device = [manual_force] {
      ProbeOutcome outcome;
      outcome.acquired = rex::graphics::GetNrVulkanDevice(&outcome.device);
      if (manual_force || !outcome.device.shader_int64 || !outcome.acquired) {
        const auto get_instance_proc_addr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(outcome.device.get_instance_proc_addr);
        if (get_instance_proc_addr && outcome.device.instance && outcome.device.physical_device) {
          const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
              get_instance_proc_addr(static_cast<VkInstance>(outcome.device.instance),
                                     "vkGetPhysicalDeviceProperties"));
          if (get_properties) {
            VkPhysicalDeviceProperties properties{};
            get_properties(static_cast<VkPhysicalDevice>(outcome.device.physical_device),
                           &properties);
            outcome.device_name = properties.deviceName;
          }
        }
      }
      return outcome;
    };
    const auto enqueue = [command_processor](std::function<void()> callback) {
      command_processor->CallInThread(std::move(callback));
    };
    const auto run_probe = [&](const char* phase) {
      const auto probed =
          nr::startup::ProbeOnWorker(probe_device, enqueue, std::chrono::seconds(10));
      if (probed.status == nr::startup::ProbeStatus::kTimedOut) {
        REXLOG_ERROR("[wwe2k14] {}: the shared Vulkan device probe timed out on the graphics thread.",
                     phase);
        throw nr::startup::NativeStartupRefusal(nr::startup::RefusalMessage());
      }
      if (probed.status == nr::startup::ProbeStatus::kThrew || !probed.value) {
        REXLOG_ERROR("[wwe2k14] {}: the shared Vulkan device probe failed.", phase);
        throw nr::startup::NativeStartupRefusal(nr::startup::RefusalMessage());
      }
      return *probed.value;
    };
    ProbeOutcome probe = run_probe("initial");

    nr_constants32::ChoiceInput choice_input;
    choice_input.native_selected = true;
    choice_input.direct_present = true;
    choice_input.output_populated = probe.device.device != nullptr;
    choice_input.shader_int64 = probe.device.shader_int64;
    choice_input.buffer_device_address = probe.device.buffer_device_address;
    choice_input.descriptor_indexing = probe.device.descriptor_indexing;
    choice_input.manual_force = manual_force;
    choice_input.primary = primary;
    choice_input.secondary = secondary;
    const nr_constants32::ChoiceResult choice = nr_constants32::DecideStartupChoice(
        choice_input, nr_constants32::InspectMarker, nr_constants32::InspectCompleteSet);
    if (!choice.error.empty() && manual_force) {
      REXLOG_ERROR("[wwe2k14] constants32 choice: {}", choice.error);
      throw nr::startup::NativeStartupRefusal(nr::startup::RefusalMessage());
    }
    if (!choice.error.empty()) {
      // Automatic runs stay on the primary folder and continue to the normal feature decision.
      REXLOG_WARN("[wwe2k14] constants32 choice: {}", choice.error);
    }

    // Validate the final folder/flag pair before any environment change. A mixed secondary
    // still refuses; nothing below clears or falls back around one.
    const char* const chosen_flag = choice.activate ? "1" : original_flag;
    const nr_constants32::Selection selection =
        nr_constants32::ReadSelection(choice.primary, secondary, chosen_flag);
    if (!selection.ok) {
      REXLOG_ERROR("[wwe2k14] constants ABI: {}", selection.error);
      throw nr::startup::NativeStartupRefusal(nr::startup::RefusalMessage());
    }

    if (choice.activate || choice.reprobe) {
      const auto set_environment = [](const char* name, const char* value) {
#ifdef _WIN32
        return _putenv_s(name, value) == 0;
#else
        return setenv(name, value, 1) == 0;
#endif
      };
      if (!set_environment(nr_constants32::kPrimaryFolderSwitch, choice.primary.c_str())) {
        REXLOG_ERROR("[wwe2k14] could not set {} to select the constants32 shader set.",
                     nr_constants32::kPrimaryFolderSwitch);
        throw nr::startup::NativeStartupRefusal(nr::startup::RefusalMessage());
      }
      if (!set_environment(nr_constants32::kSwitch, "1")) {
        REXLOG_ERROR("[wwe2k14] could not set {} for the constants32 shader set.",
                     nr_constants32::kSwitch);
        throw nr::startup::NativeStartupRefusal(nr::startup::RefusalMessage());
      }
      // Re-read the device after the ABI change; the final decision must rest on that result,
      // not on the pre-activation probe.
      probe = run_probe("activation");
    }

    const nr::startup::SharedDeviceFeatures features{
        probe.device.device != nullptr, probe.device.shader_int64,
        probe.device.buffer_device_address, probe.device.descriptor_indexing};
    const nr::startup::StartupDecision decision = nr::startup::DecideNativeSharedDevice(
        true, true, probe.acquired, features, selection.enabled, probe.device_name);
    if (decision.refuse) {
      REXLOG_ERROR("[wwe2k14] {}", decision.log_detail);
      throw nr::startup::NativeStartupRefusal(decision.message);
    }
    if (choice.activate) {
      REXLOG_INFO("[wwe2k14] constants32 shaders selected ({}) from {}; shared device re-probe passed.",
                  manual_force ? "manual" : "automatic", choice.primary);
    }
  }
#endif
#if defined(WWE2K14_HAS_NR) || defined(WWE2K14_W13R)  // overlay UI: settings menu, FPS counter (w13r: decision 0008)
  // In-game FPS counter (owner 2026-10-22): F3 toggles it; WWE2K14_FPS_OVERLAY=1 shows it from the start.
  // The dialog exists only while shown: ImGuiDrawer repaints continuously while any dialog exists, which with the
  // mailbox present mode made every session present nonstop (card G36, GitHub #36).
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    fps_drawer_ = drawer;
    // Card R14: "Preparing shaders" box while the before-play pipeline prewarm runs; it closes (and deletes) itself.
    const char* pw = std::getenv("WWE2K14_NR_PREWARM");
#ifdef WWE2K14_HAS_NR
    if (pw && pw[0] == '1') prewarm_overlay_ = new PrewarmOverlay(drawer, &prewarm_overlay_);
#else
    (void)pw;
#endif
    const char* e = std::getenv("WWE2K14_FPS_OVERLAY");
    if (e && e[0] == '1') fps_overlay_ = std::make_unique<FpsOverlay>(drawer);
    if (!REXCVAR_GET(debug_ui)) {
      rex::ui::RegisterBind("bind_fps_overlay", "F3", "Toggle FPS counter", [this] {
        if (fps_overlay_) fps_overlay_.reset();
        else if (fps_drawer_) fps_overlay_ = std::make_unique<FpsOverlay>(fps_drawer_);
      });
      fps_bind_registered_ = true;
      // G1: in-game settings menu (settings_menu.h). F1 is not a keyboard-as-controller key.
      rex::ui::RegisterBind("bind_settings_menu", "F1", "Toggle the in-game settings menu", [this] {
        if (settings_menu_) CloseSettingsMenu();
        else OpenSettingsMenu();
      });
      menu_bind_registered_ = true;
    }
    // Back+Start (input wrapper, polling thread) -> UI thread. Set before the game starts polling.
    // Review fix 5: assigned once here and never reassigned; toggle_enabled gates the calls and is cleared at
    // shutdown. The posted function runs only on the UI thread while its loop runs (dropped after it ends).
    rex::ui::WindowedAppContext* ui = &app_context();
    wwe2k14::overlay_input::Get().on_toggle = [this, ui] {
      ui->CallInUIThread([this] {
        if (settings_menu_) CloseSettingsMenu();
        else OpenSettingsMenu();
      });
    };
    wwe2k14::overlay_input::Get().toggle_enabled.store(true, std::memory_order_release);
    if (const int period = wwe2k14::menu::TestAutoOpenPeriodSeconds())
      REXLOG_INFO("[ovl-test] scripted pad: Back+Start, D-pad, A, B every {} s from {} s; t0 epoch={:.3f}", period,
                  wwe2k14::menu::TestAutoOpenStartSeconds(), wwe2k14::menu::ProcessStartEpochSeconds());
  }
#endif
  // Runs before the ImGui drawer is destroyed (ReXApp::OnDestroy).
  void OnShutdown() override {
    // Card LOG1: last timeline line and write first; Stop() wakes the writer at once (the SDK hard-kills ~3 s after
    // the window closes).
    wwe2k14::timeline::Note("event", "EXIT");
    wwe2k14::timeline::Stop();
#if defined(WWE2K14_HAS_NR) || defined(WWE2K14_W13R)  // overlay UI: settings menu, FPS counter (w13r: decision 0008)
#ifdef WWE2K14_HAS_NR
    nr::shared::RuntimeShaderShutdown();  // card RT1: the detached translator must not log after the logger is gone
#endif
    wwe2k14::KeepDisplayAwake(false);
    if (fullscreen_bind_registered_) rex::ui::UnregisterBind("bind_toggle_fullscreen");
    fullscreen_bind_registered_ = false;
    if (fps_bind_registered_) rex::ui::UnregisterBind("bind_fps_overlay");
    fps_bind_registered_ = false;
    wwe2k14::overlay_input::Get().toggle_enabled.store(false, std::memory_order_release);  // no more UI posts
    if (menu_bind_registered_) rex::ui::UnregisterBind("bind_settings_menu");
    menu_bind_registered_ = false;
    CloseSettingsMenu();
    fps_overlay_.reset();
#ifdef WWE2K14_HAS_NR
    delete prewarm_overlay_;  // still open at quit (prewarm stopped early); clears prewarm_overlay_ in OnClose
    prewarm_overlay_ = nullptr;
#endif
    fps_drawer_ = nullptr;
#endif
  }

 private:
  bool fullscreen_bind_registered_ = false;
  // Card LOG1: the SDK hard-exits 3 s after the window closes and OnShutdown may never run, so the timeline gets its
  // "window closed" line and a synchronous write here (UI thread; a few KB).
  struct TimelineCloseListener : rex::ui::WindowListener {
    void OnClosing(rex::ui::UIEvent&) override {
      wwe2k14::timeline::Note("event", "window closed");
      wwe2k14::timeline::Flush();
    }
  } timeline_close_listener_;
#if defined(WWE2K14_HAS_NR) || defined(WWE2K14_W13R)  // overlay UI: settings menu, FPS counter (w13r: decision 0008)
  // Game frames per second (Swap calls, every 0.5 s) and the longest frame in that interval, top-left.
  class FpsOverlay : public rex::ui::ImGuiDialog {
   public:
    explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

   protected:
    void OnDraw(ImGuiIO&) override {
      const auto now = std::chrono::steady_clock::now();
      const double dt = std::chrono::duration<double>(now - last_).count();
      if (dt >= 0.5) {
        uint64_t swaps = 0, max_gap = 0;
        wwe2k14::d3d::GameFrameStats(&swaps, &max_gap);
        fps_ = last_swaps_ ? double(swaps - last_swaps_) / dt : 0.0;
        worst_ms_ = double(max_gap) / 1e6;
        last_swaps_ = swaps;
        last_ = now;
      }
      ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.45f);
      constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
      if (ImGui::Begin("##wwe2k14_fps", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(1.6f);
        ImGui::Text("%.0f fps   worst %.1f ms", fps_, worst_ms_);
      }
      ImGui::End();
    }

   private:
    double fps_ = 0.0, worst_ms_ = 0.0;
    uint64_t last_swaps_ = 0;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
  };
  // Card R14: centred progress box while the pipeline prewarm runs before play. Shown after 0.5 s (a warm driver cache
  // finishes sooner, so later launches show nothing); closes when the prewarm ends, or after 60 s if it never started
  // (a dialog left open would make the drawer repaint nonstop, card G36).
#ifdef WWE2K14_HAS_NR  // shipped renderer's shader prewarm box
  class PrewarmOverlay : public rex::ui::ImGuiDialog {
   public:
    PrewarmOverlay(rex::ui::ImGuiDrawer* drawer, PrewarmOverlay** owner) : rex::ui::ImGuiDialog(drawer), owner_(owner) {}

   protected:
    void OnClose() override {
      if (owner_ && *owner_ == this) *owner_ = nullptr;
    }
    void OnDraw(ImGuiIO& io) override {
      const nr::backend::PrewarmStatus st = nr::backend::GetPrewarmStatus();
      const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - created_).count();
      if ((st.total && !st.running) || (!st.running && age > 60.0) || (st.running && !st.before_play)) {
        Close();
        return;
      }
      if (!st.running || st.seconds < 0.5) return;
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                              ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowBgAlpha(0.8f);
      constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
      if (ImGui::Begin("##wwe2k14_prewarm", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(1.6f);
        ImGui::Text("Preparing shaders for your graphics card...");
        ImGui::Text("%zu / %zu", st.done, st.total);
        ImGui::ProgressBar(st.total ? float(st.done) / float(st.total) : 0.0f, ImVec2(420.0f, 0.0f));
        ImGui::Text("This happens once (and again after a driver update).");
      }
      ImGui::End();
    }

   private:
    PrewarmOverlay** owner_;
    std::chrono::steady_clock::time_point created_ = std::chrono::steady_clock::now();
  };
#endif
#ifdef WWE2K14_HAS_NR
  PrewarmOverlay* prewarm_overlay_ = nullptr;
#endif
  // G1 in-game settings menu. UI thread only: open/close from F1, Back+Start (via on_toggle), the menu's Close/B/Esc.
  std::vector<wwe2k14::menu::MenuItem> SettingsMenuItems() {
#ifdef WWE2K14_HAS_NR
    namespace live = wwe2k14::d3d::live;
#endif
    auto cvar_on = [](const char* name) {
      const std::string v = rex::cvar::GetFlagByName(name);
      return v == "true" || v == "1";
    };
    std::vector<wwe2k14::menu::MenuItem> items;
#ifdef WWE2K14_HAS_NR  // shipped renderer's live options (w13r equivalents: later)
    items.push_back({"Crowd", [] { return live::Get().crowd.load() != 0; }, [](bool on) { live::SetCrowdEnabled(on); },
                     "crowd", [] { return std::to_string(live::Get().crowd.load()); }});  // 2 (Half) stays 2
    items.push_back({"Shadows", [] { return live::Get().shadows.load(); }, [](bool on) { live::SetShadows(on); },
                     "shadows"});
#endif
    items.push_back({"FPS counter (F3)", [this] { return fps_overlay_ != nullptr; },
                     [this](bool on) {
                       if (!on) fps_overlay_.reset();
                       else if (!fps_overlay_ && fps_drawer_) fps_overlay_ = std::make_unique<FpsOverlay>(fps_drawer_);
                     },
                     nullptr});
    items.push_back({"Lower CPU use", [] { return wwe2k14::live_game::LowPowerOn(); },
                     [](bool on) { wwe2k14::live_game::SetLowPower(on); }, "lower_cpu"});
#ifdef WWE2K14_HAS_NR  // SDK presenter options: no effect with the w13r renderer (it presents itself)
    // SDK cvars read on every use: vsync worker loop (graphics_system.cpp) and every paint (presenter.cpp).
    items.push_back({"Sync frames to monitor", [cvar_on] { return cvar_on("vsync_host_display"); },
                     [](bool on) { rex::cvar::SetFlagByName("vsync_host_display", on ? "true" : "false"); },
                     "sync_frames_to_monitor"});
    items.push_back({"Stretch to fill wide screens", [cvar_on] { return !cvar_on("present_letterbox"); },
                     [](bool on) { rex::cvar::SetFlagByName("present_letterbox", on ? "false" : "true"); },
                     "stretch_to_fill_wide_screens"});
#endif
    return items;
  }
  void OpenSettingsMenu() {
    if (settings_menu_ || !fps_drawer_) return;
    wwe2k14::menu::MenuHooks hooks;
    hooks.items = SettingsMenuItems();
    hooks.changed = [this](const wwe2k14::menu::MenuItem& item, bool on) {
      const bool now = item.get();  // read back through the option's own reader
      REXLOG_INFO("[ovl] {} -> {} (reads back {})", item.label, on ? "on" : "off", now ? "on" : "off");
      if (item.ini_key) menu_dirty_[item.ini_key] = item.ini_value ? item.ini_value() : (now ? "1" : "0");
    };
    hooks.request_close = [this] { app_context().CallInUIThreadDeferred([this] { CloseSettingsMenu(); }); };
    ImGuiIO& io = fps_drawer_->GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    // No key repeat while the menu is open (ovl1-dx1, Arc D3D12): the pad state reaches the menu only when the game
    // polls it, so a game hitch of 100-230 ms made a 150 ms D-pad press look held past ImGui's 0.275 s repeat delay
    // and the cursor jumped two rows. Six rows need no repeat; edges stay exact.
    saved_key_repeat_delay_ = io.KeyRepeatDelay;
    io.KeyRepeatDelay = 1.0e6f;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.WantCaptureMouse = true;  // keyboard/mouse game input stops now, not one frame later
    wwe2k14::overlay_input::Get().menu_open.store(true);
    settings_menu_ = new wwe2k14::menu::SettingsMenu(fps_drawer_, std::move(hooks));
    if (auto* w = window()) w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    REXLOG_INFO("[ovl] settings menu open");
  }
  void CloseSettingsMenu() {
    if (!settings_menu_) return;
    const uint64_t draws = settings_menu_->draws();
    delete settings_menu_;  // as its own type; removes itself from the drawer
    settings_menu_ = nullptr;
    wwe2k14::overlay_input::Get().menu_open.store(false);
    wwe2k14::overlay_input::Get().close_count.fetch_add(1);  // keystroke gate drains what was typed inside
    if (fps_drawer_) {
      ImGuiIO& io = fps_drawer_->GetIO();
      io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
      io.KeyRepeatDelay = saved_key_repeat_delay_;
      io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
      io.WantCaptureMouse = false;  // no ImGui frame may follow to recompute it
    }
    const char* show = std::getenv("WWE2K14_SHOW_CURSOR");
    if (auto* w = window(); w && !(show && show[0] == '1'))
      w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
#ifdef WWE2K14_HAS_NR
    namespace live = wwe2k14::d3d::live;
#endif
    auto& in = wwe2k14::overlay_input::Get();
    REXLOG_INFO("[ovl] settings menu closed after {} menu frames; pad: toggles={} polls_blocked={} presses_blocked={} "
                "leaked_while_open={} combo_hidden_polls={} keystrokes_dropped={} keystroke_polls={} keystrokes_delivered={} "
                "closes_with_buttons_held={} leaked_after_close={} start_after_close={}; draws: crowd_dropped={} "
                "shadow_dropped={}",
                draws, in.toggles.load(), in.polls_blocked.load(), in.presses_blocked.load(),
                in.leaked_while_open.load(), in.combo_hidden_polls.load(), in.keystrokes_dropped.load(),
                in.keystroke_polls.load(), in.keystrokes_delivered.load(), in.closes_with_buttons_held.load(),
                in.leaked_after_close.load(), in.start_after_close.load(),
#ifdef WWE2K14_HAS_NR
                live::Get().crowd_dropped.load(), live::Get().shadow_dropped.load());
#else
                0, 0);
#endif
    SaveMenuSettings();
  }
  // Launcher settings file: next to the game, or one folder up when the game sits in bin/ (launcher paths.cpp).
  void SaveMenuSettings() {
    if (menu_dirty_.empty()) return;
    wwe2k14::launcher_ini::KeyValues kv(menu_dirty_.begin(), menu_dirty_.end());
    menu_dirty_.clear();
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    std::filesystem::path file = exe_dir / "wwe2k14-enhanced.ini";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) && exe_dir.filename() == "bin") file = exe_dir.parent_path() / "wwe2k14-enhanced.ini";
    const auto r = wwe2k14::launcher_ini::UpdateFile(file, kv);
    std::string keys;
    for (const auto& [k, v] : kv) keys += k + "=" + v + " ";
    REXLOG_INFO("[ovl] save {}: {}({}){}", rex::path_to_utf8(file), keys, r.message, r.written ? "" : ", not written");
  }
  std::map<std::string, std::string> menu_dirty_;  // launcher key -> value changed in the menu since the last save
  wwe2k14::menu::SettingsMenu* settings_menu_ = nullptr;
  float saved_key_repeat_delay_ = 0.275f;
  bool menu_bind_registered_ = false;
  rex::ui::ImGuiDrawer* fps_drawer_ = nullptr;
  bool fps_bind_registered_ = false;
  std::unique_ptr<FpsOverlay> fps_overlay_;
#endif
#ifdef WWE2K14_HAS_NR
  // From the window close on, teardown races of the D3D renderer threads are not crashes (capture.h NoteWindowClosing).
  struct CloseListener : rex::ui::WindowListener {
    void OnClosing(rex::ui::UIEvent&) override { wwe2k14::d3d::NoteWindowClosing(); }
  } close_listener_;
#endif
#ifdef WWE2K14_HAS_NR
  // Native renderer (src/native/nr). The observer registers only when WWE2K14_NR=shadow|replace; otherwise the SDK's
  // emulated renderer remains active. The SDK cvar nr_shared_device (read at device creation) is selected by the
  // separate direct-device integration switch WWE2K14_NR_DIRECT_PRESENT=1.
  void ConfigureNativeRenderer() {
    const char* mode = std::getenv("WWE2K14_NR");
    if (!mode || (std::strcmp(mode, "shadow") != 0 && std::strcmp(mode, "replace") != 0)) return;
    nr::backend::SetPipelineCacheFile(rex::path_to_utf8(user_data_root_ / "cache" / "nr" / "pipelines-vulkan.bin"));
    nr::shared::SetRuntimeShaderCacheDir(user_data_root_ / "cache" / "nr" / "runtime-shaders");  // card RT1
    auto env_is_one = [](const char* name) {
      const char* v = std::getenv(name);
      return v && std::strcmp(v, "1") == 0;
    };
    const char* backend = std::getenv("WWE2K14_NR_BACKEND");
    if (backend && std::strcmp(backend, "d3d12") == 0) {
      if (env_is_one("WWE2K14_NR_D3D12_VSYNC_FIFO")) {
        rex::cvar::SetFlagByName("nr_d3d12_vsync_fifo", "true");
      }
      const char* latency = std::getenv("WWE2K14_NR_D3D12_LATENCY");
      if (latency && (std::strcmp(latency, "0") == 0 || std::strcmp(latency, "1") == 0 ||
                      std::strcmp(latency, "2") == 0 || std::strcmp(latency, "3") == 0)) {
        rex::cvar::SetFlagByName("nr_d3d12_latency", latency);
      }
      const char* scan_buffers = std::getenv("WWE2K14_NR_D3D12_SCAN_BUFFERS");
      if (scan_buffers && (std::strcmp(scan_buffers, "2") == 0 ||
                           std::strcmp(scan_buffers, "3") == 0)) {
        rex::cvar::SetFlagByName("nr_d3d12_scan_buffers", scan_buffers);
      }
    }
    if (env_is_one("WWE2K14_NR_DIRECT_PRESENT")) {
      rex::cvar::SetFlagByName("nr_shared_device", "true");
      // Card C3 (proposal SDK): the D3D12 counterpart; inert unless the SDK runs its D3D12 backend.
      const char* be = std::getenv("WWE2K14_NR_BACKEND");
      if (be && std::strcmp(be, "d3d12") == 0) rex::cvar::SetFlagByName("nr_d3d12_shared_device", "true");
    }
    REXLOG_INFO("[wwe2k14] native renderer WWE2K14_NR={} nr_shared_device={}", mode,
                rex::cvar::GetFlagByName("nr_shared_device"));
  }
#endif
  std::filesystem::path user_data_root_;
};

#ifdef WWE2K14_HAS_NR
// Card G56: startup boundary around the native-selected app. The entry point only ever
// sees this wrapper: it forwards the parsed positional arguments to the real app, turns
// the dedicated startup refusal into one blocking message and a failed initialization,
// and owns the real app for its whole lifetime. Non-native runs never use it.
class NativeStartupBoundaryApp : public rex::ui::WindowedApp {
 public:
  NativeStartupBoundaryApp(rex::ui::WindowedAppContext& app_context,
                           std::unique_ptr<rex::ui::WindowedApp> inner)
      : rex::ui::WindowedApp(app_context, inner->GetName(), inner->GetPositionalOptionsUsage()),
        inner_(std::move(inner)) {
    for (const std::string& option : inner_->GetPositionalOptions()) AddPositionalOption(option);
  }

  bool OnInitialize() override {
    std::map<std::string, std::string> arguments = nr::startup::CopyPositionalArguments(
        GetPositionalOptions(), [this](const std::string& name) { return GetArgument(name); });
    return nr::startup::InitializeInnerApp(
        *inner_, std::move(arguments), [](std::string_view message) {
          const std::string text(message);
          REXLOG_ERROR("[wwe2k14] {}", text);
#if !defined(REX_UWP)  // Xbox: no SDL message box; the log line above carries the reason
          SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "WWE 2K14", text.c_str(), nullptr);
#endif
        });
  }

 protected:
  void OnDestroy() override { inner_->InvokeOnDestroy(); }

 private:
  std::unique_ptr<rex::ui::WindowedApp> inner_;
};
#endif  // WWE2K14_HAS_NR

// Defined after NativeStartupBoundaryApp: wrap only the native-selected path, so the
// non-native launch keeps the plain app and its original behaviour.
inline std::unique_ptr<rex::ui::WindowedApp> Wwe2k14App::Create(rex::ui::WindowedAppContext& ctx) {
  std::unique_ptr<rex::ui::WindowedApp> inner(new Wwe2k14App(ctx, "wwe2k14", PPCImageConfig));
#ifdef WWE2K14_HAS_NR
  if (nr::startup::NativeModeSelected(std::getenv("WWE2K14_NR"))) {
    return std::make_unique<NativeStartupBoundaryApp>(ctx, std::move(inner));
  }
#endif
  return inner;
}
