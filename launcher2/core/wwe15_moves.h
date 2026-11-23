#pragma once

// WWE 2K15 import, step D: move data (ports of bray15/moves15.py and p15roster build.py move
// pieces). Inputs are decoded tables from the player's own WWE 2K15 and WWE 2K14 files.

#include "wwe15_convert.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

// WAZE move-list records: u32 0xff000000, u32 count, 163 u16 category counts at +8, 160-byte
// records at +336 (category masks at +0, name at +24, owner u16 at +128, flags at +149, ID u16
// at +152); named records first; the table ends on 256 bytes.
constexpr size_t kWWE15WazeHeader = 336;
constexpr size_t kWWE15WazeRecord = 160;

// moves15.py add_waze: each 2K15 record for `ids` not already listed is inserted after its
// nearest preceding 2K15 neighbour that 2K14 lists (bytes 154-159 set to ff as in 2K14), and
// the category counts grow (except 120-123, which the game sets itself). `added` gets one line
// per ID ("50625 at 812" / "50625 already").
bool AddWWE15MoveRecords(const WWE15Bytes& waze14, const WWE15Bytes& waze15,
                         const std::vector<uint16_t>& ids, WWE15Bytes* output,
                         std::vector<std::string>* added, std::string* error);

// The owner u16 (+128) of a 2K15 move record; false if the move has no record.
bool ReadWWE15MoveOwner(const WWE15Bytes& waze15, uint16_t id, uint16_t* owner);

// build.py fix_owner: set each listed move's owner; owner 0x0200 (anyone) also clears the
// owner-exclusive bit 0x08 of byte +149. `changes` lists "id old->new" / "id b149 value".
bool FixWWE15MoveOwners(WWE15Bytes* waze, const std::map<uint16_t, uint16_t>& owners,
                        std::vector<std::string>* changes, std::string* error);

// moves15.py extend_prm + build.py "already there" rule: WWE 2K14's m3_patch.pac MDAT/PRM6 gets the
// WWE 2K15 rows (m3.pac PRM, leaves 0, 1, 4, 6 and their little-endian mirrors 10, 11, 14, 16; leaf 4's
// index ranges move to new rows appended to leaves 5/15) of each move in `ids`, re-sorted by the
// u32 key the game binary-searches. Moves whose PRM6 leaf-0 rows already exist must equal 2K15's
// rows and are not added twice (listed in `already`). Output: the rebuilt plain m3_patch.pac.
// WWE 2K15 m3.pac PRM part 0, every leaf unpacked (0 = 40-byte big-endian move rows, 10 = mirror...).
bool ReadWWE15PrmTables(const WWE15Bytes& wwe15_m3_pac, std::map<uint32_t, WWE15Bytes>* tables, std::string* error);

struct WWE15PrmReport {
  std::vector<uint16_t> already;
  std::map<uint32_t, size_t> rows_added;  // leaf -> rows (leaf 5: index entries)
};
bool ExtendWWE15Prm6(const WWE15Bytes& m3_patch_pac, const WWE15Bytes& wwe15_m3_pac,
                     const std::vector<uint16_t>& ids, WWE15Bytes* output, WWE15PrmReport* report,
                     std::string* error);

// build.py add_pilot_d004_motc: in the decoded MOTC table (moveset-menu motion groups), every set
// that lists a retail add-on group (D002-D005) gets the routed move IDs in its D004 group, which is
// inserted at its canonical slot 21 when absent (stock IDs kept first, no duplicates). Returns the
// number of sets updated in `updated`.
bool AddWWE15D004Motc(const WWE15Bytes& motc, const std::vector<uint16_t>& ids, WWE15Bytes* output,
                      size_t* updated, std::string* error);

// build.py: a PAC with MOVS/WAZE and MOVS/MOTC (createmode.pac, misc_dlc0N.pac) gets the 2K15 move
// records (+ owner fix) and the D004 route; both tables are stored unpacked, MOTC padded to 256.
bool UpdateWWE15MoveMenus(const WWE15Bytes& pac, const WWE15Bytes& waze15, const std::vector<uint16_t>& ids,
                          const std::map<uint16_t, uint16_t>& owners, const std::vector<uint16_t>& d004_ids,
                          WWE15Bytes* output, size_t* d004_sets, std::string* error);

