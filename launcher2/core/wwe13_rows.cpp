#include "wwe13_convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace wwe2k14::launcher {
namespace {

using Bytes = WWE13ConvertBytes;
constexpr size_t kPro13Size = 1180;
constexpr size_t kPro14Size = 1276;
constexpr uint16_t kPlayableSentinel = 255;
constexpr std::array<uint8_t, 6> kProPattern{0xB6, 0x4E, 0xAC, 0x26, 0x91, 0x1A};
constexpr std::array<size_t, 6> kSetFields{576, 578, 586, 592, 594, 602};

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

uint16_t ReadLe16(const Bytes& bytes, size_t at) {
  return static_cast<uint16_t>(uint16_t(bytes[at]) | (uint16_t(bytes[at + 1]) << 8));
}

uint16_t ReadBe16(const Bytes& bytes, size_t at) {
  return static_cast<uint16_t>((uint16_t(bytes[at]) << 8) | uint16_t(bytes[at + 1]));
}

void WriteLe16(Bytes* bytes, size_t at, uint16_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value);
  (*bytes)[at + 1] = static_cast<uint8_t>(value >> 8);
}

void WriteLe32(Bytes* bytes, size_t at, uint32_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value);
  (*bytes)[at + 1] = static_cast<uint8_t>(value >> 8);
  (*bytes)[at + 2] = static_cast<uint8_t>(value >> 16);
  (*bytes)[at + 3] = static_cast<uint8_t>(value >> 24);
}

bool ParseNumber(const std::string& text, uint32_t* value) {
  if (text.empty()) return false;
  uint64_t parsed = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    parsed = parsed * 10 + static_cast<uint64_t>(c - '0');
    if (parsed > std::numeric_limits<uint32_t>::max()) return false;
  }
  *value = static_cast<uint32_t>(parsed);
  return true;
}

std::vector<uint16_t> U16BigEndian(const Bytes& bytes) {
  std::vector<uint16_t> values;
  values.reserve(bytes.size() / 2);
  for (size_t at = 0; at + 1 < bytes.size(); at += 2) values.push_back(ReadBe16(bytes, at));
  return values;
}

struct ProMap {
  std::map<size_t, size_t> source_column;
  std::map<size_t, uint16_t> constant;
  std::array<uint16_t, 638> template_row{};
  std::array<std::set<uint16_t>, 638> allowed_values;
  std::set<uint16_t> playable_moves;
};

