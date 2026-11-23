#ifdef WWE2K14_NO_TEST_TOOLS
// Public builds omit the direct-match test hook.
#else
// TEST-ONLY direct match start (owner 2026-11-05: tests and clips reach the action in about a minute). Default OFF: inert
// unless the environment switches below are set; the launcher never sets them (same rule as test_scenario.cpp).
//
//   WWE2K14_TEST_DIRECT_MATCH=<id1>,<id2>[,<arena>]   once the main menu is idle, start this one-on-one match
//   WWE2K14_TEST_DIRECT_CAPTURE=<folder>              dump the match description and related state to files, and log the menu
//                                                     state words; a menu-started match also gets a dry run of the builder
//
// Everything cited below was read in generated/wwe2k14_recomp.N.cpp ("N:line"). "candidate" = inferred, not proven.
// The flow, in the game's own order (exhibition finalize sub_82928F60, 56:9086):
//   M = *0x836F2DD8 (56:9103), type setter sub_822CBBD0 resets M (3:68995), participant setters, arena setter, then from
//   56:11004: apply sub_822CB950, wrestler list sub_822CCF38 (56:11035), sub_827D9370(G+472) (56:11071), G = 0x8376AB44
//   (45:64369). The menu's final confirm asks the SmartMenu (*0x839294AC) to exit with action 1: sub_8283EC48(SM, 1)
//   (50:33658), which stores the action at 0x83929484 (49:1080). The game then does the rest itself: the main-menu
//   sequence's update sub_82827EA8 (48:25639) sees the action, sets SM+11404; the page callback sub_828420E8 (49:8936)
//   deletes the SmartMenu and runs the dispatcher sub_82835010 (48:56634), whose case 1 calls sub_827DEDB8 (46:3109) which
//   starts the loading sequence through sub_8225E800 (0:35134).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

extern "C" {
PPC_FUNC(sub_827DB010);  // global options block (3:69019)
PPC_FUNC(sub_822C5FB8);  // controller of a slot (M, i, a, b): entry+41, +42
PPC_FUNC(sub_822C6020);  // copies the menu's controller profile into P (56:10878, 3:55411)
PPC_FUNC(sub_82255D68);  // profile record for controller/profile index (0 means player 1; 56:10857, 0:14292)
PPC_FUNC(sub_822CBBD0);  // match type setter (M, type, options or 0): resets M
PPC_FUNC(sub_82A21200);  // mode setter (M, mode): M+4 (63)
PPC_FUNC(sub_822CA958);  // arena setter (M, arena, flag), hooked in test_scenario.cpp
PPC_FUNC(sub_822C9C08);  // participant setter, hooked in test_scenario.cpp
PPC_FUNC(sub_822C7930);  // (M, i, value): entry+8
PPC_FUNC(sub_822C7950);  // (M, i, byte): entry+13
PPC_FUNC(sub_822C79F8);  // (M, i, byte): entry+21
PPC_FUNC(sub_822C61E8);  // (M, i, byte): entry+3576
PPC_FUNC(sub_822C7A18);  // (M, i, value): entry attire choice (candidate)
PPC_FUNC(sub_822C6208);  // (M, i, id, attire, 1): the menu's attire call when its slot record +28 != 512 (56:9612)
PPC_FUNC(sub_822C9F48);  // (M) finalize's step before apply
PPC_FUNC(sub_82B65740);  // () finalize's step before apply; online only inside
PPC_FUNC(__imp__sub_822CB950);
PPC_FUNC(sub_822CB950);  // apply (M)
PPC_FUNC(sub_822CCF38);  // wrestler list from the slots (): entrances need it
PPC_FUNC(sub_827D9370);  // (G+472)
PPC_FUNC(sub_8283EC48);  // SmartMenu exit request (SM, action)
PPC_FUNC(sub_8283C558);  // SmartMenu predicate (SM), candidate: page accepts input
PPC_FUNC(sub_822C1A80);  // per-character data (id) -> pointer
PPC_FUNC(sub_827F6F58);  // arena record flag (P+144, 1) (46:62428), as the menu at 49:33218
PPC_FUNC(sub_827F6F68);  // arena record setter (P+144, arena) (46:62446), as the menu at 49:33232
PPC_FUNC(__imp__sub_82827EA8);
PPC_FUNC(sub_82827EA8);
PPC_FUNC(__imp__sub_82928F60);
PPC_FUNC(sub_82928F60);
PPC_FUNC(__imp__sub_82C3DB40);
PPC_FUNC(sub_82C3DB40);
}

