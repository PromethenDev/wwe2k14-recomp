// Host-only test of the N4e loading detector. g++ -std=c++17 -I.. load-lock-test.cpp
#include "../load_lock_core.h"
#include <cstdio>
using namespace wwe2k14::loadlock;
int main() {
  int bad = 0;
  auto chk = [&](bool v, const char* n) { if (!v) { ++bad; std::printf("FAIL %s\n", n); } };
  Detector d;
  d.enter_frames = 45;
  for (int i = 0; i < 1000; ++i) d.Step(500);
  chk(!d.loading, "scene frames never load");
  for (int i = 0; i < 44; ++i) d.Step(3);
  chk(!d.loading, "44 non-scene frames not yet loading");
  d.Step(0);
  chk(d.loading, "45th non-scene frame -> loading");
  d.Step(500);
  chk(d.loading, "one scene frame keeps loading (hysteresis)");
  d.Step(0);
  chk(d.loading, "scene run reset by non-scene frame");
  d.Step(500); d.Step(70);
  chk(!d.loading, "two consecutive scene frames end loading");
  Detector e;  // short non-scene gaps inside a scene never trigger
  for (int r = 0; r < 100; ++r) { for (int i = 0; i < 44; ++i) e.Step(0); e.Step(100); }
  chk(!e.loading, "gaps of 44 frames never trigger");
  Detector f; f.enter_frames = 3; f.exit_frames = 1;
  f.Step(0); f.Step(0); chk(!f.loading, "custom enter 2/3");
  f.Step(0); chk(f.loading, "custom enter 3/3"); f.Step(64); chk(!f.loading, "custom exit threshold 64 inclusive");
  {  // default threshold cases (kDefaultEnterFrames)
    chk(kDefaultEnterFrames == 90, "default enter = 90 frames");
    Detector m;  // match -> results/menu: long non-scene run => loading (60), next match scene ends it
    for (int i = 0; i < 3000; ++i) m.Step(900);
    for (int i = 0; i < 89; ++i) m.Step(0);
    chk(!m.loading, "menu after match: not loading before 90 frames");
    m.Step(0); chk(m.loading, "menu after match: loading at 90th frame");
    for (int i = 0; i < 5000; ++i) m.Step(10);
    chk(m.loading, "menu stays 60 while it lasts");
    m.Step(300); m.Step(300); chk(!m.loading, "next scene returns to 30");
    Detector p;  // pause-length gap (60 frames) inside a match must not flap
    for (int i = 0; i < 500; ++i) p.Step(800);
    bool flapped = false;
    for (int i = 0; i < 60; ++i) flapped |= p.Step(0);
    for (int i = 0; i < 500; ++i) flapped |= p.Step(800);
    chk(!flapped, "60-frame low-draw gap inside a match: no flap");
    Detector sf;  // short fade (20 frames)
    bool fl2 = false;
    for (int r = 0; r < 20; ++r) { for (int i = 0; i < 20; ++i) fl2 |= sf.Step(2); for (int i = 0; i < 100; ++i) fl2 |= sf.Step(400); }
    chk(!fl2, "20-frame fades never flap");
    Detector lf;  // long fade (150 frames, > threshold): documented as loading, recovers at the first two scene frames
    for (int i = 0; i < 300; ++i) lf.Step(400);
    bool seen = false; for (int i = 0; i < 150; ++i) seen |= lf.Step(0);
    chk(seen && lf.loading, "150-frame fade is treated as loading (documented)");
    lf.Step(400); chk(lf.loading, "one scene frame keeps it"); lf.Step(400); chk(!lf.loading, "recovered after 2 scene frames");
  }
  std::printf(bad ? "FAILED %d\n" : "ok\n", bad);
  return bad != 0;
}
