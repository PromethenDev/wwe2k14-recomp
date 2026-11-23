// Imported arenas on the arena-select WrestleMania page (G4 Path B, docs/tasks/G4-arenas-report.md). OFF unless
// WWE2K14_ARENA_LIST=<file> names a list the launcher writes: one "id tag" per line (id < 100, a free arena number; tag =
// the picture name in MatchHD/SD.pac MLYE/MCAS leaf 7001). "N:line" = generated/wwe2k14_recomp.N.cpp.
//
// The page (update image) shows the 28 WrestleMania entries of the (id, tag) table at 0x82039618 as a scrolling grid:
// 5 per row, 4 rows visible, scroll 0..2 (max = 6 - 4 is a constant in the scroll handler sub_82871C08, 50:45196; the 28
// is a constant in five more functions). Rather than replace those functions, this hook treats the 28 table slots as a
// window onto a longer virtual list (28 WrestleManias + the imports, padded with 300 = random to whole rows):
//   - page open sub_82871390 (50:43940): window back to the start, table restored.
//   - scroll handler: cursor on the last visible row, scroll already 2, Down pressed, more rows below -> move the window one
//     row on, rewrite the 28 slots and set scroll to 1; the original handler then scrolls 1 -> 2 itself, refreshing the
//     pictures (sub_82871688) and cursor as usual. Up at the top mirrors it.
// Every function reads the arena id from the table, so the highlighted / chosen id is the imported one; the unlock check
// sub_822BF480 returns 1 for ids outside 15 and 28-55. Tag strings live in zeroed slack after BINKCONS (0x8224B600..).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace {
constexpr uint32_t kWmTable = 0x82039618;   // 28 x (u32 id, u32 tag pointer), big endian
constexpr uint32_t kWmCount = 28;
constexpr uint32_t kTagArea = 0x8224B600;   // zero slack between BINKCONS end 0x8224B580 and .text 0x82250000
constexpr uint32_t kTagAreaEnd = 0x8224B800;
constexpr uint32_t kRecArea = 0x8224B800;      // relocated arena record table (same slack, up to 0x82250000)
constexpr uint32_t kRecAreaEnd = 0x82250000;
constexpr uint32_t kRecPtr = 0x836F2C84;       // game: pointer to the 108-byte arena records (3:53756), count at +4
constexpr uint32_t kRecSize = 108;
constexpr uint32_t kRecTemplateId = 5;         // imported arenas take this arena's match-type flags (a plain pay-per-view arena)
constexpr uint32_t kFlagFirst = 24, kFlagLast = 43;  // match-type permission bytes of an arena record
constexpr uint32_t kRecUnusedId = 20;         // an unused id with a record ('(null)' name): shape for new records
constexpr uint32_t kPadMasks = 0x8200F520;  // per pad 16 bytes: up, left?, down, right? masks (50:45196 reads +0 / +8)
constexpr uint32_t kRandom = 300;

struct Entry { uint32_t id, tag; };
std::mutex g_mu;
bool g_ready = false, g_on = false;
std::vector<Entry> g_virtual;   // whole list, length multiple of 5 beyond the first 28
uint32_t g_window = 0;          // first virtual row shown in slot row 0
Entry g_orig[kWmCount];
// GitHub #171 #175 #176 (11-22): the loader frees *kRecPtr (sub_822C4850 -> sub_82C32298) before it builds a new table.
// After a relocation *kRecPtr is the static slack, so a second load (it runs again once DLC is mounted) handed a non-heap
// address to the game's free list and that free spun forever (freeze report: sub_82C32048 <- sub_822C4850 <- the loader,
// title screen never advances). Keep the game's own table (never freed by us) and put it back before every load.
uint32_t g_heap_table = 0, g_heap_count = 0, g_reloc_bytes = 0;


