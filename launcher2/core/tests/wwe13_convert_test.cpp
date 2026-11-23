#include "../launcher_core.h"
#include "../wwe13_convert.h"
#include "../wwe13_pipeline.h"
#include "../internal/miniz_api.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace wwe2k14::launcher;
using Bytes = WWE13ConvertBytes;
namespace fs = std::filesystem;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void PutLe32(Bytes* bytes, size_t at, uint32_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value);
  (*bytes)[at + 1] = static_cast<uint8_t>(value >> 8);
  (*bytes)[at + 2] = static_cast<uint8_t>(value >> 16);
  (*bytes)[at + 3] = static_cast<uint8_t>(value >> 24);
}

void PutLe16(Bytes* bytes, size_t at, uint16_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value);
  (*bytes)[at + 1] = static_cast<uint8_t>(value >> 8);
}

uint16_t ReadLe16(const Bytes& bytes, size_t at) {
  return static_cast<uint16_t>(uint16_t(bytes[at]) | (uint16_t(bytes[at + 1]) << 8));
}

uint32_t Le32(const Bytes& bytes, size_t at) {
  return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) |
         (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
}

Bytes Pach(const std::vector<std::pair<uint32_t, Bytes>>& leaves) {
  Bytes output(8 + leaves.size() * 12, 0);
  std::memcpy(output.data(), "PACH", 4);
  PutLe32(&output, 4, static_cast<uint32_t>(leaves.size()));
  Bytes body;
  for (size_t i = 0; i < leaves.size(); ++i) {
    while (body.size() % 16) body.push_back(0);
    const size_t record = 8 + i * 12;
    PutLe32(&output, record, leaves[i].first);
    PutLe32(&output, record + 4, static_cast<uint32_t>(body.size()));
    PutLe32(&output, record + 8, static_cast<uint32_t>(leaves[i].second.size()));
    body.insert(body.end(), leaves[i].second.begin(), leaves[i].second.end());
  }
  output.insert(output.end(), body.begin(), body.end());
  return output;
}

Bytes Epk8(const std::vector<std::pair<std::string, Bytes>>& entries) {
  Bytes output(0x4000, 0);
  std::memcpy(output.data(), "EPK8", 4);
  PutLe32(&output, 4, static_cast<uint32_t>(12 + entries.size() * 16));
  PutLe32(&output, 12, 7);
  size_t directory = 0x800;
  std::memcpy(output.data() + directory, "EMD ", 4);
  PutLe32(&output, directory + 4, static_cast<uint32_t>(entries.size() * 4) | 0x10000);
  directory += 12;
  Bytes body;
  for (const auto& [name, data] : entries) {
    Check(name.size() == 8, "synthetic EMD name must be eight bytes");
    std::copy(name.begin(), name.end(), output.begin() + static_cast<std::ptrdiff_t>(directory));
    PutLe32(&output, directory + 8, static_cast<uint32_t>(body.size() / 0x800));
    PutLe32(&output, directory + 12, static_cast<uint32_t>((data.size() + 0xff) / 0x100));
    body.insert(body.end(), data.begin(), data.end());
    body.resize((body.size() + 0x7ff) & ~size_t(0x7ff), 0);
    directory += 16;
  }
  PutLe32(&output, 8, static_cast<uint32_t>(body.size()));
  output.insert(output.end(), body.begin(), body.end());
  output.resize(output.size() + 0x800, 0);
  return output;
}

Bytes LeafBytes(uint8_t value, size_t size = 1) { return Bytes(size, value); }

Bytes AttireColorLeaf() {
  Bytes data(2 + 2 * 664, 0);
  PutLe16(&data, 0, 2);
  for (size_t row = 0; row < 2; ++row) {
    const size_t begin = 2 + row * 664;
    PutLe32(&data, begin, static_cast<uint32_t>(219 + row));
    for (size_t i = 4; i < 664; ++i) data[begin + i] = static_cast<uint8_t>((row * 17 + i) & 0xff);
  }
  return data;
}

