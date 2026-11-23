// G1 in-game settings menu: input wrapper (see overlay_input.h, overlay_pad_core.h).
//
// The SDK's default input system (input_system.cpp CreateDefaultInputSystem) is rebuilt here with the same drivers in
// the same order (XInput if input_backend=xinput, SDL if input_backend=sdl, keyboard/mouse, NOP), but inside one gate
// driver. The SDK later gives the outer system its "active" callback (ImGui WantCaptureMouse, rex_app.cpp); that now
// reaches only the gate, so the SDL pad keeps being read while the menu is open and the gate decides what the game
// sees. The keyboard/mouse driver gets the gate's state as its own callback, so it releases the mouse and ignores keys
// exactly as before whenever the game input is off.
#include "overlay_input.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/nop/nop_input_driver.h>
#if defined(REX_UWP)
namespace rex::input::uwp {  // Xbox UWP SDK (src/input/uwp/xinputuap_input_driver.cpp)
std::unique_ptr<InputDriver> CreateXinputUapInputDriver(rex::ui::Window* window, size_t window_z_order);
}  // namespace rex::input::uwp
#else
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#if REX_PLATFORM_WIN32
#include <rex/input/xinput/xinput_input_driver.h>
#endif
#endif
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

#include "overlay_pad_core.h"
#include "settings_menu_test.h"

namespace wwe2k14::overlay_input {

Shared& Get() {
  static Shared* shared = new Shared();  // process lifetime: guest threads may poll during teardown
  return *shared;
}

namespace {
using namespace rex;
using namespace rex::input;

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

class GateDriver final : public InputDriver {
 public:
  GateDriver(std::unique_ptr<InputSystem> inner, bool xinput_backend)
      : InputDriver(nullptr, 0), inner_(std::move(inner)), xinput_backend_(xinput_backend) {}

  // Game input on: the SDK's callback (no ImGui window wants the mouse) and the menu closed.
  bool GameInputOn() const { return is_active() && !Get().menu_open.load(std::memory_order_relaxed); }
  // Review fix 3: the SDK's XInput driver never honoured the ImGui callback; only the SDL pad is blocked by it.
  bool ImguiBlocksPad() const { return !xinput_backend_ && !is_active(); }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void OnWindowAvailable(rex::ui::Window* window) override { inner_->AttachWindow(window); }

  X_RESULT GetCapabilities(uint32_t user, uint32_t flags, X_INPUT_CAPABILITIES* caps) override {
    SyncTextTarget();
    X_RESULT r = inner_->GetCapabilities(user, flags, caps);
    if (r != X_ERROR_SUCCESS && user == 0 && Scripted() && caps) {  // test script on a machine without a pad
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;
      caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xFFFF;
      r = X_ERROR_SUCCESS;
    }
    return r;
  }

