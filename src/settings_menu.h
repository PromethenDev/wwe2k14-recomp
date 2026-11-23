// G1 (owner 2026-11-07): in-game settings menu. F1, or Back + Start on a pad (owner's choice), opens and closes it.
//
// Drawing: an ImGui dialog on the SDK's ImGui drawer, the same layer as the F3 FPS counter. The SDK presenter paints UI
// drawers after the guest output on both of its backends (Vulkan and D3D12), and the native renderer's direct present
// hands its picture to that presenter, so the menu needs no renderer code.
// Input: the game-side input wrapper (overlay_input.cpp) gives the game an untouched pad and no keystrokes while the
// menu is open, and publishes the real pad, which this menu turns into ImGui gamepad navigation (D-pad / left stick
// move, A toggles, B closes). Keyboard and mouse: the menu also holds ImGui's WantCaptureMouse, which the SDK's
// keyboard/mouse driver honours (keys ignored, mouse released); the app clears it on close, because ImGui stops running
// frames once no dialog is left.
// Options apply from the next frame; the ones the launcher also has are saved to its settings file on close
// (launcher_ini_update.h).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>

#include "overlay_input.h"
#include "overlay_pad_core.h"

namespace wwe2k14::menu {

struct MenuItem {
  const char* label;
  std::function<bool()> get;
  std::function<void(bool)> set;
  const char* ini_key;  // launcher settings key, nullptr = not saved
  std::function<std::string()> ini_value = nullptr;  // value to save; default "1"/"0" from get()
};

struct MenuHooks {
  std::vector<MenuItem> items;
  std::function<void(const MenuItem&, bool)> changed;  // after set(): log + remember for saving
  std::function<void()> request_close;  // deferred: the dialog must not delete itself inside its own draw
};

class SettingsMenu : public rex::ui::ImGuiDialog {
 public:
  SettingsMenu(rex::ui::ImGuiDrawer* drawer, MenuHooks hooks) : rex::ui::ImGuiDialog(drawer), hooks_(std::move(hooks)) {}
  uint64_t draws() const { return draws_; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    ++draws_;
    // Keep the SDK's keyboard/mouse gate closed: for this frame (already past NewFrame) and the next one.
    io.WantCaptureMouse = true;
    ImGui::SetNextFrameWantCaptureMouse(true);
    FeedPad(io);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("Settings##wwe2k14_menu", nullptr, kFlags)) {
      ImGui::SetWindowFontScale(1.5f);
      ImGui::TextUnformatted("Changes apply at once and are kept for next time.");
      ImGui::TextUnformatted("The game ignores your controls while this menu is open.");
      ImGui::Separator();
      for (size_t i = 0; i < hooks_.items.size(); ++i) {
        const MenuItem& item = hooks_.items[i];
        // Every opening starts on the first row: ImGui would otherwise restore the row the last opening ended on
        // (run ovl1-r3: from the second opening the scripted presses landed one row off and hit Close).
        if (i == 0 && draws_ == 1) ImGui::SetKeyboardFocusHere();
        bool v = item.get();
        if (ImGui::Checkbox(item.label, &v)) {
          item.set(v);
          if (hooks_.changed) hooks_.changed(item, v);
        }
      }
      ImGui::Separator();
      ImGui::TextDisabled("Picture size, anti-aliasing, frame rate and the rest: in the launcher.");
      ImGui::TextDisabled("Pad: D-pad to move, A to switch, B or Back+Start to close.");
      if (ImGui::Button("Close (F1)") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) Close_();
    }
    ImGui::End();
  }

 private:
  void Close_() {
    if (hooks_.request_close) hooks_.request_close();
  }
  // Real pad (all four) -> ImGui gamepad keys. Events added here are read at the next ImGui frame.
  void FeedPad(ImGuiIO& io) {
    namespace op = wwe2k14::overlay_pad;
    auto& sh = wwe2k14::overlay_input::Get();
    uint16_t b = 0;
    int ly = 0;
    for (int u = 0; u < 4; ++u) {
      b |= sh.raw_buttons[u].load(std::memory_order_relaxed);
      const int v = sh.raw_ly[u].load(std::memory_order_relaxed);
      if (std::abs(v) > std::abs(ly)) ly = v;
    }
    // Back+Start is the toggle, not navigation.
    if ((b & op::kCombo) == op::kCombo) b &= uint16_t(~op::kCombo);
    if (first_) {  // a button already down when the menu opened is not a press
      prev_ = b;
      first_ = false;
    }
    struct Map { uint16_t bit; ImGuiKey key; };
    static constexpr Map kMap[] = {{op::kDpadUp, ImGuiKey_GamepadDpadUp},     {op::kDpadDown, ImGuiKey_GamepadDpadDown},
                                   {op::kDpadLeft, ImGuiKey_GamepadDpadLeft}, {op::kDpadRight, ImGuiKey_GamepadDpadRight},
                                   {op::kA, ImGuiKey_GamepadFaceDown}};
    for (const Map& m : kMap) {
      const bool now = (b & m.bit) != 0, before = (prev_ & m.bit) != 0;
      if (now != before) io.AddKeyEvent(m.key, now);
    }
    constexpr int kDead = 12000;
    const float up = ly > kDead ? float(ly - kDead) / float(32767 - kDead) : 0.0f;
    const float down = ly < -kDead ? float(-ly - kDead) / float(32768 - kDead) : 0.0f;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, up > 0.0f, up);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, down > 0.0f, down);
    if ((b & op::kB) && !(prev_ & op::kB)) Close_();
    prev_ = b;
  }

  MenuHooks hooks_;
  uint64_t draws_ = 0;
  uint16_t prev_ = 0;
  bool first_ = true;
};

}  // namespace wwe2k14::menu