Bytes ZlibLeaf(const Bytes& plain) {
  const wwe_mz_ulong bound = mz_compressBound(static_cast<wwe_mz_ulong>(plain.size()));
  Bytes compressed(static_cast<size_t>(bound));
  wwe_mz_ulong compressed_size = bound;
  Check(mz_compress2(compressed.data(), &compressed_size, plain.data(),
                     static_cast<wwe_mz_ulong>(plain.size()), 9) == kWweMzOk,
        "synthetic SDB compression failed");
  Bytes leaf(16, 0);
  std::memcpy(leaf.data(), "ZLIB", 4);
  PutLe32(&leaf, 8, static_cast<uint32_t>(compressed_size));
  PutLe32(&leaf, 12, static_cast<uint32_t>(plain.size()));
  leaf.insert(leaf.end(), compressed.begin(), compressed.begin() + static_cast<std::ptrdiff_t>(compressed_size));
  return leaf;
}

Bytes SdbTable() {
  const std::string old_name("old\0", 4);
  const std::string sentinel("base\0", 5);
  Bytes plain(8 + 2 * 12, 0);
  PutLe32(&plain, 4, 2);
  PutLe32(&plain, 8, 32);
  PutLe32(&plain, 12, static_cast<uint32_t>(old_name.size()));
  PutLe32(&plain, 16, 60050);
  PutLe32(&plain, 20, 36);
  PutLe32(&plain, 24, static_cast<uint32_t>(sentinel.size()));
  PutLe32(&plain, 28, 60139);
  plain.insert(plain.end(), old_name.begin(), old_name.end());
  plain.insert(plain.end(), sentinel.begin(), sentinel.end());
  return plain;
}

Bytes ProRow(uint32_t id, size_t size) {
  Bytes row(size, 0);
  for (size_t i = 0; i < size / 2; ++i) {
    const uint16_t value = static_cast<uint16_t>((i * 13 + id * 31) % 60000 + 1);
    row[i * 2] = static_cast<uint8_t>(value >> 8);
    row[i * 2 + 1] = static_cast<uint8_t>(value);
  }
  if (size == 1276) PutLe16(&row, 576, 100);
  return row;
}

Bytes DatRow(size_t size, uint8_t value) {
  Bytes row(size, value);
  PutLe16(&row, 6, 10);
  return row;
}

Bytes SyntheticLsd() {
  Bytes table(10, 0);
  PutLe16(&table, 0, 1);
  PutLe16(&table, 2, 1); PutLe16(&table, 4, 1101);
  PutLe16(&table, 6, 1); PutLe16(&table, 8, 0);
  return table;
}

Bytes SyntheticBin() {
  Bytes table(36, 0);
  PutLe16(&table, 0, 1);
  for (size_t i = 2; i < 16; ++i) table[i] = static_cast<uint8_t>(0x30 + i);
  PutLe32(&table, 16, 0);
  PutLe32(&table, 20, 60001);
  PutLe16(&table, 24, 10001);
  PutLe16(&table, 26, 5000);
  PutLe16(&table, 30, 100);
  PutLe16(&table, 32, 1101);
  return table;
}

Bytes RegistryFixture(const Bytes& lsd_table, const Bytes& bin_table) {
  std::vector<WWE13PachLeafData> lsd, bin;
  for (uint32_t id : {0u, 2u, 3u, 8u, 9u, 10u, 11u, 16u, 17u})
    lsd.push_back({id, StoreWWE13BpeLiteral(lsd_table)});
  for (uint32_t id = 0; id <= 4; ++id)
    bin.push_back({id, StoreWWE13BpeLiteral(bin_table)});
  Bytes lsd_pach, bin_pach;
  std::string error;
  Check(BuildWWE13PachLeaves(lsd, &lsd_pach, true, &error), "synthetic LSD PACH build failed");
  Check(BuildWWE13PachLeaves(bin, &bin_pach, true, &error), "synthetic BIN PACH build failed");
  WWE13PacGroupData group;
  group.name = "ENEX";
  group.entries = {{"LSD ", lsd_pach}, {"BIN ", bin_pach}};
  Bytes result;
  Check(BuildWWE13PacGroups({group}, &result, &error), "synthetic DLC PAC build failed");
  return result;
}

