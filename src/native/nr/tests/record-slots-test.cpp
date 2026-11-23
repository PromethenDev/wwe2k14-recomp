// Night 11-18: RecordQueue run-time capacity (SetCapacity) under random changes: order kept, no unconsumed slot
// overwritten (each slot carries its sequence; the consumer checks it), Sync drains everything.
#include "../nr_record_queue.h"
#include <cstdio>
#include <random>
struct Item { uint64_t seq = 0; uint64_t check = 0; };
int main() {
  using namespace nr;
  uint64_t expect = 0, bad = 0;
  std::atomic<uint64_t> max_out{0};
  RecordQueue<Item, 512> q([&](Item& it) {
    if (it.seq != expect || it.check != it.seq * 2654435761u) ++bad;
    ++expect;
  });
  std::mt19937 rng(7);
  for (uint64_t i = 0; i < 2000000; ++i) {
    if ((i & 1023) == 0) q.SetCapacity(1u << (1 + rng() % 9));  // 2..512
    Item* it = q.Acquire();
    it->seq = i; it->check = i * 2654435761u;
    q.SubmitBatch(true, 64);
    if (rng() % 5000 == 0) q.Sync();
  }
  q.Sync();
  std::printf("items=%llu consumed=%llu bad=%llu %s\n", 2000000ull, (unsigned long long)expect,
              (unsigned long long)bad, bad == 0 && expect == 2000000 ? "PASS" : "FAIL");
  return bad == 0 && expect == 2000000 ? 0 : 1;
}
