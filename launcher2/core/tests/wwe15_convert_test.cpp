// Synthetic host tests for the WWE 2K15 converters (no game files needed). The byte-identical
// checks against the python route on the player's real files are run with wwe15_convert_probe
// (docs/tasks/P15-LAUNCHER-plan.md, "Evidence").
#include "../wwe15_convert.h"
#include "../wwe15_moves.h"
#include "../wwe15_portraits.h"
#include "../wwe15_install.h"
#include "../wwe15_pipeline.h"
#include "../wwe15_looks.h"
#include <filesystem>
#include <fstream>
#include <chrono>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace wwe2k14::launcher;
using Bytes = WWE15Bytes;

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void PutLe32(Bytes* bytes, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) (*bytes)[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

Bytes Pach(const std::vector<std::pair<uint32_t, Bytes>>& leaves) {
  Bytes output(8 + leaves.size() * 12, 0);
  std::memcpy(output.data(), "PACH", 4);
  PutLe32(&output, 4, static_cast<uint32_t>(leaves.size()));
  Bytes body;
  for (size_t i = 0; i < leaves.size(); ++i) {
    const size_t record = 8 + i * 12;
    PutLe32(&output, record, leaves[i].first);
    PutLe32(&output, record + 4, static_cast<uint32_t>(body.size()));
    PutLe32(&output, record + 8, static_cast<uint32_t>(leaves[i].second.size()));
    body.insert(body.end(), leaves[i].second.begin(), leaves[i].second.end());
  }
  output.insert(output.end(), body.begin(), body.end());
  return output;
}

Bytes Leaf(uint8_t value, size_t size = 5) { return Bytes(size, value); }
Bytes PacEntryBytes(Bytes data) { data.resize((data.size() + 0xff) & ~size_t(0xff), 0); return data; }

Bytes ModelPac(const std::vector<std::pair<std::string, Bytes>>& entries) {
  WWE13PacGroupData group;
  group.name = "EMD ";
  group.wide = true;
  for (const auto& [name, data] : entries) group.entries.push_back({name, data});
  Bytes pac;
  std::string error;
  Check(BuildWWE13PacGroups({group}, &pac, &error), "synthetic PAC: " + error);
  return pac;
}

std::map<std::string, std::map<uint32_t, Bytes>> ReadModel(const Bytes& pac) {
  std::vector<WWE13PacGroupData> groups;
  std::string error;
  Check(ReadWWE13PacGroups(pac, &groups, &error), "read converted PAC: " + error);
  Check(groups.size() == 1 && groups[0].name == "EMD " && groups[0].wide, "one wide EMD group");
  std::map<std::string, std::map<uint32_t, Bytes>> out;
  for (const auto& entry : groups[0].entries) {
    std::vector<WWE13PachLeafData> leaves;
    Check(ReadWWE13PachLeaves(entry.data, &leaves, &error), "read converted PACH: " + error);
    for (auto& leaf : leaves) {
      // the entry is sector padded; the leaf sizes in the table are exact
      out[entry.name][leaf.id] = leaf.data;
    }
  }
  return out;
}

void TestExistingLooks() {
  const Bytes old_shared = Pach({{0, Leaf(0x10)}, {31, Leaf(0x31)}});
  const Bytes old_only = Pach({{10, Leaf(0x12)}});
  const Bytes from15 = Pach({{10, Leaf(0x25)}, {100, Leaf(0x64)}});
  WWE13PacGroupData base_emd;
  base_emd.name = "EMD "; base_emd.wide = true;
  base_emd.entries = {{"00010002", StoreWWE13BpeLiteral(old_shared)},
                      {"00010012", StoreWWE13BpeLiteral(old_only)}};
  WWE13PacGroupData other;
  other.name = "TEX ";
  other.entries = {{"KEEP", Leaf(0x55)}};
  Bytes base;
  std::string error;
  Check(BuildWWE13PacGroups({base_emd, other}, &base, &error), "base looks PAC: " + error);
  WWE13PacGroupData source_emd;
  source_emd.name = "EMD "; source_emd.wide = true;
  source_emd.entries = {{"00010002", StoreWWE13BpeLiteral(from15)},
                        {"00010032", StoreWWE13BpeLiteral(Pach({{0, Leaf(0x32)}}))}};
  Bytes source;
  Check(BuildWWE13PacGroups({source_emd}, &source, &error), "source looks PAC: " + error);

  Bytes output;
  WWE15LooksReport report;
  Check(ConvertWWE15LooksPac(base, source, 100, &output, &report, &error), "looks conversion: " + error);
  Check(report.id == 100 && report.swapped == std::vector<std::string>{"00010002"} &&
            report.kept14 == std::vector<std::string>{"00010012"} &&
            report.only15 == std::vector<std::string>{"00010032"},
        "looks report classifies shared, 2K14-only and 2K15-only attire names");
  Check(report.dropped_leaves.size() == 1 && report.dropped_leaves.at("00010002") == std::vector<uint32_t>{100},
        "unknown 2K15 leaves are reported");

  std::vector<WWE13PacGroupData> groups;
  Check(ReadWWE13PacGroups(output, &groups, &error), "read looks output: " + error);
  Check(groups.size() == 2 && groups[1].name == "TEX " &&
            groups[1].entries[0].data == PacEntryBytes(Leaf(0x55)),
        "other PAC groups are retained");
  Check(groups[0].entries[1].data == PacEntryBytes(StoreWWE13BpeLiteral(old_only)),
        "2K14-only attire is byte-preserved");
  WWE13ConvertBytes merged_plain;
  Check(DecodeWWE13Bpe(groups[0].entries[0].data, &merged_plain, &error), "decode merged PACH: " + error);
  std::vector<WWE13PachLeafData> merged;
  Check(ReadWWE13PachLeaves(merged_plain, &merged, &error), "read merged PACH: " + error);
  std::map<uint32_t, Bytes> leaf_map;
  for (const auto& leaf : merged) leaf_map[leaf.id] = leaf.data;
  Check(leaf_map.size() == 3 && leaf_map.at(0) == Leaf(0x10) && leaf_map.at(10) == Leaf(0x25) &&
            leaf_map.at(31) == Leaf(0x31),
        "2K15 known leaves replace, while 2K14-only leaves survive");

  WWE13PacGroupData no_match;
  no_match.name = "EMD "; no_match.wide = true; no_match.entries = {{"00010099", StoreWWE13BpeLiteral(old_only)}};
  Bytes unmatched;
  Check(BuildWWE13PacGroups({no_match}, &unmatched, &error), "unmatched PAC: " + error);
  Check(ConvertWWE15LooksPac(unmatched, source, 100, &output, &report, &error) && output == unmatched,
        "no shared attire is left byte-identical");
}

void TestModelSingle00() {
  // Staff-style model: only ..00. Leaf 31 and 100 are not 2K14 leaves.
  const Bytes source = ModelPac({{"00018900", Pach({{100, Leaf(1)}, {0, Leaf(2, 7)}, {31, Leaf(3)}, {10, Leaf(4)}})}});
  Bytes output;
  std::vector<std::string> notes;
  std::string error;
  Check(ConvertWWE15CharacterPac(source, 189, 300, &output, &notes, &error), "single 00: " + error);
  const auto model = ReadModel(output);
  Check(model.size() == 2 && model.count("00030002") && model.count("00030004"), "00 -> 02 and 04");
  Check(model.at("00030002").size() == 2 && model.at("00030002").at(0) == Leaf(2, 7) &&
            model.at("00030002").at(10) == Leaf(4),
        "leaves filtered to the 2K14 set");
  Check(model.at("00030004") == model.at("00030002"), "04 copies 02");
  Check(notes.size() == 3 && notes[0] == "00018900 dropped leaves [31, 100]" &&
            notes[1] == "single 2K15 00 model mapped to default 02" && notes[2] == "04 = copy of 02",
        "notes name every change");
  // Leaf alignment: second leaf starts on a 16-byte boundary.
  std::vector<WWE13PacGroupData> groups;
  Check(ReadWWE13PacGroups(output, &groups, &error), error);
  const Bytes& pach = groups[0].entries[0].data;
  Check(pach[8 + 12 + 4] == 16, "leaf 10 starts at 16 (16-byte aligned after a 7-byte leaf)");
}

void TestModelKeeps04AndErrors() {
  const Bytes source = ModelPac({{"00040002", Pach({{0, Leaf(5)}})}, {"00040004", Pach({{0, Leaf(6)}})},
                                 {"00040012", Pach({{0, Leaf(7)}})}});
  Bytes output;
  std::vector<std::string> notes;
  std::string error;
  Check(ConvertWWE15CharacterPac(source, 400, 178, &output, &notes, &error), "keep 04: " + error);
  const auto model = ReadModel(output);
  Check(model.size() == 3 && model.at("00017804").at(0) == Leaf(6) && notes.empty(), "own 04 kept, no notes");

  const Bytes wrong = ModelPac({{"00040102", Pach({{0, Leaf(5)}})}});
  Check(!ConvertWWE15CharacterPac(wrong, 400, 178, &output, &notes, &error) &&
            error.find("WWE 2K15 wrestler 400") != std::string::npos &&
            error.find("00040102") != std::string::npos,
        "a foreign entry name is refused with the wrestler ID and the entry");
  const Bytes no_default = ModelPac({{"00040012", Pach({{0, Leaf(5)}})}});
  Check(!ConvertWWE15CharacterPac(no_default, 400, 178, &output, &notes, &error) &&
            error.find("no default look") != std::string::npos,
        "missing default look is named");
  Check(!ConvertWWE15CharacterPac(Bytes(16, 0), 400, 178, &output, &notes, &error) &&
            error.find("ch400.pac could not be read") != std::string::npos,
        "a non-PAC file names the file");
}

void PutBe32(Bytes* b, size_t at, uint32_t v) {
  (*b)[at] = uint8_t(v >> 24); (*b)[at + 1] = uint8_t(v >> 16); (*b)[at + 2] = uint8_t(v >> 8); (*b)[at + 3] = uint8_t(v);
}
uint32_t Be32(const Bytes& b, size_t at) {
  return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | b[at + 3];
}

// One track, one list with one 788-byte element using 10 path records, two pointers (one before
// and one after the element) and the second size field equal to the body size.
Bytes SyntheticEvp2() {
  Bytes p(0x3A8, 0);
  std::memcpy(p.data(), "0FOP", 4);
  PutBe32(&p, 4, 0x3A0);
  p[8] = 'E'; p[9] = 'V';
  PutBe32(&p, 0x0c, 0x3A0);
  PutBe32(&p, 0x40, 1);              // one track
  p[0x64] = 0; p[0x65] = 1;          // list 1: one element
  PutBe32(&p, 0x68, 0x88);           // its offset (file 0x90)
  for (size_t k = 0; k < 10; ++k)
    for (size_t i = 0; i < 24; ++i) p[0x90 + 16 + 24 * k + i] = uint8_t(k + 1);
  p[0x90 + 204 + 3] = 0x77;          // data in the removed 2K15 extension
  PutBe32(&p, 0x3A4, 0x88);          // a pointer after the element
  const uint8_t pof[] = {'P', 'O', 'F', '0', 0, 0, 0, 4, 0x58, 0x80, 0xCF, 0};
  p.insert(p.end(), std::begin(pof), std::end(pof));
  return p;
}

void TestEvp2() {
  Bytes evp;
  WWE15Evp2Info info;
  std::string error;
  Check(ConvertWWE15Evp2(SyntheticEvp2(), "504", &evp, &info, &error), "evp2: " + error);
  Check(info.lists == 1 && info.elements == 1 && info.dropped_nonzero == 1 && info.pointers == 2 &&
            info.resampled == 1,
        "evp2 info");
  Check(Be32(evp, 4) == 0x3A0 - 576 && Be32(evp, 0x0c) == 0x3A0 - 576, "both size fields shrink by 576");
  Check(Be32(evp, 0x68) == 0x88 && Be32(evp, 0x3A4 - 576) == 0x88, "pointers moved and kept");
  const size_t picks[] = {0, 1, 3, 4, 5, 6, 8, 9};  // round(i * 9 / 7)
  for (size_t i = 0; i < 8; ++i) Check(evp[0x90 + 16 + 24 * i] == picks[i] + 1, "resampled record " + std::to_string(i));
  const size_t pof = 8 + Be32(evp, 4);
  Check(std::memcmp(evp.data() + pof, "POF0", 4) == 0 && Be32(evp, pof + 4) == 4 && evp[pof + 8] == 0x58 &&
            evp[pof + 9] == 0x7F && evp[pof + 10] == 0 && evp.size() == pof + 12,
        "pointer table rebuilt (step 207 -> 63 words fits one byte)");

  Bytes cut = SyntheticEvp2();
  cut.resize(0x60);
  Check(!ConvertWWE15Evp2(cut, "6702", &evp, &info, &error) && error.find("script 6702") != std::string::npos &&
            error.find("96 bytes") != std::string::npos,
        "a truncated script names its key and size: " + error);

  // Opponent track 1 with no positions takes the donor's on steps 0 and 5.
  Bytes mine(0x4c + 64 + 8 * 212 + 8, 0), donor;
  PutBe32(&mine, 0x40, 2);
  PutBe32(&mine, 0x4c + 32 + 4, 25001);
  PutBe32(&mine, 0x4c + 32 + 8, 0x01000000);
  mine[0x4c + 32 + 24] = 0; mine[0x4c + 32 + 25] = 6;
  PutBe32(&mine, 0x4c + 32 + 28, 0x4c + 64);
  donor = mine;
  for (size_t e = 0; e < 6; ++e) donor[8 + 0x4c + 64 + e * 212 + 16] = uint8_t(0x30 + e);
  std::vector<std::pair<uint32_t, uint32_t>> filled;
  Check(FillWWE15WrestlerTracks(&mine, donor, &filled, &error), "fill: " + error);
  Check(filled.size() == 2 && filled[0] == std::make_pair(1u, 0u) && filled[1] == std::make_pair(1u, 5u) &&
            mine[8 + 0x4c + 64 + 16] == 0x30 && mine[8 + 0x4c + 64 + 5 * 212 + 16] == 0x35 &&
            mine[8 + 0x4c + 64 + 212 + 16] == 0,
        "steps 0 and 5 filled from the donor, others untouched");
}

uint16_t Le16(const Bytes& b, size_t at) { return uint16_t(b[at] | (b[at + 1] << 8)); }
uint32_t Le32(const Bytes& b, size_t at) { return uint32_t(Le16(b, at)) | (uint32_t(Le16(b, at + 2)) << 16); }

void TestRows() {
  WWE15RowsPort port;
  port.source_id = 189; port.target_id = 300; port.donor_id = 290; port.dat_index = 262; port.donor_music = true;
  port.slots.assign(262, 7); port.extras.assign(22, 9);
  Bytes d15(60, 0x15), d14(140, 0x14), p14(1276, 0x41);
  WWE15Rows rows;
  std::string error;
  Check(ConvertWWE15Rows(port, d15, d14, p14, &rows, &error), "rows: " + error);
  Check(Le16(rows.dat, 4) == 300 && Le16(rows.dat, 6) == 262 && Le16(rows.dat, 10) == 300 + 0xEA60 &&
            rows.dat[22] == 0x15 && rows.dat[24] == 0x14 && rows.dat[26] == 0x15 && rows.dat[49] == 0x15 &&
            rows.dat[50] == 0x14 && Le16(rows.dat, 120) == 30001 && Le16(rows.dat, 126) == 30004 &&
            Le16(rows.dat, 136) == 300,
        "DAT fields");
  Check(rows.dsbd.size() == 80 && Le16(rows.dsbd, 6) == 30004 && rows.dsbd[8] == 0, "DSBD codes + 72 zero bytes");
  Check(Le16(rows.pro, 576) == 290 && Le16(rows.pro, 592) == 290 && Le16(rows.pro, 578) == 300 &&
            Le16(rows.pro, 4) == 7 && Le16(rows.pro, 526) == 7 && Le16(rows.pro, 528) == 9 && Le16(rows.pro, 570) == 9 &&
            rows.pro[572] == 0x41,
        "PRO set fields, donor music, slots and extras");
  Bytes short_pro(1200, 0);
  Check(!ConvertWWE15Rows(port, d15, d14, short_pro, &rows, &error) && error.find("wrestler 189") != std::string::npos &&
            error.find("1200 bytes; 1276 expected") != std::string::npos,
        "a wrong PRO size names the wrestler and both sizes: " + error);
}

Bytes Waze(const std::vector<std::pair<uint16_t, bool>>& records) {  // (id, named)
  Bytes w(kWWE15WazeHeader, 0);
  PutLe32(&w, 0, 0xff000000);
  PutLe32(&w, 4, static_cast<uint32_t>(records.size()));
  for (const auto& [id, named] : records) {
    Bytes r(kWWE15WazeRecord, 0);
    r[0] = 0x05;  // categories 0 and 2
    r[15] = 0x08; // category 123 (left to the game)
    if (named) r[24] = 'M';
    r[152] = uint8_t(id); r[153] = uint8_t(id >> 8);
    r[128] = 0x99; r[129] = 0x01;  // owner 409
    r[149] = 0x08;
    w.insert(w.end(), r.begin(), r.end());
  }
  w.resize((w.size() + 0xff) & ~size_t(0xff), 0);
  return w;
}

void TestWaze() {
  const Bytes w14 = Waze({{10, true}, {30, true}, {0, false}});
  const Bytes w15 = Waze({{10, true}, {20, true}, {40, true}, {30, true}});
  Bytes out;
  std::vector<std::string> added;
  std::string error;
  Check(AddWWE15MoveRecords(w14, w15, {40, 20, 10}, &out, &added, &error), "waze: " + error);
  Check(added.size() == 3 && added[0] == "10 already" && added[1] == "20 at 1" && added[2] == "40 at 2",
        "records placed after their nearest 2K15 neighbour");
  Check(Le16(out, 4) == 5 && out.size() % 256 == 0, "count and 256-byte end");
  Check(Le16(out, 336 + 160 * 1 + 152) == 20 && Le16(out, 336 + 160 * 2 + 152) == 40 &&
            Le16(out, 336 + 160 * 3 + 152) == 30 && out[336 + 160 * 4 + 24] == 0,
        "order 10 20 40 30, unnamed last");
  Check(out[336 + 160 + 154] == 0xff && out[336 + 160 + 159] == 0xff, "bytes 154-159 = ff");
  Check(Le16(out, 8) == 2 && Le16(out, 12) == 2 && Le16(out, 8 + 2 * 123) == 0, "category counts grow, 123 untouched");
  std::vector<std::string> changes;
  Check(FixWWE15MoveOwners(&out, {{20, 0x0200}, {40, 175}}, &changes, &error), "owners: " + error);
  Check(Le16(out, 336 + 160 + 128) == 0x200 && (out[336 + 160 + 149] & 8) == 0 &&
            Le16(out, 336 + 320 + 128) == 175 && (out[336 + 320 + 149] & 8) == 8 && changes.size() == 3,
        "anyone clears the exclusive bit; a real owner keeps it");
  Check(!AddWWE15MoveRecords(w14, w15, {55}, &out, &added, &error) && error.find("Move 55") != std::string::npos,
        "a move with no 2K15 record is named");
}

void TestMotc() {
  auto u32 = [](Bytes* b, uint32_t v) { for (int i = 0; i < 4; ++i) b->push_back(uint8_t(v >> (8 * i))); };
  auto u16 = [](Bytes* b, uint16_t v) { b->push_back(uint8_t(v)); b->push_back(uint8_t(v >> 8)); };
  auto head = [](std::vector<std::pair<size_t, const char*>> names) {
    Bytes h(248, 0);
    for (auto [slot, n] : names) std::memcpy(h.data() + 4 * slot, n, 4);
    return h;
  };
  Bytes m(248, 0);
  u32(&m, 0); u32(&m, 0);  // two empty ID lists
  u32(&m, 1);              // one table
  u32(&m, 2);              // two sets
  Bytes a = head({{3, "D002"}});
  m.insert(m.end(), a.begin(), a.end());
  u32(&m, 1); u16(&m, 5); u16(&m, 0);
  Bytes b = head({{21, "D004"}});
  m.insert(m.end(), b.begin(), b.end());
  u32(&m, 2); u16(&m, 7); u16(&m, 9);
  m.push_back(0xAB);       // tail kept
  Bytes out;
  size_t updated = 0;
  std::string error;
  Check(AddWWE15D004Motc(m, {9, 3}, &out, &updated, &error), "motc: " + error);
  Check(updated == 2, "both sets updated");
  size_t p = 248 + 16;
  Check(std::memcmp(out.data() + p + 84, "D004", 4) == 0, "D004 inserted at slot 21");
  p += 248;
  Check(Le32(out, p) == 1 && Le16(out, p + 4) == 5 && Le32(out, p + 8) == 2 && Le16(out, p + 12) == 3 &&
            Le16(out, p + 14) == 9,
        "set A: stock D002 block then new D004 block (3, 9)");
  p += 16 + 248;
  Check(Le32(out, p) == 3 && Le16(out, p + 4) == 7 && Le16(out, p + 6) == 9 && Le16(out, p + 8) == 3 &&
            Le16(out, p + 10) == 0 && out.back() == 0xAB,
        "set B: stock 7, 9 kept, 3 appended once, padded; tail kept");
  Bytes bad = m;
  std::memcpy(bad.data() + 248 + 16 + 4 * 22, "D004", 4);
  Check(!AddWWE15D004Motc(bad, {3}, &out, &updated, &error) && error.find("outside its slot 21") != std::string::npos,
        "D004 in a non-canonical slot is refused");
}

void TestPortrait() {
  // 256-square portrait: top half (stored last) red opaque, bottom half blue half-transparent; pixels R,A,B,G.
  Bytes raw(44 + 256 * 256 * 4, 0);
  raw[12] = 0; raw[13] = 1;
  for (int row = 0; row < 256; ++row)
    for (int x = 0; x < 256; ++x) {
      uint8_t* p = &raw[44 + (size_t(row) * 256 + x) * 4];
      if (row >= 128) { p[0] = 255; p[1] = 255; p[2] = 0; p[3] = 0; }   // stored rows 128+ = top of the image
      else { p[0] = 0; p[1] = 128; p[2] = 255; p[3] = 0; }
    }
  Bytes dds, rgba;
  std::string error;
  Check(ConvertWWE15Portrait(raw, 512, 256, &dds, &error), "portrait: " + error);
  int w = 0, h = 0, mips = 0;
  Check(DecodeWWE15Dxt5Dds(dds, &w, &h, &mips, &rgba, &error), "decode: " + error);
  Check(w == 512 && h == 256 && mips == 10, "512x256, 10 mips");
  Check(dds.size() == 128 + 131072 + 32768 + 8192 + 2048 + 512 + 128 + 32 + 16 + 16 + 16, "every mip level present");
  Check(Le32(dds, 8) == 0xA1007 && Le32(dds, 20) == 131072 && Le32(dds, 108) == 0x401008, "header fields as Yuke's sheets");
  Check(rgba[0] == 255 && rgba[2] == 0 && rgba[3] == 255, "top is red and opaque (flipped)");
  const size_t bottom = (size_t(255) * 512) * 4;
  Check(rgba[bottom] == 0 && rgba[bottom + 2] == 255 && rgba[bottom + 3] == 128, "bottom is blue, alpha 128");
  raw[13] = 3;
  Check(!ConvertWWE15Portrait(raw, 512, 512, &dds, &error) && error.find("768") != std::string::npos, "a bad side is named");
}

namespace fs = std::filesystem;

void Touch(const fs::path& p, const Bytes& data = Bytes(16, 0)) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}
Bytes Catalog(uint16_t pack, uint16_t offers) {
  Bytes c(0x1c + 0x40 * offers + 4, 0);
  std::memcpy(c.data(), "DLCC", 4);
  c[8] = uint8_t(pack); c[12] = uint8_t(offers);
  return c;
}

