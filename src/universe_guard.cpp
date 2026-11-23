// Universe guard (GitHub #128 #129, owner 2026-11-10: "do the quick guard first then the proper fix"). Always on.
// "N:line" = generated/wwe2k14_recomp.N.cpp.
//
// Crash: editing allies / teams / rivals in Universe after a WWE '13 import read guest 0x2D in sub_822C2648 (3:46306), the
// wrestler rating (attribute bytes -> 0..100 in r3). Its caller sub_822C27D0 (3:46512) passes sub_827DCA40(...) + 32;
// sub_827DCA40 -> sub_827DC388 (45:72223) maps a wrestler id to a Universe roster slot through sub_822C1C68 and returns NULL
// when the id has no slot (512) - imported wrestlers have none in a Universe started before the import. 0 + 32 + 13 = 0x2D.
// Guard: a record pointer in the first 64 KB (no valid guest data lives there) gets rating 0 instead of a crash. Covers all
// 33 callers of the rating. Proper fix (imported wrestlers get Universe slots) is separate work.
#include <cstdint>
#include <atomic>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
std::atomic<uint32_t> g_logged{0};
}

extern "C" {
PPC_FUNC(__imp__sub_822C2648);
PPC_FUNC(sub_822C2648);
}

extern "C" PPC_FUNC(sub_822C2648) {
  if (ctx.r3.u32 < 0x10000) {
    if (g_logged.fetch_add(1) < 3)
      REXLOG_WARN("[universe-guard] wrestler rating asked for a missing Universe record (record {:08X}, caller {:08X}): "
                  "rating 0 instead of a crash (#128)", ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_822C2648(ctx, base);
}

// ---------------------------------------------------------------------------------------------------------------------
// GitHub #132 (v1.5.4 still crashes scrolling Universe / Create-an-Entrance / stable lists after a WWE '13 import).
// All five player logs: guest read of 0x69 in sub_822C30E8 (3:47966): sub_827DC388(id) returned NULL, then lbz 1(r3+104).
// Same cause as #128 (no Universe slot: sub_822C1C68 -> 512), different reader. Two lookups return NULL for such ids:
//   sub_827DC388 (45:72223, also reached by the tail branch sub_827DCA40) -> 204-byte Universe record (ids 0-99: a
//     2924-byte record whose first 204 bytes have the same layout);
//   sub_827DC258 (also via the tail branch sub_827DCA30) -> 1272-byte profile row (slot >= 400 -> NULL; ids 0-99: the
//     2924-byte record + 456). Hooked in unlock_all.cpp, which calls UniverseGuardProfileRow below.
// 168 + 36 call sites; most test r3 for NULL. The sites below were read one by one (gh132 lane, 2026-11-11) and use the
// result without a test (read a field, memcpy it, or write into it). Only for these return addresses a NULL answer becomes
// a pointer to a private dummy record in guest memory (system heap), refilled before every such answer so writes into it are
// dropped. Callers that test for NULL keep getting NULL. Sites whose callee tests NULL itself (sub_822C1F80/2068/2150, the
// rating guard above, sub_827DC900), or that drop the result, are not listed.
// Dummy contents follow the game's own defaults: Universe record all zero (byte 17 = 0 = "not in Universe", as every
// checked caller reads it) except byte 23 = 2, the value sub_822C2598 / sub_822C36D0 / sub_822C3800 use when the record is
// missing; profile row all zero except the three 132-byte slots at +860 whose first word is -1, the empty-slot form
// sub_833A0AA8 writes (memset 132 + stw -1) and sub_833A1550 / sub_82A34358 test.
// Not covered (would still crash, never seen in logs): sub_82819F78 / sub_8281A0A0 compute a NULL copy destination from
// sub_822C1C68 directly. Proper fix (Universe slots for imported ids) is separate work.
#include <cstring>
#include <mutex>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace {
// Return addresses (bl site + 4) of unchecked sub_827DC388 / sub_827DCA40 callers.
constexpr uint32_t kUnivSites[] = {
    0x822C2560, 0x822C2E90, 0x822C3100, 0x822C31A8, 0x822C374C, 0x822C3774, 0x822C381C, 0x822C3884, 0x822C38AC,
    0x822CF0D8, 0x822CF120, 0x822CF34C, 0x822DA814, 0x82315858, 0x8251F050, 0x82819E94, 0x82819FF0, 0x8281A118,
    0x8281A2AC, 0x8281A338, 0x82835700, 0x828F3D88, 0x828FD098, 0x828FD148, 0x82949BE8, 0x82949BF4, 0x829D9A8C,
    0x82AA4AF4, 0x82B68210, 0x833BA928, 0x833BAE50, 0x833BB0B8, 0x833BB4C0, 0x833BB760, 0x833BB978, 0x833BBB80,
    0x833BBC88, 0x833DC8E8,
    // 2026-11-13 #132 part 3: callers of the tail branch sub_822C1A80 (b sub_827DCA40 -> sub_827DC388; lr = their
    // return address) were never listed. 129 sites: 56 test r3 at once; 73 read by hand -> these 60 read, copy or
    // write the record unchecked. Left out (test later, callee copes, or unclear): 0x833F28E4 0x829C6A58 0x829EF634
    // 0x829EF728 0x82A25FC8 0x82A53670 0x82A54860 0x82A60230 0x82B638A0 0x82B75378 0x82B75384 0x82B8D434 0x82B8D440.
    0x82281B94, 0x822C7BD0, 0x822C7E60, 0x822C8CE8, 0x822C9D74, 0x822C9EC0, 0x822CA424, 0x822DE104, 0x826D4AA4,
    0x826D58F0, 0x826FEDAC, 0x827B16BC, 0x82801EFC, 0x82835E6C, 0x8284AF4C, 0x8284B03C, 0x8285ED2C, 0x82929294,
    0x829295CC, 0x829296F8, 0x8294A8E4, 0x82950438, 0x82950568, 0x82970EF8, 0x82970FF4, 0x829C1B90, 0x829C6AC4,
    0x829EA160, 0x829EA2CC, 0x829EECA8, 0x829EECE8, 0x829F0070, 0x829F00E8, 0x829F0230, 0x829F0278, 0x829F08B0,
    0x829F08D4, 0x829F0B18, 0x829F0B3C, 0x82A22F30, 0x82A26E8C, 0x82AA3120, 0x82AA3EA8, 0x82AA4270, 0x82AA4420,
    0x82AA7834, 0x82B61264, 0x82B6340C, 0x82B8DCA8, 0x82B8DCB4, 0x82BA56F4, 0x82BA5D74, 0x8339D470, 0x833C5AD4,
    0x833C5BC4, 0x833C60FC, 0x833F3E1C, 0x833F4570, 0x83430FB0, 0x83433390,
    // 2026-11-14 night (#132 part 5): the 13 left-out sub_822C1A80 callers, one Qwen flow card each (out/flow/b1114-g132),
    // every UNTESTED claim re-read by Claude. Unchecked use: 0x833F28E4 (lhz 10/12/8(r28), 162.cpp:62317) and 0x82A25FC8
    // (r28 = r29 - r30, then lhzx r11,r28,r31 = record+116, 63.cpp:63772). Safe, not listed: tested 0x829EF634 0x829EF728
    // 0x82B638A0 0x82B75378 0x82B75384 (cmplwi r3 right after) 0x82B8D434 0x82B8D440; result unused 0x829C6A58;
    // passed to sub_822C2598, which tests NULL itself (returns 2): 0x82A53670 0x82A54860 0x82A60230.
    0x833F28E4, 0x82A25FC8,
};
// Return addresses (bl site + 4) of unchecked sub_827DC258 / sub_827DCA30 callers.
constexpr uint32_t kProfSites[] = {
    0x822C42B8, 0x822CF138, 0x822CF198, 0x822CF368, 0x822DA900, 0x827B1B5C, 0x827DD1B8, 0x827DD2A4, 0x82819E88,
    0x82819FFC, 0x8281A124, 0x8281A2B8, 0x8285EB54, 0x829D9930, 0x82A231F8, 0x82A3459C, 0x82B8E860, 0x83352A84,
    0x833567F4, 0x833A0984, 0x833A0AC4, 0x833A1574, 0x833A15C8, 0x833A15D4, 0x833A1628, 0x833A1644, 0x833A1710,
    0x833A172C, 0x833B5E30, 0x833B9898,
    // 2026-11-13 (#132 player, v1.5.5-test1 crash at sub_8226ADC8: copy of 288 bytes from row+572, fault 0x23C): callers of
    // the second tail branch sub_822C1A70 (b sub_827DCA30 -> sub_827DC258, lr = their return address) were not in the list.
    // 54 sites read one by one; these 25 use the row without a NULL test (read/copy/deref, or 0x829C6AAC copies INTO it).
    // Left out: the 21 that test r3 and 0x829D21F8 / 0x829D2230 / 0x829D2310 / 0x82B5DAA8 (test later or unclear).
    0x8226AE78, 0x822DE144, 0x823101CC, 0x82316598, 0x82512A28, 0x8251F044, 0x827B16C8, 0x828959A0, 0x828F3C0C,
    0x829A6634, 0x829C15DC, 0x829C1BA8, 0x829C6A00, 0x829C6A18, 0x829C6AAC, 0x829D8FF0, 0x829D9A80, 0x82A22F3C,
    0x82A26E98, 0x8335096C, 0x833530E0, 0x83354C40, 0x833597F4, 0x83433314, 0x83433334,
    // 2026-11-14 night (#132 part 5, flow b1114-g132 + Claude re-read): 0x829D21F8 / 0x829D2230 / 0x829D2310 keep the row
    // in r29 into the switch at loc_829D238C (61.cpp:19664), whose cases index it untested (lhzx r4,r5,r29, 61.cpp:19752).
    // 0x82B5DAA8 tests r29 (cmplwi) first: not listed.
    0x829D21F8, 0x829D2230, 0x829D2310,
};
constexpr uint32_t kDummySize = 4096;   // > 2924 (largest record a lookup can stand for) and > 1272

template <size_t N>
bool Listed(const uint32_t (&sites)[N], uint32_t ret) {
  for (uint32_t s : sites) if (s == ret) return true;
  return false;
}

struct Dummy {
  std::once_flag once;
  uint32_t addr = 0;
  std::atomic<uint32_t> logged{0};
};
Dummy g_univ, g_row, g_ext;

uint32_t Alloc(Dummy& d, const char* what) {
  std::call_once(d.once, [&] {
    auto* ks = rex::system::kernel_state();
    auto* mem = ks ? ks->memory() : nullptr;
    d.addr = mem ? mem->SystemHeapAlloc(kDummySize, 0x100) : 0;
    if (d.addr) REXLOG_INFO("[universe-guard] {} dummy record at {:08X} ({} bytes, #132)", what, d.addr, kDummySize);
    else REXLOG_ERROR("[universe-guard] {} dummy record: guest allocation failed, guard off (#132)", what);
  });
  return d.addr;
}

// Called only when the lookup answered NULL to a listed caller. Returns the refilled dummy (0 if allocation failed).
uint32_t Substitute(Dummy& d, const char* what, uint8_t* base, uint32_t id, uint32_t ret, bool profile_row) {
  const uint32_t a = Alloc(d, what);
  if (!a) return 0;
  std::memset(PPC_RAW_ADDR(a), 0, kDummySize);
  if (profile_row) {
    for (uint32_t k = 0; k < 3; ++k) PPC_STORE_U32(a + 860 + 132 * k, 0xFFFFFFFFu);
  } else {
    PPC_STORE_U8(a + 23, 2);
  }
  if (d.logged.fetch_add(1) < 8)
    REXLOG_WARN("[universe-guard] wrestler {} has no {} record (caller {:08X}): empty stand-in instead of a crash (#132)",
                id, what, ret);
  return a;
}
}  // namespace

void OwnMainFixRecord(uint8_t* base, uint32_t id, uint32_t rec);  // unlock_all.cpp
extern "C" {
PPC_FUNC(__imp__sub_827DC388);
PPC_FUNC(sub_827DC388);
}

extern "C" PPC_FUNC(sub_827DC388) {
  const uint32_t ret = static_cast<uint32_t>(ctx.lr), id = ctx.r3.u32;
  __imp__sub_827DC388(ctx, base);
  OwnMainFixRecord(base, id, ctx.r3.u32);  // unlock_all.cpp, test switch WWE2K14_OWN_MAIN (off by default)
  if (ctx.r3.u32 == 0 && Listed(kUnivSites, ret)) {
    if (const uint32_t a = Substitute(g_univ, "Universe", base, id, ret, false)) ctx.r3.u64 = a;
  }
}

// 2026-11-13 #132 part 4: sub_827DC2E8 (45:72132, reached only via sub_822C1A78 -> b sub_827DCA38 -> b sub_827DC2E8)
// returns a 40-byte sub-record; for ids >= 100 whose slot (sub_822C1C68) is >= 400 it returns 0 (li r3,0 at
// loc_827DC334). Its only two callers copy 40 bytes from it right after the Universe and profile lookups this file
// already guards (sub_827B16xx, sub_82A22Fxx), so without this they still crash one step later.
constexpr uint32_t kExtSites[] = {0x827B16D4, 0x82A22F48};

extern "C" {
PPC_FUNC(__imp__sub_827DC2E8);
PPC_FUNC(sub_827DC2E8);
}

extern "C" PPC_FUNC(sub_827DC2E8) {
  const uint32_t ret = static_cast<uint32_t>(ctx.lr), id = ctx.r3.u32;
  __imp__sub_827DC2E8(ctx, base);
  if (ctx.r3.u32 == 0 && Listed(kExtSites, ret)) {
    if (const uint32_t a = Substitute(g_ext, "wrestler extra", base, id, ret, false)) {
      PPC_STORE_U8(a + 23, 0);  // Substitute's Universe default; this 40-byte record stays all zero
      ctx.r3.u64 = a;
    }
  }
}

// sub_827DC258 is already hooked in unlock_all.cpp (own-slots row keeper); that hook calls this with the lookup's answer.
uint32_t UniverseGuardProfileRow(uint8_t* base, uint32_t id, uint32_t ret, uint32_t row) {
  if (row != 0 || !Listed(kProfSites, ret)) return row;
  const uint32_t a = Substitute(g_row, "profile", base, id, ret, true);
  return a ? a : row;
}
