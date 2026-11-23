// Unattended navigation for UHD measurements (card S3-16; owner OK 2026-10-15) - test harness only, default OFF.
//
// WWE2K14_AUTOPILOT=1 replaces the SDK input system with ONE scripted pad driver (no keyboard/SDL/XInput merged in):
// from 20 s it taps Start and A alternately (every 3.5 s) until the first entrance cadence is seen (or 240 s), then
// taps A every 3.5 s - the menus' defaults (PLAY > ONE ON ONE > NORMAL, default wrestlers) lead into a match, as the
// Linux navigator's logs show (it only presses Start then A). While the game presents at its entrance cadence
// (present interval 2, docs/EVIDENCE.md 2026-10-14) it presses nothing, so entrances play out in full.
// Logs "[autopilot]" lines with the phase changes. Tunables: WWE2K14_AUTOPILOT_A_MS (default 3500).
// WWE2K14_AUTOPILOT_CMDFILE=<path> (card S5-2, Windows menu routes): the pad is driven only by tokens appended to that
// file (ported from the rd-media lane): a b x y s(tart) k(back) u d l r q(LB) e(RB), optional *N repeat, wN wait ms.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

// Present interval of the game's last flip (src/present_interval.cpp, sub_82CB6318) and its time.
extern std::atomic<uint32_t> g_wwe2k14_present_interval;
extern std::atomic<uint64_t> g_wwe2k14_present_interval_us;