// Guest memory this hook writes: the WM table (0x82039618, image page 0x82030000) and the slack 0x8224B580-0x82250000
// (image page 0x82240000). Both belong to the game image's single allocation (XexModule loads the whole image with one
// AllocFixed at 0x82000000, so no other component can be handed these pages), and LoadContinue marks image code/rodata
// pages read-only. Log what the heap says and make the two 64 KiB pages writable explicitly instead of relying on luck.
bool g_mem_ok = false;
bool CheckAndOpen(uint32_t addr, uint32_t size, const char* what) {
  auto* ks = rex::system::kernel_state();
  auto* mem = ks ? ks->memory() : nullptr;
  auto* heap = mem ? mem->LookupHeap(addr) : nullptr;
  if (!heap) { REXLOG_ERROR("[arena-page] {} {:#x}: no guest heap", what, addr); return false; }
  rex::memory::HeapAllocationInfo info{};
  if (!heap->QueryRegionInfo(addr, &info)) { REXLOG_ERROR("[arena-page] {} {:#x}: no region info", what, addr); return false; }
  REXLOG_INFO("[arena-page] {} {:#x}: allocation base {:#x} size {:#x}, region {:#x}+{:#x}, state {:#x}, protect {:#x}",
              what, addr, info.allocation_base, info.allocation_size, info.base_address, info.region_size, info.state, info.protect);
  if (info.allocation_base != 0x82000000 || !(info.state & rex::memory::kMemoryAllocationCommit)) {
    REXLOG_ERROR("[arena-page] {} {:#x}: not inside the committed game image allocation - refusing", what, addr); return false;
  }
  const uint32_t page = heap->page_size();
  const uint32_t first = addr & ~(page - 1), last = (addr + size - 1) & ~(page - 1);
  uint32_t prot = 0;
  if (heap->QueryProtect(addr, &prot) && (prot & rex::memory::kMemoryProtectWrite)) return true;
  if (!heap->Protect(first, last - first + page, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
    REXLOG_ERROR("[arena-page] {} {:#x}: could not make writable (protect was {:#x})", what, addr, prot); return false;
  }
  REXLOG_INFO("[arena-page] {} pages {:#x}-{:#x} made writable (were {:#x})", what, first, last + page, prot);
  return true;
}

void Load(uint8_t* base) {
  if (g_ready) return;
  g_ready = true;
  const char* path = std::getenv("WWE2K14_ARENA_LIST");
  if (!path || !*path) return;
  g_mem_ok = CheckAndOpen(kWmTable, 8 * kWmCount, "WM table") && CheckAndOpen(kTagArea, kRecAreaEnd - kTagArea, "slack");
  if (!g_mem_ok) { REXLOG_ERROR("[arena-page] guest memory check failed: imported arenas off"); return; }
  for (uint32_t i = 0; i < kWmCount; ++i)
    g_orig[i] = {PPC_LOAD_U32(kWmTable + 8 * i), PPC_LOAD_U32(kWmTable + 8 * i + 4)};
  for (const auto& e : g_orig) g_virtual.push_back(e);
  FILE* f = std::fopen(path, "r");
  if (!f) { REXLOG_INFO("[arena-page] list {} not readable: off", path); return; }
  uint32_t tag_at = kTagArea, dummy_tag = 0;
  char line[256];
  auto put_tag = [&](const char* s) -> uint32_t {
    const size_t n = std::strlen(s) + 1;
    if (tag_at + n > kTagAreaEnd) return 0;
    const uint32_t at = tag_at;
    for (size_t k = 0; k < n; ++k) PPC_STORE_U8(at + k, static_cast<uint8_t>(s[k]));
    tag_at = (tag_at + n + 3) & ~3u;
    return at;
  };
  while (std::fgets(line, sizeof line, f)) {
    unsigned id = 0; char tag[64] = {0};
    if (std::sscanf(line, "%u %63s", &id, tag) != 2 || id >= 100) continue;
    const uint32_t t = put_tag(tag);
    if (!t) break;
    g_virtual.push_back({id, t});
  }
  std::fclose(f);
  if (g_virtual.size() == kWmCount) return;
  dummy_tag = put_tag("dummy");   // an 8x8 texture of the MCAS bank: filler cells pick a random arena
  while ((g_virtual.size() - kWmCount) % 5) g_virtual.push_back({kRandom, dummy_tag});
  g_on = true;
  REXLOG_INFO("[arena-page] on: {} entries, {} extra rows", g_virtual.size(), (g_virtual.size() - kWmCount) / 5);
}

uint32_t MaxWindow() { return g_on ? static_cast<uint32_t>((g_virtual.size() - kWmCount) / 5) : 0; }

void Show(uint8_t* base, uint32_t window) {
  g_window = window;
  for (uint32_t i = 0; i < kWmCount; ++i) {
    const Entry& e = g_virtual[5 * window + i];
    PPC_STORE_U32(kWmTable + 8 * i, e.id);
    PPC_STORE_U32(kWmTable + 8 * i + 4, e.tag);
  }
  REXLOG_INFO("[arena-page] window row {} (slot 0 = id {})", window, g_virtual[5 * window].id);
}
// The game's arena records (loader sub_822C54E8, 3:53710): count at kRecPtr+4, records of 108 bytes, +4 arena id, +7 order
// index (sub_822C4428 maps index -> id), +25/+38 per-match-type flags (sub_822C56E8). The exhibition store sub_82927A90 only
// accepts ids of the allowed list sub_829249D0 builds from these records, so an id without a record is dropped (g4-menu3/4).
// After every load, copy the table into the slack with one record per imported id (template = kRecTemplateId's record).
void ExtendRecords(uint8_t* base) {
  if (!g_on) return;
  const uint32_t table = PPC_LOAD_U32(kRecPtr), count = PPC_LOAD_U32(kRecPtr + 4);
  if (!table || !count) return;
  uint32_t templ = 0, unused = 0; uint8_t used[256] = {0};
  bool have[100] = {false};
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t r = table + kRecSize * i; const uint8_t id = PPC_LOAD_U8(r + 4);
    used[PPC_LOAD_U8(r + 7)] = 1; if (id < 100) have[id] = true;
    if (id == kRecTemplateId) templ = r;
    if (id == kRecUnusedId) unused = r;
  }
  // Existing target records keep their first 8 bytes (+4 arena id, +7 order index) and take the template's bytes 24-43,
  // the per-match-type flags read by sub_822C56E8. Targets without a record need a new entry in the relocated table.
  std::vector<uint32_t> add;
  uint32_t existing_records_updated = 0;
  for (size_t k = kWmCount; k < g_virtual.size(); ++k) {
    const uint32_t id = g_virtual[k].id;
    if (id >= 100 || id == kRandom) continue;
    if (have[id]) {
      for (uint32_t i = 0; i < count && templ; ++i) {
        const uint32_t r = table + kRecSize * i;
        if (PPC_LOAD_U8(r + 4) != id) continue;
        for (uint32_t b = kFlagFirst; b <= kFlagLast; ++b) PPC_STORE_U8(r + b, PPC_LOAD_U8(templ + b));
        ++existing_records_updated;
      }
      continue;
    }
    add.push_back(id); have[id] = true;
  }
  if (add.empty() || !templ) {
    if (existing_records_updated)
      REXLOG_INFO("[arena-page] enabled match-type flags on {} existing arena record(s)", existing_records_updated);
    return;
  }
  const uint32_t total = count + static_cast<uint32_t>(add.size());
  if (kRecArea + kRecSize * total > kRecAreaEnd) { REXLOG_ERROR("[arena-page] records: no room for {} - imported ids without a record stay unpickable", total); return; }
  // The slack 0x8224B580-0x82250000 lies between BINKCONS (0x82247800 + 0x3D80) and .text (0x82250000) of the update image,
  // outside every section (analysis/wwe2k14-tu-image.bin.sections.txt), zero in the image; tag strings already live there.
  // Refuse (loudly) if anything else has written into the part we need.
  for (uint32_t b = 0; b < kRecSize * total; ++b)
    if (PPC_LOAD_U8(kRecArea + b) != 0 && !(PPC_LOAD_U32(kRecPtr) == kRecArea)) {
      REXLOG_ERROR("[arena-page] records: slack at {:#x} is not free (byte {} set) - not relocating", kRecArea, b); return;
    }
  for (uint32_t b = 0; b < kRecSize * count; ++b) PPC_STORE_U8(kRecArea + b, PPC_LOAD_U8(table + b));
  uint32_t next_index = 0;
  for (size_t k = 0; k < add.size(); ++k) {
    const uint32_t r = kRecArea + kRecSize * (count + static_cast<uint32_t>(k));
    // Shape of an unused id's record (+0 name key 40002+id, +4 id, +5 0x15, +7 order index, no path, later fields as the
    // unused record), plus the flag block 24-43 of the template, as for imported ids that already have a record.
    for (uint32_t b = 0; b < kRecSize; ++b) PPC_STORE_U8(r + b, unused ? PPC_LOAD_U8(unused + b) : 0);
    for (uint32_t b = kFlagFirst; b <= kFlagLast; ++b) PPC_STORE_U8(r + b, PPC_LOAD_U8(templ + b));
    for (uint32_t b = 8; b < kFlagFirst; ++b) PPC_STORE_U8(r + b, 0);
    while (next_index < 255 && used[next_index]) ++next_index;
    used[next_index] = 1;
    PPC_STORE_U32(r + 0, 40002 + add[k]);
    PPC_STORE_U8(r + 4, static_cast<uint8_t>(add[k]));
    PPC_STORE_U8(r + 7, static_cast<uint8_t>(next_index));
    REXLOG_INFO("[arena-page] record added: id {} order index {}", add[k], next_index);
  }
  g_heap_table = table; g_heap_count = count; g_reloc_bytes = kRecSize * total;
  PPC_STORE_U32(kRecPtr, kRecArea);
  PPC_STORE_U32(kRecPtr + 4, total);
  REXLOG_INFO("[arena-page] records: {} -> {} at {:#x} (was {:#x})", count, total, kRecArea, table);
}
// Before the loader runs again: give it back the heap table it allocated (it frees that one), and clear our copy so the
// next relocation finds the slack free.
void RestoreHeapTable(uint8_t* base) {
  if (!g_heap_table) return;
  if (PPC_LOAD_U32(kRecPtr) == kRecArea) {
    PPC_STORE_U32(kRecPtr, g_heap_table);
    PPC_STORE_U32(kRecPtr + 4, g_heap_count);
    for (uint32_t b = 0; b < g_reloc_bytes; ++b) PPC_STORE_U8(kRecArea + b, 0);
    REXLOG_INFO("[arena-page] reload: game table {:#x} ({} records) handed back before the loader frees it", g_heap_table, g_heap_count);
  } else {
    REXLOG_WARN("[arena-page] reload: record pointer is {:#x}, not the relocated table - left as is", PPC_LOAD_U32(kRecPtr));
  }
  g_heap_table = g_heap_count = g_reloc_bytes = 0;
}
}  // namespace

