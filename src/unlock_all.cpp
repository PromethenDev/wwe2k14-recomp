#include <chrono>
#include <cstdlib>
// TEST-ONLY "unlock everything" switch (WWE2K14_UNLOCK_ALL=1, default OFF).
//
// All profile unlock data lives in one block of the in-memory save profile. sub_827DAAA0 is its trivial getter
// (lis r11,0x8377; lwz r11,-0xbf0(r11); addis r3,r11,7; addi r3,r3,0x5e44; blr), i.e. block = *(0x8376F410) +
// 0x75E44; 27 guest functions call it, including the 30 Years of WrestleMania timeline menu (sub_82851D08, the
// per-slot icon chooser, copies 0xe38 bytes of the block and reads three 80-word arrays at +0x64 / +0x1a4 / +0x2e4,
// picking the TLicon_WMxx _unlock/_lock/_comp/_clear image) and the Unlockables screen (sub_82879A10 / sub_8287B8F0:
// 256 words at +0x524, index = item id, 0 = locked, 1 = unlocked/new, 2 = seen).
//
// With the switch on, every call of the getter first marks the 30 Years slots (80 = 5 eras x 16) available and
// unlocked and all Unlockables items unlocked in the live profile memory. Completion words (+0x2e4) are left alone.
//
// Eras 2-5 of the 30 Years carousel are NOT gated by those arrays (forcing +0x64/+0x1a4, and +0x2e4 as a test, left
// them locked: logs/u3-unlock, logs/u6-era). The carousel init sub_82877550 fills its per-era flags (obj+396) from the
// generic unlock query sub_822BCC48(id) with ids {196 (era 1, always open), 166, 167, 168, 169}; the draw code
// sub_82877F60 shows the LOCKED widget when the flag is 0. With the switch on, the query answers "unlocked" for 166-169
// only (no save data is written by this part).
// CAVEAT: the values live in the real in-memory profile, so a later game save writes them to SaveData.Dat. Use a
// copy of the userdata folder for testing.

#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <set>
#include <tuple>
#include <map>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
// Diagnostic (WWE2K14_UNLOCK_TRACE=1, default OFF): log each distinct caller of the unlock-block getter and each
// distinct (caller, lock id, result) of the menu lock test sub_8283ED38 (r4 = lock id, returns 1 = locked, 0 =
// unlocked), to find which code gates the 30 Years era carousel (the per-slot arrays above do not).
bool TraceOn() {
#ifdef WWE2K14_NO_TEST_TOOLS
  return false;  // public builds: test diagnostic compiled out
#endif
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_UNLOCK_TRACE");
    return v && v[0] == '1';
  }();
  return on;
}
std::mutex g_trace_mutex;
}  // namespace

extern "C" PPC_FUNC(__imp__sub_8283ED38);
extern "C" PPC_FUNC(sub_8283ED38) {
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint32_t lock_id = ctx.r4.u32;
  __imp__sub_8283ED38(ctx, base);
  if (!TraceOn()) return;
  static std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
  std::lock_guard<std::mutex> lock(g_trace_mutex);
  if (seen.emplace(caller, lock_id, ctx.r3.u32).second)
    REXLOG_INFO("[unlock-trace] lock-test caller=0x{:08X} id={} result={}", caller, lock_id, ctx.r3.u32);
}

static bool UnlockAllOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_UNLOCK_ALL");
    const bool on = v && v[0] == '1';
    if (on) REXLOG_INFO("[unlock-all] WWE2K14_UNLOCK_ALL=1: 30 Years eras/slots + Unlockables forced unlocked (test only)");
    return on;
  }();
  return on;
}

