#include "wwe13_arena.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

#include "wwe13_pipeline.h"

namespace wwe2k14::launcher {
namespace {

using Bytes = WWE13ConvertBytes;
constexpr uint32_t kArenaNameSentinel = 40002;   // key of arena 0's name: present in the 9 arena-name tables
constexpr uint32_t kArenaNameBase = 40002;

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}
uint32_t Le32(const Bytes& b, size_t at) {
  return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
}
void PutLe32(Bytes* b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) (*b)[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
  return s;
}
std::string Pad(unsigned value, int width) {
  std::string s = std::to_string(value);
  return s.size() < size_t(width) ? std::string(size_t(width) - s.size(), '0') + s : s;
}

constexpr size_t kArenaLightTableOffset = 0x41610;  // G4-R11-Fable: BATL/MSC2 + 0x41610
constexpr size_t kArenaLightChild102Offset = 0x3B88C;
constexpr size_t kArenaLightTableIn102 = 0x5D84;
constexpr size_t kArenaLightRowSize = 16;
constexpr uint32_t kArenaLightRowCount = 100;

struct ContainerChildRange {
  uint32_t id = 0;
  size_t start = 0;
  size_t size = 0;
};

bool FindEpacEntry(const Bytes& epac, const std::string& wanted_group, const std::string& wanted_name,
                   size_t* file_offset, size_t* byte_size, std::string* error) {
  if (epac.size() < 0x4000 || (std::memcmp(epac.data(), "EPAC", 4) != 0 && std::memcmp(epac.data(), "EPK8", 4) != 0))
    return Fail(error, "misc02_ingame.pac did not decode to EPAC/EPK8.");
  size_t cursor = 0x800;
  uint32_t found = 0;
  while (cursor + 12 <= 0x4000 && std::memcmp(epac.data() + cursor, "\0\0\0\0", 4) != 0) {
    const std::string group(reinterpret_cast<const char*>(epac.data() + cursor), 4);
    const uint32_t word = Le32(epac, cursor + 4);
    const size_t stride = (word & 0x00010000) ? 16 : 12;
    const uint32_t count_words = word & 0xffff;
    if (count_words % (stride / 4)) return Fail(error, "misc02_ingame.pac has a malformed EPAC group count.");
    const size_t count = count_words / (stride / 4);
    size_t entry = cursor + 12;
    for (size_t i = 0; i < count; ++i, entry += stride) {
      if (entry + stride > 0x4000) return Fail(error, "misc02_ingame.pac EPAC directory exceeds 0x4000 bytes.");
      const size_t name_width = stride - 8;
      const std::string name = Trim(std::string(reinterpret_cast<const char*>(epac.data() + entry), name_width));
      const uint32_t sector = Le32(epac, entry + name_width);
      const uint32_t size_units = Le32(epac, entry + name_width + 4);
      const uint64_t start64 = 0x4000ull + uint64_t(sector) * 0x800;
      const uint64_t size64 = uint64_t(size_units) * 0x100;
      if (start64 + size64 > epac.size()) return Fail(error, "misc02_ingame.pac has an EPAC entry outside the file.");
      if (Trim(group) == wanted_group && name == wanted_name) {
        *file_offset = static_cast<size_t>(start64);
        *byte_size = static_cast<size_t>(size64);
        ++found;
      }
    }
    cursor = entry;
  }
  if (found != 1) return Fail(error, "misc02_ingame.pac must contain exactly one BATL/MSC2 leaf.");
  return true;
}

bool FindPachChildRange(const Bytes& pach, uint32_t wanted_id, ContainerChildRange* child,
                        const char* label, std::string* error) {
  if (pach.size() < 8 || std::memcmp(pach.data(), "PACH", 4) != 0)
    return Fail(error, std::string(label) + " is not a PACH.");
  const uint32_t count = Le32(pach, 4);
  const uint64_t table_end64 = 8ull + uint64_t(count) * 12;
  if (count > 100000 || table_end64 > pach.size()) return Fail(error, std::string(label) + " has a malformed PACH table.");
  const size_t table_end = static_cast<size_t>(table_end64);
  uint32_t found = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = 8 + size_t(i) * 12;
    const uint32_t id = Le32(pach, at);
    const uint32_t relative = Le32(pach, at + 4);
    const uint32_t size = Le32(pach, at + 8);
    const uint64_t start64 = uint64_t(table_end) + relative;
    if (start64 + size > pach.size()) return Fail(error, std::string(label) + " has a PACH child outside its bounds.");
    if (id == wanted_id) {
      child->id = id;
      child->start = static_cast<size_t>(start64);
      child->size = size;
      ++found;
    }
  }
  if (found != 1) return Fail(error, std::string(label) + " must contain exactly one child " + std::to_string(wanted_id) + ".");
  return true;
}

