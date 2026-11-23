// Host test for nr/rec_reuse.h. g++ -std=c++20 -O2 -Wall -Wextra rec-reuse-test.cpp -o rec-reuse-test
#include "../rec_reuse.h"

#include <cassert>

int main() {
  using nr::backend::rec_reuse::Probe;
  {
    const auto r = Probe(false, 0, false, 0, 0x1000);
    assert(!r.hit && !r.mismatch);
  }
  {
    const auto r = Probe(true, 0x1000, true, 0x1000, 0x1000);
    assert(r.hit && !r.mismatch);
  }
  {
    const auto r = Probe(true, 0x1000, true, 0x1000, 0x2000);
    assert(!r.hit && !r.mismatch);
  }
  assert(Probe(true, 0x1000, false, 0, 0x1000).mismatch);
  assert(Probe(false, 0, true, 0x1000, 0x1000).mismatch);
}