namespace {
constexpr uint32_t kMatchPtr = 0x836F2DD8;       // M (56:9103)
constexpr uint32_t kSmartMenuPtr = 0x839294AC;   // SmartMenu (48:24045 store, 50:24231 load)
constexpr uint32_t kExitAction = 0x83929484;     // (49:1080)
constexpr uint32_t kExhibitionPtr = 0x83929510;  // finalize's object (0:63524)
constexpr uint32_t kSeqMgrPtr = 0x83940148;      // sequence manager; [[mgr]+0]+48 = id (80:56074)
constexpr uint32_t kSettingsG = 0x8376AB44;      // G (45:64369: lis r11,-31881; addi r3,r11,-21692)
constexpr uint32_t kSettingsSize = 18600;        // G+18576 is the last field read in apply; rounded to the object's end (candidate)
constexpr uint32_t kGameDataPtr = 0x8376AB40;    // P (45:64024); P+144 object (45:64031)
constexpr uint32_t kMatchDumpSize = 0x2E31C;     // M's allocation size (46:14130 area, sub_827E3xxx: lis r3,2; ori r3,r3,58140)
constexpr uint32_t kExhibitionSize = 30064;      // (0:63512)
constexpr uint32_t kSeqMainMenu = 75;            // (48:24142)
constexpr uint32_t kMenuPageMain = 1;            // SM+364 on the idle main menu (run dm1 log)
constexpr int32_t kDefaultArena = 16;            // no arena asked: BG16 (test_scenario.cpp notes), has the broadcast bug the navigator reads
constexpr uint32_t kIdleTicks = 120;             // main-menu updates the state must stay unchanged before launching

struct Request {
  bool on = false;
  uint32_t id[2] = {0, 0};
  int32_t arena = -1;
  int32_t attire[2] = {-1, -1};  // WWE2K14_TEST_DIRECT_ATTIRE=a0,a1 (-1 = the menus' default, no call)
};
const Request& Req() {
  static const Request r = [] {
    Request q;
    const char* e = std::getenv("WWE2K14_TEST_DIRECT_MATCH");
    if (!e || !*e) return q;
    uint32_t v[3] = {0, 0, 0};
    int n = 0;
    for (const char* p = e; *p && n < 3;) {
      char* end = nullptr;
      const unsigned long x = std::strtoul(p, &end, 10);
      if (end == p) break;
      v[n++] = uint32_t(x);
      p = *end == ',' ? end + 1 : end;
    }
    if (n < 2) {
      REXLOG_WARN("[test-direct] WWE2K14_TEST_DIRECT_MATCH='{}' ignored (expected id1,id2[,arena])", e);
      return q;
    }
    q.on = true;
    q.id[0] = v[0];
    q.id[1] = v[1];
    if (n == 3) q.arena = int32_t(v[2]);
    if (const char* a = std::getenv("WWE2K14_TEST_DIRECT_ATTIRE"); a && *a) {
      char* end = nullptr;
      q.attire[0] = int32_t(std::strtol(a, &end, 10));
      if (end && *end == ',') q.attire[1] = int32_t(std::strtol(end + 1, nullptr, 10));
      REXLOG_INFO("[test-direct] WWE2K14_TEST_DIRECT_ATTIRE: {} / {} (test only)", q.attire[0], q.attire[1]);
    }
    REXLOG_INFO("[test-direct] WWE2K14_TEST_DIRECT_MATCH: {} vs {} arena {} (test only)", q.id[0], q.id[1], q.arena);
    return q;
  }();
  return r;
}
const std::string& CaptureDir() {
  static const std::string d = [] {
    const char* e = std::getenv("WWE2K14_TEST_DIRECT_CAPTURE");
    std::string s = e ? e : "";
    if (!s.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(s, ec);
      REXLOG_INFO("[test-direct] capture folder {} (test only)", s);
    }
    return s;
  }();
  return d;
}
bool AnyOn() {
  static const bool on = Req().on || !CaptureDir().empty();
  return on;
}

void Dump(uint8_t* base, const char* name, uint32_t addr, uint32_t size) {
  if (CaptureDir().empty() || !addr) return;
  const std::string path = CaptureDir() + "/" + name;
  if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(base + addr, 1, size, f);
    std::fclose(f);
    REXLOG_INFO("[test-direct] dumped {} bytes of 0x{:08X} to {}", size, addr, path);
  }
}

