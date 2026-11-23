// G1 in-game settings menu (owner 2026-11-07): weak-PC options the player can change while playing.
// The launcher's environment gives the starting value (same parsing as before: weakpc_options.h); the in-game menu
// (src/wwe2k14_app.h SettingsMenu) writes a new value; draw translation latches it once per guest frame (draw.cpp), so a
// frame never mixes two settings. Only options whose reader is a per-draw test with no pipeline, target or cache built
// from the value are here (docs/tasks/G1-overlay-report.md, inventory).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "weakpc_options.h"

namespace wwe2k14::d3d::live {

struct Options {
  std::atomic<int> crowd{1};           // 1 full, 2 half, 0 none (WWE2K14_D3D_CROWD)
  std::atomic<int> crowd_on_mode{1};   // what "Crowd on" restores: the last non-zero mode (2 stays 2)
  std::atomic<bool> shadows{true};     // false = shadow passes removed (WWE2K14_D3D_SHADOWS=0)
  std::atomic<uint32_t> generation{0};  // bumped on every menu change (logged by the draw latch)
  // Draw-side evidence for the test hook: draws dropped by each option since start.
  std::atomic<uint64_t> crowd_dropped{0};
  std::atomic<uint64_t> shadow_dropped{0};
};

inline Options& Get() {
  static Options* options = [] {
    auto* o = new Options();  // process lifetime: draw threads may still read it during teardown
    const int mode = weakpc::ParseCrowdMode(std::getenv("WWE2K14_D3D_CROWD"));
    o->crowd.store(mode, std::memory_order_relaxed);
    o->crowd_on_mode.store(mode != 0 ? mode : 1, std::memory_order_relaxed);
    o->shadows.store(weakpc::ParseShadowsEnabled(std::getenv("WWE2K14_D3D_SHADOWS")), std::memory_order_relaxed);
    return o;
  }();
  return *options;
}

inline void SetCrowd(int mode) {
  Options& o = Get();
  const int m = mode == 0 || mode == 2 ? mode : 1;
  if (m != 0) o.crowd_on_mode.store(m, std::memory_order_relaxed);
  o.crowd.store(m, std::memory_order_relaxed);
  o.generation.fetch_add(1, std::memory_order_release);
}

// Menu switch (review fix 4): off = 0; on = the last non-zero mode, so a launcher "Half" (2) comes back as Half.
inline void SetCrowdEnabled(bool on) { SetCrowd(on ? Get().crowd_on_mode.load(std::memory_order_relaxed) : 0); }

inline void SetShadows(bool on) {
  Options& o = Get();
  o.shadows.store(on, std::memory_order_relaxed);
  o.generation.fetch_add(1, std::memory_order_release);
}

// Per-thread value latched at the first draw of each guest frame.
struct FrameLatch {
  uint32_t frame = ~uint32_t{0};
  uint32_t generation = ~uint32_t{0};
  int crowd = 1;
  bool shadows = true;
  // True when the frame changed; *changed is set when the menu generation moved since the last latch.
  bool Update(uint32_t guest_frame, bool* changed) {
    *changed = false;
    if (guest_frame == frame) return false;
    frame = guest_frame;
    Options& o = Get();
    const uint32_t g = o.generation.load(std::memory_order_acquire);
    *changed = g != generation && generation != ~uint32_t{0};
    generation = g;
    crowd = o.crowd.load(std::memory_order_relaxed);
    shadows = o.shadows.load(std::memory_order_relaxed);
    return true;
  }
};

}  // namespace wwe2k14::d3d::live