uint32_t Be32(const Bytes& b, size_t at) {
  return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | uint32_t(b[at + 3]);
}
void PutBe32(Bytes* b, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) (*b)[at + i] = static_cast<uint8_t>(value >> (24 - i * 8));
}

const Bytes* Find(const std::map<std::string, Bytes>& files, const std::string& key) {
  const auto it = files.find(key);
  return it == files.end() ? nullptr : &it->second;
}

bool FindEntry(const std::vector<WWE13PacGroupData>& groups, const std::string& group,
               const std::string& name, const WWE13PacEntryData** entry) {
  for (const auto& g : groups) {
    if (Trim(g.name) != group) continue;
    for (const auto& e : g.entries)
      if (Trim(e.name) == name) { *entry = &e; return true; }
  }
  return false;
}

// One added entry: name keeps the source name's width and padding style (make_import.py carry()).
struct Added {
  std::string group;
  WWE13PacEntryData entry;
};

bool Carry(const std::map<std::string, Bytes>& source, const std::map<std::string, Bytes>& game,
           const std::string& rel, const std::string& group, const std::string& source_name,
           const std::string& new_name, std::map<std::string, std::vector<Added>>* adds,
           std::map<std::string, std::vector<WWE13PacGroupData>>* source_cache, std::string* error) {
  const Bytes* src = Find(source, rel);
  const Bytes* dst = Find(game, rel);
  if (!src || !dst) return Fail(error, "Missing input file " + rel + ".");
  auto cached = source_cache->find(rel);
  if (cached == source_cache->end()) {
    std::vector<WWE13PacGroupData> groups;
    if (!ReadWWE13PacGroups(*src, &groups, error)) return false;
    cached = source_cache->emplace(rel, std::move(groups)).first;
  }
  const WWE13PacEntryData* entry = nullptr;
  if (!FindEntry(cached->second, group, source_name, &entry)) return true;  // the '13 file lacks it: skipped, as the reference
  std::vector<WWE13PacGroupData> target;
  if (!ReadWWE13PacGroups(*dst, &target, error)) return false;
  const WWE13PacEntryData* existing = nullptr;
  if (FindEntry(target, group, new_name, &existing))
    return Fail(error, rel + " already has " + group + "/" + new_name + ": that arena number is not free.");
  for (const auto& added : (*adds)[rel])
    if (added.group == group && Trim(added.entry.name) == new_name)
      return Fail(error, "Two imported arenas use " + group + "/" + new_name + ".");
  Added added;
  added.group = group;
  added.entry.name = new_name;
  const char pad = (!entry->name.empty() && entry->name.back() == ' ') ? ' ' : '\0';
  added.entry.name.resize(entry->name.size(), pad);
  added.entry.data = entry->data;
  (*adds)[rel].push_back(std::move(added));
  return true;
}

