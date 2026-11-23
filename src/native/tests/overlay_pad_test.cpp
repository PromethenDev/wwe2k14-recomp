// G1: host test for the Back+Start combo gate (overlay_pad_core.h) and the test script timing (settings_menu_test.cpp).
// Build: g++ -std=c++20 -I src src/native/tests/overlay_pad_test.cpp src/settings_menu_test.cpp -o /tmp/overlay_pad_test
#include <cstdio>
#include <deque>

#include "overlay_pad_core.h"
#include "settings_menu_test.h"

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
  std::printf("check=%s result=%s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) ++failures;
}
using namespace wwe2k14::overlay_pad;
constexpr uint64_t ms = 1000;
}  // namespace

int main() {
  {  // both in the same poll: toggle, neither reaches the game, held pair stays hidden until both are up
    PadGate g;
    auto o = g.Step(0, kStart | kBack, false);
    Check(o.toggle && o.block_all && o.guest_buttons == 0, "same_poll_combo_toggles_and_hides");
    o = g.Step(16 * ms, kStart | kBack | kA, false);
    Check(!o.toggle && o.guest_buttons == kA, "held_combo_no_repeat_toggle_other_buttons_pass");
    o = g.Step(32 * ms, kStart, false);
    Check(o.guest_buttons == 0, "half_released_combo_still_hidden");
    o = g.Step(48 * ms, 0, false);
    o = g.Step(64 * ms, 0, false);
    Check(o.guest_buttons == 0 && !o.toggle, "no_replay_after_combo");
  }
  {  // second button inside the window: combo, and the first never reached the game
    PadGate g;
    auto o = g.Step(0, kBack, false);
    Check(o.guest_buttons == 0, "lone_back_held_back");
    o = g.Step(50 * ms, kBack | kStart, false);
    Check(o.toggle && o.guest_buttons == 0, "second_inside_window_is_combo");
  }
  {  // lone Start held: passed on after the window, no combo afterwards
    PadGate g;
    g.Step(0, kStart, false);
    auto o = g.Step(100 * ms, kStart, false);
    Check(o.guest_buttons == 0, "start_hidden_inside_window");
    o = g.Step(130 * ms, kStart, false);
    Check(o.guest_buttons == kStart, "start_passed_after_window");
    o = g.Step(200 * ms, kStart | kBack, false);
    Check(!o.toggle && o.guest_buttons == (kStart | kBack), "late_second_press_not_a_combo");
    o = g.Step(230 * ms, kStart, false);
    Check(o.guest_buttons == kStart, "pair_half_release_no_new_hold");
  }
  {  // quick tap shorter than the window is replayed so the game still sees it
    PadGate g;
    g.Step(0, kStart, false);
    auto o = g.Step(60 * ms, 0, false);
    Check(o.guest_buttons == kStart, "short_tap_replayed");
    o = g.Step(140 * ms, 0, false);
    Check(o.guest_buttons == kStart, "replay_lasts");
    o = g.Step(170 * ms, 0, false);
    Check(o.guest_buttons == 0, "replay_ends");
  }
  {  // menu open: everything blocked; combo at any timing closes
    PadGate g;
    auto o = g.Step(0, kA | kDpadDown, true);
    Check(o.block_all && o.guest_buttons == 0, "menu_open_blocks_all");
    g.Step(20 * ms, kStart, true);
    o = g.Step(500 * ms, kStart | kBack, true);
    Check(o.toggle, "menu_open_late_combo_closes");
  }
  {  // other buttons are never delayed
    PadGate g;
    auto o = g.Step(0, kA | kDpadUp, false);
    Check(o.guest_buttons == (kA | kDpadUp) && !o.block_all, "other_buttons_immediate");
  }
  {  // review fix 1: B held across the close never reaches the game; released and pressed again it does
    PadGate g;
    g.Step(0, 0, true);
    g.Step(16 * ms, kB, true);                       // B pressed inside the menu (closes it)
    auto o = g.Step(33 * ms, kB, false);             // first poll after the close, B still held
    Check(o.guest_buttons == 0, "close_b_held_hidden");
    o = g.Step(300 * ms, kB | kA, false);
    Check(o.guest_buttons == kA, "close_b_still_hidden_other_new_press_passes");
    g.Step(320 * ms, 0, false);
    o = g.Step(340 * ms, kB, false);
    Check(o.guest_buttons == kB, "b_after_release_passes");
  }
  {  // review fix 1: Esc (keyboard Start) closes the menu: the keyboard driver reports Start only once active again
    PadGate g;
    g.Step(0, 0, true);                              // Esc down inside the menu: keyboard driver inactive, raw 0
    bool any_start = false;
    auto o = g.Step(20 * ms, kStart, false);         // menu closed, keyboard driver active, Esc still held
    any_start |= (o.guest_buttons & kStart) != 0;
    for (uint64_t t = 40; t <= 400; t += 20) {       // released at 60 ms; no late replay either
      o = g.Step(t * ms, t < 60 ? kStart : 0, false);
      any_start |= (o.guest_buttons & kStart) != 0;
    }
    Check(!any_start, "esc_close_never_yields_start_tap");
    o = g.Step(500 * ms, kStart, false);
    o = g.Step(700 * ms, kStart, false);
    Check(o.guest_buttons == kStart, "later_start_works_again");
  }
  {  // keystrokes (review fix 2)
    struct Src {
      std::deque<Keystroke> q;
      bool operator()(Keystroke* k) { if (q.empty()) return false; *k = q.front(); q.pop_front(); return true; }
    };
    auto ks = [](uint16_t vk, uint16_t flags) { Keystroke k; k.vk = vk; k.flags = flags; return k; };
    constexpr uint16_t kVkA = 0x5800, kVkB = 0x5801;
    {  // (a) lone Start tap: delayed, not lost
      KeystrokeGate g; Src src; Keystroke out;
      src.q = {ks(kVkPadStart, kKsDown), ks(kVkPadStart, kKsUp)};
      Check(!g.Next(0, false, 0, 0, src, &out), "ks_start_held_back");
      Check(!g.Next(100 * ms, false, 0, 0, src, &out), "ks_start_still_in_window");
      Check(g.Next(170 * ms, false, 0, 0, src, &out) && out.vk == kVkPadStart && (out.flags & kKsDown), "ks_start_down_late");
      Check(g.Next(171 * ms, false, 0, 0, src, &out) && out.vk == kVkPadStart && (out.flags & kKsUp), "ks_start_up_follows");
    }
    {  // (b) keystroke polled before the state: the combo's DOWNs never reach the game
      KeystrokeGate g; Src src; Keystroke out;
      src.q = {ks(kVkPadStart, kKsDown), ks(kVkPadBack, kKsDown)};
      Check(!g.Next(1000 * ms, false, 0, 0, src, &out), "ks_combo_downs_held");
      // state poll sees the combo 20 ms later
      bool leaked = false;
      for (uint64_t t = 1020; t <= 1400; t += 20) {
        if (g.Next(t * ms, t >= 1030, 0, 1020 * ms, src, &out)) leaked |= out.vk == kVkPadStart || out.vk == kVkPadBack;
        if (t == 1100) { src.q.push_back(ks(kVkPadStart, kKsUp)); src.q.push_back(ks(kVkPadBack, kKsUp)); }
      }
      Check(!leaked, "ks_combo_never_leaks_down_or_up");
    }
    {  // (c) A held when the menu opens: synthetic UP; inside-menu keys dropped; after close nothing typed inside arrives
      KeystrokeGate g; Src src; Keystroke out;
      src.q = {ks(kVkA, kKsDown)};
      Check(g.Next(0, false, 0, 0, src, &out) && out.vk == kVkA, "ks_a_down_delivered");
      Check(g.Next(10 * ms, true, 0, 0, src, &out) && out.vk == kVkA && (out.flags & kKsUp), "ks_open_synthesizes_up");
      src.q = {ks(kVkA, kKsUp), ks(kVkB, kKsDown)};
      Check(!g.Next(20 * ms, true, 0, 0, src, &out), "ks_inside_menu_dropped");
      src.q = {ks(0x1B, kKsDown)};                    // Esc typed inside the menu, polled only after the close
      Check(!g.Next(30 * ms, false, 1, 0, src, &out), "ks_typed_inside_not_delivered_after_close");
      src.q = {ks(kVkB, kKsUp)};                      // B released after the close: game never had its DOWN
      Check(!g.Next(40 * ms, false, 1, 0, src, &out), "ks_orphan_up_dropped");
      src.q = {ks(kVkB, kKsDown)};
      Check(g.Next(50 * ms, false, 1, 0, src, &out) && out.vk == kVkB, "ks_normal_after_close");
    }
    {  // close drain keeps the UP of a key the game still holds
      KeystrokeGate g; Src src; Keystroke out;
      src.q = {ks(0x0D, kKsDown)};
      g.Next(0, false, 0, 0, src, &out);
      src.q = {ks(0x0D, kKsUp)};                      // released while closing; queued before the close poll
      Check(g.Next(10 * ms, false, 1, 0, src, &out) && out.vk == 0x0D && (out.flags & kKsUp), "ks_drain_keeps_held_up");
    }
  }
  {  // test script: period 50 from 90; close by cycle % 3: B tap, B held 300 ms, keyboard Esc (fallback B at 40 s)
    using wwe2k14::menu::TestScriptButtonsAt;
    using wwe2k14::menu::TestCloseMode;
    Check(TestScriptButtonsAt(89.9, 50, 90, false) == 0, "script_off_before_start");
    Check(TestScriptButtonsAt(90.1, 50, 90, false) == 0x0030, "script_combo_at_start");
    Check(TestScriptButtonsAt(93.05, 50, 90, true) == 0x1000, "script_a_at_3s");
    Check(TestScriptButtonsAt(140.1, 50, 90, false) == 0x0030, "script_repeats_each_period");
    Check(TestScriptButtonsAt(120.10, 50, 90, true) == 0x2000 && TestScriptButtonsAt(120.20, 50, 90, true) == 0,
          "script_cycle0_b_tap");
    Check(TestScriptButtonsAt(170.25, 50, 90, true) == 0x2000 && TestScriptButtonsAt(170.35, 50, 90, true) == 0,
          "script_cycle1_b_held_300ms");
    Check(TestScriptButtonsAt(220.05, 50, 90, true) == 0 && TestScriptButtonsAt(230.05, 50, 90, true) == 0x2000 &&
              TestScriptButtonsAt(230.05, 50, 90, false) == 0,
          "script_cycle2_esc_then_fallback_only_if_open");
    Check(TestCloseMode(225.0, 50, 90) == 2 && TestCloseMode(275.0, 50, 90) == 0 && TestCloseMode(10, 50, 90) == -1,
          "script_close_modes");
    Check(TestScriptButtonsAt(100.0, 50, 90, true) == 0, "script_idle_between_presses");
    Check(TestScriptButtonsAt(100.0, 0, 90, true) == 0, "script_off_without_period");
  }
  std::printf("overlay_pad_test %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