const WWE13PacEntryData* Entry(const std::vector<WWE13PacGroupData>& groups,
                               const char* group, const char* name) {
  for (const auto& g : groups) if (g.name.substr(0, 4) == group)
    for (const auto& e : g.entries) if (e.name.substr(0, std::strlen(name)) == name) return &e;
  return nullptr;
}

Bytes RegistryLeaf(const WWE13PacEntryData* entry, uint32_t id) {
  if (!entry) throw std::runtime_error("synthetic registry PAC entry is missing");
  Bytes plain;
  std::string error;
  Check(DecodeWWE13Bpe(entry->data, &plain, &error), "synthetic registry BPE decode failed");
  std::vector<WWE13PachLeafData> leaves;
  Check(ReadWWE13PachLeaves(plain, &leaves, &error), "synthetic registry PACH parse failed");
  const auto found = std::find_if(leaves.begin(), leaves.end(), [id](const auto& leaf) { return leaf.id == id; });
  if (found == leaves.end()) throw std::runtime_error("synthetic registry PACH leaf is missing");
  Bytes table;
  Check(DecodeWWE13Bpe(found->data, &table, &error), "synthetic registry leaf BPE decode failed");
  return table;
}

std::vector<std::pair<uint32_t, Bytes>> PrimaryLeaves(uint8_t leaf10000) {
  return {{0, LeafBytes(0x10)}, {10, LeafBytes(0x11)}, {12, LeafBytes(0x12)},
          {13, LeafBytes(0x13)}, {40, LeafBytes(0x14)}, {80, LeafBytes(0x15)},
          {100, LeafBytes(0x16)}, {110, LeafBytes(0x17)}, {10000, LeafBytes(leaf10000)},
          {20002, LeafBytes(0x18)}};
}

Bytes Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("synthetic PAC could not be read");
  return Bytes(std::istreambuf_iterator<char>(input), {});
}

}  // namespace