// One guest call on the live context: arguments in r3.., the caller restores the registers afterwards.
using Fn = void (*)(PPCContext&, uint8_t*);
uint32_t Call(PPCContext& ctx, uint8_t* base, Fn fn, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0,
              uint64_t e = 0, uint64_t f = 0, uint64_t g = 0, uint64_t h = 0) {
  ctx.r3.u64 = a;
  ctx.r4.u64 = b;
  ctx.r5.u64 = c;
  ctx.r6.u64 = d;
  ctx.r7.u64 = e;
  ctx.r8.u64 = f;
  ctx.r9.u64 = g;
  ctx.r10.u64 = h;
  ctx.lr = 0;
  fn(ctx, base);
  return ctx.r3.u32;
}

// Writes M the way the finalize does for a one-on-one exhibition with the menus' defaults (56:9121 .. 56:9719). Argument
// values are what run dm1 logged at the menus' own calls; the dry run there diffed this builder's M against theirs.
void BuildMatch(PPCContext& ctx, uint8_t* base, uint32_t m, const uint32_t id[2], int32_t arena, uint32_t arena_flag,
                const int32_t* attire = nullptr) {
  // Options record: the game's defaults (block+0x80458, 3:69022) with the four bytes the menus' record differs in (dump
  // ex-menu.bin, obj+1100+8.. run dm1). Passed on the guest stack below the live frame.
  constexpr uint32_t kRecordSize = 92;
  const uint32_t rec = (ctx.r1.u32 - 512) & ~15u;
  const uint32_t block = Call(ctx, base, sub_827DB010);
  std::memcpy(base + rec, base + block + 0x80458, kRecordSize);
  PPC_STORE_U8(rec + 2, 1);
  PPC_STORE_U8(rec + 4, 1);
  PPC_STORE_U8(rec + 14, 0);
  PPC_STORE_U8(rec + 27, 0);
  ctx.r1.u32 = rec - 256;  // the callees' frames stay below the record; the caller restores r1
  Call(ctx, base, sub_822CBBD0, m, 0, rec);
  Call(ctx, base, sub_822CA958, m, arena, arena_flag);
  Call(ctx, base, sub_82A21200, m, 0);
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t data = Call(ctx, base, sub_822C1A80, id[i]);
    const uint32_t ability = data ? PPC_LOAD_U8(data + 108 + 1) : 2;  // difficulty index 1, the menus' default (56:9570)
    const uint64_t team = i == 0 ? 0 : 0xFFFFFFFFull;                 // dm1 finalize call: slot 0 = 0, slot 1 = -1
    Call(ctx, base, sub_822C9C08, m, i, id[i], team, 0, ability, 1, 1);
    if (attire && attire[i] >= 0)  // the menu's order: participant, then attire (56:9602-9612)
      Call(ctx, base, sub_822C6208, m, i, id[i], uint32_t(attire[i]), 1);
    Call(ctx, base, sub_822C7930, m, i, 0);
    Call(ctx, base, sub_822C7950, m, i, i);
    Call(ctx, base, sub_822C79F8, m, i, 0);
    Call(ctx, base, sub_822C61E8, m, i, 0);
    Call(ctx, base, sub_822C7A18, m, i, 0xFFFFFFFFull);  // the menus' value: no attire choice
  }
  Call(ctx, base, sub_822C5FB8, m, 0, 0, 255);  // slot 0 is the human on controller 0, like the menus (56:10824)
  const uint32_t profile = Call(ctx, base, sub_82255D68, 0);  // same profile source as finalize's controller copy
  Call(ctx, base, sub_822C6020, m, 0, 0, profile, 255, 0);  // finalize's one-on-one controller-0 call (56:10857-10878)
}

