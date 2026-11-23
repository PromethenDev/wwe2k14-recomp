// H1e host test: g++ -std=c++20 -I src src/native/tests/h1-cache-copy-test.cpp -fsanitize=address,undefined -o t && ./t
#include <cassert>
#include <cstdio>
#include <vector>
#include "h1_cache_copy_core.h"
using namespace wwe2k14::h1e;
struct Fake {  // heap with evictable blocks; blocked = not evictable (the read buffer in the hang)
  uint64_t total;
  std::vector<uint64_t> evictable;
  uint64_t blocked;
  int allocs = 0, evicts = 0;
  bool evict_fails = false;
  uint32_t Alloc(uint32_t size, uint32_t) {
    ++allocs;
    uint64_t used = blocked;
    for (auto e : evictable) used += e;
    return used + size <= total ? 0x1000 : 0;
  }
  int32_t EvictableCount() { return int32_t(evictable.size()); }
  int32_t EvictOne() {
    if (evict_fails) return 0;
    ++evicts;
    evictable.pop_back();
    return 1;
  }
};
int main() {
  Fake fits{11534336, {}, 0};
  assert(TryAllocNoWait(fits, 11247616, kCopyAlign) == 0x1000 && fits.allocs == 1);
  Fake evict{11534336, {4000000, 4000000}, 0};
  assert(TryAllocNoWait(evict, 11247616, kCopyAlign) == 0x1000 && evict.evicts == 2);
  Fake hang{11534336, {}, 1501200};  // the logged hang: must return, never wait
  assert(TryAllocNoWait(hang, 11247616, kCopyAlign) == 0 && hang.allocs == 1);
  Fake partial{11534336, {500000}, 1501200};  // evict all it can, then skip
  assert(TryAllocNoWait(partial, 11247616, kCopyAlign) == 0 && partial.evicts == 1 && partial.allocs == 2);
  Fake spin{11534336, {500000}, 1501200};  // the original would spin: stop instead
  spin.evict_fails = true;
  assert(TryAllocNoWait(spin, 11247616, kCopyAlign) == 0);
  assert(CopySize(0x0000ABA0) == 0x00ABA000u && CopySize(0x01000000) == 0);
  printf("h1-cache-copy-test PASS\n");
}