namespace {
using namespace rex;
using namespace rex::input;

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

class AutopilotDriver final : public InputDriver {
 public:
  AutopilotDriver() : InputDriver(nullptr, 0), t0_(NowUs()) {
    if (const char* e = std::getenv("WWE2K14_AUTOPILOT_A_MS")) a_ms_ = std::strtoul(e, nullptr, 10);
    if (a_ms_ < 500) a_ms_ = 3500;
    if (const char* c = std::getenv("WWE2K14_AUTOPILOT_CMDFILE")) {
      if (*c) {
        cmd_path_ = c;
        cmd_on_ = true;
        REXLOG_INFO("[autopilot] live command file {}", cmd_path_);
      }
    }
  }
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  X_RESULT GetCapabilities(uint32_t user, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    if (user != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;
      caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t user, X_INPUT_STATE* out) override {
    if (user != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    const uint16_t buttons = Update();
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->packet_number = packet_;
      out->gamepad.buttons = buttons;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t user, X_INPUT_VIBRATION*) override {
    return user == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetKeystroke(uint32_t user, uint32_t, X_INPUT_KEYSTROKE* out) override {
    if (user != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    Update();
    std::lock_guard<std::mutex> lk(mu_);
    if (keys_.empty()) return X_ERROR_EMPTY;
    if (out) *out = keys_.front();
    keys_.pop_front();
    return X_ERROR_SUCCESS;
  }

 private:
  static constexpr uint16_t kVkA = 0x5800, kVkStart = 0x5814;
  struct Step {
    uint16_t mask;
    uint32_t ms;
  };
  // Reads tokens appended to the command file since the last poll (every 400 ms) into the tap queue.
  void PollCmd(uint64_t t) {
    if (t < cmd_poll_t_ + 400) return;
    cmd_poll_t_ = t;
    FILE* f = std::fopen(cmd_path_.c_str(), "rb");
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    if (sz > long(cmd_off_)) {
      std::string buf(size_t(sz) - cmd_off_, '\0');
      std::fseek(f, long(cmd_off_), SEEK_SET);
      buf.resize(std::fread(&buf[0], 1, buf.size(), f));
      const size_t last = buf.find_last_of(",; \r\n");
      if (last != std::string::npos) {
        cmd_off_ += last + 1;
        const std::string chunk = buf.substr(0, last + 1);
        auto tap = [&](uint16_t m) {
          cmd_q_.push_back({m, 150});
          cmd_q_.push_back({0, 500});
        };
        size_t pos = 0;
        while (pos < chunk.size()) {
          const size_t nx = chunk.find_first_of(",; \r\n", pos);
          const std::string tok = chunk.substr(pos, nx - pos);
          pos = nx + 1;
          if (tok.empty()) continue;
          unsigned rep = 1;
          const size_t star = tok.find('*');
          if (star != std::string::npos) rep = std::min(40u, unsigned(std::strtoul(tok.c_str() + star + 1, nullptr, 10)));
          uint16_t m = 0;
          switch (tok[0]) {
            case 'w': cmd_q_.push_back({0, uint32_t(std::strtoul(tok.c_str() + 1, nullptr, 10))}); continue;
            case 'a': m = X_INPUT_GAMEPAD_A; break;
            case 'b': m = X_INPUT_GAMEPAD_B; break;
            case 'x': m = X_INPUT_GAMEPAD_X; break;
            case 'y': m = X_INPUT_GAMEPAD_Y; break;
            case 's': m = X_INPUT_GAMEPAD_START; break;
            case 'k': m = X_INPUT_GAMEPAD_BACK; break;
            case 'u': m = X_INPUT_GAMEPAD_DPAD_UP; break;
            case 'd': m = X_INPUT_GAMEPAD_DPAD_DOWN; break;
            case 'l': m = X_INPUT_GAMEPAD_DPAD_LEFT; break;
            case 'r': m = X_INPUT_GAMEPAD_DPAD_RIGHT; break;
            case 'q': m = X_INPUT_GAMEPAD_LEFT_SHOULDER; break;
            case 'e': m = X_INPUT_GAMEPAD_RIGHT_SHOULDER; break;
            default: continue;
          }
          for (unsigned i = 0; i < rep; ++i) tap(m);
          REXLOG_INFO("[autopilot] t={} ms cmd '{}'", t, tok);
        }
      }
    }
    std::fclose(f);
  }
  uint16_t CmdMask(uint64_t t) {
    if (t < cmd_end_t_) return cmd_mask_;
    if (cmd_q_.empty()) return 0;
    const Step st = cmd_q_.front();
    cmd_q_.pop_front();
    cmd_mask_ = st.mask;
    cmd_end_t_ = t + st.ms;
    return cmd_mask_;
  }
  static uint64_t HoldMaxUs() {
    static const uint64_t us = [] {
      const char* v = std::getenv("WWE2K14_AUTOPILOT_HOLD_MAX_S");
      const long s = v ? std::strtol(v, nullptr, 10) : 0;
      return uint64_t(s > 0 ? s : 0) * 1000000ull;
    }();
    return us;
  }
  uint64_t hold_start_us_ = 0;
  // Current pressed mask; queues key down/up events on edges.
  uint16_t Update() {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t now = NowUs();
    const uint64_t t = (now - t0_) / 1000;  // ms since start
    bool entrance = g_wwe2k14_present_interval.load(std::memory_order_relaxed) == 2 &&
                    now - g_wwe2k14_present_interval_us.load(std::memory_order_relaxed) < 1500000;
    // WWE2K14_AUTOPILOT_HOLD_MAX_S=<s> (test harness, default off): stop holding after s seconds of 30 fps cadence, so
    // a post-match screen (also interval 2) is tapped through and the attract loop reaches further matches.
    if (entrance) {
      if (!hold_start_us_) hold_start_us_ = now;
      if (HoldMaxUs() && now - hold_start_us_ > HoldMaxUs()) entrance = false;
    } else {
      hold_start_us_ = 0;
    }
    if (entrance != in_entrance_) {
      in_entrance_ = entrance;
      REXLOG_INFO("[autopilot] t={} ms {}", t, entrance ? "entrance cadence: holding" : "resume tapping A");
    }
    if (entrance) seen_entrance_ = true;
    uint16_t mask = 0;
    if (cmd_on_) {
      PollCmd(t);
      mask = CmdMask(t);
    } else if (!entrance && t >= 20000) {
      if (!seen_entrance_ && t < 240000) {
        // Until the first entrance: Start and A alternately (title "press start" can come late on a slow machine).
        const uint64_t p = (t - 20000) % 7000;
        if (p < 150) mask = X_INPUT_GAMEPAD_START;
        else if (p >= 3500 && p < 3650) mask = X_INPUT_GAMEPAD_A;
      } else if ((t - 20000) % a_ms_ < 150) {
        mask = X_INPUT_GAMEPAD_A;
      }
    }
    if (mask != mask_) {
      Edge(mask_ & ~mask, false);
      Edge(mask & ~mask_, true);
      mask_ = mask;
      ++packet_;
    }
    return mask;
  }
  void Edge(uint16_t bits, bool down) {
    auto push = [&](uint16_t vk) {
      X_INPUT_KEYSTROKE ks{};
      ks.virtual_key = vk;
      ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
      ks.user_index = 0;
      if (keys_.size() < 64) keys_.push_back(ks);
    };
    if (bits & X_INPUT_GAMEPAD_A) push(kVkA);
    if (bits & X_INPUT_GAMEPAD_START) push(kVkStart);
    if (bits & X_INPUT_GAMEPAD_DPAD_DOWN) push(0x5811);
    if (bits & X_INPUT_GAMEPAD_DPAD_UP) push(0x5810);
    if (bits & X_INPUT_GAMEPAD_B) push(0x5801);
    if (bits & X_INPUT_GAMEPAD_X) push(0x5802);
    if (bits & X_INPUT_GAMEPAD_Y) push(0x5803);
    if (bits & X_INPUT_GAMEPAD_RIGHT_SHOULDER) push(0x5804);
    if (bits & X_INPUT_GAMEPAD_LEFT_SHOULDER) push(0x5805);
    if (bits & X_INPUT_GAMEPAD_DPAD_LEFT) push(0x5812);
    if (bits & X_INPUT_GAMEPAD_DPAD_RIGHT) push(0x5813);
    if (bits & X_INPUT_GAMEPAD_BACK) push(0x5815);
  }
  const uint64_t t0_;
  uint64_t a_ms_ = 3500;
  std::mutex mu_;
  std::deque<X_INPUT_KEYSTROKE> keys_;
  uint16_t mask_ = 0;
  uint32_t packet_ = 1;
  bool in_entrance_ = false, seen_entrance_ = false;
  std::string cmd_path_;
  bool cmd_on_ = false;
  size_t cmd_off_ = 0;
  uint64_t cmd_poll_t_ = 0, cmd_end_t_ = 0;
  uint16_t cmd_mask_ = 0;
  std::deque<Step> cmd_q_;
};
}  // namespace

namespace wwe2k14 {
bool AutopilotEnabled() {
  const char* e = std::getenv("WWE2K14_AUTOPILOT");
  return e && *e == '1';
}
std::unique_ptr<rex::system::IInputSystem> CreateAutopilotInputSystem(bool) {
  auto input = std::make_unique<InputSystem>(nullptr);
  input->AddDriver(std::make_unique<AutopilotDriver>());
  REXLOG_INFO("[autopilot] scripted pad driver is the only input (WWE2K14_AUTOPILOT=1)");
  return input;
}
}  // namespace wwe2k14
