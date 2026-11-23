// G1 (owner 2026-11-07): pad side of the in-game settings menu, pure logic (host-tested in
// native/tests/overlay_pad_test.cpp). The input wrapper (overlay_input.cpp) runs one PadGate per pad on every
// guest XInputGetState poll.
//
// Back + Start pressed together opens or closes the menu (owner's choice 2026-11-07). So the game never sees half of
// the combo, a lone Back or Start is held back for kComboWindowUs after it goes down: if the other button follows in
// that window it is the combo and neither reaches the game (until both are released); otherwise the press is passed
// on late, and a tap released inside the window is replayed for kReplayUs so the game still sees it. While the menu is
// open the game gets an untouched pad. Buttons held when it closes stay hidden until released.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

namespace wwe2k14::overlay_pad {

// XINPUT_GAMEPAD button bits.
inline constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
inline constexpr uint16_t kStart = 0x0010, kBack = 0x0020;
inline constexpr uint16_t kA = 0x1000, kB = 0x2000;
inline constexpr uint16_t kCombo = kStart | kBack;
inline constexpr uint64_t kComboWindowUs = 120000;
inline constexpr uint64_t kReplayUs = 100000;

struct PadGate {
  struct Out {
    uint16_t guest_buttons = 0;  // what the game may see (0 while the menu is open)
    bool toggle = false;         // Back + Start completed on this poll
    bool block_all = false;      // the game must get an untouched pad (sticks and triggers too)
  };

  Out Step(uint64_t now_us, uint16_t raw_in, bool menu_open) {
    Out out;
    // Review fix 1: the press that closed the menu (B, A on Close, Back+Start, or Esc = the keyboard's Start) must not
    // reach the game. On the open -> closed edge every button then down is latched and hidden until it is released.
    if (prev_open_ && !menu_open) latched_ = raw_in;
    prev_open_ = menu_open;
    latched_ &= raw_in;
    const uint16_t raw = uint16_t(raw_in & ~latched_);
    const uint16_t bs = raw & kCombo;
    const uint16_t prev_bs = prev_raw_ & kCombo;
    uint16_t hide = 0;
    // Together = same poll, or the second inside the window. A lone press already passed on to the game does not
    // become a combo later (the game has acted on it), except while the menu is open (the game sees nothing then).
    const bool in_window = pending_ != 0 && now_us - pending_since_ < kComboWindowUs;
    if (bs == kCombo && !swallow_ && (prev_bs == 0 || in_window || menu_open)) {
      out.toggle = true;
      swallow_ = true;
      pending_ = 0;
      replay_mask_ = 0;
    }
    if (swallow_) {
      if (bs == 0) swallow_ = false;
      hide = kCombo;
    } else if (bs == kCombo) {
      pending_ = 0;  // late second press: both go to the game as usual
    } else if (bs == kStart || bs == kBack) {
      if (prev_bs == 0) {  // newly pressed alone (not: the other of a held pair let go)
        pending_ = bs;
        pending_since_ = now_us;
      }
      if (pending_ == bs && now_us - pending_since_ < kComboWindowUs) hide = bs;
      else pending_ = 0;
    } else {  // neither held
      if (pending_ && (prev_bs & pending_) && now_us - pending_since_ < kComboWindowUs) {
        replay_mask_ = pending_;  // a tap shorter than the window: show it now, late
        replay_until_ = now_us + kReplayUs;
      }
      pending_ = 0;
    }
    prev_raw_ = raw;
    uint16_t guest = uint16_t(raw & ~hide);
    if (replay_mask_) {
      if (now_us < replay_until_) guest |= replay_mask_;
      else replay_mask_ = 0;
    }
    if (menu_open || out.toggle) {
      out.block_all = true;
      guest = 0;
    }
    out.guest_buttons = guest;
    return out;
  }

  // Back/Start currently held back from the game (combo window or a combo still held).
  bool HidingCombo() const { return swallow_ || pending_ != 0; }