extern "C" {
PPC_FUNC(__imp__sub_82871390);
PPC_FUNC(sub_82871390);
PPC_FUNC(__imp__sub_82871C08);
PPC_FUNC(sub_82871C08);
PPC_FUNC(__imp__sub_822C54E8);
PPC_FUNC(sub_822C54E8);
}

// Arena record loader: original, then the extension.
extern "C" PPC_FUNC(sub_822C54E8) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    RestoreHeapTable(base);
  }
  __imp__sub_822C54E8(ctx, base);
  std::lock_guard<std::mutex> lk(g_mu);
  Load(base);
  ExtendRecords(base);
}

// WrestleMania page open: start from the first window.
extern "C" PPC_FUNC(sub_82871390) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    Load(base);
    if (g_on && g_window != 0) Show(base, 0);
  }
  __imp__sub_82871390(ctx, base);
}

// Scroll handler, r3 = page list object: +12 pad, +56 buttons, +24 visible rows, +252 scroll, +348 cursor row.
extern "C" PPC_FUNC(sub_82871C08) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    Load(base);
    if (g_on) {
      const uint32_t obj = ctx.r3.u32;
      const uint32_t pad = PPC_LOAD_U32(obj + 12), buttons = PPC_LOAD_U32(obj + 56);
      const uint32_t visible = PPC_LOAD_U32(obj + 24), row = PPC_LOAD_U32(obj + 348);
      const int32_t scroll = static_cast<int32_t>(PPC_LOAD_U32(obj + 252));
      const bool up = (PPC_LOAD_U32(kPadMasks + 16 * pad) & buttons) != 0;
      const bool down = (PPC_LOAD_U32(kPadMasks + 16 * pad + 8) & buttons) != 0;
      if (down && row + 1 == visible && scroll >= 2 && g_window < MaxWindow()) {
        Show(base, g_window + 1);
        PPC_STORE_U32(obj + 252, 1);   // the original moves 1 -> 2 and refreshes the pictures
      } else if (up && row == 0 && scroll <= 0 && g_window > 0) {
        Show(base, g_window - 1);
        PPC_STORE_U32(obj + 252, 1);   // the original moves 1 -> 0
      }
    }
  }
  __imp__sub_82871C08(ctx, base);
}
