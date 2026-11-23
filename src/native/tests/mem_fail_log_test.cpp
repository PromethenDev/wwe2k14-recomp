// Host test for src/mem_fail_log.h (T2 MEM-NEVERFAIL): guest call-chain walk, once-per-caller registry, chain format,
// below-pool span and lender refusals. Fault injection: synthetic guest stacks with broken links, a registry driven to
// its cap, lender states that must refuse.
// Build: g++ -std=c++20 -O1 -Wall -Wextra -I src src/native/tests/mem_fail_log_test.cpp -o /tmp/mflt && /tmp/mflt
#include "../../mem_fail_log.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

namespace fl = wwe2k14::mem::faillog;

namespace {
// Guest memory: word map (big-endian value semantics are the loader's job; here words are stored as values).
struct Guest {
  std::map<uint32_t, uint32_t> w;
  uint32_t lo = 0, hi = 0;  // readable range [lo, hi)
  uint32_t Load(uint32_t a) const {
    auto it = w.find(a);
    return it == w.end() ? 0 : it->second;
  }
  bool Readable(uint32_t a, uint32_t n) const { return a >= lo && a + n <= hi; }
  // Push a frame: callee stack pointer sp whose back chain is `back`, and the caller saved LR `saved` at back - 8.
  void Frame(uint32_t sp, uint32_t back, uint32_t saved) {
    w[sp] = back;
    w[back - 8] = saved;
  }
};

fl::Chain WalkG(const Guest& g, uint32_t lr, uint32_t sp) {
  return fl::Walk(lr, sp, [&](uint32_t a) { return g.Load(a); }, [&](uint32_t a, uint32_t n) { return g.Readable(a, n); });
}
}  // namespace