bool MuasTexture(const Bytes& match_hd13, const std::string& name, Bytes* texture, std::string* error) {
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(match_hd13, &groups, error)) return false;
  const WWE13PacEntryData* muas = nullptr;
  if (!FindEntry(groups, "MLYE", "MUAS", &muas)) return Fail(error, "WWE '13 MatchHD has no MLYE/MUAS pictures.");
  Bytes pach = muas->data;
  if (pach.size() >= 4 && std::memcmp(pach.data(), "BPE ", 4) == 0 && !DecodeWWE13Bpe(muas->data, &pach, error)) return false;
  WWE13RawRowMap leaves;
  if (!ReadWWE13Pach(pach, &leaves, error)) return false;
  auto leaf = leaves.find(7001);
  if (leaf == leaves.end()) return Fail(error, "WWE '13 MUAS has no picture bank.");
  Bytes bank = leaf->second;
  if (bank.size() >= 4 && std::memcmp(bank.data(), "BPE ", 4) == 0 && !DecodeWWE13Bpe(leaf->second, &bank, error)) return false;
  if (bank.size() < 16) return Fail(error, "WWE '13 picture bank is truncated.");
  const uint32_t count = Le32(bank, 0);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t e = 16 + size_t(i) * 32;
    if (e + 32 > bank.size()) break;
    std::string entry_name(reinterpret_cast<const char*>(bank.data() + e), 16);
    entry_name = entry_name.substr(0, entry_name.find('\0'));
    if (entry_name != name) continue;
    const uint32_t size = Le32(bank, e + 20), offset = Le32(bank, e + 24);
    if (uint64_t(offset) + size > bank.size()) return Fail(error, "A WWE '13 picture points outside its bank.");
    texture->assign(bank.begin() + offset, bank.begin() + offset + size);
    return true;
  }
  return Fail(error, "WWE '13 picture " + name + " was not found.");
}

}  // namespace

std::vector<std::string> WWE13ArenaGameFiles() {
  return {"plist360.def", "plist360_4x3.def", "plist360.arc", "plist360_4x3.arc", "pac/string.pac",
          "pac/menu/MatchHD.pac", "pac/menu/MatchSD.pac", "pac/game2dHD.pac", "pac/game2dSD.pac",
          "pac/evt/evtgdhd.pac", "pac/evt/evtgdsd.pac", "pac/evt/evtmov.pac", "pac/misc02_ingame.pac"};
}

std::vector<std::string> WWE13ArenaSourceFiles(const std::vector<WWE13ArenaPort>& ports) {
  std::vector<std::string> files{"pac/menu/MatchHD.pac", "pac/menu/MatchSD.pac", "pac/game2dHD.pac", "pac/game2dSD.pac",
                                 "pac/evt/evtgdhd.pac", "pac/evt/evtgdsd.pac", "pac/evt/evtmov.pac"};
  for (const auto& port : ports) files.push_back("pac/bg/bg" + Pad(port.source_id, 2) + ".pac");
  return files;
}