int main() {
  try {
    const Bytes source = Epk8({
        {"00005002", Pach(PrimaryLeaves(0x21))},
        {"00005012", Pach({{0, LeafBytes(0x31)}, {10, LeafBytes(0x32)}, {12, LeafBytes(0x33)},
                             {13, LeafBytes(0x34)}, {40, LeafBytes(0x35)}, {50, LeafBytes(0x36)},
                             {80, LeafBytes(0x37)}, {100, LeafBytes(0x38)}, {110, LeafBytes(0x39)},
                             {10000, LeafBytes(0x3a)}})}});
    const Bytes replacement = Epk8({{"00005102", Pach({{0, LeafBytes(0x40)}, {10000, LeafBytes(0x4a)}})}});
    const Bytes leaf50 = LeafBytes(0x50, 303);
    WWE13CharacterPacRecipe recipe;
    recipe.source_id = 50;
    recipe.target_id = 266;
    recipe.leaf_10000_source_id = 51;
    recipe.add_shared_leaf50 = true;
    recipe.waive_missing_20002 = true;
    Bytes converted;
    std::string error;
    Check(ConvertWWE13CharacterPac(recipe, source, &replacement, nullptr, nullptr,
                                   &leaf50, &converted, &error), "stage 2 conversion failed");
    Check(error.empty(), "successful conversion left an error");

    Bytes leaf;
    Check(ReadWWE13CharacterLeaf(converted, "00026602", 50, &leaf, &error), "leaf 50 was not added");
    Check(leaf == leaf50, "leaf 50 bytes changed");
    Check(ReadWWE13CharacterLeaf(converted, "00026602", 10000, &leaf, &error), "leaf 10000 is missing");
    Check(leaf == LeafBytes(0x4a), "leaf 10000 replacement did not come from the selected source");
    Check(!ReadWWE13CharacterLeaf(converted, "00026602", 100, &leaf, &error), "leaf 100 was retained");
    Check(ReadWWE13CharacterLeaf(converted, "00026604", 50, &leaf, &error), "the missing 04 entry was not added");
    Check(leaf == leaf50, "the generated 04 entry is not a byte copy of 02");
    Check(ReadWWE13CharacterLeaf(converted, "00026612", 0, &leaf, &error), "the explicit 20002 waiver was not honored");

    const Bytes attire_source = Epk8({{"MISC0001", Pach({{600, AttireColorLeaf()}, {700, LeafBytes(0x71)}})}});
    const Bytes source_attire_leaf = AttireColorLeaf();
    std::vector<WWE13AttireColorPort> attire_ports{{266, 219}};
    Bytes converted_attire;
    Check(ConvertWWE13AttireColors(attire_source, attire_ports, &converted_attire, &error),
          "stage 4 attire-color conversion failed");
    Check(ReadWWE13PacLeaf(converted_attire, "EMD", "MISC0001", 600, &leaf, &error),
          "stage 4 converted leaf 600 could not be read");
    Check(leaf.size() == 2 + 3 * 664 && ReadLe16(leaf, 0) == 3,
          "stage 4 attire-color record count is wrong");
    const size_t donor_record = 2;
    const size_t retained_record = 2 + 664;
    const size_t added_record = 2 + 2 * 664;
    Check(Le32(leaf, donor_record) == 219 && Le32(leaf, retained_record) == 220 &&
              Le32(leaf, added_record) == 266,
          "stage 4 attire-color records are not sorted or have incorrect IDs");
    Check(std::equal(leaf.begin() + static_cast<std::ptrdiff_t>(donor_record + 4),
                     leaf.begin() + static_cast<std::ptrdiff_t>(donor_record + 664),
                     leaf.begin() + static_cast<std::ptrdiff_t>(added_record + 4)),
          "stage 4 donor attire-color payload was not cloned");
    Check(std::equal(source_attire_leaf.begin() + 6, source_attire_leaf.begin() + 666,
                     leaf.begin() + static_cast<std::ptrdiff_t>(added_record + 4)),
          "stage 4 cloned payload does not match the source donor record");
    std::map<uint32_t, Bytes> attire_records;
    Check(ReadWWE13AttireColorRecords(converted_attire, &attire_records, &error) &&
              attire_records.size() == 3 && attire_records.count(266) == 1,
          "stage 4 attire-color record reader returned the wrong records");

    const Bytes string_source = Epk8({{"STRING01", Pach({{1, ZlibLeaf(SdbTable())}})}});
    WWE13StringKeyMap keyed_strings{{60050, Bytes{'N', 'e', 'w'}},
                                    {62050, Bytes{'D', 'i', 's', 'p'}},
                                    {64050, Bytes{'S', 'h', 'o', 'r', 't'}},
                                    {79050, Bytes{'@', 'h', 'a', 'n', 'd', 'l', 'e'}}};
    Bytes converted_strings;
    size_t inserted_keys = 0;
    Check(ConvertWWE13StringKeys(string_source, keyed_strings, &converted_strings,
                                 &inserted_keys, &error),
          "stage 5 SDB key insertion failed");
    Check(inserted_keys == keyed_strings.size(), "stage 5 did not count all inserted/replaced keys");
    WWE13StringKeyMap actual_strings;
    Check(ReadWWE13StringKeyValues(converted_strings, &actual_strings, &error),
          "stage 5 SDB key reader failed");
    for (const auto& [key, value] : keyed_strings) {
      const auto found = actual_strings.find(key);
      Check(found != actual_strings.end() && found->second.size() == value.size() + 1 &&
                std::equal(value.begin(), value.end(), found->second.begin()) && found->second.back() == 0,
            "stage 5 inserted string value differs from the requested value");
    }

    recipe.waive_missing_20002 = false;
    Check(ConvertWWE13CharacterPac(recipe, source, &replacement, nullptr, nullptr,
                                   &leaf50, &converted, &error), "stage 2 conversion without waiver failed");
    Check(!ReadWWE13CharacterLeaf(converted, "00026612", 0, &leaf, &error),
          "an incomplete remapped secondary entry was not omitted");
    Check(ReadWWE13CharacterLeaf(converted, "00026604", 0, &leaf, &error),
          "the generated 04 entry disappeared after filtering");

    WWE13RowsInput row_input;
    for (uint32_t id = 100; id < 124; ++id) {
      row_input.training13_pro[id] = ProRow(id, 1180);
      row_input.game14_pro[id] = ProRow(id, 1276);
    }
    for (const uint32_t id : {50u, 267u}) {
      row_input.source13_dat[id] = DatRow(324, static_cast<uint8_t>(id));
      row_input.source13_dat[id][257] = 3;
      row_input.source13_dat[id][52] = 0xA4;
      row_input.source13_dat[id][53] = 0x0F;
      for (size_t i = 4; i < 18; ++i) row_input.source13_dat[id][i] = static_cast<uint8_t>(i + id);
      row_input.source13_pro[id] = ProRow(id, 1180);
      PutLe16(&row_input.source13_pro[id], 584, 613);
    }
    for (const uint32_t id : {50u, 219u, 266u, 267u}) {
      row_input.game14_dat[id] = DatRow(208, static_cast<uint8_t>(id));
      row_input.game14_dsbd[id] = Bytes(80, static_cast<uint8_t>(id));
      row_input.game14_pro[id] = ProRow(id, 1276);
    }
    const std::array<uint8_t, 6> expected_pattern{0xB6, 0x4E, 0xAC, 0x26, 0x91, 0x1A};
    std::copy(expected_pattern.begin(), expected_pattern.end(), row_input.game14_pro[267].begin() + 4);
    for (size_t at : {576u, 578u, 586u, 592u, 594u, 602u}) PutLe16(&row_input.game14_pro[267], at, 255);
    PutLe16(&row_input.game14_pro[50], 584, 613);
    PutLe16(&row_input.game14_pro[50], 576, 100);
    PutLe16(&row_input.game14_pro[267], 584, 613);
    PutLe16(&row_input.game14_pro[219], 584, 600);
    row_input.occupied_dat_indices = {1, 3};

    std::vector<WWE13RowsPort> row_ports{{50, 266, 219, "13", "free", true},
                                         {267, 267, 219, "13", "free", true}};
    WWE13RowsOptions row_options;
    std::vector<WWE13ConvertedRows> rows;
    Check(ConvertWWE13Rows(row_ports, row_input, row_options, &rows, &error),
          "stage 3 synthetic chEtc conversion failed");
    Check(rows.size() == 2 && rows[0].target_id == 266 && rows[0].dat_index == 2 &&
              rows[1].target_id == 267 && rows[1].dat_index == 4,
          "free DAT indices were not assigned in ascending target order");
    Check(ReadLe16(rows[0].dat, 4) == 266 && ReadLe16(rows[0].dat, 6) == 2 &&
              rows[0].dat[22] == 0xA4 && rows[0].dat[23] == 0x0F && rows[0].dat[26] == 3,
          "stage 3 DAT identifiers, index, height, or body class are wrong");
    Check(ReadLe16(rows[0].dat, 120) == 26601 && ReadLe16(rows[0].dsbd, 0) == 26601,
          "stage 3 own attire codes were not assigned");
    Check(rows[0].pro.size() == 1276 && rows[1].pro.size() == 1276,
          "stage 3 PRO rows do not have the 2K14 row size");
    Check(std::all_of(rows[1].pro.begin() + 4, rows[1].pro.begin() + 10,
                      [](uint8_t value) { return value == 0; }),
          "stage 3 expected PRO byte pattern was not cleared");
    for (size_t at : {576u, 578u, 586u, 592u, 594u, 602u})
      Check(ReadLe16(rows[1].pro, at) == 267, "stage 3 match-setup field was not assigned");
    Check(ReadLe16(rows[1].pro, 584) == 613 && ReadLe16(rows[1].pro, 600) == 613,
          "stage 3 size set did not retain the matching playable source value");

    {  // #141 (owner 2026-11-14): the free pool continues past 299 up to 999, and fails cleanly when 1..999 are all used.
      WWE13RowsInput full = row_input;
      for (uint16_t index = 1; index < 300; ++index) full.occupied_dat_indices.insert(index);
      std::vector<WWE13ConvertedRows> high;
      Check(ConvertWWE13Rows(row_ports, full, row_options, &high, &error),
            "stage 3 conversion with 1..299 occupied failed");
      Check(high.size() == 2 && high[0].dat_index == 300 && high[1].dat_index == 301 &&
                ReadLe16(high[0].dat, 6) == 300,
            "free DAT indices did not continue at 300");
      for (uint16_t index = 300; index < 1000; ++index) full.occupied_dat_indices.insert(index);
      std::vector<WWE13ConvertedRows> none;
      Check(!ConvertWWE13Rows(row_ports, full, row_options, &none, &error) &&
                error.find("1-999") != std::string::npos,
            "a full 1..999 pool did not fail with the plain reason");
      error.clear();
    }

    row_options.own_attire_codes = false;
    std::vector<WWE13ConvertedRows> donor_code_rows;
    Check(ConvertWWE13Rows(row_ports, row_input, row_options, &donor_code_rows, &error),
          "stage 3 donor-code mode failed");
    Check(std::equal(donor_code_rows[0].dat.begin() + 120, donor_code_rows[0].dat.begin() + 128,
                     row_input.game14_dat[219].begin() + 120) &&
              donor_code_rows[0].dsbd == row_input.game14_dsbd[219],
          "stage 3 donor-code mode did not preserve DAT/DSBD attire codes");

    const Bytes registry_source = RegistryFixture(SyntheticLsd(), SyntheticBin());
    Bytes registry_output;
    const std::vector<WWE13RegistryChar> registry_chars{{266, 5, 6}, {267, 7, 8}};
    Check(ConvertWWE13DlcRegistry(registry_source, registry_chars, "DLC_dlc01.pac",
                                  &registry_output, &error),
          "stage 6 DLC registry conversion failed");
    std::vector<WWE13PacGroupData> registry_groups;
    Check(ReadWWE13PacGroups(registry_output, &registry_groups, &error),
          "stage 6 registry output PAC could not be parsed");
    const auto* lsd_entry = Entry(registry_groups, "ENEX", "LSD");
    const auto* bin_entry = Entry(registry_groups, "ENEX", "BIN");
    Check(lsd_entry && bin_entry, "stage 6 registry output lost LSD or BIN");
    for (uint32_t id : {0u, 2u, 3u, 8u, 9u, 10u, 11u, 16u, 17u}) {
      const Bytes table = RegistryLeaf(lsd_entry, id);
      Check(ReadLe16(table, 0) == 3, "stage 6 LSD row count is wrong");
      const size_t row = 2 + 1 * 8;
      const uint16_t expected_id = static_cast<uint16_t>(266 +
          (id == 8 ? 11000 : id == 9 ? 12000 : id == 10 ? 13000 : id == 11 ? 14000 : id == 16 ? 10000 : 0));
      Check(ReadLe16(table, row) == expected_id && ReadLe16(table, row + 2) == 1366,
            "stage 6 LSD target or package ID is wrong");
      Check(ReadLe16(table, row + 4) == (id == 0 ? 5 : id == 2 ? 6 : 60266),
            "stage 6 LSD rank or display-name key is wrong");
    }
    for (uint32_t id = 0; id <= 4; ++id) {
      const Bytes table = RegistryLeaf(bin_entry, id);
      Check(ReadLe16(table, 0) == 3, "stage 6 BIN row count is wrong");
      const size_t row = 16 + 1 * 20;
      const uint32_t name = Le32(table, row + 4);
      const uint16_t movie = ReadLe16(table, row + 8);
      const uint16_t pack = ReadLe16(table, row + 16);
      Check(Le32(table, row) == 1 && name == 60266 &&
                movie == static_cast<uint16_t>(10000 + id * 1000 + 266) &&
                ReadLe16(table, row + 10) == 5000 && ReadLe16(table, row + 14) == 100 &&
                pack == 1366,
            "stage 6 BIN generated row fields are wrong");
    }
    Bytes duplicate_registry;
    Check(!ConvertWWE13DlcRegistry(registry_output, registry_chars, "DLC_dlc01.pac",
                                   &duplicate_registry, &error),
          "stage 6 accepted duplicate registry character IDs");

    Bytes padded_lsd = SyntheticLsd();
    padded_lsd.insert(padded_lsd.end(), 3, 0);
    Bytes padded_registry;
    Check(ConvertWWE13DlcRegistry(RegistryFixture(padded_lsd, SyntheticBin()), registry_chars,
                                  "DLC_dlc01.pac", &padded_registry, &error),
          "stage 6 rejected zero-padded LSD registry tables");
    std::vector<WWE13PacGroupData> padded_groups;
    Check(ReadWWE13PacGroups(padded_registry, &padded_groups, &error),
          "stage 6 zero-padded registry output PAC could not be parsed");
    const auto* padded_lsd_entry = Entry(padded_groups, "ENEX", "LSD");
    for (uint32_t id : {0u, 2u, 3u, 8u, 9u, 10u, 11u, 16u, 17u}) {
      const Bytes table = RegistryLeaf(padded_lsd_entry, id);
      Check(table.size() == 2 + 3 * 8 && ReadLe16(table, 0) == 3,
            "stage 6 did not rewrite padded LSD data to the exact row-table size");
    }

    Bytes short_lsd = SyntheticLsd();
    short_lsd.pop_back();
    Bytes short_registry;
    Check(!ConvertWWE13DlcRegistry(RegistryFixture(short_lsd, SyntheticBin()), registry_chars,
                                   "DLC_dlc01.pac", &short_registry, &error),
          "stage 6 accepted a short LSD registry table");
    Check(error.find("LSD registry table (leaf 0)") != std::string::npos &&
              error.find("DLC_dlc01.pac") != std::string::npos &&
              error.find("10 bytes") != std::string::npos &&
              error.find("9 bytes") != std::string::npos,
          "stage 6 short LSD error did not include file name and required/actual byte counts");

    Bytes nonzero_padded_lsd = SyntheticLsd();
    nonzero_padded_lsd.push_back(0x7f);
    Bytes nonzero_registry;
    Check(!ConvertWWE13DlcRegistry(RegistryFixture(nonzero_padded_lsd, SyntheticBin()), registry_chars,
                                   "DLC_dlc01.pac", &nonzero_registry, &error),
          "stage 6 accepted non-zero trailing LSD data");
    Check(error.find("LSD registry table (leaf 0)") != std::string::npos &&
              error.find("DLC_dlc01.pac") != std::string::npos &&
              error.find("10 bytes") != std::string::npos &&
              error.find("11 bytes") != std::string::npos,
          "stage 6 non-zero LSD padding error did not include file name and required/actual byte counts");

    Bytes nonzero_padded_bin = SyntheticBin();
    nonzero_padded_bin.push_back(0);
    Bytes invalid_bin_registry;
    Check(!ConvertWWE13DlcRegistry(RegistryFixture(SyntheticLsd(), nonzero_padded_bin),
                                   registry_chars, "DLC_dlc01.pac",
                                   &invalid_bin_registry, &error),
          "stage 6 accepted a BIN table with an invalid size");
    Check(error.find("BIN registry table (leaf 0)") != std::string::npos &&
              error.find("DLC_dlc01.pac") != std::string::npos &&
              error.find("36 bytes") != std::string::npos &&
              error.find("37 bytes") != std::string::npos,
          "stage 6 BIN error did not include file name and required/actual byte counts");

    const fs::path root = fs::temp_directory_path() / "wwe13-convert-synthetic";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    std::ofstream(root / "synthetic-source.pac", std::ios::binary).write(
        reinterpret_cast<const char*>(source.data()), static_cast<std::streamsize>(source.size()));
    Check(Read(root / "synthetic-source.pac") == source, "synthetic data write/read roundtrip failed");
    fs::remove_all(root, ignored);
    std::cout << "wwe13_convert_test: PASS (stages 2-5 synthetic PAC, row, attire-color, and SDB conversion)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "wwe13_convert_test: FAIL: " << error.what() << '\n';
    return 1;
  }
}