// Menu state words, logged whenever one changes (capture mode only): timeline to tell dialogs from the idle main menu.
void LogMenuWords(PPCContext& ctx, uint8_t* base, uint32_t seq, uint32_t sm, uint64_t tick) {
  static uint32_t last[12];
  static int lines = 0;
  if (lines > 400) return;
  const uint32_t now[12] = {seq,
                            sm,
                            sm ? PPC_LOAD_U32(sm + 364) : 0,
                            sm ? PPC_LOAD_U32(sm + 792) : 0,
                            sm ? PPC_LOAD_U32(sm + 796) : 0,
                            sm ? PPC_LOAD_U32(sm + 11404) : 0,
                            sm ? PPC_LOAD_U32(sm + 11412) : 0,
                            sm ? PPC_LOAD_U32(sm + 11492) : 0,
                            sm ? PPC_LOAD_U32(sm + 11500) : 0,
                            sm ? PPC_LOAD_U32(sm + 11512) : 0,
                            PPC_LOAD_U32(kExitAction),
                            sm ? Call(ctx, base, sub_8283C558, sm) : 0};
  if (std::memcmp(now, last, sizeof(now)) == 0) return;
  std::memcpy(last, now, sizeof(now));
  ++lines;
  REXLOG_INFO("[test-direct] menu tick={} seq={} sm=0x{:08X} page={} 792={} pad={} 11404={} 11412=0x{:08X} 11492={} 11500={} 11512={} "
              "exit={} ready={}",
              tick, now[0], now[1], int32_t(now[2]), now[3], int32_t(now[4]), now[5], now[6], now[7], now[8], now[9], now[10], now[11]);
}

bool g_launched = false;
}  // namespace