extern "C" PPC_FUNC(__imp__sub_822BCC48);
extern "C" PPC_FUNC(sub_822BCC48) {
  const uint32_t id = ctx.r3.u32;
  if (UnlockAllOn() && id >= 166 && id <= 169) {  // 30 Years eras 2-5
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_822BCC48(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_827DAAA0);
extern "C" PPC_FUNC(sub_827DAAA0) {
  if (TraceOn()) {
    static std::set<uint32_t> seen;
    const uint32_t caller = static_cast<uint32_t>(ctx.lr);
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    if (seen.insert(caller).second) REXLOG_INFO("[unlock-trace] getter caller=0x{:08X}", caller);
  }
  __imp__sub_827DAAA0(ctx, base);
  if (!UnlockAllOn()) return;
  const uint32_t blk = ctx.r3.u32;
  if (blk < 0x82000000u) return;  // profile base pointer not set yet (block would be 0x75E44)
  for (uint32_t i = 0; i < 80; ++i) {
    PPC_STORE_U32(blk + 0x64 + 4 * i, 1);   // unlocked
    PPC_STORE_U32(blk + 0x1a4 + 4 * i, 1);  // available
  }
  for (uint32_t i = 0; i < 256; ++i) {
    if (PPC_LOAD_U32(blk + 0x524 + 4 * i) == 0) PPC_STORE_U32(blk + 0x524 + 4 * i, 1);
  }
}

// ======================================================================================================================
// Mod support: own roster slots for imported wrestlers, read ONCE at startup from own-slots.txt next to wwe2k14.exe (one decimal id
// per line, '#' comments, valid 100..499). No file or no valid id: every hook below is inert. WWE2K14_OWN_SLOTS=0 turns it off.
// FACT (static + launch o7, out/worker-reports/20261025/booker-shane.md): the chEtc character-database loader (sub_8342EF68) reads a
// row for an id only when sub_822C1CC0(id) == 1, i.e. when the static byte table at 0x835DE760 holds 1 for that id; the retail image
// has 0 for the own ids. The hook below answers 1 for exactly the listed ids.
// ======================================================================================================================
#include <filesystem>
#include <algorithm>
#include "own_slots_parse.h"
#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void*, wchar_t*, unsigned long);
#endif
namespace {
std::vector<uint32_t> g_ownIds;  // written once inside OwnSlotsOn()'s static initialiser, read-only afterwards

bool OwnSlotsOn() {
  static const bool on = [] {
    if (const char* e = std::getenv("WWE2K14_OWN_SLOTS")) {
      if (e[0] == '0') return false;
    }
    std::error_code ec;
    std::filesystem::path dir;
#if defined(_WIN32)
    wchar_t buf[1024] = {};
    const unsigned long n = GetModuleFileNameW(nullptr, buf, 1023);
    if (n > 0 && n < 1023) dir = std::filesystem::path(buf).parent_path();
#else
    dir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();  // next to the game, as on Windows
#endif
    g_ownIds = ownslots::LoadFile(dir / "own-slots.txt");
    if (g_ownIds.empty()) return false;
    std::string list;
    for (uint32_t id : g_ownIds) list += (list.empty() ? "" : ",") + std::to_string(id);
    REXLOG_INFO("[own-slots] {} ids: {}", g_ownIds.size(), list);
    return true;
  }();
  return on;
}
inline bool IsOwnId(uint32_t id) {
  return std::find(g_ownIds.begin(), g_ownIds.end(), id) != g_ownIds.end();
}
}  // namespace

extern "C" PPC_FUNC(__imp__sub_822C1CC0);
extern "C" PPC_FUNC(sub_822C1CC0) {
  const uint32_t id = ctx.r3.u32;
  __imp__sub_822C1CC0(ctx, base);
  if (OwnSlotsOn() && IsOwnId(id)) ctx.r3.u64 = 1;
}

// Save-independence (launch o14 on the owner's real save): the profile load is one memcpy of the whole 1189036-byte in-memory profile
// (dest = *(0x8376F410) = 0x8376F41C, source = the loaded SaveData copy, caller 0x8290F674). It overwrites the 204-byte character
// records with the saved ones, which are all zero for own ids in a save that never had them, so the own slots vanish again after
// the DLC rows were loaded. Keep the live record of slot (id-100) across that one copy when the saved record is empty.

namespace ownrow {
// Row keeper. FACT (launch y1): the 1272-byte PRO row of an own id (0x838918CC + idx*1272, sub_827DC258) is filled correctly by the
// PRO loaders at boot but is ALL ZERO at match setup. Keep the good boot copy per id and put it back when the row is found empty
// (never overwrites a non-empty row).
std::mutex g_mu;
std::map<uint32_t, std::vector<uint8_t>> g_good;
std::set<uint32_t> g_saidSave, g_saidRestore;
inline bool RowNonZero(uint8_t* base, uint32_t row) {
  for (uint32_t i = 116; i < 404; ++i) if (PPC_LOAD_U8(row + i)) return true;
  return false;
}
// A stock placeholder over an imported row (P15MUS-3, 11-16): at match setup the game rewrites some own slots with WWE 2K14's
// own stub row for that slot instead of zeroing them (e.g. 476: entrance music 255 = file offset 576, here 572). The entrance
// then posts Play_MUS_0255 instead of the wrestler's theme. Treat it like an empty row: the boot copy goes back.
inline bool RowIsStockStub(uint8_t* base, uint32_t row, const std::vector<uint8_t>& good) {
  if (good.size() < 1272) return false;
  const uint16_t now = PPC_LOAD_U16(row + 572), boot = uint16_t((uint32_t(good[572]) << 8) | good[573]);
  return now == 255 && boot != 255;
}
std::map<uint32_t, uint32_t> g_rowAddr;
uint64_t g_sweepFixed = 0;
// Other code reads these rows directly, not through sub_827DC258 (run sweep2: 99 of 106 empty before a match), so keep every own row
// filled while mods are on. WWE2K14_OWN_SWEEP=0 turns it off.
inline void SweepAll(uint8_t* base) {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_OWN_SWEEP");
    return !(e && e[0] == '0');
  }();
  if (!on) return;
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::milliseconds(250)) return;
  last = now;
  for (auto& [id, g] : g_good) {
    const auto it = g_rowAddr.find(id);
    if (g.empty() || it == g_rowAddr.end() || (RowNonZero(base, it->second) && !RowIsStockStub(base, it->second, g))) continue;
    for (uint32_t i = 0; i < 1272; ++i) PPC_STORE_U8(it->second + i, g[i]);
    if (g_sweepFixed++ < 400) REXLOG_INFO("[own-slots] sweep refilled id {} (row=0x{:08X})", id, it->second);
  }
}
inline void KeepRow(uint8_t* base, uint32_t id, uint32_t row) {
  if (!IsOwnId(id) || row < 0x10000u || row >= 0xC0000000u) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_rowAddr[id] = row;
  SweepAll(base);
  auto& g = g_good[id];
  if (RowNonZero(base, row) && !RowIsStockStub(base, row, g)) {
    if (g.empty()) {
      g.resize(1272);
      for (uint32_t i = 0; i < 1272; ++i) g[i] = PPC_LOAD_U8(row + i);
      if (g_saidSave.insert(id).second) REXLOG_INFO("[own-slots] id {}: saved good PRO row (row=0x{:08X})", id, row);
    }
  } else if (!g.empty()) {
    for (uint32_t i = 0; i < 1272; ++i) PPC_STORE_U8(row + i, g[i]);
    if (g_saidRestore.insert(id).second) REXLOG_INFO("[own-slots] id {}: PRO row was empty or a stock stub, restored from boot copy (row=0x{:08X})", id, row);
  }
}
}  // namespace ownrow

