#include <cassert>

#include "../d3d/native_efa4_schedule.h"

int main() {
  using wwe2k14::d3d::efa4::NativeActive;
  assert(!NativeActive(0, 1));
  assert(NativeActive(1, 1));
  assert(NativeActive(1, 1201));
  assert(!NativeActive(2, 0));
  assert(NativeActive(2, 1));
  assert(NativeActive(2, 600));
  assert(!NativeActive(2, 601));
  assert(!NativeActive(2, 1200));
  assert(NativeActive(2, 1201));
  assert(NativeActive(2, 1800));
  assert(!NativeActive(2, 1801));
  assert(!NativeActive(2, 2400));
  assert(NativeActive(2, 2401));
}
