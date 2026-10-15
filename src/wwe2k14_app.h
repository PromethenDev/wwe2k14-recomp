
// wwe2k14 - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>

#include <rex/cvar.h>
#include <rex/diagnostics/freeze_logger.h>
#include <rex/logging.h>
#include <rex/graphics/flags.h>
#include <rex/rex_app.h>
#include <rex/ui/window.h>

#include "shader_cache_seed.h"

namespace wwe2k14 {
// Installs the guest-state dump used by the always-on freeze report
// (defined in src/freeze_guest_state.cpp).
void RegisterFreezeGuestState();
}  // namespace wwe2k14

class Wwe2k14App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Wwe2k14App>(new Wwe2k14App(ctx, "wwe2k14",
        PPCImageConfig));
  }

  // Always-on freeze logger: a 1 Hz watchdog plus a lock-free kernel-event ring
  // that writes a report into the player's logs folder if the game stops
  // presenting. Default on; WWE2K14_FREEZE_LOG=0 disables it.
  void OnPostSetup() override {
    rex::diagnostics::freeze_logger::Start();
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
  }

  // Float24 depth: the game saves the scene depth (D24FS8) to a texture and restores it with a
  // per-tile quad before drawing characters (reversed depth, GEQUAL). On the default host
  // render-target path the restored depth comes back too near, so wrestlers far from the camera
  // (start of entrances) fail the depth test and vanish. Converting float24 in the pixel shader
  // fixes it. WWE2K14_DEPTH_FLOAT24_PS=0 restores the old path.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    (void)config;
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
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    user_data_root_ = paths.user_data_root;
  }

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}

 private:
  std::filesystem::path user_data_root_;
};