extern "C" PPC_FUNC(__imp__sub_82C7C8F0);
extern "C" PPC_FUNC(sub_82C7C8F0) {
  if (OwnSlotsOn() && !g_ownIds.empty()) {
    const uint32_t dest = ctx.r3.u32, size = ctx.r5.u32;
    const uint32_t prof = PPC_LOAD_U32(0x8376F410);
    if (size == 1189036u || (prof != 0 && dest >= prof && dest < prof + 1189036u && size > 4096)) {
      const bool exact = (size == 1189036u && dest == prof && dest != 0);
      const size_t n = g_ownIds.size();
      std::vector<uint8_t> keep(exact ? n * 204 : 0);
      if (exact)
        for (size_t k = 0; k < n; ++k)
          for (int i = 0; i < 204; ++i) keep[k * 204 + i] = PPC_LOAD_U8(dest + (g_ownIds[k] - 100u) * 204 + i);
      __imp__sub_82C7C8F0(ctx, base);
      if (exact) {
        for (size_t k = 0; k < n; ++k) {
          const uint32_t a = dest + (g_ownIds[k] - 100u) * 204;
          // 'record is valid' = on the roster (+16 == 1) or an add-on record (+17 != 0). +16 added 11-15 because the
          // main-grid switch (OWN_MAIN below) clears +17 of own ids in memory.
          const bool kept_valid = keep[k * 204 + 16] == 1 || keep[k * 204 + 17] != 0;
          const bool loaded_empty = PPC_LOAD_U8(a + 16) == 0 && PPC_LOAD_U8(a + 17) == 0;
          if (kept_valid && loaded_empty)
            for (int i = 0; i < 204; ++i) PPC_STORE_U8(a + i, keep[k * 204 + i]);
        }
      }
      return;
    }
  }
  __imp__sub_82C7C8F0(ctx, base);
}

// Imports in the MAIN select grid (owner 11-15; docs/tasks/SELGRID-research.md). Record +17 (chEtc DAT byte 21) is the add-on
// pack number: != 0 sends a wrestler to the Downloaded tab (sub_8282EAB8 tests lbz 17), the Superstars tab rule
// (sub_8282AA30) does not read it. TEST SWITCH WWE2K14_OWN_MAIN=1 (default off): every record lookup of an own id that is
// on the roster (+16 == 1) gets +17 = 0. Called from the sub_827DC388 hook in universe_guard.cpp (all +17 readers use it).
namespace owngrid {
std::mutex g_mu;
std::map<uint32_t, uint8_t> g_gender;  // own id -> record +23 (0 = man, 1 = woman), seen through the record lookup
}  // namespace owngrid