bool LearnProMap(const WWE13RowsInput& input, ProMap* result, std::string* error) {
  if (!result) return Fail(error, "The PRO conversion map output is missing.");
  std::vector<uint32_t> shared;
  std::map<uint32_t, std::vector<uint16_t>> p13;
  std::map<uint32_t, std::vector<uint16_t>> p14;
  for (const auto& [id, row] : input.training13_pro) {
    const auto game14 = input.game14_pro.find(id);
    if (id >= 400 || game14 == input.game14_pro.end()) continue;
    if ((row.size() & 1) || (game14->second.size() & 1))
      return Fail(error, "A shared PRO training row has an odd byte length.");
    std::vector<uint16_t> source = U16BigEndian(row);
    std::vector<uint16_t> target = U16BigEndian(game14->second);
    if (std::set<uint16_t>(source.begin(), source.end()).size() <= 20 ||
        std::set<uint16_t>(target.begin(), target.end()).size() <= 20) continue;
    if (row.size() != kPro13Size || game14->second.size() != kPro14Size)
      return Fail(error, "A qualifying shared PRO training row has an unexpected size.");
    shared.push_back(id);
    p13.emplace(id, std::move(source));
    p14.emplace(id, std::move(target));
  }
  if (shared.empty()) return Fail(error, "No shared character rows are available to learn the PRO conversion.");

  for (const auto& [id, row] : input.game14_pro) {
    (void)id;
    if (row.size() != kPro14Size) continue;
    for (size_t j = 0; j < 638; ++j) {
      const uint16_t value = ReadBe16(row, j * 2);
      result->allowed_values[j].insert(value);
    }
    if (ReadLe16(row, 576) == kPlayableSentinel) continue;
    for (size_t slot = 0; slot < 262; ++slot) {
      const uint16_t move = ReadLe16(row, 4 + slot * 2);
      if (move) result->playable_moves.insert(move);
    }
  }

  for (size_t j = 0; j < 638; ++j) {
    std::vector<uint16_t> column;
    column.reserve(shared.size());
    for (uint32_t id : shared) column.push_back(p14.at(id)[j]);
    std::vector<uint16_t> sorted = column;
    std::sort(sorted.begin(), sorted.end());
    const size_t mid = sorted.size() / 2;
    const uint32_t median = sorted.size() & 1
                                ? sorted[mid]
                                : (uint32_t(sorted[mid - 1]) + uint32_t(sorted[mid])) / 2;
    result->template_row[j] = static_cast<uint16_t>(median);
    if (std::all_of(column.begin(), column.end(), [&](uint16_t value) { return value == column.front(); })) {
      result->constant[j] = column.front();
      continue;
    }

    size_t best_column = 0;
    double best_score = -1.0;
    for (size_t i = 0; i < 590; ++i) {
      size_t denominator = 0;
      size_t numerator = 0;
      for (uint32_t id : shared) {
        const uint16_t source_value = p13.at(id)[i];
        const uint16_t target_value = p14.at(id)[j];
        if (source_value != 0 || target_value != 0) {
          ++denominator;
          if (source_value == target_value) ++numerator;
        }
      }
      const double score = denominator ? static_cast<double>(numerator) / denominator : 0.0;
      if (score > best_score) {
        best_score = score;
        best_column = i;
      }
    }
    if (best_score >= 0.6) result->source_column[j] = best_column;
  }
  return true;
}

bool ConvertPro(const Bytes& source, const Bytes& destination, const ProMap& mapping,
                Bytes* output, std::string* error) {
  if (source.size() != kPro13Size || destination.size() != kPro14Size)
    return Fail(error, "A PRO row has an unexpected size for conversion.");
  Bytes converted(kPro14Size, 0);
  for (size_t j = 0; j < 638; ++j) {
    uint16_t value = mapping.template_row[j];
    const auto constant = mapping.constant.find(j);
    if (constant != mapping.constant.end()) {
      value = constant->second;
    } else {
      const auto column = mapping.source_column.find(j);
      if (column != mapping.source_column.end()) {
        const uint16_t source_value = ReadBe16(source, column->second * 2);
        if (mapping.allowed_values[j].contains(source_value)) value = source_value;
        else value = ReadBe16(destination, j * 2);
      }
    }
    converted[j * 2] = static_cast<uint8_t>(value >> 8);
    converted[j * 2 + 1] = static_cast<uint8_t>(value);
  }
  std::copy(destination.begin() + 576, destination.begin() + 604, converted.begin() + 576);

  for (size_t target_slot = 5; target_slot < 264; ++target_slot) {
    const auto column = mapping.source_column.find(target_slot);
    if (column == mapping.source_column.end()) continue;
    const uint16_t move = ReadLe16(source, column->second * 2);
    if (move && mapping.playable_moves.contains(move))
      WriteLe16(&converted, target_slot * 2, move);
  }
  *output = std::move(converted);
  return true;
}

std::optional<uint16_t> OwnSizeSet(uint32_t source_id, const WWE13RowsInput& input,
                                  const WWE13RawRowMap& playable_values) {
  const auto own14 = input.game14_pro.find(source_id);
  if (own14 != input.game14_pro.end() && own14->second.size() == kPro14Size &&
      ReadLe16(own14->second, 576) != kPlayableSentinel)
    return ReadLe16(own14->second, 584);

  const auto own13 = input.source13_pro.find(source_id);
  if (own13 == input.source13_pro.end() || own13->second.size() < 586) return std::nullopt;
  const uint16_t value = ReadLe16(own13->second, 584);
  for (const auto& [id, row] : playable_values) {
    (void)id;
    if (row.size() == kPro14Size && ReadLe16(row, 576) != kPlayableSentinel &&
        ReadLe16(row, 584) == value) return value;
  }
  return std::nullopt;
}

}  // namespace