  X_RESULT GetState(uint32_t user, X_INPUT_STATE* out) override {
    SyncTextTarget();
    X_INPUT_STATE st{};
    X_RESULT r = inner_->GetState(user, &st);
    const uint16_t scripted =
        user == 0 ? menu::TestScriptButtons(menu::SecondsSinceProcessStart(),
                                            Get().menu_open.load(std::memory_order_relaxed))
                  : 0;
    if (r != X_ERROR_SUCCESS && user == 0 && Scripted()) {
      st = {};
      r = X_ERROR_SUCCESS;
    }
    if (user >= 4) return r;
    Shared& sh = Get();
    if (r != X_ERROR_SUCCESS) {  // review fix 6: a pad that went away must not keep driving the menu
      sh.raw_buttons[user].store(0, std::memory_order_relaxed);
      sh.raw_ly[user].store(0, std::memory_order_relaxed);
      return r;
    }
    const uint16_t raw = uint16_t(uint16_t(st.gamepad.buttons) | scripted);
    std::lock_guard lock(mutex_[user]);
    const bool open = sh.menu_open.load(std::memory_order_relaxed);
    const overlay_pad::PadGate::Out o = gate_[user].Step(NowUs(), raw, open);
    sh.raw_buttons[user].store(raw, std::memory_order_relaxed);
    sh.raw_ly[user].store(int16_t(st.gamepad.thumb_ly), std::memory_order_relaxed);
    if (o.toggle) {
      const uint64_t n = sh.toggles.fetch_add(1, std::memory_order_relaxed) + 1;
      sh.last_toggle_us.store(NowUs(), std::memory_order_relaxed);
      REXLOG_INFO("[ovl-pad] Back+Start on pad {} (menu was {}), toggle {}{}", user, open ? "open" : "closed", n,
                  scripted ? " (test script)" : "");
      if (sh.toggle_enabled.load(std::memory_order_acquire) && sh.on_toggle) sh.on_toggle();
    }
    X_INPUT_STATE g = st;
    const bool block = o.block_all || ImguiBlocksPad();
    if (block) {
      std::memset(&g.gamepad, 0, sizeof(g.gamepad));
      sh.polls_blocked.fetch_add(1, std::memory_order_relaxed);
      if (raw) sh.presses_blocked.fetch_add(1, std::memory_order_relaxed);
    } else {
      g.gamepad.buttons = o.guest_buttons;
      if ((raw & overlay_pad::kCombo) != (o.guest_buttons & overlay_pad::kCombo))
        sh.combo_hidden_polls.fetch_add(1, std::memory_order_relaxed);
    }
    if (open && uint16_t(g.gamepad.buttons) != 0) sh.leaked_while_open.fetch_add(1, std::memory_order_relaxed);
    // Independent check of review fix 1: buttons down at the first poll after a close must not reach the game
    // until released; and no Start within 1 s of a close (an Esc close would show as Start).
    {
      const uint64_t closes = sh.close_count.load(std::memory_order_relaxed);
      if (closes != seen_close_[user]) {
        seen_close_[user] = closes;
        held_at_close_[user] = raw;
        close_seen_us_[user] = NowUs();
        if (raw) sh.closes_with_buttons_held.fetch_add(1, std::memory_order_relaxed);
      }
      held_at_close_[user] &= raw;
      const uint16_t gb = g.gamepad.buttons;
      if (gb & held_at_close_[user]) sh.leaked_after_close.fetch_add(1, std::memory_order_relaxed);
      if ((gb & overlay_pad::kStart) && close_seen_us_[user] && NowUs() - close_seen_us_[user] < 1000000)
        sh.start_after_close.fetch_add(1, std::memory_order_relaxed);
    }
    // A changed picture of the pad needs a new packet number, or the game may skip it.
    if (std::memcmp(&g.gamepad, &last_[user], sizeof(g.gamepad)) != 0 &&
        std::memcmp(&st.gamepad, &last_raw_[user], sizeof(st.gamepad)) == 0)
      ++epoch_[user];
    last_[user] = g.gamepad;
    last_raw_[user] = st.gamepad;
    g.packet_number = uint32_t(st.packet_number) + epoch_[user];
    if (out) *out = g;
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetState(uint32_t user, X_INPUT_VIBRATION* vibration) override {
    return inner_->SetState(user, vibration);
  }

  X_RESULT GetKeystroke(uint32_t user, uint32_t flags, X_INPUT_KEYSTROKE* out) override {
    SyncTextTarget();
    Shared& sh = Get();
    const bool closed_to_game = sh.menu_open.load(std::memory_order_relaxed) || ImguiBlocksPad();
    bool any_connected = false;
    auto fetch = [&](overlay_pad::Keystroke* k) {
      X_INPUT_KEYSTROKE ks{};
      const X_RESULT r = inner_->GetKeystroke(user, flags, &ks);
      if (r != X_ERROR_DEVICE_NOT_CONNECTED) any_connected = true;
      if (r != X_ERROR_SUCCESS) return false;
      k->vk = ks.virtual_key;
      k->unicode = ks.unicode;
      k->flags = ks.flags;
      k->user = ks.user_index;
      k->hid = ks.hid_code;
      return true;
    };
    sh.keystroke_polls.fetch_add(1, std::memory_order_relaxed);
    overlay_pad::Keystroke k;
    bool got;
    uint64_t dropped;
    {
      std::lock_guard lock(ks_mutex_);
      got = ks_gate_.Next(NowUs(), closed_to_game, sh.close_count.load(std::memory_order_relaxed),
                          sh.last_toggle_us.load(std::memory_order_relaxed), fetch, &k);
      dropped = ks_gate_.dropped();
    }
    sh.keystrokes_dropped.store(dropped, std::memory_order_relaxed);
    if (!got) return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
    sh.keystrokes_delivered.fetch_add(1, std::memory_order_relaxed);
    if (out) {
      out->virtual_key = k.vk;
      out->unicode = k.unicode;
      out->flags = k.flags;
      out->user_index = k.user;
      out->hid_code = k.hid;
    }
    return X_ERROR_SUCCESS;
  }

 private:
  static bool Scripted() { return menu::TestAutoOpenPeriodSeconds() > 0; }
  void SyncTextTarget() {
    TextEntryTarget* t = text_entry_target();
    TextEntryTarget* seen = synced_target_.load(std::memory_order_acquire);
    if (t != seen && synced_target_.compare_exchange_strong(seen, t)) inner_->SetTextEntryTarget(t);
  }

  std::unique_ptr<InputSystem> inner_;
  std::atomic<TextEntryTarget*> synced_target_{nullptr};
  std::mutex mutex_[4];
  overlay_pad::PadGate gate_[4];
  bool xinput_backend_ = false;
  std::mutex ks_mutex_;
  overlay_pad::KeystrokeGate ks_gate_;
  X_INPUT_GAMEPAD last_[4]{};
  X_INPUT_GAMEPAD last_raw_[4]{};
  uint32_t epoch_[4]{};
  uint64_t seen_close_[4]{};
  uint16_t held_at_close_[4]{};
  uint64_t close_seen_us_[4]{};
};
}  // namespace

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
  if (tool_mode) return rex::input::CreateDefaultInputSystem(true);
  auto inner = std::make_unique<InputSystem>(nullptr);
  const std::string backend = rex::cvar::GetFlagByName("input_backend");
#if defined(REX_UWP)
  // Xbox: the app-family XInput pad driver (as the UWP SDK's CreateDefaultInputSystem), no SDL, no keyboard/mouse.
  auto uap_driver = rex::input::uwp::CreateXinputUapInputDriver(nullptr, 0);
  if (uap_driver && uap_driver->Setup() == X_STATUS_SUCCESS) inner->AddDriver(std::move(uap_driver));
  inner->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, 1));
  auto gate = std::make_unique<GateDriver>(std::move(inner), true);
  auto outer = std::make_unique<InputSystem>(nullptr);
  outer->AddDriver(std::move(gate));
  REXLOG_INFO("[ovl-pad] input wrapper on (Xbox XInputUAP): Back+Start toggles the settings menu");
  return outer;
#else
#if REX_PLATFORM_WIN32
  if (backend == "xinput") {
    auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
    if (xinput_driver->Setup() == X_STATUS_SUCCESS) inner->AddDriver(std::move(xinput_driver));
  }
#endif
  if (backend == "sdl") {
    auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
    if (sdl_driver->Setup() == X_STATUS_SUCCESS) inner->AddDriver(std::move(sdl_driver));
  }
  auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
  mnk::MnkInputDriver* mnk = mnk_driver.get();
  const bool mnk_ok = mnk_driver->Setup() == X_STATUS_SUCCESS;
  if (mnk_ok) inner->AddDriver(std::move(mnk_driver));
  inner->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, 1));
  auto gate = std::make_unique<GateDriver>(std::move(inner), backend == "xinput");
  GateDriver* gate_ptr = gate.get();
  if (mnk_ok) mnk->set_is_active_callback([gate_ptr] { return gate_ptr->GameInputOn(); });
  auto outer = std::make_unique<InputSystem>(nullptr);
  outer->AddDriver(std::move(gate));
  REXLOG_INFO("[ovl-pad] input wrapper on (backend {}): Back+Start toggles the settings menu", backend);
  return outer;
#endif
}

}  // namespace wwe2k14::overlay_input