void OwnMainFixRecord(uint8_t* base, uint32_t id, uint32_t rec) {
  if (id >= 100 && id < 500 && rec >= 0x10000u && rec < 0xC0000000u && OwnSlotsOn() && IsOwnId(id) &&
      PPC_LOAD_U8(rec + 16) == 1) {
    std::lock_guard<std::mutex> lk(owngrid::g_mu);
    owngrid::g_gender[id] = PPC_LOAD_U8(rec + 23);
  }
#ifdef WWE2K14_NO_TEST_TOOLS
  return;  // public builds: OWN_MAIN experiment compiled out
#endif
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_OWN_MAIN");
    const bool v = e && e[0] == '1';
    if (v) REXLOG_INFO("[own-main] WWE2K14_OWN_MAIN=1: own ids leave the Downloaded tab (record +17 = 0) (test only)");
    return v;
  }();
  if (!on || id < 100 || id >= 500 || rec < 0x10000u || rec >= 0xC0000000u || !OwnSlotsOn() || !IsOwnId(id)) return;
  if (PPC_LOAD_U8(rec + 16) != 1 || PPC_LOAD_U8(rec + 17) == 0) return;
  PPC_STORE_U8(rec + 17, 0);
  static std::mutex mu;
  static std::set<uint32_t> cleared;
  std::lock_guard<std::mutex> lk(mu);
  if (cleared.insert(id).second && (cleared.size() <= 3 || cleared.size() % 20 == 0 || cleared.size() == g_ownIds.size()))
    REXLOG_INFO("[own-main] cleared={} (last id {}, record 0x{:08X})", cleared.size(), id, rec);
}

