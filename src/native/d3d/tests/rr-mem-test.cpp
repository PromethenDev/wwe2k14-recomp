// RR1 host test: guest-memory recording (rr_mem_core.h). A random "game" writes two guest spaces while a "worker" reads
// ranges at increasing event seqs and the write-watch fires invalidations. The recorder (PageDedupe + Encode*) writes
// records into one byte stream; the stream is decoded, sorted by seq as sidefile::Reader does, and replayed into a
// sparse image with Applier. At every recorded read the replayed bytes of the read range must equal what the live worker
// read, and every invalidation must arrive before the first event after it. Dedupe: an unchanged page is written once.
// Planted bugs (each must be caught): 1 = apply pages with seq < instead of <=, 2 = only the first record of each page
// survives (a changed page is never rewritten), 3 = the last page of a read span is not recorded.
// Build + run: tools/nr/rr-mem-test.sh [cases]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

#include "native/d3d/rr_mem_core.h"

using namespace wwe2k14::d3d::rr;

namespace {

constexpr uint32_t kPages = 64;  // per space (small so pages are reused and rewritten often)
constexpr uint32_t kBytes = kPages * kPageBytes;

struct Live {
  std::vector<uint8_t> mem[2];
  Live() {
    mem[0].assign(kBytes, 0);
    mem[1].assign(kBytes, 0);
  }
};

struct ReadCheck {
  uint32_t seq, space, addr, len;
  std::vector<uint8_t> bytes;
};

// Returns the number of mismatches (0 = pass). `bug` selects a planted bug.
uint64_t RunCase(std::mt19937_64& rng, int bug, uint64_t* pages_written, uint64_t* pages_unchanged) {
  Live live;
  PageDedupe dedupe;
  std::vector<uint8_t> stream;
  std::vector<ReadCheck> checks;
  std::vector<InvRecord> live_inv;
  const uint32_t events = 8 + uint32_t(rng() % 120);
  // Base offsets: virtual space addresses start at 0x40000000 (a heap), physical at 0 (small physical addresses).
  const uint32_t base[2] = {0x40000000u, 0x00100000u};
  for (uint32_t seq = 1; seq <= events; ++seq) {
    // The game writes between events (sometimes nothing, sometimes a page-straddling block).
    const int writes = int(rng() % 4);
    for (int w = 0; w < writes; ++w) {
      const uint32_t sp = uint32_t(rng() & 1);
      const uint32_t off = uint32_t(rng() % kBytes), len = std::min<uint32_t>(1 + uint32_t(rng() % 6000), kBytes - off);
      for (uint32_t i = 0; i < len; ++i) live.mem[sp][off + i] = uint8_t(rng());
      if (sp == kPhysical && (rng() & 1)) {  // the write-watch saw it (any thread): tagged with the worker's seq
        InvRecord v{seq - 1, base[1] + off, len};
        live_inv.push_back(v);
        EncodeInv(&stream, v.seq, v.start, v.length);
      }
    }
    // The worker translates event `seq`: a few reads.
    const int reads = int(rng() % 5);
    for (int r = 0; r < reads; ++r) {
      const uint32_t sp = uint32_t(rng() & 1);
      const uint32_t off = uint32_t(rng() % kBytes), len = std::min<uint32_t>(1 + uint32_t(rng() % 9000), kBytes - off);
      const uint32_t addr = base[sp] + off;
      ReadCheck c{seq, sp, addr, len, std::vector<uint8_t>(live.mem[sp].begin() + off, live.mem[sp].begin() + off + len)};
      checks.push_back(std::move(c));
      uint64_t rec_len = len;
      if (bug == 3) {  // last page of the span lost
        const uint64_t first_page = addr & ~uint64_t(kPageBytes - 1), last_page = (uint64_t(addr) + len - 1) & ~uint64_t(kPageBytes - 1);
        if (last_page > first_page) rec_len = last_page - addr;
      }
      dedupe.Touch(sp, addr, rec_len,
                   [&](uint32_t s, uint32_t p) -> const uint8_t* {
                     if (p < base[s] || p - base[s] >= kBytes) return nullptr;
                     return live.mem[s].data() + (p - base[s]);
                   },
                   [&](uint32_t s, uint32_t p, const uint8_t* bytes) { EncodeMem(&stream, seq, s, p, bytes); });
    }
  }
  // Decode (as sidefile::Reader: records in order, then stable sort by seq).
  std::vector<MemRecord> mem;
  std::vector<InvRecord> inv;
  {
    size_t off = 0;
    std::map<uint64_t, int> written_once;
    while (off + 8 <= stream.size()) {
      const uint32_t tag = Get32(stream.data() + off), body = Get32(stream.data() + off + 4);
      const uint8_t* b = stream.data() + off + 8;
      if (tag == kMemTag) {
        MemRecord m;
        if (!DecodeMem(b, body, &m)) return 1000000;
        const uint64_t key = (uint64_t(m.space) << 32) | m.page_addr;
        if (bug == 2 && written_once[key]++) {  // planted: later content of a page is lost
          off += 8 + body;
          continue;
        }
        mem.push_back(m);
      } else if (tag == kInvTag) {
        InvRecord v;
        if (!DecodeInv(b, body, &v)) return 1000000;
        inv.push_back(v);
      } else {
        return 1000000;
      }
      off += 8 + body;
    }
    std::stable_sort(mem.begin(), mem.end(), [](const MemRecord& a, const MemRecord& b) { return a.seq < b.seq; });
    std::stable_sort(inv.begin(), inv.end(), [](const InvRecord& a, const InvRecord& b) { return a.seq < b.seq; });
  }
  // Replay.
  std::vector<uint8_t> img[2] = {std::vector<uint8_t>(kBytes, 0xCD), std::vector<uint8_t>(kBytes, 0xCD)};
  Applier ap(&mem, &inv);
  std::vector<InvRecord> replay_inv;
  std::vector<uint32_t> inv_seen_before;  // seq before which each invalidation was applied
  uint64_t bad = 0;
  size_t ci = 0;
  for (uint32_t seq = 1; seq <= events; ++seq) {
    const uint32_t apply_seq = bug == 1 ? seq - 1 : seq;
    ap.Before(apply_seq,
              [&](uint32_t s, uint32_t p, const uint8_t* bytes) {
                std::copy(bytes, bytes + kPageBytes, img[s].begin() + (p - base[s]));
              },
              [&](uint32_t start, uint32_t length) {
                replay_inv.push_back({seq, start, length});
              });
    for (; ci < checks.size() && checks[ci].seq == seq; ++ci) {
      const ReadCheck& c = checks[ci];
      const uint8_t* got = img[c.space].data() + (c.addr - base[c.space]);
      if (!std::equal(c.bytes.begin(), c.bytes.end(), got)) ++bad;
    }
  }
  // Invalidations: same count, same order, each applied before the first event after the one it was tagged with.
  if (bug == 0 || bug == 1) {
    if (replay_inv.size() != live_inv.size()) ++bad;
    for (size_t i = 0; i < std::min(replay_inv.size(), live_inv.size()); ++i) {
      if (replay_inv[i].start != live_inv[i].start || replay_inv[i].length != live_inv[i].length) ++bad;
      if (replay_inv[i].seq != live_inv[i].seq + 1 && !(live_inv[i].seq + 1 > events)) ++bad;
    }
  }
  *pages_written += dedupe.written();
  *pages_unchanged += dedupe.unchanged();
  return bad;
}

}  // namespace

