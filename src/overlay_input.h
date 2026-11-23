// G1: game-side input wrapper for the in-game settings menu (overlay_input.cpp). Replaces the SDK's default input
// system with the same drivers behind one gate driver: Back + Start opens/closes the menu (overlay_pad_core.h), the
// menu reads the real pad for navigation, and the game gets an untouched pad and no keystrokes while it is open.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include <rex/system/interfaces/input.h>

namespace wwe2k14::overlay_input {

struct Shared {
  std::atomic<bool> menu_open{false};
  std::array<std::atomic<uint16_t>, 4> raw_buttons{};  // real buttons per pad, latest poll (menu navigation)
  std::array<std::atomic<int16_t>, 4> raw_ly{};        // left stick Y per pad
  std::atomic<uint64_t> toggles{0};
  std::atomic<uint64_t> polls_blocked{0};        // game polls answered with an untouched pad
  std::atomic<uint64_t> presses_blocked{0};      // ... of which the real pad had buttons down
  std::atomic<uint64_t> leaked_while_open{0};    // game saw a button while the menu was open (must stay 0)
  std::atomic<uint64_t> combo_hidden_polls{0};   // polls where Back/Start were held back
  std::atomic<uint64_t> keystrokes_dropped{0};
  std::atomic<uint64_t> keystroke_polls{0};          // game XamInputGetKeystrokeEx calls reaching the wrapper
  std::atomic<uint64_t> keystrokes_delivered{0};
  std::atomic<uint64_t> closes_with_buttons_held{0};
  std::atomic<uint64_t> leaked_after_close{0};       // game saw a button held across a close (must stay 0)
  std::atomic<uint64_t> start_after_close{0};        // game saw Start within 1 s of a close (must stay 0 here)
  std::atomic<uint64_t> close_count{0};    // menu closes so far (the keystroke gate drains on a change)
  std::atomic<uint64_t> last_toggle_us{0};  // steady-clock time of the latest Back+Start toggle
  // Set once before the game starts polling and never reassigned (no race on the object); called on the polling
  // thread only while toggle_enabled, which the app clears at shutdown (review fix 5).
  std::function<void()> on_toggle;
  std::atomic<bool> toggle_enabled{false};
};
Shared& Get();

// For RuntimeConfig::input_factory. tool_mode: the SDK default (no wrapper).
std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);

}  // namespace wwe2k14::overlay_input