// Imports in the main select grid, step P (owner 11-15; docs/tasks/SELGRID-layout-research.md). The Superstars/Divas grid is
// the fixed POSI table of menu/menu.pac (MEFP/POSI 'FTPS'): 23 x 4 drawn cells, 75 stock men, 17 empty (-1). Every cell is read
// through sub_8293CD90(M = r3, col = r4, row = r5): table k = M+88 (0 = Superstars, 1 = Divas), returns the id or 512 = empty.
// TEST SWITCH WWE2K14_OWN_GRID=1 (default off): an EMPTY drawn cell (row 0-3, col 0-22) answers with the next own id of the
// table's gender (men for k 0, women for k 1), in own-slots.txt order. Nothing is written to the table or the save.
namespace owngrid {
int Mode() {  // WWE2K14_OWN_GRID: 1 = imports in the empty cells + pages by cursor (player option, launcher Mods page); 2 = same
  static const int m = [] {
    const char* e = std::getenv("WWE2K14_OWN_GRID");
    int v = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
    if (v) REXLOG_INFO("[own-grid] WWE2K14_OWN_GRID={}: imported wrestlers in the main select grid", v);
    return v;
  }();
  return m;
}
int32_t StockCell(uint8_t* base, uint32_t m, uint32_t k, uint32_t col, uint32_t row) {
  const uint32_t rows = PPC_LOAD_U32(m + 52 + 12 * k), cols = PPC_LOAD_U32(m + 48 + 12 * k), rp = PPC_LOAD_U32(m + 56 + 12 * k);
  if (!rp || row >= rows || col >= cols) return -1;
  return int32_t(PPC_LOAD_U32(PPC_LOAD_U32(rp + 4 * row) + 4 * col));
}
std::vector<uint32_t> OwnOf(uint32_t k) {  // own ids of table k's gender (0 = men, 1 = women), own-slots.txt order
  std::vector<uint32_t> out;
  std::lock_guard<std::mutex> lk(g_mu);
  for (uint32_t id : g_ownIds) {
    const auto g = g_gender.find(id);
    if (g != g_gender.end() && (k == 0 ? g->second == 0 : g->second == 1)) out.push_back(id);
  }
  return out;
}
// Frozen view (owner 11-16 "scrolling skips lines"). Two causes, both fixed here:
// (a) g_gender is learned lazily from record lookups, so OwnOf() grew WHILE the grid was on screen: the faces are baked at
//     redraw (sub_82832B48), but the cursor mover / selection read the cells live, so an id learned later shifted every later
//     cell by one and the cursor no longer matched the pictures.
// (b) the game's mover sub_8282E218 lands only on a cell whose id passes sub_8282DE68(screen, id) (id < 500 and listed with a
//     non-zero flag in the screen's pick list); any other cell is passed over, and a row without one is skipped entirely.
// So: the ids per table are snapshotted when the grid is actually redrawn (latch screen+0x1B140 != 1), keeping only ids the
// mover accepts; every reader (draw, mover, id->cell, page count) uses that snapshot until the next redraw.
struct Frozen {
  uint32_t m = 0;
  bool valid = false;
  std::vector<uint32_t> own[2];
};
std::mutex g_fmu;
Frozen g_frozen;
// Read-only copy of sub_8282DE68 (48.cpp): pick list = sub+40216 (count, ids at +4, flags at +2004), or sub+16192 when that
// count is 0. Returns -1 when both lists are empty (not built yet: no filter).
int Pickable(uint8_t* base, uint32_t sub, uint32_t id) {
  uint32_t list = sub + 40216;
  if (int32_t(PPC_LOAD_U32(list)) == 0) list = sub + 16192;
  const int32_t n = int32_t(PPC_LOAD_U32(list));
  if (n <= 0 || n > 500) return -1;
  if (id >= 500) return 0;
  for (int32_t i = 0; i < n; ++i)
    if (PPC_LOAD_U32(list + 4 + 4 * i + 2000) != 0 && PPC_LOAD_U32(list + 4 + 4 * i) == id) return 1;
  return 0;
}
void Freeze(uint8_t* base, uint32_t screen, uint32_t m) {
  const uint32_t sub = PPC_LOAD_U32(screen + 16);
  Frozen f;
  f.m = m;
  f.valid = true;
  uint32_t dropped = 0;
  for (uint32_t k = 0; k < 2; ++k)
    for (uint32_t id : OwnOf(k)) {
      if (Pickable(base, sub, id) == 0) { ++dropped; continue; }
      f.own[k].push_back(id);
    }
  static std::mutex mu;
  static std::set<std::tuple<size_t, size_t, uint32_t>> said;
  {
    std::lock_guard<std::mutex> lk(mu);
    if (said.insert({f.own[0].size(), f.own[1].size(), dropped}).second && said.size() <= 40)
      REXLOG_INFO("[own-grid] view frozen: men {} women {} (not pickable, left out: {})", f.own[0].size(), f.own[1].size(),
                  dropped);
  }
  std::lock_guard<std::mutex> lk(g_fmu);
  g_frozen = std::move(f);
}
std::vector<uint32_t> ViewOwn(uint32_t m, uint32_t k) {
  {
    std::lock_guard<std::mutex> lk(g_fmu);
    if (g_frozen.valid && g_frozen.m == m) return g_frozen.own[k];
  }
  return OwnOf(k);  // before the first redraw of this grid
}
// Pages (mode 2): page 0 = stock + imports in the empty cells, page p >= 1 = the next 92 imports (row-major). Down past the
// bottom row / Up past the top row of the grid changes page (owner 11-16: "scroll up/down"); the game's Superstars/Divas
// flag (screen+0x1B208) is never touched, so the selection path keeps reading the right table (selgrid-6 lesson).
std::atomic<uint32_t> g_page{0};
uint32_t CurPage() { return Mode() ? g_page.load() : 0; }  // pages proven selgrid-9 (11-16)
// Id shown in every drawn cell of table k on `page` (row-major, 4 x 23); 512 = empty.
void ViewIds(uint8_t* base, uint32_t m, uint32_t k, uint32_t page, uint32_t out[4][23]) {
  const std::vector<uint32_t> own = ViewOwn(m, k);
  uint32_t empties = 0;
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t c = 0; c < 23; ++c)
      if (StockCell(base, m, k, c, r) == -1) ++empties;
  uint32_t nth = 0;
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t c = 0; c < 23; ++c) {
      const int32_t s = StockCell(base, m, k, c, r);
      size_t idx;
      if (page == 0) {
        if (s != -1) { out[r][c] = uint32_t(s); continue; }
        idx = nth++;
      } else {
        idx = empties + 92 * (page - 1) + r * 23 + c;
      }
      out[r][c] = idx < own.size() ? own[idx] : 512;
    }
}
uint32_t PageCount(uint8_t* base, uint32_t m, uint32_t k) {
  uint32_t empties = 0;
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t c = 0; c < 23; ++c)
      if (StockCell(base, m, k, c, r) == -1) ++empties;
  const size_t own = ViewOwn(m, k).size();
  return own > empties ? 1 + uint32_t((own - empties + 91) / 92) : 1;
}
bool GridReady(uint8_t* base, uint32_t m) { return m >= 0x10000u && m < 0xC0000000u && PPC_LOAD_U32(m + 76) == 2; }
uint32_t GridOf(uint8_t* base, uint32_t screen) {  // screen -> grid M (sub = screen+16, M = sub+56792), 0 if not ready
  if (screen < 0x10000u || screen >= 0xC0000000u) return 0;
  const uint32_t sub = PPC_LOAD_U32(screen + 16);
  if (sub < 0x10000u || sub >= 0xC0000000u) return 0;
  const uint32_t m = PPC_LOAD_U32(sub + 56792);
  return GridReady(base, m) ? m : 0;
}
thread_local bool t_frozenByMover = false;
}  // namespace owngrid