int main(int argc, char** argv) {
  const uint64_t cases = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 20000;
  int fails = 0;
  // Dedupe sanity: the same unchanged page read twice is written once; after a change it is written again.
  {
    std::vector<uint8_t> m(2 * kPageBytes, 7);
    PageDedupe d;
    int emits = 0;
    auto pp = [&](uint32_t, uint32_t p) -> const uint8_t* { return p < m.size() ? m.data() + p : nullptr; };
    auto em = [&](uint32_t, uint32_t, const uint8_t*) { ++emits; };
    d.Touch(kPhysical, 100, 5000, pp, em);  // pages 0 and 1
    d.Touch(kPhysical, 4000, 10, pp, em);   // page 0 again, unchanged
    m[4100] = 9;
    d.Touch(kPhysical, 4096, 4, pp, em);  // page 1 changed
    if (emits != 3 || d.unchanged() != 1) {
      std::printf("FAIL dedupe sanity: emits=%d unchanged=%llu\n", emits, (unsigned long long)d.unchanged());
      ++fails;
    }
    uint32_t sp, a;
    if (!Normalize(0xA0012345u, &sp, &a) || sp != kPhysical || a != 0x12345u || !Normalize(0xE1000000u, &sp, &a) ||
        a != 0x01001000u || !Normalize(0xC0002000u, &sp, &a) || a != 0x2000u || !Normalize(0x82000000u, &sp, &a) || sp != kVirtual || Normalize(0x10u, &sp, &a)) {
      std::printf("FAIL normalize\n");
      ++fails;
    }
  }
  for (int bug = 0; bug <= 3; ++bug) {
    std::mt19937_64 rng(0x52523131ull + uint64_t(bug));
    uint64_t bad = 0, failing_cases = 0, written = 0, unchanged = 0;
    for (uint64_t i = 0; i < cases; ++i) {
      const uint64_t b = RunCase(rng, bug, &written, &unchanged);
      bad += b;
      failing_cases += b != 0;
    }
    if (bug == 0) {
      std::printf("rr-mem: %llu cases, %llu mismatching reads (%llu failing cases); pages written %llu, deduped %llu\n",
                  (unsigned long long)cases, (unsigned long long)bad, (unsigned long long)failing_cases,
                  (unsigned long long)written, (unsigned long long)unchanged);
      if (bad) ++fails;
      if (!unchanged) {
        std::printf("FAIL: dedupe never skipped a page\n");
        ++fails;
      }
    } else {
      std::printf("planted bug %d: %s (%llu failing cases)\n", bug, failing_cases ? "caught" : "NOT CAUGHT",
                  (unsigned long long)failing_cases);
      if (!failing_cases) ++fails;
    }
  }
  std::printf(fails ? "RR_MEM_TEST_FAIL\n" : "RR_MEM_TEST_PASS\n");
  return fails ? 1 : 0;
}
