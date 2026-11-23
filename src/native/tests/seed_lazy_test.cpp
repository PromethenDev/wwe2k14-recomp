#include "../d3d/seed_lazy.h"

#include <cassert>
#include <vector>

using namespace wwe2k14::d3d::seed_lazy;

int main() {
  PixelCoverage all(16, 16);
  all.MarkRect(0, 0, 16, 16);
  std::vector<Rect> rects;
  assert(all.Covers(0, 0, 16, 16));
  assert(all.UncoveredRects(&rects) && rects.empty());
  const Observer covered_bind = all.Covers(0, 0, 16, 16) ? Observer::kNone : Observer::kUncoveredBind;
  assert(Decide(true, true, covered_bind) == Action::kSkip);

  PixelCoverage partial(16, 16);
  partial.MarkRect(0, 0, 8, 16);
  const Observer uncovered_bind = partial.Covers(0, 0, 16, 16) ? Observer::kNone : Observer::kUncoveredBind;
  assert(Decide(true, true, uncovered_bind) == Action::kMaterialize);
  assert(Decide(true, true, Observer::kWriteBack) == Action::kSkip);

  // A sub-cell resolve still protects its exact pixels when the seed is copied around it.
  PixelCoverage edge(8, 8);
  edge.MarkRect(1, 2, 4, 5);
  assert(edge.UncoveredRects(&rects));
  for (uint32_t y = 0; y < 8; ++y) {
    for (uint32_t x = 0; x < 8; ++x) {
      bool copied_from_seed = false;
      for (const Rect& r : rects)
        copied_from_seed |= x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
      assert(copied_from_seed == !edge.Covered(x, y));
    }
  }

  // A size-growing propagation copies the entire old image rectangle, so the new seed must not replace any of it.
  PixelCoverage grown(16, 16);
  grown.MarkRect(0, 0, 8, 8);
  assert(grown.Covers(0, 0, 8, 8));
  assert(!grown.Covered(8, 0) && !grown.Covered(0, 8));

  // Dirty direct binds retain the existing refusal. Write-back/propagation can reproduce the old image from its
  // immutable source backup, even when guest bytes have since changed.
  assert(Decide(true, false, Observer::kUncoveredBind) == Action::kRefuseDirtyBind);
  assert(Decide(true, false, Observer::kWriteBack) == Action::kSkip);
  assert(Decide(true, false, Observer::kPropagation) == Action::kMaterialize);
  assert(Decide(false, true, Observer::kUncoveredBind) == Action::kNoPending);
}