// Imports in the main select grid (owner 11-15; docs/tasks/SELGRID-layout-research.md). The Superstars/Divas grid is the fixed
// POSI table of menu/menu.pac (MEFP/POSI 'FTPS'): 23 x 4 drawn cells, 75 stock men, 17 empty (-1). Every cell is read through
// sub_8293CD90(M = r3, col = r4, row = r5): table k = M+88 (0 = Superstars, 1 = Divas), returns the id or 512 = empty.
// WWE2K14_OWN_GRID (default off). 1: an EMPTY drawn cell answers with the next own id of the table's gender. 2: also pages.
extern "C" PPC_FUNC(__imp__sub_8293CD90);
extern "C" PPC_FUNC(sub_8293CD90) {
  const uint32_t m = ctx.r3.u32, col = ctx.r4.u32, row = ctx.r5.u32;
  __imp__sub_8293CD90(ctx, base);
  if (!owngrid::Mode() || col > 22 || row > 3 || !OwnSlotsOn() || PPC_LOAD_U32(m + 76) != 2) return;
  const uint32_t k = PPC_LOAD_U32(m + 88);
  if (k > 1) return;
  const uint32_t page = owngrid::CurPage();
  if (page == 0 && ctx.r3.u32 != 512) return;
  uint32_t ids[4][23];
  owngrid::ViewIds(base, m, k, page, ids);
  const uint32_t pick = ids[row][col];
  if (page == 0 && pick == 512) return;
  ctx.r3.u64 = pick;
  if (pick == 512) return;
  static std::mutex mu;
  static std::set<uint64_t> said;
  std::lock_guard<std::mutex> lk(mu);
  if (said.insert((uint64_t(k) << 40) | (uint64_t(page) << 32) | pick).second && said.size() <= 400)
    REXLOG_INFO("[own-grid] table {} page {} cell col {} row {} -> id {}", k, page, col, row, pick);
}

// id -> cell (sub_8293CE08(M, id, &col, &row), 1 = found): used for the chosen-wrestler markers (sub_8282DAC8 area) and for
// placing the cursor on a wrestler (sub_8285C8F8; not found = cursor goes to the strip). It scans the POSI table directly,
// so it never saw imports. Answer from the current view instead: a stock id is found only on page 0, an import only on the
// page that shows it.
extern "C" PPC_FUNC(__imp__sub_8293CE08);
extern "C" PPC_FUNC(sub_8293CE08) {
  const uint32_t m = ctx.r3.u32, id = ctx.r4.u32, pc = ctx.r5.u32, pr = ctx.r6.u32;
  __imp__sub_8293CE08(ctx, base);
  if (!owngrid::Mode() || !OwnSlotsOn() || !owngrid::GridReady(base, m)) return;
  const uint32_t k = PPC_LOAD_U32(m + 88), page = owngrid::CurPage();
  if (k > 1 || (page == 0 && ctx.r3.u32 == 1)) return;  // page 0 stock hit: unchanged
  uint32_t ids[4][23];
  owngrid::ViewIds(base, m, k, page, ids);
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t c = 0; c < 23; ++c)
      if (ids[r][c] == id) {
        PPC_STORE_U32(pc, c);
        PPC_STORE_U32(pr, r);
        ctx.r3.u64 = 1;
        return;
      }
  PPC_STORE_U32(pc, 0xFFFFFFFFu);
  PPC_STORE_U32(pr, 0xFFFFFFFFu);
  ctx.r3.u64 = 0;
}

// The Superstars/Divas button and the screen setup set the shown table (stw r4,88(r3)): back to page 0, request unchanged.
extern "C" PPC_FUNC(__imp__sub_8293CED0);
extern "C" PPC_FUNC(sub_8293CED0) {
  if (owngrid::Mode() && owngrid::g_page.exchange(0) != 0)
    REXLOG_INFO("[own-grid] table set to {} -> page 0", ctx.r4.u32);
  __imp__sub_8293CED0(ctx, base);
}