 private:
  uint16_t prev_raw_ = 0;
  uint16_t pending_ = 0;
  uint64_t pending_since_ = 0;
  bool swallow_ = false;
  uint16_t replay_mask_ = 0;
  uint64_t replay_until_ = 0;
  bool prev_open_ = false;
  uint16_t latched_ = 0;
};

// Review fix 2: keystrokes (XamInputGetKeystrokeEx; the title polls it on its text screens) made consistent with the
// pad gate above. Pure logic; the input wrapper feeds it the inner drivers' keystrokes.
//  - menu open: nothing new reaches the game; at the open edge every key the game has seen go down (and not up) gets
//    a synthetic UP first, like sdl_input_driver.cpp does when its input goes inactive.
//  - menu closed again: keystrokes queued meanwhile are drained (DOWNs dropped, UPs of keys the game saw go down kept),
//    so a key pressed inside the menu (Esc, B, A) is not delivered afterwards.
//  - a pad Start/Back DOWN is held back like the pad state (kComboWindowUs + kKeystrokeSlackUs, also covering polls
//    of keystrokes before state): dropped if Back+Start toggled within the window around it, else delivered late; its
//    UP/REPEAT queue behind it, so a tap is replayed, not lost.
//  - an UP/REPEAT whose DOWN the game never got is dropped.
inline constexpr uint16_t kVkPadStart = 0x5814, kVkPadBack = 0x5815;  // rex::ui::VirtualKey kXInputPad*
inline constexpr uint16_t kVkPacket = 0x00E7;                            // VirtualKey::kPacket (text characters)
inline constexpr uint16_t kKsDown = 0x0001, kKsUp = 0x0002, kKsRepeat = 0x0004;
inline constexpr uint64_t kKeystrokeSlackUs = 40000;

struct Keystroke {
  uint16_t vk = 0, unicode = 0, flags = 0;
  uint8_t user = 0, hid = 0;
};

class KeystrokeGate {
 public:
  // fetch(Keystroke*) -> bool pulls the next keystroke from the inner drivers. last_toggle_us: time of the latest
  // Back+Start toggle (0 = never). close_count: number of menu closes so far. Returns true with *out to deliver.
  template <class Fetch>
  bool Next(uint64_t now_us, bool menu_open, uint64_t close_count, uint64_t last_toggle_us, Fetch&& fetch,
            Keystroke* out) {
    if (menu_open && !last_open_) {  // open edge: release everything the game holds
      for (const Keystroke& k : down_) {
        Keystroke up = k;
        up.flags = kKsUp;
        ready_.push_back(up);
      }
      down_.clear();
      held_.clear();
    }
    last_open_ = menu_open;
    if (close_count != seen_close_) {  // close edge: drain what was typed inside the menu
      seen_close_ = close_count;
      Keystroke k;
      int guard = 0;
      while (guard++ < 1024 && fetch(&k)) {
        if ((k.flags & kKsUp) && IsDown(k)) {
          EraseDown(k);
          ready_.push_back(k);
        } else {
          ++dropped_;
        }
      }
      held_.clear();
    }
    if (!ready_.empty()) {  // synthetic / drained UPs go first, also while the menu is open
      *out = ready_.front();
      ready_.pop_front();
      return true;
    }
    // Held Start/Back keystrokes whose window has passed.
    while (!held_.empty()) {
      Held& h = held_.front();
      if (Dropped(h, last_toggle_us)) {
        DropHeldVk(h.k.vk);
        continue;
      }
      if ((h.k.flags & kKsDown) && now_us < h.t + kComboWindowUs + kKeystrokeSlackUs) break;
      Keystroke k = h.k;
      held_.pop_front();
      if (menu_open) { ++dropped_; continue; }
      if (Deliver(k, out)) return true;
    }
    Keystroke k;
    int guard = 0;
    while (guard++ < 1024 && fetch(&k)) {
      if (menu_open) { ++dropped_; continue; }
      const bool combo_key = k.vk == kVkPadStart || k.vk == kVkPadBack;
      if (combo_key && ((k.flags & kKsDown) || HeldHas(k.vk))) {
        held_.push_back({k, now_us});
        continue;
      }
      if (Deliver(k, out)) return true;
    }
    return false;
  }
  uint64_t dropped() const { return dropped_; }

 private:
  struct Held {
    Keystroke k;
    uint64_t t;
  };
  bool IsDown(const Keystroke& k) const {
    return std::any_of(down_.begin(), down_.end(), [&](const Keystroke& d) { return d.vk == k.vk && d.user == k.user; });
  }
  void EraseDown(const Keystroke& k) {
    down_.erase(std::remove_if(down_.begin(), down_.end(),
                               [&](const Keystroke& d) { return d.vk == k.vk && d.user == k.user; }),
                down_.end());
  }
  bool HeldHas(uint16_t vk) const {
    return std::any_of(held_.begin(), held_.end(), [&](const Held& h) { return h.k.vk == vk; });
  }
  // A held DOWN is dropped when Back+Start toggled within the window around it; its queued UP/REPEAT go with it.
  bool Dropped(const Held& h, uint64_t last_toggle_us) const {
    if (!(h.k.flags & kKsDown) || !last_toggle_us) return false;
    const uint64_t w = kComboWindowUs + kKeystrokeSlackUs;
    return last_toggle_us + w >= h.t && last_toggle_us <= h.t + w;
  }
  void DropHeldVk(uint16_t vk) {
    const size_t before = held_.size();
    held_.erase(std::remove_if(held_.begin(), held_.end(), [&](const Held& h) { return h.k.vk == vk; }), held_.end());
    dropped_ += before - held_.size();
  }
  // DOWN: remember; UP/REPEAT: only if the game got the DOWN.
  bool Deliver(const Keystroke& k, Keystroke* out) {
    if (k.vk == kVkPacket) {  // typed/pasted characters: DOWN only, nothing to pair
      *out = k;
      return true;
    }
    if (k.flags & kKsDown) {
      if (!IsDown(k)) down_.push_back(k);
    } else if (!IsDown(k)) {
      ++dropped_;
      return false;
    } else if (k.flags & kKsUp) {
      EraseDown(k);
    }
    *out = k;
    return true;
  }

  std::vector<Keystroke> down_;
  std::deque<Held> held_;
  std::deque<Keystroke> ready_;
  bool last_open_ = false;
  uint64_t seen_close_ = 0;
  uint64_t dropped_ = 0;
};

}  // namespace wwe2k14::overlay_pad
