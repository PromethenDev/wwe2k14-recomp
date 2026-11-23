// w13r_bridge.h - what 2K14's app (src/wwe2k14_app.h) needs from the w13r renderer (decision 0008).
// Plain declarations only, so the app does not include the renderer's SDK-0.10-style headers.
#pragma once
#include <memory>
namespace rex::system { class IGraphicsSystem; }
namespace rex::ui { class Window; class ImmediateDrawer; class ImGuiDrawer; class WindowedAppContext; }
namespace w13r {
// The guest GPU for a w13r build: runs the game's command stream (fences, interrupts, vblank) and draws nothing.
std::unique_ptr<rex::system::IGraphicsSystem> CreateNullGpu();
// Brings the host renderer up on the game window. Shows a message and exits if no graphics API works.
void Start(rex::ui::Window* window);
// The in-game overlay (settings menu, FPS counter) in the SDK's detached overlay mode: a drawer that draws through the
// renderer (ReXApp::OnCreateImmediateDrawer), and the hook that draws the open dialogs into each presented frame.
std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer();
void InstallOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& app_context);
}  // namespace w13r
