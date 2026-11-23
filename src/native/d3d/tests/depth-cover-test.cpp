// Host-only checks for the G18d depth-cover sizing rule (card G18b).
#include "../depth_cover_core.h"

#include <cstdio>

using namespace wwe2k14::d3d::depth_cover;

int main() {
  uint32_t bad = 0;
  auto check = [&](bool value, const char* name) {
    if (!value) {
      ++bad;
      std::printf("FAIL %s\n", name);
    }
  };
  // Attire composite: colour 2080x1024, depth declared 720 rows, no image yet -> created at 1024 rows, nothing lost.
  Plan p = PlanCover(2080, 0, 720, 2080, 1024);
  check(p.cover && !p.recreate && p.width == 2080 && p.need_h == 1024, "new depth slot sized to the colour");
  // Same, depth image already 720 rows -> replaced (taller).
  p = PlanCover(2080, 720, 720, 2080, 1024);
  check(p.cover && p.recreate && p.need_h == 1024, "existing short depth image recreated taller");
  // Scene: colour 1280x720 with depth 1280x720 -> untouched.
  p = PlanCover(1280, 720, 720, 1280, 720);
  check(!p.cover && !p.recreate && p.width == 1280 && p.need_h == 720, "equal sizes untouched");
  // Depth larger than colour -> untouched (a larger attachment is valid).
  p = PlanCover(1280, 1024, 1024, 1280, 720);
  check(!p.cover && !p.recreate && p.need_h == 1024, "larger depth untouched");
  // Wider colour (tiled scene widen) -> recreated wider, keeps its own taller rows.
  p = PlanCover(1280, 736, 736, 1440, 720);
  check(!p.cover && !p.recreate && p.width == 1280 && p.need_h == 736, "narrow depth left as is (height-only rule)");
  // No colour image -> untouched.
  p = PlanCover(2080, 0, 720, 0, 0);
  check(!p.cover && p.need_h == 720, "no colour: declared extent");
  // Existing image tall enough but Ensure's own need is small (e.g. resolve with need 0 -> sl->height): no recreate.
  p = PlanCover(2080, 1024, 720, 2080, 1024);
  check(!p.recreate && p.need_h <= 1024, "already covered image kept");
  std::printf("%s (%u failures)\n", bad ? "FAILED" : "OK", bad);
  return bad ? 1 : 0;
}