bool ConvertWWE13Rows(const std::vector<WWE13RowsPort>& ports, const WWE13RowsInput& input,
                      const WWE13RowsOptions& options,
                      std::vector<WWE13ConvertedRows>* output, std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The converted chEtc rows output is missing.");
  if (ports.empty()) return Fail(error, "No wrestlers were selected for chEtc conversion.");

  ProMap mapping;
  if (!LearnProMap(input, &mapping, error)) return false;
  std::vector<WWE13RowsPort> ordered = ports;
  std::sort(ordered.begin(), ordered.end(), [](const WWE13RowsPort& left, const WWE13RowsPort& right) {
    return left.target_id < right.target_id;
  });

  std::map<uint32_t, uint16_t> fixed;
  for (const WWE13RowsPort& port : ordered) {
    uint32_t value = 0;
    if (ParseNumber(port.index, &value)) {
      if (value > std::numeric_limits<uint16_t>::max())
        return Fail(error, "A fixed DAT index is out of range.");
      fixed[port.target_id] = static_cast<uint16_t>(value);
    } else if (port.index.rfind("donor:", 0) == 0) {
      uint32_t donor = 0;
      if (!ParseNumber(port.index.substr(6), &donor))
        return Fail(error, "A donor DAT index selector is invalid.");
      const auto row = input.game14_dat.find(donor);
      if (row == input.game14_dat.end() || row->second.size() < 8)
        return Fail(error, "The selected DAT index donor row is missing.");
      fixed[port.target_id] = ReadLe16(row->second, 6);
    } else if (port.index != "free" && !port.index.empty()) {
      return Fail(error, "A DAT index selector is not recognized.");
    }
  }

  std::vector<uint16_t> free_indices;
  // Owner 2026-11-14 (#141): the pool was 1..299 (inherited from the port tool, not a game limit). Stock 2K14 uses 1..121
  // for wrestlers and 1000+row for the Create-a-Wrestler rows; the game treats 1000..1049 specially (sub_82A25C90
  // compares DAT+6 with 1000/1050), so imports may use 1..999. Values below 300 are handed out first, as before.
  for (uint16_t index = 1; index < 1000; ++index) {
    const bool fixed_value = std::any_of(fixed.begin(), fixed.end(), [index](const auto& item) {
      return item.second == index;
    });
    if (!input.occupied_dat_indices.contains(index) && !fixed_value) free_indices.push_back(index);
  }

  std::vector<WWE13ConvertedRows> converted_rows;
  converted_rows.reserve(ordered.size());
  for (const WWE13RowsPort& port : ordered) {
    const auto source_dat = input.source13_dat.find(port.source_id);
    const auto source_pro = input.source13_pro.find(port.source_id);
    const auto donor_dat = input.game14_dat.find(port.donor_id);
    const auto donor_dsbd = input.game14_dsbd.find(port.donor_id);
    const auto donor_pro = input.game14_pro.find(port.donor_id);
    const auto own_dat = input.game14_dat.find(port.target_id);
    const auto own_dsbd = input.game14_dsbd.find(port.target_id);
    const auto own_pro = input.game14_pro.find(port.target_id);
    if (source_dat == input.source13_dat.end() || source_pro == input.source13_pro.end() ||
        donor_dat == input.game14_dat.end() || donor_dsbd == input.game14_dsbd.end() ||
        donor_pro == input.game14_pro.end() || own_dat == input.game14_dat.end() ||
        own_dsbd == input.game14_dsbd.end() || own_pro == input.game14_pro.end())
      return Fail(error, "A required WWE '13, donor, or target chEtc row is missing.");
    if (donor_dat->second.size() < 128 || donor_dsbd->second.size() < 8 ||
        donor_pro->second.size() != kPro14Size || own_pro->second.size() != kPro14Size ||
        source_dat->second.size() <= 257 || source_pro->second.size() != kPro13Size)
      return Fail(error, "A required chEtc row has an unexpected size.");

    Bytes dat = donor_dat->second;
    const uint32_t id = port.target_id;
    WriteLe16(&dat, 4, static_cast<uint16_t>(id));
    WriteLe16(&dat, 8, static_cast<uint16_t>(id));
    WriteLe16(&dat, 10, static_cast<uint16_t>(id + 0xEA60));
    WriteLe16(&dat, 12, static_cast<uint16_t>(id + 0xF230));
    WriteLe16(&dat, 14, static_cast<uint16_t>(id + 0xFA00));
    WriteLe32(&dat, 16, 79000 + id);
    dat[20] = 1;
    dat[21] = 2;
    if (port.attrs == "13") {
      dat[22] = source_dat->second[52];
      dat[23] = source_dat->second[53];
      std::copy(source_dat->second.begin() + 4, source_dat->second.begin() + 18, dat.begin() + 36);
    } else {
      uint32_t attrs_id = 0;
      if (!ParseNumber(port.attrs, &attrs_id)) return Fail(error, "A DAT attributes source is invalid.");
      const auto attrs = input.game14_dat.find(attrs_id);
      if (attrs == input.game14_dat.end() || attrs->second.size() < 50)
        return Fail(error, "The selected DAT attributes source row is missing.");
      dat[22] = attrs->second[22];
      dat[23] = attrs->second[23];
      std::copy(attrs->second.begin() + 36, attrs->second.begin() + 50, dat.begin() + 36);
    }
    WriteLe16(&dat, dat.size() - 4, static_cast<uint16_t>(id));

    const auto fixed_index = fixed.find(id);
    uint16_t dat_index = 0;
    if (fixed_index != fixed.end()) {
      dat_index = fixed_index->second;
    } else {
      if (free_indices.empty()) return Fail(error, "No unused DAT row index (1-999) is available.");
      dat_index = free_indices.front();
      free_indices.erase(free_indices.begin());
    }
    WriteLe16(&dat, 6, dat_index);
    dat[26] = source_dat->second[257];
    Bytes dsbd;
    if (options.own_attire_codes) {
      const uint32_t attire_base = id * 100;
      if (attire_base + 4 > std::numeric_limits<uint16_t>::max())
        return Fail(error, "The target ID cannot be represented by its attire codes.");
      for (size_t i = 0; i < 4; ++i)
        WriteLe16(&dat, 120 + i * 2, static_cast<uint16_t>(attire_base + i + 1));
      dsbd.assign(80, 0);
      for (size_t i = 0; i < 4; ++i)
        WriteLe16(&dsbd, i * 2, static_cast<uint16_t>(attire_base + i + 1));
    } else {
      const auto donor_dsbd = input.game14_dsbd.find(port.donor_id);
      if (donor_dsbd == input.game14_dsbd.end())
        return Fail(error, "The selected donor's DSBD row is missing.");
      dsbd = donor_dsbd->second;
    }

    Bytes pro = own_pro->second;
    if (port.source_id != port.target_id) {
      if (!ConvertPro(source_pro->second, own_pro->second, mapping, &pro, error)) return false;
    }
    if (port.own_pro) {
      if (std::equal(kProPattern.begin(), kProPattern.end(), pro.begin() + 4))
        std::fill(pro.begin() + 4, pro.begin() + 10, 0);
      for (const size_t offset : kSetFields) {
        if (ReadLe16(pro, offset) == kPlayableSentinel) WriteLe16(&pro, offset, static_cast<uint16_t>(id));
      }
      const uint16_t donor_size = ReadLe16(donor_pro->second, 584);
      const std::optional<uint16_t> own_size = OwnSizeSet(port.source_id, input, input.game14_pro);
      const uint16_t size_set = own_size && *own_size == 613 ? *own_size : donor_size;
      WriteLe16(&pro, 584, size_set);
      WriteLe16(&pro, 600, size_set);
    } else {
      pro = donor_pro->second;
    }

    converted_rows.push_back({id, dat_index, std::move(dat), std::move(dsbd), std::move(pro)});
  }
  *output = std::move(converted_rows);
  return true;
}

}  // namespace wwe2k14::launcher
