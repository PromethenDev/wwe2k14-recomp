// Card N4e (opt-in WWE2K14_LOCK_30=4): renderer-observable "loading screen up" detector. A frame is a "scene" frame
// when it carried >= kSceneZDraws depth-writing draws (same threshold/idea as card S3-39's 3D-scene test). Loading is
// declared after kEnterFrames consecutive non-scene frames and cleared after kExitFrames consecutive scene frames.
// Pure state machine (host-testable); the published atomic is read by the guest main thread at the frame-end hook.
#pragma once
#include <atomic>
#include <cstdint>

namespace wwe2k14::loadlock {

constexpr uint32_t kSceneZDraws = 64;
// 90 consecutive non-scene frames (1.5 s at 60, 3 s at 30) before 60 fps is requested.
// Evidence (n4e-l1c, n4e-l4a trace, ~13500 scene frames): entrances (588/620 frames) and matches (4940/3393 frames)
// contained NO non-scene interval at all; the shortest non-scene interval anywhere was a real load (blank, 155 frames),
// the others 325/342 (tip card) and menus (thousands). So the observed in-scene gap is 0 frames; 90 is below every real
// load segment seen (155) yet well above a few-frame fade/cut. NOT verified: pause menu and long fades-to-black inside
// a match (no pause occurred in those logs). A pause or fade with no scene draws that lasts > 90 frames is treated as
// loading: the rate becomes 60 while it lasts and the game's halve is re-applied when scene draws return. That only
// changes the time step used at resume (the game's own halve constants), nothing is skipped.
// Menus/results/loading all run at 60 by design (the game ran at 60 outside the 3D scene before its first halve).
constexpr uint32_t kDefaultEnterFrames = 90;

struct Detector {
  uint32_t enter_frames = kDefaultEnterFrames;
  uint32_t exit_frames = 2;
  uint32_t no_scene_run = 0, scene_run = 0;
  bool loading = false;
  // Returns the (possibly updated) loading state after this frame.
  bool Step(uint32_t zdraws) {
    if (zdraws >= kSceneZDraws) {
      ++scene_run;
      no_scene_run = 0;
      if (loading && scene_run >= exit_frames) loading = false;
    } else {
      ++no_scene_run;
      scene_run = 0;
      if (!loading && no_scene_run >= enter_frames) loading = true;
    }
    return loading;
  }
};

inline std::atomic<bool> g_loading{false};

}  // namespace wwe2k14::loadlock
