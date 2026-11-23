#include "../v17c_ab.h"

#include <cassert>

int main() {
  using namespace wwe2k14::d3d::v17c_ab::schedule;
  bool invalid = false;
  assert(ParseBlockFrames(nullptr, &invalid) == 0 && !invalid);
  assert(ParseBlockFrames("", &invalid) == 0 && !invalid);
  assert(ParseBlockFrames("600", &invalid) == 600 && !invalid);
  assert(ParseBlockFrames("0", &invalid) == 0 && !invalid);
  assert(ParseBlockFrames("600x", &invalid) == 0 && invalid);
  assert(ParseBlockFrames("1000001", &invalid) == 0 && invalid);
  assert(ModeForFrame(0, 0) == -1);
  assert(ModeForFrame(600, 0) == 0);
  assert(ModeForFrame(600, 599) == 0);
  assert(ModeForFrame(600, 600) == 1);
  assert(ModeForFrame(600, 1199) == 1);
  assert(ModeForFrame(600, 1200) == 0);
}
