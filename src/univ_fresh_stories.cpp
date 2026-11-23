// Universe "fresh stories" (owner 2026-11-12: "more variety, not the same matches/cutscenes all the time").
// "N:line" = generated/wwe2k14_recomp.N.cpp.
//
// Stock picker: sub_82B85BE8 (74:44887) walks every story template, keeps each castable (template, cast) pair in a
// heap vector r30 = {u32 count, u32 cap, 32-byte entries from +8}, entry = 24-byte cast + u32 story id at +24, then
// calls the shared RNG sub_82C425C8 (return address 0x82B85DF4) and takes entry [rand % count] (divwu with r31 = count).
// It keeps no history, and a template with many possible casts owns many entries, so the same stories come back.
//
// This hook answers ONLY that one RNG call (identified by its return address; the other ~80 callers get the stock RNG):
//   off (stock):  RNG unchanged; the pick is logged.
//   on  (WWE2K14_UNIV_FRESH_STORIES=1): choose a story id evenly among the distinct ids NOT in the last kRecent picks
//        (all ids if every one is recent), then one of its casts; return that entry index (index < count, so the
//        game's own "% count" keeps it). The game's own random value drives both choices.
// History lives in memory only (cleared when the game restarts).
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
constexpr uint32_t kStorySite = 0x82B85DF4;  // return address of 'bl 0x82c425c8' in sub_82B85BE8
constexpr uint32_t kMaxCandidates = 4096;
constexpr size_t kRecent = 16;

bool On() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_UNIV_FRESH_STORIES");
    const bool v = e && e[0] == '1' && e[1] == '\0';
    REXLOG_INFO("[univ-story] fresh stories {}", v ? "ON (recent stories skipped, one chance per story)" : "off (stock)");
    return v;
  }();
  return on;
}

#if !defined(WWE2K14_NO_TEST_TOOLS)
// TEST-ONLY (UNIV-2K15 stories, 2026-11-14): WWE2K14_TEST_UNIV_PREFER=<id,id,...> makes the picker take a listed story
// whenever one is among the castable candidates, so one Universe run shows whether added story templates play.
const std::vector<uint32_t>& Prefer() {
  static const std::vector<uint32_t> ids = [] {
    std::vector<uint32_t> v; const char* e = std::getenv("WWE2K14_TEST_UNIV_PREFER");
    for (const char* p = e; p && *p;) { char* end = nullptr; unsigned long x = std::strtoul(p, &end, 10);
      if (end == p) break; v.push_back(static_cast<uint32_t>(x)); p = (*end == ',') ? end + 1 : end; }
    if (!v.empty()) REXLOG_INFO("[univ-story] TEST prefer list: {} ids", v.size());
    return v;
  }();
  return ids;
}
#endif

std::mutex g_mu;
std::vector<uint32_t> g_recent;  // newest last
uint32_t g_picks = 0;

bool Recent(uint32_t id) {
  for (uint32_t r : g_recent) if (r == id) return true;
  return false;
}
}  // namespace

namespace wwe2k14::guest_watch {
void Note(uint8_t* base);  // test_guest_watch.cpp (test builds only; no-op otherwise)
void Find(uint8_t* base, const char* tag);
}

extern "C" {
PPC_FUNC(__imp__sub_82C425C8);
PPC_FUNC(sub_82C425C8);
}

extern "C" PPC_FUNC(sub_82C425C8) {
  wwe2k14::guest_watch::Note(base);
  const uint32_t site = static_cast<uint32_t>(ctx.lr);
  const uint32_t vec = ctx.r30.u32, count = ctx.r31.u32;
  __imp__sub_82C425C8(ctx, base);
  if (site != kStorySite) return;
  if (count == 0 || count > kMaxCandidates || vec < 0x10000 || PPC_LOAD_U32(vec) != count) return;  // not the shape we know
  const uint32_t rnd = ctx.r3.u32;
  auto id_at = [&](uint32_t i) { return PPC_LOAD_U32(vec + 8 + i * 32 + 24); };

  std::lock_guard<std::mutex> lock(g_mu);
  std::vector<uint32_t> ids;  // distinct story ids, first-seen order
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = id_at(i);
    bool seen = false;
    for (uint32_t x : ids) if (x == id) { seen = true; break; }
    if (!seen) ids.push_back(id);
  }
  uint32_t pick = rnd % count;  // stock choice
  uint32_t fresh = 0;
  for (uint32_t x : ids) if (!Recent(x)) ++fresh;
  const char* mode = "stock";
  if (On()) {
    std::vector<uint32_t> pool;
    for (uint32_t x : ids) if (fresh == 0 || !Recent(x)) pool.push_back(x);
    const uint32_t want = pool[rnd % pool.size()];
    std::vector<uint32_t> casts;
    for (uint32_t i = 0; i < count; ++i) if (id_at(i) == want) casts.push_back(i);
    pick = casts[(rnd / static_cast<uint32_t>(pool.size())) % casts.size()];
    ctx.r3.u64 = pick;
    mode = fresh ? "fresh" : "fresh-all-recent";
  }
#if !defined(WWE2K14_NO_TEST_TOOLS)
  if (!Prefer().empty()) {
    std::vector<uint32_t> hits;
    for (uint32_t i = 0; i < count; ++i)
      for (uint32_t x : Prefer()) if (id_at(i) == x) { hits.push_back(i); break; }
    if (!hits.empty()) { pick = hits[rnd % hits.size()]; ctx.r3.u64 = pick; mode = "test-prefer"; }
  }
#endif
  const uint32_t chosen = id_at(pick);
  const bool was_recent = Recent(chosen);
  g_recent.push_back(chosen);
  if (g_recent.size() > kRecent) g_recent.erase(g_recent.begin());
  ++g_picks;
  wwe2k14::guest_watch::Find(base, "after-story-pick");
  REXLOG_INFO("[univ-story] pick #{} story={} candidates={} stories={} not-recent={} repeat-of-recent={} mode={}", g_picks,
              chosen, count, ids.size(), fresh, was_recent ? 1 : 0, mode);
}
