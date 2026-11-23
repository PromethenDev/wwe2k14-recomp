#pragma once

// WWE 2K15 -> WWE 2K14 converters for the "Import from WWE 2K15" feature. Every input is the
// player's own WWE 2K15 Xbox 360 files, read at import time; nothing converted ships with the
// launcher. Each converter is a port of the project's own python route (tools/port/p15roster,
// tools/port/bray15) and is accepted only when its output is byte-identical to that route for
// the same wrestlers (docs/tasks/P15-LAUNCHER-plan.md). Error messages name the wrestler ID,
// the entry and the size seen vs expected, so a player report says exactly what differs.

#include "wwe13_convert.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

using WWE15Bytes = std::vector<uint8_t>;

// Leaf IDs WWE 2K14's model loader knows (every leaf any shipped 2K14 ch PAC entry carries,
// minus 100). 2K15 leaves outside this set are dropped from converted models.
bool IsWWE2K14ModelLeaf(uint32_t leaf_id);

// Step A (build.py model + model_pac): the player's 2K15 pac/ch/ch<source>.pac -> a WWE 2K14
// add-on ch<target>_dlc02.pac. Every EMD entry is kept with its leaves filtered to the 2K14
// set and renamed to the target ID; a single-look 2K15 model named ..00 becomes the default
// ..02; ..04 is a copy of ..02 when 2K15 has none. `notes` receives one line per change.
bool ConvertWWE15CharacterPac(const WWE15Bytes& source_wwe15_pac, uint32_t source_id,
                              uint32_t target_id, WWE15Bytes* output,
                              std::vector<std::string>* notes, std::string* error);

// Replaces matching WWE 2K14 attire models with the WWE 2K15 leaves known by WWE 2K14. The
// original's leaves unknown to 2K15 are retained; unknown WWE 2K15 leaves are reported and dropped.
struct WWE15LooksReport {
  uint32_t id = 0;
  std::vector<std::string> swapped;
  std::vector<std::string> kept14;
  std::vector<std::string> only15;
  std::map<std::string, std::vector<uint32_t>> dropped_leaves;
};
bool ConvertWWE15LooksPac(const WWE15Bytes& wwe14_pac, const WWE15Bytes& wwe15_pac,
                          uint32_t id, WWE15Bytes* output, WWE15LooksReport* report,
                          std::string* error);

// Step C (bray15/evp2.py): a decompressed 2K15 EVP2 entrance/victory script -> a decompressed
// 2K14 EVP. Each 788-byte list element loses its 576-byte 2K15 extension (cut at the record
// boundary, byte 204) and the POF0 pointer table is rebuilt; elements that use more than 2K14's
// 8 path records are resampled to 8 (first and last kept). `key` names the entry in messages.
struct WWE15Evp2Info {
  size_t lists = 0;
  size_t elements = 0;
  size_t dropped_nonzero = 0;  // elements whose removed extension held data
  size_t pointers = 0;
  size_t tail_padding = 0;
  size_t resampled = 0;
};
bool ConvertWWE15Evp2(const WWE15Bytes& evp2, const std::string& key, WWE15Bytes* evp,
                      WWE15Evp2Info* info, std::string* error);

// evp2.py fill_wrestler_tracks: opponent/referee tracks (flags 0x01000000, track > 0) that hold
// no standing positions get the donor EVP's records on steps 0, 5, 10... (same track number and
// actor). Both inputs are decompressed 2K14 EVPs. `filled` lists (track, step) pairs.
bool FillWWE15WrestlerTracks(WWE15Bytes* evp, const WWE15Bytes& donor_evp,
                             std::vector<std::pair<uint32_t, uint32_t>>* filled,
                             std::string* error);

// build.py entrance rewrite on a converted EVP: own key (u16 at +32) = target; track 0's attire
// code (u32 at 0x50) moves to target*100+look when its person part is the source ID, the 2K15
// entrance key or the 2K14 original (alternates); otherwise it is left and a note says so.
bool RetargetWWE15Entrance(WWE15Bytes* evp, uint32_t target_id, uint32_t source_id,
                           uint32_t entrance_key, uint32_t original14_id,
                           std::vector<std::string>* notes, std::string* error);
// Event objects (props) a converted EVP loads: (track index, object ID) for tracks whose
// flags word is 0x100. SetWWE15EntranceProp replaces one track's object ID.
struct WWE15EntranceProp {
  uint32_t track = 0;
  uint32_t object_id = 0;
};
bool ListWWE15EntranceProps(const WWE15Bytes& evp, std::vector<WWE15EntranceProp>* props,
                            std::string* error);
bool SetWWE15EntranceProp(WWE15Bytes* evp, uint32_t track, uint32_t object_id, std::string* error);

// Step B (build.py "rows"): the new wrestler's chEtc rows. DAT = the donor's stock 2K14 row with
// the target's IDs, name/handle keys, own attire codes, the free DAT index chosen by the caller,
// and 2K15's height (22-23), class (26) and attributes (36-49). DSBD = the four attire codes +
// 72 zero bytes. PRO = the donor's stock 2K14 row (1276 bytes) with the target in its set
// fields, the music selectors per kind, and the planned 262 move slots + 22 extras.
struct WWE15RowsPort {
  uint32_t source_id = 0;
  uint32_t target_id = 0;
  uint32_t donor_id = 0;
  uint16_t dat_index = 0;
  bool alternate = false;         // kind "alt": music = the 2K14 original's theme
  uint32_t original14_id = 0;     // alternates only
  bool donor_music = false;       // no 2K15 Play_MUS event: music = the donor's stock theme
  std::vector<uint16_t> slots;    // 262 planned move slots
  std::vector<uint16_t> extras;   // 22 planned extra IDs
};
struct WWE15Rows {
  WWE15Bytes dat;
  WWE15Bytes dsbd;
  WWE15Bytes pro;
};
bool ConvertWWE15Rows(const WWE15RowsPort& port, const WWE15Bytes& source15_dat,
                      const WWE15Bytes& donor14_dat, const WWE15Bytes& donor14_pro,
                      WWE15Rows* output, std::string* error);

}  // namespace wwe2k14::launcher