// Grid cursor move: sub_8282E218(screen = r3, player = r4, r5 = {up, down}, r6 = {left, right}, r7 = &col, r8 = &row)
// (48.cpp; row 4 = the category strip, the result is also stored at screen + player*32 + 0x1B14C/0x1B150). Mode 2: Down
// from row 3 with a next page -> next page, row 0; Up from row 0 on page > 0 -> previous page, row 3. Then the grid is
// redrawn with sub_82832B48(screen), as the Superstars/Divas button does.
// Grid redraw sub_82832B48(screen): skips while its "drawn" latch screen+0x1B140 == 1, else bakes every cell's face and
// sets the latch. Freeze the view just before a real redraw so the faces and every later cell read agree; if it did not
// draw after all (latch still not 1), the previous view stays (it is what is on screen).
extern "C" PPC_FUNC(__imp__sub_82832B48);
extern "C" PPC_FUNC(sub_82832B48) {
  const uint32_t screen = ctx.r3.u32;
  uint32_t m = 0;
  if (owngrid::Mode() && OwnSlotsOn() && !owngrid::t_frozenByMover && (m = owngrid::GridOf(base, screen)) != 0 &&
      PPC_LOAD_U32(screen + 0x1B140) != 1) {
    owngrid::Frozen old;
    {
      std::lock_guard<std::mutex> lk(owngrid::g_fmu);
      old = owngrid::g_frozen;
    }
    owngrid::Freeze(base, screen, m);
    {  // Fable review: a re-freeze with fewer ids must not leave the page index past the last page (blank grid)
      const uint32_t k = PPC_LOAD_U32(m + 88);
      if (k <= 1) {
        const uint32_t pages = owngrid::PageCount(base, m, k);
        if (owngrid::g_page.load() >= pages) owngrid::g_page.store(pages ? pages - 1 : 0);
      }
    }
    __imp__sub_82832B48(ctx, base);
    if (PPC_LOAD_U32(screen + 0x1B140) != 1) {
      std::lock_guard<std::mutex> lk(owngrid::g_fmu);
      owngrid::g_frozen = std::move(old);
    }
    return;
  }
  __imp__sub_82832B48(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_8282E218);
extern "C" PPC_FUNC(sub_8282E218) {
  const uint32_t screen = ctx.r3.u32, player = ctx.r4.u32, pcol = ctx.r7.u32, prow = ctx.r8.u32;
  const bool up = (ctx.r5.u64 >> 32) != 0, down = (ctx.r5.u64 & 0xFFFFFFFFu) != 0;
  // Fable review 11-16 (checked: selgrid-14 paged two presses after a forced row 2, i.e. from row 0 reached in ONE press):
  // the col/row passed in (r7/r8) are already the stepped TARGET; the mover tests that cell first and stores the result
  // at screen+player*32+0x1B14C/0x1B150. The cursor's real position before this press is that stored pair.
  if (player >= 6) { __imp__sub_8282E218(ctx, base); return; }
  const int32_t oldc = int32_t(PPC_LOAD_U32(screen + player * 32 + 0x1B14C)),
                oldr = int32_t(PPC_LOAD_U32(screen + player * 32 + 0x1B150));
  __imp__sub_8282E218(ctx, base);
  if (!owngrid::Mode() || !OwnSlotsOn() || (up == down)) return;
  {  // every vertical press, first 80 (cheap; tells "game moved from -> to" apart from our overrides below)
    static std::atomic<uint32_t> n{0};
    if (n++ < 80)
      REXLOG_INFO("[own-grid] press {} page {}: cursor col {} row {} -> game col {} row {}", down ? "down" : "up",
                  owngrid::g_page.load(), oldc, oldr, PPC_LOAD_U32(pcol), PPC_LOAD_U32(prow));
  }
  if (oldc < 0 || oldc > 22 || oldr < 0 || oldr > 4 || (oldr == 4 && !up)) return;
  const uint32_t sub = PPC_LOAD_U32(screen + 16);
  if (sub < 0x10000u || sub >= 0xC0000000u) return;
  const uint32_t m = PPC_LOAD_U32(sub + 56792);
  if (!owngrid::GridReady(base, m)) return;
  const uint32_t k = PPC_LOAD_U32(m + 88);
  if (k > 1) return;
  const uint32_t page = owngrid::g_page.load(), pages = owngrid::PageCount(base, m, k);
  {  // diagnostic: a vertical move inside the grid that passed over a row (should not happen with the frozen view)
    const int32_t nr = int32_t(PPC_LOAD_U32(prow));
    if (oldr <= 3 && nr >= 0 && nr <= 3 && (nr - oldr > 1 || oldr - nr > 1)) {
      static std::atomic<uint32_t> n{0};
      if (n++ < 40)
        REXLOG_INFO("[own-grid] row skip: {} page {} col {} row {} -> col {} row {}", down ? "down" : "up", page, oldc, oldr,
                    PPC_LOAD_U32(pcol), nr);
    }
  }
  uint32_t np, tr;
  if (down && oldr == 3 && page + 1 < pages) { np = page + 1; tr = 0; }
  else if (up && oldr == 0 && page > 0) { np = page - 1; tr = 3; }
  else {
    // One row per press (owner 11-16, selgrid-11): on a part-filled import page the game's mover can leave the cursor on an
    // empty cell (Up from the button strip lands on row 3 at a fixed column) or pass over a row. When the game did not land
    // on a filled cell of the next row, put the cursor on the nearest filled cell of the next row that has one (Up from
    // the strip: the lowest such row). Down into rows that are all empty keeps the game's move (to the strip).
    const int32_t nr = int32_t(PPC_LOAD_U32(prow)), nc = int32_t(PPC_LOAD_U32(pcol));
    // The game refused the move (cursor stays where it was): its decision, never turn it into a move.
    if (nr == oldr && nc == oldc) return;
    uint32_t ids[4][23];
    owngrid::ViewIds(base, m, k, page, ids);
    auto filled = [&](int32_t r, int32_t c) {
      return r >= 0 && r <= 3 && c >= 0 && c <= 22 && ids[r][c] != 512 && owngrid::Pickable(base, sub, ids[r][c]) != 0;
    };
    int32_t want = down ? oldr + 1 : oldr - 1;
    if (want < 0 || want > 3) return;  // Up from row 0 on page 0 / Down from row 3 on the last page: the game's move
    if (nr == want && filled(nr, nc)) return;  // the game's own move is already one row, onto a wrestler
    int32_t bc = -1, br = -1;
    for (int32_t r = want; r >= 0 && r <= 3 && bc < 0; r += down ? 1 : -1) {
      for (int32_t d = 0; d < 23 && bc < 0; ++d)
        for (int32_t c : {oldc - d, oldc + d})
          if (filled(r, c)) { bc = c; br = r; break; }
      if (down) break;  // Down: only the row right below
    }
    if (bc < 0) return;
    PPC_STORE_U32(pcol, uint32_t(bc));
    PPC_STORE_U32(prow, uint32_t(br));
    PPC_STORE_U32(screen + player * 32 + 0x1B14C, uint32_t(bc));
    PPC_STORE_U32(screen + player * 32 + 0x1B150, uint32_t(br));
    static std::atomic<uint32_t> said{0};
    if (said++ < 60)
      REXLOG_INFO("[own-grid] one row: {} page {} col {} row {} -> col {} row {} (game had col {} row {})", down ? "down" : "up",
                  page, oldc, oldr, bc, br, nc, nr);
    return;
  }
  owngrid::Frozen before;
  {
    std::lock_guard<std::mutex> lk(owngrid::g_fmu);
    before = owngrid::g_frozen;
  }
  owngrid::Freeze(base, screen, m);  // the view the redraw below will draw; cursor placed from the same snapshot
  uint32_t ids[4][23];
  owngrid::ViewIds(base, m, k, np, ids);
  int32_t bc = -1, br = -1;
  for (int32_t step = 0; step < 4 && bc < 0; ++step) {  // target row first, then rows toward the other end
    const int32_t r = tr == 0 ? step : 3 - step;
    for (int32_t d = 0; d < 23 && bc < 0; ++d)
      for (int32_t c : {oldc - d, oldc + d})
        if (c >= 0 && c <= 22 && ids[r][c] != 512 && owngrid::Pickable(base, sub, ids[r][c]) != 0) { bc = c; br = r; break; }
  }
  if (bc < 0) {  // empty page (the refreshed view is shorter): keep the game's move and the view on screen
    std::lock_guard<std::mutex> lk(owngrid::g_fmu);
    owngrid::g_frozen = std::move(before);
    return;
  }
  owngrid::g_page.store(np);
  PPC_STORE_U32(pcol, uint32_t(bc));
  PPC_STORE_U32(prow, uint32_t(br));
  PPC_STORE_U32(screen + player * 32 + 0x1B14C, uint32_t(bc));
  PPC_STORE_U32(screen + player * 32 + 0x1B150, uint32_t(br));
  // The redraw sets a "drawn" latch at screen+0x1B140 when done and skips while it is 1; the Superstars/Divas button clears it
  // first (sub_82833E18: stwx r28(=0),r3,0x1B140 before bl sub_82832B48). selgrid-8: without this the faces stayed stock.
  PPC_STORE_U32(screen + 0x1B140, 0);
  PPCContext c = ctx;
  c.r3.u64 = screen;
  owngrid::t_frozenByMover = true;
  sub_82832B48(c, base);
  owngrid::t_frozenByMover = false;
  REXLOG_INFO("[own-grid] {} -> table {} page {}/{} cursor col {} row {} (id {})", down ? "down" : "up", k, np, pages, bc, br,
              ids[br][bc]);
}

// sub_827DC258 (recomp.45.cpp): id -> pointer to the unpacked 1272 B profile row (0 if none).
extern "C" PPC_FUNC(__imp__sub_827DC258);
uint32_t UniverseGuardProfileRow(uint8_t* base, uint32_t id, uint32_t ret, uint32_t row);  // universe_guard.cpp (#132)
extern "C" PPC_FUNC(sub_827DC258) {
  const uint32_t id = ctx.r3.u32, ret = static_cast<uint32_t>(ctx.lr);
  __imp__sub_827DC258(ctx, base);
  if (OwnSlotsOn()) ownrow::KeepRow(base, id, ctx.r3.u32);
  if (ctx.r3.u32 == 0) ctx.r3.u64 = UniverseGuardProfileRow(base, id, ret, 0);
}