int main() {
  // 1. Normal chain: allocator called from F (sp 0x70001000), F from G (0x70001100), G from H (0x70001200), H from I.
  {
    Guest g;
    g.lo = 0x70000000;
    g.hi = 0x70010000;
    g.Frame(0x70001000, 0x70001100, 0x82BDB0F4);  // F's saved LR (return into G)
    g.Frame(0x70001100, 0x70001200, 0x8279AA60);  // G's
    g.Frame(0x70001200, 0x70001300, 0x82600010);  // H's
    g.Frame(0x70001300, 0x70001400, 0x82500020);  // I's
    const fl::Chain c = WalkG(g, 0x82250080, 0x70001000);
    assert(c.n == fl::kFrames);
    assert(c.ret[0] == 0x82250080 && c.ret[1] == 0x82BDB0F4 && c.ret[2] == 0x8279AA60 && c.ret[3] == 0x82600010 &&
           c.ret[4] == 0x82500020);
    char buf[16 * fl::kFrames];
    fl::FormatChain(c, buf, sizeof(buf));
    assert(std::strcmp(buf, "82250080<-82BDB0F4<-8279AA60<-82600010<-82500020") == 0);
    char small[20];  // stops before a frame that would not fit; never overruns
    fl::FormatChain(c, small, sizeof(small));
    assert(std::strcmp(small, "82250080<-82BDB0F4") == 0);
  }
  // 2. Broken links stop the walk, never read outside the readable range.
  {
    Guest g;
    g.lo = 0x70000000;
    g.hi = 0x70002000;
    g.Frame(0x70001000, 0x70001100, 0x82BDB0F4);
    g.w[0x70001100] = 0x70001100;  // back chain not increasing -> stop after frame 1
    assert(WalkG(g, 0x82250080, 0x70001000).n == 2);
    g.w[0x70001100] = 0x70001100 + (2u << 20);  // jump > 1 MiB -> stop
    assert(WalkG(g, 0x82250080, 0x70001000).n == 2);
    g.w[0x70001100] = 0x70001FFE;  // misaligned
    assert(WalkG(g, 0x82250080, 0x70001000).n == 2);
    g.w[0x70001100] = 0x70003000;  // saved-LR slot not readable
    assert(WalkG(g, 0x82250080, 0x70001000).n == 2);
    g.w[0x70001100] = 0x70001200;
    g.w[0x70001200 - 8] = 0x12345678;  // not a code address
    assert(WalkG(g, 0x82250080, 0x70001000).n == 2);
    g.w[0x70001200 - 8] = 0x82000002;  // misaligned code address
    assert(WalkG(g, 0x82250080, 0x70001000).n == 2);
    assert(WalkG(g, 0x82250080, 0).n == 1);           // no stack pointer
    assert(WalkG(g, 0x82250080, 0x60000000).n == 1);  // unreadable stack pointer
  }
  // 3. Registry: once per (kind, caller); logs on 1, 2, 4, 8; cap on distinct callers.
  {
    fl::Registry r;
    std::vector<uint32_t> logged;
    for (int i = 0; i < 20; ++i) {
      const uint32_t n = r.Hit(1, 0x82C58500);
      if (fl::LogThisHit(n)) logged.push_back(n);
    }
    assert((logged == std::vector<uint32_t>{1, 2, 4, 8, 16}));
    assert(r.Hit(2, 0x82C58500) == 1);  // other kind, same caller = new key
    assert(r.distinct() == 2);
    for (uint32_t i = 0; r.distinct() < fl::Registry::kMaxKeys; ++i) r.Hit(3, 0x82000000 + 4 * i);
    assert(r.Hit(4, 0x83000000) == 0 && r.overflow() == 1);  // full: counted, not logged
    assert(!fl::LogThisHit(0));
    assert(r.Hit(1, 0x82C58500) == 21);  // known callers keep counting when full
  }
  // 4. Registry under threads: exactly one first hit per caller.
  {
    fl::Registry r;
    std::atomic<int> firsts{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
      ts.emplace_back([&] {
        for (uint32_t c = 0; c < 200; ++c)
          if (r.Hit(7, 0x82000000 + 4 * c) == 1) firsts.fetch_add(1);
      });
    for (auto& t : ts) t.join();
    assert(firsts.load() == 200 && r.distinct() == 200);
  }
  // 5. Below-pool span = [0, lowest window start); empty windows ignored.
  {
    uint32_t lo[56] = {}, hi[56] = {};
    assert(!fl::BelowPool(lo, hi, 56).ok());  // no windows yet
    lo[0] = 0x0F800000;
    hi[0] = 0x13800000;
    lo[5] = 0x14000000;
    hi[5] = 0x142A0000;
    lo[54] = 0x1C000000;
    hi[54] = 0x1F2C8000;
    lo[17] = 0x0E000000;  // empty (hi <= lo) -> ignored
    hi[17] = 0x0E000000;
    const fl::Span s = fl::BelowPool(lo, hi, 56);
    assert(s.ok() && s.lo == 0 && s.hi == 0x0F800000);
    lo[3] = 0;
    hi[3] = 0x100000;  // a window starting at 0 leaves nothing below
    assert(!fl::BelowPool(lo, hi, 56).ok());
  }
  // 6. Below-pool refusals (lender full): cap, reserve, fragmentation; memshare runs had 44,032 KiB free.
  {
    const uint64_t MB = 1ull << 20, freeNow = 44032ull << 10;
    assert(fl::LowRefusal(freeNow, freeNow, 4 * MB, 0) == nullptr);
    assert(fl::LowRefusal(freeNow, freeNow, 4 * MB, 21 * MB) != nullptr);        // 21 + 4 > 24 MB cap
    assert(fl::LowRefusal(freeNow, freeNow, 28 * MB + 1, 0) != nullptr);         // over the cap alone
    assert(fl::LowRefusal(20 * MB, 20 * MB, 5 * MB, 0) != nullptr);              // 20 - 5 < 16 MB reserve
    assert(fl::LowRefusal(20 * MB, 20 * MB, 4 * MB, 0) == nullptr);              // exactly the reserve left
    assert(fl::LowRefusal(freeNow, 2 * MB, 3 * MB, 0) != nullptr);               // no run large enough
  }
  std::puts("mem_fail_log_test: PASS (walk 9 cases, registry 3, below-pool 3, refusals 6)");
  return 0;
}