bool FillWWE13ArenaLightRows(const Bytes& misc02_pac, const std::vector<uint32_t>& arena_ids,
                             Bytes* plain_output, bool* changed, std::string* error) {
  if (error) error->clear();
  if (!plain_output || !changed) return Fail(error, "The stands-light output is missing.");
  *changed = false;
  Bytes plain;
  if (!DecodeWWE13PacPlain(misc02_pac, &plain, error)) return false;
  size_t msc_offset = 0, msc_size = 0;
  if (!FindEpacEntry(plain, "BATL", "MSC2", &msc_offset, &msc_size, error)) return false;
  if (msc_size < kArenaLightTableOffset + kArenaLightRowCount * kArenaLightRowSize)
    return Fail(error, "BATL/MSC2 is shorter than the verified 100-row stands-light table.");
  const Bytes msc(plain.begin() + msc_offset, plain.begin() + msc_offset + msc_size);
  ContainerChildRange child102;
  if (!FindPachChildRange(msc, 102, &child102, "BATL/MSC2", error)) return false;
  if (child102.start != kArenaLightChild102Offset)
    return Fail(error, "BATL/MSC2 child 102 moved from the verified offset 0x3B88C.");
  const Bytes data102(msc.begin() + child102.start, msc.begin() + child102.start + child102.size);
  ContainerChildRange child5;
  if (!FindPachChildRange(data102, 5, &child5, "BATL/MSC2 child 102", error)) return false;
  const size_t table_in102 = kArenaLightTableOffset - child102.start;
  if (table_in102 != kArenaLightTableIn102)
    return Fail(error, "The stands-light table no longer matches child 102 offset 0x5D84.");
  if (child5.start > table_in102 || table_in102 + kArenaLightRowCount * kArenaLightRowSize > child5.start + child5.size)
    return Fail(error, "The 100-row stands-light span is outside child 5.");

  std::set<uint32_t> seen;
  bool did_change = false;
  for (uint32_t id : arena_ids) {
    if (id >= kArenaLightRowCount) return Fail(error, "Arena number " + std::to_string(id) + " is outside the 100-row table.");
    if (!seen.insert(id).second) return Fail(error, "Arena number " + std::to_string(id) + " was listed twice.");
    const size_t row = msc_offset + kArenaLightTableOffset + size_t(id) * kArenaLightRowSize;
    if (Be32(plain, row) || Be32(plain, row + 4) || Be32(plain, row + 8) || Be32(plain, row + 12)) continue;
    PutBe32(&plain, row, 1);
    PutBe32(&plain, row + 4, 5600);
    PutBe32(&plain, row + 8, 5750);
    PutBe32(&plain, row + 12, 250);
    did_change = true;
  }
  *plain_output = std::move(plain);
  *changed = did_change;
  return true;
}

bool RenameWWE13ArenaPac(const Bytes& source_pac, uint32_t source_id, uint32_t target_id, Bytes* plain,
                         std::string* error) {
  if (!plain) return Fail(error, "The arena PAC output is missing.");
  if (!DecodeWWE13PacPlain(source_pac, plain, error)) return false;
  const std::string from = Pad(source_id, 4), to = Pad(target_id, 4);
  size_t at = 0x800;
  while (at + 12 <= 0x4000 && plain->size() >= 0x4000 &&
         !((*plain)[at] == 0 && (*plain)[at + 1] == 0 && (*plain)[at + 2] == 0 && (*plain)[at + 3] == 0)) {
    const uint32_t word = Le32(*plain, at + 4);
    const size_t record = ((word >> 16) & 1) ? 16 : 12;   // the '13 group words carry extra flag bits: only bit 16 is width
    size_t e = at + 12;
    for (size_t i = 0; i < (word & 0xffff) * 4 / record; ++i, e += record) {
      std::string name(reinterpret_cast<const char*>(plain->data() + e), record - 8);
      if (Trim(name) != from) continue;
      std::memcpy(plain->data() + e, to.data(), to.size());
      return true;
    }
    at = e;
  }
  return Fail(error, "The WWE '13 arena PAC has no STG entry " + from + ".");
}

bool InsertWWE13ArenaPacListLine(Bytes* def, uint32_t target_id, uint16_t* line, std::string* error) {
  if (!def || !line) return Fail(error, "The pac list is missing.");
  std::vector<std::string> lines;
  const std::string text(def->begin(), def->end());
  size_t start = 0;
  while (true) {
    const size_t end = text.find("\r\n", start);
    lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 2;
  }
  int last = -1;
  for (size_t k = 0; k < lines.size(); ++k) {
    const std::string& l = lines[k];
    if (l.rfind("pac\\bg\\bg", 0) != 0 || l.size() < 11 || !isdigit(static_cast<unsigned char>(l[9])) ||
        !isdigit(static_cast<unsigned char>(l[10])))
      continue;
    if (uint32_t((l[9] - '0') * 10 + (l[10] - '0')) < target_id) last = static_cast<int>(k);
  }
  if (last < 0 || size_t(last) + 1 > 0xfffe) return Fail(error, "The pac list has no arena line before the new one.");
  const std::string added = "pac\\bg\\bg" + Pad(target_id, 2) + ".pac";
  for (const auto& l : lines)
    if (l == added) return Fail(error, "The pac list already names " + added + ".");
  lines.insert(lines.begin() + last + 1, added);
  std::string joined;
  for (size_t k = 0; k < lines.size(); ++k) joined += (k ? "\r\n" : "") + lines[k];
  def->assign(joined.begin(), joined.end());
  *line = static_cast<uint16_t>(last + 1);
  return true;
}