// ---- motion archives (ZMOT/ZMVM: entry -> PACH banks -> PACH parts -> PACH of move records)
// True for a disc stand-in PAC (one "DMMY" entry, no moves; WWE 2K15's dlc_mot01-03.pac).
bool IsWWE15PlaceholderPac(const WWE15Bytes& plain_pac);
// Every move ID the archive holds at depth 3 of any group (research index rule used by the plan:
// a leaf is any node that is not a readable PACH, packed or not).
bool ScanWWE15MotionIds(const WWE15Bytes& pac, std::set<uint16_t>* ids, std::string* error);
// Locations of `ids` in the ZMOT/ZMVM groups (build.py scan_motion_archives).
struct WWE15MotionHit {
  std::string group, entry;
  uint32_t bank = 0, part = 0;
  uint16_t id = 0;
};
bool ScanWWE15MotionRoutes(const WWE15Bytes& pac, const std::set<uint16_t>& ids,
                           std::vector<WWE15MotionHit>* hits, std::string* error);
// moves15.py index15 for `ids` only: the first even/odd record (packed bytes) per move, archives in
// the caller's order. Existing map entries are kept (first wins across calls).
struct WWE15MotionRecord {
  WWE15Bytes even, odd;
  bool has_even = false, has_odd = false;
};
bool CollectWWE15MotionRecords(const WWE15Bytes& pac, const std::set<uint16_t>& ids,
                               std::map<uint16_t, WWE15MotionRecord>* records, std::string* error);
// build.py merge_d004_archive: the routed moves' 2K15 records go into bank 399 of the retail add-on 4
// dlc_mot04.pac (ZMOT/DLC4 and ZMVM/D004, which must agree); every stock record is kept.
bool MergeWWE15D004Archive(const WWE15Bytes& dlc_mot04_pac, const std::vector<uint16_t>& ids,
                           const std::map<uint16_t, WWE15MotionRecord>& records, WWE15Bytes* output,
                           std::string* error);

// Presence of each move's even/odd record (index15 rule: any group, first record per part parity).
bool ScanWWE15MotionParts(const WWE15Bytes& pac, std::set<uint16_t>* even, std::set<uint16_t>* odd, std::string* error);

// ---- move plan (p15roster plan.py): the 2K15 PRO row's 262 slots + 22 extras -> 2K14 values.
struct WWE15MoveFacts {
  std::set<uint16_t> base14;       // IDs in the player's WWE 2K14 game motion archives (pac/*mot*.pac)
  std::set<uint16_t> addon14;      // IDs in the WWE 2K14 add-on motion archives (dlc_mot*.pac)
  std::set<uint16_t> all15;        // IDs in the WWE 2K15 motion archives (pac/*mot*.pac)
  std::set<uint16_t> even15, odd15;// IDs with an even / odd record in WWE 2K15 mot00-03, 05-08
  WWE15Bytes prm15_rows;           // WWE 2K15 PRM leaf 0 (40-byte rows)
  std::set<uint16_t> waze15;       // IDs with a WWE 2K15 move-list record
  std::set<uint16_t> playable14;   // IDs in any playable stock WWE 2K14 PRO row (field 576 != 255)
};
// data/wwe2k15-moves.tsv: research decisions, move/wrestler IDs only.
struct WWE15MoveRules {
  std::map<uint16_t, std::string> unsupported;            // 2K15 moves using events/bone flags 2K14 lacks
  std::set<uint32_t> stall_donor_212;                      // source IDs: slots 212-213 from the donor
  std::map<uint32_t, std::map<uint32_t, uint16_t>> stall_slot;  // source ID -> slot -> 2K15 value to plan
  std::set<uint16_t> allow_loop;  // 2K15 moves whose attacker-only self-loop is proven safe in 2K14 (skip the lock rule)
};
bool ReadWWE15MoveRules(const std::string& text, WWE15MoveRules* rules, std::string* error);
struct WWE15MovePlan {
  std::vector<uint16_t> slots;   // 262
  std::vector<uint16_t> extras;  // 22
  std::vector<uint16_t> port;    // 2K15 moves to bring over (sorted)
  std::vector<std::string> notes;
};
bool PlanWWE15Moves(uint32_t source_id, const WWE15Bytes& pro15, const WWE15Bytes& donor_pro14,
                    const WWE15MoveFacts& facts, const WWE15MoveRules& rules, WWE15MovePlan* plan,
                    std::string* error);

}  // namespace wwe2k14::launcher