// Per-frame on the main menu: the sequence 75 update (no direct caller, reached through the object made by sub_828274D0
// at 48:24126, vtable 0x820360E0; candidate until the run logs show it ticking).
extern "C" PPC_FUNC(sub_82827EA8) {
  if (AnyOn() && !g_launched) {
    static uint64_t tick = 0;
    static uint64_t stable = 0;
    if (tick == 0) REXLOG_INFO("[test-direct] main-menu update is ticking");
    ++tick;
    auto saved = std::make_unique<PPCContext>(ctx);
    const uint32_t mgr = PPC_LOAD_U32(kSeqMgrPtr);
    const uint32_t cur = mgr ? PPC_LOAD_U32(mgr) : 0;
    const uint32_t seq = cur ? PPC_LOAD_U32(cur + 48) : 0;
    const uint32_t sm = PPC_LOAD_U32(kSmartMenuPtr);
    if (!CaptureDir().empty()) LogMenuWords(ctx, base, seq, sm, tick);
    const Request& r = Req();
    if (r.on) {
      const bool ready = seq == kSeqMainMenu && sm && PPC_LOAD_U32(sm + 364) == kMenuPageMain && PPC_LOAD_U32(kExitAction) == 0 &&
                         Call(ctx, base, sub_8283C558, sm) == 1;
      stable = ready ? stable + 1 : 0;
      const uint32_t m = PPC_LOAD_U32(kMatchPtr);
      if (stable >= kIdleTicks && m) {
        g_launched = true;
        REXLOG_INFO("[test-direct] launch: main menu idle {} updates, M=0x{:08X}, {} vs {}", stable, m, r.id[0], r.id[1]);
        BuildMatch(ctx, base, m, r.id, r.arena < 0 ? kDefaultArena : r.arena, 0, r.attire);
        Call(ctx, base, sub_822C9F48, m);
        Call(ctx, base, sub_82B65740);
        Dump(base, "m-direct.bin", m, kMatchDumpSize);
        Call(ctx, base, sub_822CB950, m);
        Call(ctx, base, sub_822CCF38);
        Call(ctx, base, sub_827D9370, kSettingsG + 472);
        Dump(base, "g-direct.bin", kSettingsG, kSettingsSize);
        const uint32_t p = PPC_LOAD_U32(kGameDataPtr);
        Call(ctx, base, sub_827F6F58, p + 144, 1);
        Call(ctx, base, sub_827F6F68, p + 144, PPC_LOAD_U32(m + 532));
        Dump(base, "p-direct.bin", p, 0x4000);
        if (int32_t(PPC_LOAD_U32(sm + 796)) < 0) PPC_STORE_U32(sm + 796, 0);  // menu runs hand on controller 0
        Call(ctx, base, sub_8283EC48, sm, 1);
        REXLOG_INFO("[test-direct] exit requested; arena {} pad {}", PPC_LOAD_U32(m + 532), int32_t(PPC_LOAD_U32(sm + 796)));
      }
    }
    ctx = *saved;
  }
  __imp__sub_82827EA8(ctx, base);
}

// Capture: a menu-started match. Before apply: the menus' M, then a dry run of the builder over a copy (M is restored).
extern "C" PPC_FUNC(sub_822CB950) {
  if (!CaptureDir().empty() && uint32_t(ctx.lr) == 0x82929CD8) {  // return address of the finalize's call (56:11006)
    auto saved = std::make_unique<PPCContext>(ctx);
    const uint32_t m = ctx.r3.u32;
    Dump(base, "m-menu.bin", m, kMatchDumpSize);
    Dump(base, "ex-menu.bin", PPC_LOAD_U32(kExhibitionPtr), kExhibitionSize);
    std::vector<uint8_t> copy(base + m, base + m + kMatchDumpSize);
    const uint32_t id[2] = {PPC_LOAD_U32(m + 724 + 56), PPC_LOAD_U32(m + 724 + 3580 + 56)};
    BuildMatch(ctx, base, m, id, int32_t(PPC_LOAD_U32(m + 532)), PPC_LOAD_U8(m + 537));
    Call(ctx, base, sub_822C9F48, m);
    Dump(base, "m-built.bin", m, kMatchDumpSize);
    std::memcpy(base + m, copy.data(), copy.size());
    ctx = *saved;
  }
  __imp__sub_822CB950(ctx, base);
}

extern "C" PPC_FUNC(sub_82928F60) {
  __imp__sub_82928F60(ctx, base);
  if (!CaptureDir().empty()) {
    Dump(base, "g-menu.bin", kSettingsG, kSettingsSize);
    const uint32_t p = PPC_LOAD_U32(kGameDataPtr);
    Dump(base, "p-menu.bin", p, 0x4000);
    if (const uint32_t sm = PPC_LOAD_U32(kSmartMenuPtr))
      REXLOG_INFO("[test-direct] after finalize: page={} pad={} exit={}", int32_t(PPC_LOAD_U32(sm + 364)),
                  int32_t(PPC_LOAD_U32(sm + 796)), PPC_LOAD_U32(kExitAction));
  }
}

// Every sequence start goes through this base constructor (this, id, name) (80:56248): the log's timeline.
extern "C" PPC_FUNC(sub_82C3DB40) {
  if (AnyOn()) REXLOG_INFO("[test-direct] sequence id={}", ctx.r4.u32);
  __imp__sub_82C3DB40(ctx, base);
}
#endif  // WWE2K14_NO_TEST_TOOLS