bool AddWWE13TextureBankEntries(const Bytes& bank, const std::vector<std::pair<std::string, Bytes>>& added,
                                Bytes* output, std::string* error) {
  if (!output || bank.size() < 16) return Fail(error, "The picture bank is truncated.");
  const uint32_t count = Le32(bank, 0);
  if (16 + uint64_t(count) * 32 > bank.size()) return Fail(error, "The picture bank directory is truncated.");
  std::vector<std::pair<std::string, Bytes>> items;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t e = 16 + size_t(i) * 32;
    const uint32_t size = Le32(bank, e + 20), offset = Le32(bank, e + 24);
    if (uint64_t(offset) + size > bank.size()) return Fail(error, "A picture points outside its bank.");
    items.emplace_back(std::string(reinterpret_cast<const char*>(bank.data() + e), 16),
                       Bytes(bank.begin() + offset, bank.begin() + offset + size));
  }
  for (const auto& [name, data] : added) {
    if (name.empty() || name.size() > 15) return Fail(error, "A picture name must be 1-15 characters.");
    std::string padded = name;
    padded.resize(16, '\0');
    for (const auto& item : items)
      if (item.first == padded) return Fail(error, "The picture bank already has " + name + ".");
    items.emplace_back(padded, data);
  }
  const uint64_t total = items.size();
  Bytes result(bank.begin(), bank.begin() + 16);
  PutLe32(&result, 0, static_cast<uint32_t>(total));
  Bytes body;
  const uint64_t base = 16 + total * 32;
  for (const auto& [name, data] : items) {
    const uint64_t offset = base + body.size();
    if (offset > std::numeric_limits<uint32_t>::max()) return Fail(error, "The picture bank is too large.");
    const size_t at = result.size();
    result.resize(at + 32, 0);
    std::memcpy(result.data() + at, name.data(), 16);
    std::memcpy(result.data() + at + 16, "dds", 3);
    PutLe32(&result, at + 20, static_cast<uint32_t>(data.size()));
    PutLe32(&result, at + 24, static_cast<uint32_t>(offset));
    body.insert(body.end(), data.begin(), data.end());
  }
  result.insert(result.end(), body.begin(), body.end());
  *output = std::move(result);
  return true;
}