// The checks that run before any conversion: path length, the WWE 2K15 folder, Add-On 2 by contents.
void TestImportGuards() {
  const fs::path root = fs::temp_directory_path() /
      ("wwe15-guards-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
  // 1. a launcher folder deeper than 125 characters is refused before anything else
  Paths deep;
  deep.exe_dir = root / std::string(130, 'd');
  deep.userdata_dir = deep.exe_dir / "userdata";
  deep.backups_dir = deep.exe_dir / "backups";
  CancelFlag cancel{false};
  WWE15ImportScan scan;
  const Result r = RunWWE15Import(deep, root / "game", scan, {100}, {}, cancel, nullptr);
  Check(!r.ok && r.error.find("too long for Windows") != std::string::npos, "125-character guard: " + r.error);
  // 2. the WWE 2K15 folder is found by contents
  const fs::path w15 = root / "my 2k15 copy" / "game" / "pac";
  for (const char* f : {"ch/chEtc.pac", "m3.pac", "createmode.pac", "ch/ch424.pac", "ch/ch398.pac", "menu/menuHD2.pac", "menu/menuSD2.pac"})
    Touch(w15 / f);
  Check(FindWWE15PacFolder(root / "my 2k15 copy") == w15, "2K15 pac folder found by contents");
  Check(FindWWE15PacFolder(root / "game").empty(), "no 2K15 folder in an empty tree");
  // 3. Add-On 2: none, a stray import folder with no record, then Add-On 2 under another name without its header
  const fs::path title = root / "ud" / "0000000000000000" / "545408B2" / "00000002";
  fs::create_directories(title);
  WWE15ImportPaths paths;
  paths.wwe15_root = root / "my 2k15 copy";
  paths.wwe14_root = root / "game";
  paths.userdata_root = root / "ud";
  WWE15ImportPort port;
  port.source_id = 100; port.target_id = 426; port.name = "THE ROCK '15"; port.donor_id = 100;
  WWE15ImportRunResult result;
  std::string error;
  int n = 0;
  auto run = [&] {
    result = {};
    return RunWWE15ImportStages(paths, {port}, root / ("stage" + std::to_string(n++)), {}, &result, &error);
  };
  Check(!run() && error.find("needs one version of WWE 2K14 Add-On 2") != std::string::npos, "no Add-On 2: " + error);
  fs::create_directories(title / "045BEE31BC19100A3EE21606E089EC25F739099C54");
  Check(!run() && error.find("no record") != std::string::npos && error.find("045BEE31") != std::string::npos,
        "stray import folder named: " + error);
  fs::remove_all(title / "045BEE31BC19100A3EE21606E089EC25F739099C54");
  Touch(title / "MYADDON2RENAMEDXXXXXXXXXXXXXXXXXXXXXXXXXX" / "info" / "catalog.dlc", Catalog(2, 4));
  Check(!run() && error.find(".header") != std::string::npos && error.find("MYADDON2RENAMED") != std::string::npos,
        "Add-On 2 found by contents under another name; missing header named: " + error);
  Check(!fs::exists(root / "stage0") && !fs::exists(root / "stage2"), "a refused import leaves no staging folder");
}

// The whole-PC search behind the WWE 2K15 picker (owner 11-13: "same automatic search as the WWE '13 import").
void TestWholePCSearch() {
  const fs::path root = fs::temp_directory_path() /
      ("wwe15-search-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
  fs::create_directories(root);
#ifdef _WIN32
  _wputenv_s(L"WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", root.wstring().c_str());
#else
  setenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", root.string().c_str(), 1);
#endif
  Paths paths;
  paths.exe_dir = root / "launcher";
  auto make_copy = [](const fs::path& pac, bool is_2k15) {
    for (const char* f : {"ch/chEtc.pac", "m3.pac", "createmode.pac"}) Touch(pac / f);
    if (is_2k15)
      for (const char* f : {"ch/ch424.pac", "ch/ch398.pac", "menu/menuHD2.pac", "menu/menuSD2.pac"}) Touch(pac / f);
  };
  CancelFlag cancel{false};
  bool limit = true;
  uint64_t visited = 0;
  // 1. nothing to find: empty, the search ran to the end (no limit)
  make_copy(root / "Games" / "WWE 2K14" / "pac", false);  // WWE 2K14's own pac is not a 2K15 copy
  Check(SearchWWE15PacFolder(paths, {}, &cancel, &limit, &visited).empty() && !limit && visited > 0,
        "no WWE 2K15 copy on the PC: nothing found, no limit hit");
  // 2. a copy in a skipped system folder is not walked
  make_copy(root / "Windows" / "WWE2K15" / "pac", true);
  Check(SearchWWE15PacFolder(paths, {}, &cancel, &limit, &visited).empty(), "system folders are skipped");
  // 3. an extracted disc a few folders down is found, by contents (any folder name; <folder>/game/pac too)
  const fs::path disc = root / "Users" / "me" / "Downloads" / "my stuff" / "game" / "pac";
  make_copy(disc, true);
  Check(SearchWWE15PacFolder(paths, {}, &cancel, &limit, &visited) == disc, "extracted WWE 2K15 disc found on the PC");
  // 4. stop means stop
  cancel.store(true);
  Check(SearchWWE15PacFolder(paths, {}, &cancel, &limit, &visited).empty(), "a stopped search finds nothing");
#ifdef _WIN32
  _wputenv_s(L"WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", L"");
#else
  unsetenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS");
#endif
}

}  // namespace

int main() {
  try {
    Check(CountWWE15OfferRoom({32, 18}, 32) == 14, "existing two-catalog offer budget changed");
    Check(CountWWE15OfferRoom({32, 32, 18}, 32) == 14, "additional full catalog must contribute its own zero headroom");
    Check(CountWWE15OfferRoom({32, 32, 4}, 32) == 28, "catalog-only package headroom was not counted independently");
    Check(CountWWE15OfferRoom({32, 18}) == 78, "default per-pack cap is 64 (positions 32+ listed by the game hook)");
    Check(CountWWE15OfferRoom({35, 4}, 32) == 28, "over-cap catalog must not create negative headroom");
    Check(WWE15ExistingLookIds().size() == 47, "the optional existing-wrestler look list has 47 IDs");
    TestExistingLooks();
    TestModelSingle00();
    TestModelKeeps04AndErrors();
    TestEvp2();
    TestRows();
    TestWaze();
    TestMotc();
    TestPortrait();
    TestImportGuards();
    TestWholePCSearch();
  } catch (const std::exception& ex) {
    std::cerr << "FAIL: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "wwe15_convert_test passed\n";
  return 0;
}