bool ConvertWWE13Arenas(const std::vector<WWE13ArenaPort>& ports, const WWE13ArenaInput& input,
                        WWE13ArenaOutput* output, std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The arena import output is missing.");
  if (ports.empty()) return Fail(error, "No arenas were chosen.");
  for (const auto& file : WWE13ArenaGameFiles())
    if (!Find(input.game, file)) return Fail(error, "The WWE 2K14 file " + file + " is missing.");
  for (const auto& file : WWE13ArenaSourceFiles(ports))
    if (!Find(input.source, file)) return Fail(error, "The WWE '13 file " + file + " is missing.");
  std::set<uint32_t> taken(input.existing_bg.begin(), input.existing_bg.end());
  std::set<std::string> tags;
  for (const auto& port : ports) {
    if (port.target_id >= 100) return Fail(error, "Arena number " + std::to_string(port.target_id) + " is too high.");
    if (!taken.insert(port.target_id).second)
      return Fail(error, "Arena number " + std::to_string(port.target_id) + " is already used.");
    if (!tags.insert(port.tag).second) return Fail(error, "Two arenas use the picture name " + port.tag + ".");
  }

  WWE13ArenaOutput result;
  std::map<std::string, Bytes> game = input.game;  // working copies; written back to `result` at the end
  std::map<std::string, std::vector<Added>> adds;
  std::map<std::string, std::vector<WWE13PacGroupData>> source_cache;
  WWE13StringKeyMap names;
  std::vector<std::pair<std::string, Bytes>> pictures;
  std::string listing;

  for (const auto& port : ports) {
    const std::string bg = "pac/bg/bg" + Pad(port.target_id, 2) + ".pac";
    Bytes plain;
    if (!RenameWWE13ArenaPac(input.source.at("pac/bg/bg" + Pad(port.source_id, 2) + ".pac"), port.source_id,
                             port.target_id, &plain, error))
      return false;
    for (const char* arc_name : {"plist360.arc", "plist360_4x3.arc"}) {
      std::string def_name(arc_name);
      def_name.replace(def_name.size() - 4, 4, ".def");
      uint16_t line = 0;
      if (!InsertWWE13ArenaPacListLine(&game[def_name], port.target_id, &line, error)) return false;
      if (!InsertWWE13ArcBlock(&game[arc_name], line, plain, error)) return false;
    }
    result.files[bg] = std::move(plain);
    const std::string s2 = Pad(port.source_id, 2), t2 = Pad(port.target_id, 2);
    for (const char* side : {"HD", "SD"}) {
      const std::string match = std::string("pac/menu/Match") + side + ".pac";
      const std::string g2d = std::string("pac/game2d") + side + ".pac";
      const std::string evtgd = std::string("pac/evt/evtgd") + (side[0] == 'H' ? "hd" : "sd") + ".pac";
      if (!Carry(input.source, input.game, match, "MLYE", "MB" + s2, "MB" + t2, &adds, &source_cache, error) ||
          !Carry(input.source, input.game, match, "MENU", "M" + s2 + "F", "M" + t2 + "F", &adds, &source_cache, error) ||
          !Carry(input.source, input.game, g2d, "GA2D", Pad(port.source_id, 4), Pad(port.target_id, 4), &adds,
                 &source_cache, error) ||
          !Carry(input.source, input.game, evtgd, "ETEX", Pad(port.source_id, 4), Pad(port.target_id, 4), &adds,
                 &source_cache, error) ||
          !Carry(input.source, input.game, evtgd, "ETEX", Pad(200 + port.source_id, 4), Pad(200 + port.target_id, 4),
                 &adds, &source_cache, error))
        return false;
    }
    if (!Carry(input.source, input.game, "pac/evt/evtmov.pac", "MOVT", "M" + Pad(port.source_id, 3),
               "M" + Pad(port.target_id, 3), &adds, &source_cache, error))
      return false;
    names[kArenaNameBase + port.target_id] = Bytes(port.name.begin(), port.name.end());
    Bytes picture;
    if (!MuasTexture(input.source.at("pac/menu/MatchHD.pac"), port.picture, &picture, error)) return false;
    pictures.emplace_back(port.tag, std::move(picture));
    listing += std::to_string(port.target_id) + " " + port.tag + "\n";
  }

  for (const auto& [group, entry] : input.evtmov_added) {
    for (const auto& a : adds["pac/evt/evtmov.pac"])
      if (a.group == Trim(group) && Trim(a.entry.name) == Trim(entry.name))
        return Fail(error, "evtmov.pac gets " + Trim(group) + "/" + Trim(entry.name) + " twice.");
    Added a;
    a.group = Trim(group);
    a.entry = entry;
    adds["pac/evt/evtmov.pac"].push_back(std::move(a));
  }

  for (auto& [rel, added] : adds) {
    const Bytes& original = input.game.at(rel);
    std::vector<WWE13PacGroupData> groups;
    if (!ReadWWE13PacGroups(original, &groups, error)) return false;
    for (auto& a : added) {
      // game2d PACs have two GA2D groups ('BASE' first, the per-arena entries last): add to the last group of that name.
      auto g = std::find_if(groups.rbegin(), groups.rend(), [&](const auto& x) { return Trim(x.name) == a.group; });
      if (g == groups.rend()) return Fail(error, rel + " has no " + a.group + " group.");
      g->entries.push_back(std::move(a.entry));
    }
    for (auto& g : groups)   // the reference rebuild keeps every group sorted by entry name
      std::stable_sort(g.entries.begin(), g.entries.end(), [](const auto& x, const auto& y) { return x.name < y.name; });
    if (rel.rfind("pac/menu/Match", 0) == 0) {   // arena-select pictures: MLYE/MCAS leaf 7001
      auto g = std::find_if(groups.begin(), groups.end(), [](const auto& x) { return Trim(x.name) == "MLYE"; });
      WWE13PacEntryData* mcas = nullptr;
      if (g != groups.end())
        for (auto& e : g->entries) if (Trim(e.name) == "MCAS") mcas = &e;
      if (!mcas) return Fail(error, rel + " has no MLYE/MCAS pictures.");
      std::vector<WWE13PachLeafData> leaves;
      if (!ReadWWE13PachLeaves(mcas->data, &leaves, error)) return false;
      bool found = false;
      for (auto& leaf : leaves) {
        if (leaf.id != 7001) continue;
        Bytes bank, grown, packed;
        if (!DecodeWWE13ZlibLeaf(leaf.data, &bank, error) ||
            !AddWWE13TextureBankEntries(bank, pictures, &grown, error) ||
            !EncodeWWE13ZlibLeaf(leaf.data, grown, &packed, error))
          return false;
        leaf.data = std::move(packed);
        found = true;
      }
      if (!found) return Fail(error, rel + " MCAS has no picture bank.");
      if (!BuildWWE13PachLeaves(leaves, &mcas->data, true, error)) return false;
    }
    Bytes rebuilt;
    if (!RebuildWWE13PacGroups(original, groups, &rebuilt, error)) return false;
    bool any = false;
    for (const char* arc_name : {"plist360.arc", "plist360_4x3.arc"}) {
      bool synced = false;
      if (!SyncWWE13PacArchiveIfListed(&game[arc_name], original, rebuilt, &synced, error)) return false;
      any = any || synced;
    }
    if (!any) return Fail(error, "No pac list carries " + rel + ".");
    result.files[rel] = std::move(rebuilt);
  }

  {
    const Bytes& original = input.game.at("pac/string.pac");
    Bytes rebuilt;
    size_t inserted = 0;
    if (!ConvertWWE13StringKeysAt(original, names, kArenaNameSentinel, &rebuilt, &inserted, error)) return false;
    if (inserted == 0) return Fail(error, "No arena-name table was found in string.pac.");
    bool any = false;
    for (const char* arc_name : {"plist360.arc", "plist360_4x3.arc"}) {
      bool synced = false;
      if (!SyncWWE13PacArchiveIfListed(&game[arc_name], original, rebuilt, &synced, error)) return false;
      any = any || synced;
    }
    if (!any) return Fail(error, "No pac list carries pac/string.pac.");
    result.files["pac/string.pac"] = std::move(rebuilt);
  }
  {
    std::vector<uint32_t> targets;
    targets.reserve(ports.size());
    for (const auto& port : ports) targets.push_back(port.target_id);
    Bytes patched;
    bool changed = false;
    if (!FillWWE13ArenaLightRows(input.game.at("pac/misc02_ingame.pac"), targets, &patched, &changed, error)) return false;
    if (changed) {
      const Bytes& original = input.game.at("pac/misc02_ingame.pac");
      for (const char* arc_name : {"plist360.arc", "plist360_4x3.arc"}) {
        bool synced = false;
        if (!SyncWWE13PacArchiveIfListed(&game[arc_name], original, patched, &synced, error)) return false;
      }
      result.files["pac/misc02_ingame.pac"] = std::move(patched);
    }
  }
  for (const char* name : {"plist360.arc", "plist360_4x3.arc", "plist360.def", "plist360_4x3.def"})
    result.files[name] = std::move(game[name]);
  result.arena_list = std::move(listing);
  *output = std::move(result);
  return true;
}

}  // namespace wwe2k14::launcher
