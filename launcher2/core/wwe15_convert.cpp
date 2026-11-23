#include "wwe15_convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace wwe2k14::launcher {
namespace {

using Bytes = WWE15Bytes;

// build.py VOCAB (surveyed 11-07 over every shipped 2K14 ch PAC entry, minus 100).
constexpr std::array<uint32_t, 42> kModelLeaves{
    0,     10,    12,    13,    14,    15,    16,    26,    27,    28,    29,
    40,    50,    60,    61,    80,    110,   9000,  9001,  9002,  9003,  9004,
    9005,  9010,  9012,  9013,  9015,  10000, 10010, 10500, 10501, 20000, 20001,
    20002, 21000, 21001, 21002, 21010, 21011, 21020, 40000, 40010};

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

void PutLe32(Bytes* bytes, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) (*bytes)[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

std::string Trim(const std::string& name) {
  size_t end = name.size();
  while (end > 0 && (name[end - 1] == '\0' || name[end - 1] == ' ')) --end;
  return name.substr(0, end);
}

std::string Id6(uint32_t id) {
  char text[16];
  std::snprintf(text, sizeof(text), "%06u", id);
  return text;
}

// The retail model PACH form used by the python route (build.py model_pac): leaves sorted by
// ID, each leaf 16-byte aligned from the end of the table, no tail padding.
Bytes BuildModelPach(const std::map<uint32_t, Bytes>& leaves) {
  Bytes output(8 + leaves.size() * 12, 0);
  std::memcpy(output.data(), "PACH", 4);
  PutLe32(&output, 4, static_cast<uint32_t>(leaves.size()));
  Bytes body;
  size_t index = 0;
  for (const auto& [id, data] : leaves) {
    while (body.size() % 16) body.push_back(0);
    const size_t record = 8 + index++ * 12;
    PutLe32(&output, record, id);
    PutLe32(&output, record + 4, static_cast<uint32_t>(body.size()));
    PutLe32(&output, record + 8, static_cast<uint32_t>(data.size()));
    body.insert(body.end(), data.begin(), data.end());
  }
  output.insert(output.end(), body.begin(), body.end());
  return output;
}

std::string JoinIds(const std::vector<uint32_t>& ids) {
  std::ostringstream text;
  text << "[";
  for (size_t i = 0; i < ids.size(); ++i) text << (i ? ", " : "") << ids[i];
  text << "]";
  return text.str();
}

}  // namespace

bool IsWWE2K14ModelLeaf(uint32_t leaf_id) {
  return std::find(kModelLeaves.begin(), kModelLeaves.end(), leaf_id) != kModelLeaves.end();
}

bool ConvertWWE15CharacterPac(const WWE15Bytes& source_wwe15_pac, uint32_t source_id,
                              uint32_t target_id, WWE15Bytes* output,
                              std::vector<std::string>* notes, std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The converted model output is missing.");
  const std::string who = "WWE 2K15 wrestler " + std::to_string(source_id);
  if (source_id > 999999 || target_id > 999999)
    return Fail(error, who + ": wrestler IDs must have at most six digits (target " +
                           std::to_string(target_id) + ").");
  std::vector<WWE13PacGroupData> groups;
  std::string detail;
  if (!ReadWWE13PacGroups(source_wwe15_pac, &groups, &detail))
    return Fail(error, who + ": model file ch" + std::to_string(source_id) +
                           ".pac could not be read: " + detail);

  struct Look {
    std::string name;  // source entry name, trimmed
    std::map<uint32_t, Bytes> leaves;
  };
  std::vector<Look> looks;
  std::vector<std::string> local_notes;
  const std::string prefix = Id6(source_id);
  for (const WWE13PacGroupData& group : groups) {
    if (Trim(group.name).rfind("EMD", 0) != 0) continue;
    for (const WWE13PacEntryData& entry : group.entries) {
      Look look;
      look.name = Trim(entry.name);
      if (look.name.size() != 8 || look.name.compare(0, 6, prefix) != 0)
        return Fail(error, who + ": model entry '" + look.name + "' in ch" +
                               std::to_string(source_id) + ".pac does not start with " +
                               prefix + " (expected 8 characters, " + prefix + "NN).");
      Bytes pach;
      if (!DecodeWWE13Bpe(entry.data, &pach, &detail))
        return Fail(error, who + ": model entry " + look.name + " could not be unpacked: " +
                               detail);
      std::vector<WWE13PachLeafData> leaves;
      if (!ReadWWE13PachLeaves(pach, &leaves, &detail))
        return Fail(error, who + ": model entry " + look.name + " (" +
                               std::to_string(pach.size()) + " bytes) is not a valid part list: " +
                               detail);
      std::vector<uint32_t> dropped;
      for (WWE13PachLeafData& leaf : leaves) {
        if (!IsWWE2K14ModelLeaf(leaf.id)) {
          if (std::find(dropped.begin(), dropped.end(), leaf.id) == dropped.end())
            dropped.push_back(leaf.id);
          continue;
        }
        look.leaves[leaf.id] = std::move(leaf.data);  // a repeated ID keeps the last copy (python dict)
      }
      std::sort(dropped.begin(), dropped.end());
      if (!dropped.empty())
        local_notes.push_back(look.name + " dropped leaves " + JoinIds(dropped));
      looks.push_back(std::move(look));
    }
  }
  if (looks.empty())
    return Fail(error, who + ": ch" + std::to_string(source_id) +
                           ".pac has no model (EMD) entries.");

  auto has = [&](const std::string& suffix) {
    return std::any_of(looks.begin(), looks.end(),
                       [&](const Look& l) { return l.name == prefix + suffix; });
  };
  const bool only_00 = has("00") && !has("02") && !has("04");
  std::map<std::string, std::map<uint32_t, Bytes>> converted;  // sorted by output name
  const std::string target = Id6(target_id);
  for (Look& look : looks) {
    std::string suffix = look.name.substr(6);
    if (only_00 && suffix == "00") suffix = "02";
    // python keeps the first list entry with a duplicated name and sorts by name; a PAC never
    // repeats a name, but refuse rather than guess.
    if (!converted.emplace(target + suffix, std::move(look.leaves)).second)
      return Fail(error, who + ": model entry " + target + suffix + " appears twice.");
  }
  if (only_00) local_notes.push_back("single 2K15 00 model mapped to default 02");
  if (!converted.count(target + "04")) {
    const auto base = converted.find(target + "02");
    if (base == converted.end())
      return Fail(error, who + ": ch" + std::to_string(source_id) +
                             ".pac has no default look (" + prefix + "02) to copy as " +
                             prefix + "04.");
    converted[target + "04"] = base->second;
    local_notes.push_back("04 = copy of 02");
  }

  WWE13PacGroupData emd;
  emd.name = "EMD ";
  emd.wide = true;
  emd.reserved = 0;
  for (const auto& [name, leaves] : converted) {
    WWE13PacEntryData entry;
    entry.name = name;
    entry.data = BuildModelPach(leaves);
    emd.entries.push_back(std::move(entry));
  }
  if (!BuildWWE13PacGroups({emd}, output, &detail))
    return Fail(error, who + ": the converted model file could not be built: " + detail);
  if (notes) *notes = std::move(local_notes);
  return true;
}

bool ConvertWWE15LooksPac(const WWE15Bytes& wwe14_pac, const WWE15Bytes& wwe15_pac,
                          uint32_t id, WWE15Bytes* output, WWE15LooksReport* report,
                          std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The WWE 2K15 looks output is missing.");
  if (id > 999999) return Fail(error, "Wrestler IDs must have at most six digits.");

  WWE15LooksReport local_report;
  local_report.id = id;
  std::vector<WWE13PacGroupData> groups14, groups15;
  std::string detail;
  if (!ReadWWE13PacGroups(wwe14_pac, &groups14, &detail))
    return Fail(error, "WWE 2K14 ch" + std::to_string(id) + ".pac could not be read: " + detail);
  if (!ReadWWE13PacGroups(wwe15_pac, &groups15, &detail))
    return Fail(error, "WWE 2K15 ch" + std::to_string(id) + ".pac could not be read: " + detail);

  std::map<std::pair<std::string, std::string>, const WWE13PacEntryData*> entries15;
  std::set<std::string> names15, names14;
  for (const WWE13PacGroupData& group : groups15) {
    const std::string group_name = Trim(group.name);
    if (group_name.rfind("EMD", 0) != 0) continue;
    for (const WWE13PacEntryData& entry : group.entries) {
      const std::string name = Trim(entry.name);
      names15.insert(name);
      entries15[{group_name, name}] = &entry;
    }
  }

  for (WWE13PacGroupData& group : groups14) {
    const std::string group_name = Trim(group.name);
    if (group_name.rfind("EMD", 0) != 0) continue;
    for (WWE13PacEntryData& entry : group.entries) {
      const std::string name = Trim(entry.name);
      names14.insert(name);
      const auto found15 = entries15.find({group_name, name});
      if (found15 == entries15.end()) {
        local_report.kept14.push_back(name);
        continue;
      }

      WWE15Bytes plain14, plain15;
      if (!DecodeWWE13Bpe(entry.data, &plain14, &detail))
        return Fail(error, "WWE 2K14 wrestler " + std::to_string(id) + " attire " + name +
                            " could not be unpacked: " + detail);
      if (!DecodeWWE13Bpe(found15->second->data, &plain15, &detail))
        return Fail(error, "WWE 2K15 wrestler " + std::to_string(id) + " attire " + name +
                            " could not be unpacked: " + detail);
      std::vector<WWE13PachLeafData> old_leaves, new_leaves;
      if (!ReadWWE13PachLeaves(plain14, &old_leaves, &detail))
        return Fail(error, "WWE 2K14 wrestler " + std::to_string(id) + " attire " + name +
                            " is not a valid PACH: " + detail);
      if (!ReadWWE13PachLeaves(plain15, &new_leaves, &detail))
        return Fail(error, "WWE 2K15 wrestler " + std::to_string(id) + " attire " + name +
                            " is not a valid PACH: " + detail);

      std::map<uint32_t, WWE15Bytes> merged;
      for (WWE13PachLeafData& leaf : old_leaves) merged[leaf.id] = std::move(leaf.data);
      std::set<uint32_t> dropped;
      for (WWE13PachLeafData& leaf : new_leaves) {
        if (IsWWE2K14ModelLeaf(leaf.id)) merged[leaf.id] = std::move(leaf.data);
        else dropped.insert(leaf.id);
      }
      std::vector<uint32_t> dropped_ids(dropped.begin(), dropped.end());
      if (!dropped_ids.empty()) local_report.dropped_leaves[name] = std::move(dropped_ids);
      entry.data = BuildModelPach(merged);
      local_report.swapped.push_back(name);
    }
  }
  for (const std::string& name : names15)
    if (!names14.count(name)) local_report.only15.push_back(name);

  if (local_report.swapped.empty()) {
    *output = wwe14_pac;
  } else if (!RebuildWWE13PacGroups(wwe14_pac, groups14, output, &detail)) {
    return Fail(error, "WWE 2K14 ch" + std::to_string(id) + ".pac could not be rebuilt: " + detail);
  }
  if (report) *report = std::move(local_report);
  return true;
}

}  // namespace wwe2k14::launcher

namespace wwe2k14::launcher {
// ------------------------------------------------------------------ EVP2 (step C)
namespace {

constexpr size_t kEvpBase = 8;
constexpr size_t kEvpTable = 0x44;
constexpr size_t kElement14 = 212;
constexpr size_t kElement15 = 788;
constexpr size_t kElementExtra = kElement15 - kElement14;  // 576
constexpr size_t kRecord = 24;
constexpr size_t kElementHead = 16;
constexpr size_t kRecords14 = (kElement14 - kElementHead - 4) / kRecord;  // 8
constexpr size_t kRecords15 = (kElement15 - kElementHead - 4) / kRecord;  // 32
constexpr size_t kRecordCut = 204;

struct EvpError {
  std::string message;
};

uint32_t Be32At(const Bytes& b, size_t at, const std::string& what) {
  if (at > b.size() || b.size() - at < 4)
    throw EvpError{what + " at byte " + std::to_string(at) + " is past the end (" +
                   std::to_string(b.size()) + " bytes)"};
  return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) |
         uint32_t(b[at + 3]);
}
uint16_t Be16At(const Bytes& b, size_t at, const std::string& what) {
  if (at > b.size() || b.size() - at < 2)
    throw EvpError{what + " at byte " + std::to_string(at) + " is past the end (" +
                   std::to_string(b.size()) + " bytes)"};
  return static_cast<uint16_t>((b[at] << 8) | b[at + 1]);
}
void PutBe32(Bytes* b, size_t at, uint32_t v, const std::string& what) {
  if (at > b->size() || b->size() - at < 4)
    throw EvpError{what + " at byte " + std::to_string(at) + " is past the end (" +
                   std::to_string(b->size()) + " bytes)"};
  (*b)[at] = uint8_t(v >> 24); (*b)[at + 1] = uint8_t(v >> 16);
  (*b)[at + 2] = uint8_t(v >> 8); (*b)[at + 3] = uint8_t(v);
}
// Python slice b[from:to] with clamping.
Bytes Slice(const Bytes& b, size_t from, size_t to) {
  from = std::min(from, b.size());
  to = std::min(to, b.size());
  if (to <= from) return {};
  return Bytes(b.begin() + static_cast<std::ptrdiff_t>(from), b.begin() + static_cast<std::ptrdiff_t>(to));
}

std::vector<uint64_t> PofRead(const Bytes& b) {
  std::vector<uint64_t> out;
  uint64_t a = 0;
  size_t p = 0;
  auto byte = [&](size_t at) -> uint64_t {
    if (at >= b.size()) throw EvpError{"the POF0 pointer table is truncated"};
    return b[at];
  };
  while (p < b.size()) {
    const uint8_t c = b[p];
    const int k = c >> 6;
    if (k == 0) break;
    if (k == 1) { a += uint64_t(c & 63) * 4; p += 1; }
    else if (k == 2) { a += ((uint64_t(c & 63) << 8) | byte(p + 1)) * 4; p += 2; }
    else { a += ((uint64_t(c & 63) << 24) | (byte(p + 1) << 16) | (byte(p + 2) << 8) | byte(p + 3)) * 4; p += 4; }
    out.push_back(a);
  }
  return out;
}

Bytes PofWrite(const std::vector<int64_t>& pointers) {
  Bytes out;
  int64_t a = 0;
  for (int64_t q : pointers) {
    const int64_t diff = q - a;
    if (diff % 4 != 0 || diff / 4 <= 0)
      throw EvpError{"the rebuilt pointer table is not increasing (pointer " + std::to_string(q) + ")"};
    const uint64_t d = uint64_t(diff / 4);
    a = q;
    if (d < 64) out.push_back(uint8_t(0x40 | d));
    else if (d < 0x4000) { out.push_back(uint8_t(0x80 | (d >> 8))); out.push_back(uint8_t(d & 255)); }
    else if (d < 0x40000000) {
      out.push_back(uint8_t(0xC0 | (d >> 24))); out.push_back(uint8_t((d >> 16) & 255));
      out.push_back(uint8_t((d >> 8) & 255)); out.push_back(uint8_t(d & 255));
    } else throw EvpError{"a pointer step is too large for the pointer table"};
  }
  while (out.size() % 4) out.push_back(0);
  return out;
}

// [(file offset of list start, count)] of every element list, sorted.
std::vector<std::pair<size_t, size_t>> EvpLists(const Bytes& p) {
  const uint32_t n = Be32At(p, 0x40, "the track count");
  std::vector<std::pair<size_t, size_t>> out;
  for (uint64_t i = 1; i <= n; ++i) {
    const size_t at = kEvpTable + 32 * size_t(i);
    const uint16_t c = Be16At(p, at, "list " + std::to_string(i) + "'s element count");
    const uint32_t o = Be32At(p, at + 4, "list " + std::to_string(i) + "'s offset");
    if (c) out.emplace_back(kEvpBase + o, c);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<Bytes> Records(const Bytes& element, size_t n) {
  std::vector<Bytes> out;
  for (size_t k = 0; k < n; ++k)
    out.push_back(Slice(element, kElementHead + kRecord * k, kElementHead + kRecord * (k + 1)));
  return out;
}
size_t UsedRecords(const std::vector<Bytes>& records) {
  size_t used = 0;
  for (size_t k = 0; k < records.size(); ++k)
    if (std::any_of(records[k].begin(), records[k].end(), [](uint8_t x) { return x != 0; })) used = k + 1;
  return used;
}

void ConvertCut(const Bytes& p, size_t cut, Bytes* result, WWE15Evp2Info* info) {
  if (p.size() < 10 || std::memcmp(p.data(), "0FOP", 4) != 0 || p[8] != 'E' || p[9] != 'V')
    throw EvpError{"the file does not start with 0FOP/EV"};
  const uint32_t body_size = Be32At(p, 4, "the body size");
  if (uint64_t(body_size) + 16 > p.size() || std::memcmp(p.data() + 8 + body_size, "POF0", 4) != 0)
    throw EvpError{"no POF0 pointer table after the " + std::to_string(body_size) + "-byte body"};
  const uint32_t pof_size = Be32At(p, 8 + body_size + 4, "the pointer table size");
  const size_t pof_start = 8 + size_t(body_size) + 8;
  const Bytes pof = Slice(p, pof_start, pof_start + pof_size);
  const std::vector<uint64_t> pointers = PofRead(pof);
  const auto lists = EvpLists(p);
  std::vector<std::pair<size_t, size_t>> removed;
  size_t dropped = 0, elements = 0;
  for (const auto& [start, count] : lists) {
    elements += count;
    for (size_t e = 0; e < count; ++e) {
      const size_t a = start + e * kElement15 + cut;
      const size_t b = a + kElementExtra;
      removed.emplace_back(a, b);
      const Bytes cutout = Slice(p, a, b);
      if (std::any_of(cutout.begin(), cutout.end(), [](uint8_t x) { return x != 0; })) ++dropped;
    }
  }
  std::sort(removed.begin(), removed.end());
  std::vector<size_t> starts;
  for (const auto& r : removed) starts.push_back(r.first);
  auto map = [&](int64_t off) -> int64_t {
    const size_t k = static_cast<size_t>(
        std::upper_bound(starts.begin(), starts.end(), static_cast<size_t>(std::max<int64_t>(off, 0))) - starts.begin());
    if (off < 0) return off;  // (never: offsets are file positions)
    return off - int64_t(k * kElementExtra);
  };
  Bytes out;
  size_t last = 0;
  for (const auto& [a, b] : removed) {
    const Bytes keep = Slice(p, last, a);
    out.insert(out.end(), keep.begin(), keep.end());
    last = b;
  }
  const Bytes keep = Slice(p, last, 8 + size_t(body_size));
  out.insert(out.end(), keep.begin(), keep.end());
  std::vector<int64_t> new_pointers;
  for (uint64_t q : pointers) {
    const uint32_t v = Be32At(p, kEvpBase + q, "pointer " + std::to_string(q));
    const int64_t nq = map(int64_t(kEvpBase + q));
    const int64_t nv = map(int64_t(kEvpBase) + int64_t(v)) - int64_t(kEvpBase);
    if (nv < 0 || nv > 0xffffffffLL || nq < 0)
      throw EvpError{"pointer " + std::to_string(q) + " points into a removed extension"};
    PutBe32(&out, size_t(nq), uint32_t(nv), "rebuilt pointer " + std::to_string(q));
    new_pointers.push_back(nq - int64_t(kEvpBase));
  }
  const size_t new_size = out.size() - 8;
  PutBe32(&out, 4, uint32_t(new_size), "the body size");
  if (Be32At(p, 0x0c, "the second size field") == body_size) PutBe32(&out, 0x0c, uint32_t(new_size), "the second size field");
  const Bytes pw = PofWrite(new_pointers);
  out.insert(out.end(), {'P', 'O', 'F', '0', 0, 0, 0, 0});
  PutBe32(&out, out.size() - 4, uint32_t(pw.size()), "the pointer table size");
  out.insert(out.end(), pw.begin(), pw.end());
  const Bytes tail = Slice(p, pof_start + pof.size(), p.size());
  out.insert(out.end(), tail.begin(), tail.end());
  info->lists = lists.size();
  info->elements = elements;
  info->dropped_nonzero = dropped;
  info->pointers = pointers.size();
  info->tail_padding = tail.size();
  *result = std::move(out);
}

struct EvpTrack {
  uint32_t k = 0, id = 0, flags = 0, count = 0, offset = 0;
};
std::vector<EvpTrack> EvpTracks(const Bytes& p) {
  const uint32_t n = Be32At(p, 0x40, "the track count");
  std::vector<EvpTrack> out;
  for (uint32_t k = 0; k < n; ++k) {
    const size_t r = 0x4c + 32 * size_t(k);
    const std::string what = "track " + std::to_string(k);
    out.push_back({k, Be32At(p, r + 4, what), Be32At(p, r + 8, what), Be16At(p, r + 24, what),
                   Be32At(p, r + 28, what)});
  }
  return out;
}
std::vector<Bytes> EvpElements(const Bytes& p, const EvpTrack& t) {
  std::vector<Bytes> out;
  for (size_t e = 0; e < t.count; ++e)
    out.push_back(Slice(p, 8 + size_t(t.offset) + e * kElement14, 8 + size_t(t.offset) + (e + 1) * kElement14));
  return out;
}
bool ElementUsed(const Bytes& element) { return UsedRecords(Records(element, kRecords14)) > 0; }

}  // namespace

bool ConvertWWE15Evp2(const WWE15Bytes& evp2, const std::string& key, WWE15Bytes* evp,
                      WWE15Evp2Info* info, std::string* error) {
  if (error) error->clear();
  if (!evp) return Fail(error, "The converted EVP output is missing.");
  WWE15Evp2Info local;
  try {
    Bytes out;
    ConvertCut(evp2, kRecordCut, &out, &local);
    const auto l15 = EvpLists(evp2);
    const auto l14 = EvpLists(out);
    bool same = l15.size() == l14.size();
    for (size_t i = 0; same && i < l15.size(); ++i) same = l15[i].second == l14[i].second;
    if (!same) throw EvpError{"the converted lists do not match the source lists"};
    size_t resampled = 0;
    for (size_t i = 0; i < l15.size(); ++i) {
      const auto [s5, count] = l15[i];
      const size_t s4 = l14[i].first;
      for (size_t e = 0; e < count; ++e) {
        const Bytes element = Slice(evp2, s5 + e * kElement15, s5 + (e + 1) * kElement15);
        const auto records = Records(element, kRecords15);
        const size_t n = UsedRecords(records);
        if (n <= kRecords14) continue;
        Bytes body;
        for (size_t r = 0; r < kRecords14; ++r) {
          // round(r * (n - 1) / 7): never a .5 tie (7 is odd), so llround matches python
          const size_t pick = static_cast<size_t>(std::llround(double(r * (n - 1)) / double(kRecords14 - 1)));
          body.insert(body.end(), records[pick].begin(), records[pick].end());
        }
        const size_t at = s4 + e * kElement14 + kElementHead;
        if (at + body.size() > out.size())
          throw EvpError{"a resampled element runs past the end of the converted file"};
        std::copy(body.begin(), body.end(), out.begin() + static_cast<std::ptrdiff_t>(at));
        ++resampled;
      }
    }
    local.resampled = resampled;
    *evp = std::move(out);
  } catch (const EvpError& ex) {
    return Fail(error, "WWE 2K15 script " + key + " (" + std::to_string(evp2.size()) +
                           " bytes) could not be converted: " + ex.message + ".");
  }
  if (info) *info = local;
  return true;
}

bool FillWWE15WrestlerTracks(WWE15Bytes* evp, const WWE15Bytes& donor_evp,
                             std::vector<std::pair<uint32_t, uint32_t>>* filled,
                             std::string* error) {
  if (error) error->clear();
  if (!evp) return Fail(error, "The entrance to fill is missing.");
  std::vector<std::pair<uint32_t, uint32_t>> local;
  try {
    const auto donor_tracks = EvpTracks(donor_evp);
    for (const EvpTrack& t : EvpTracks(*evp)) {
      if (t.k == 0 || t.flags != 0x01000000) continue;
      const auto mine = EvpElements(*evp, t);
      if (std::any_of(mine.begin(), mine.end(), ElementUsed)) continue;
      const auto r = std::find_if(donor_tracks.begin(), donor_tracks.end(), [&](const EvpTrack& x) {
        return x.k == t.k && x.id == t.id && x.flags == t.flags;
      });
      if (r == donor_tracks.end()) continue;
      const auto donor = EvpElements(donor_evp, *r);
      for (size_t e = 0; e < t.count; e += 5) {
        if (donor.empty()) throw EvpError{"donor track " + std::to_string(t.k) + " has no steps"};
        const Bytes& src = e < donor.size() && ElementUsed(donor[e]) ? donor[e] : donor[0];
        if (!ElementUsed(src)) continue;
        const size_t a = 8 + size_t(t.offset) + e * kElement14;
        if (src.size() != kElement14 || a + kElement14 > evp->size())
          throw EvpError{"track " + std::to_string(t.k) + " step " + std::to_string(e) + " is truncated"};
        std::copy(src.begin() + kElementHead, src.end(), evp->begin() + static_cast<std::ptrdiff_t>(a + kElementHead));
        local.emplace_back(t.k, uint32_t(e));
      }
    }
  } catch (const EvpError& ex) {
    return Fail(error, "Filling the opponent/referee positions failed: " + ex.message + ".");
  }
  if (filled) *filled = std::move(local);
  return true;
}

bool RetargetWWE15Entrance(WWE15Bytes* evp, uint32_t target_id, uint32_t source_id,
                           uint32_t entrance_key, uint32_t original14_id,
                           std::vector<std::string>* notes, std::string* error) {
  if (error) error->clear();
  if (!evp) return Fail(error, "The entrance to retarget is missing.");
  if (target_id > 0xffff) return Fail(error, "Entrance target " + std::to_string(target_id) + " does not fit 16 bits.");
  try {
    if (evp->size() < 0x44) throw EvpError{"the header is shorter than 0x44 bytes"};
    (*evp)[32] = uint8_t(target_id >> 8);
    (*evp)[33] = uint8_t(target_id);
    Be32At(*evp, 0x40, "the track count");
    const uint32_t v0 = Be32At(*evp, 0x44 + 12, "track 0's attire code");
    const uint32_t person = v0 / 100;
    if (person == source_id || person == entrance_key || (original14_id && person == original14_id))
      PutBe32(evp, 0x44 + 12, target_id * 100 + v0 % 100, "track 0's attire code");
    else if (notes)
      notes->push_back("track 0 holds " + std::to_string(v0) + " (not his attire code), left");
  } catch (const EvpError& ex) {
    return Fail(error, "Entrance for wrestler " + std::to_string(target_id) + " could not be retargeted: " +
                           ex.message + ".");
  }
  return true;
}

bool ListWWE15EntranceProps(const WWE15Bytes& evp, std::vector<WWE15EntranceProp>* props,
                            std::string* error) {
  if (error) error->clear();
  if (!props) return Fail(error, "The prop list output is missing.");
  props->clear();
  try {
    const uint32_t n = Be32At(evp, 0x40, "the track count");
    for (uint32_t i = 0; i < n; ++i) {
      const size_t o = 0x44 + 32 * size_t(i) + 12;
      const uint32_t v = Be32At(evp, o, "track " + std::to_string(i));
      const uint32_t flags = Be32At(evp, o + 4, "track " + std::to_string(i));
      if (flags == 0x100) props->push_back({i, v});
    }
  } catch (const EvpError& ex) {
    return Fail(error, "The entrance's prop list could not be read: " + ex.message + ".");
  }
  return true;
}

bool SetWWE15EntranceProp(WWE15Bytes* evp, uint32_t track, uint32_t object_id, std::string* error) {
  if (error) error->clear();
  try {
    PutBe32(evp, 0x44 + 32 * size_t(track) + 12, object_id, "track " + std::to_string(track));
  } catch (const EvpError& ex) {
    return Fail(error, "The entrance prop could not be set: " + ex.message + ".");
  }
  return true;
}

namespace {
void PutLe16At(Bytes* b, size_t at, uint32_t v) {
  (*b)[at] = uint8_t(v);
  (*b)[at + 1] = uint8_t(v >> 8);
}
}  // namespace

bool ConvertWWE15Rows(const WWE15RowsPort& port, const WWE15Bytes& source15_dat,
                      const WWE15Bytes& donor14_dat, const WWE15Bytes& donor14_pro,
                      WWE15Rows* output, std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The row output is missing.");
  const uint32_t t = port.target_id;
  const std::string who = "WWE 2K15 wrestler " + std::to_string(port.source_id) + " (slot " + std::to_string(t) + ")";
  if (t < 100 || t > 499) return Fail(error, who + ": the target slot must be 100-499.");
  // Sizes: 2K14 DAT rows end with a u16 slot 4 bytes before the end; the copied fields reach
  // byte 128. PRO rows are 1276 bytes in both games' stock tables.
  if (donor14_dat.size() < 132)
    return Fail(error, who + ": donor " + std::to_string(port.donor_id) + "'s WWE 2K14 DAT row is " +
                           std::to_string(donor14_dat.size()) + " bytes; at least 132 expected.");
  if (source15_dat.size() < 50)
    return Fail(error, who + ": the WWE 2K15 DAT row is " + std::to_string(source15_dat.size()) +
                           " bytes; at least 50 expected.");
  if (donor14_pro.size() != 1276)
    return Fail(error, who + ": donor " + std::to_string(port.donor_id) + "'s WWE 2K14 PRO row is " +
                           std::to_string(donor14_pro.size()) + " bytes; 1276 expected.");
  if (port.slots.size() != 262 || port.extras.size() != 22)
    return Fail(error, who + ": the move plan has " + std::to_string(port.slots.size()) + " slots and " +
                           std::to_string(port.extras.size()) + " extras; 262 and 22 expected.");
  Bytes r = donor14_dat;
  PutLe16At(&r, 4, t);
  PutLe16At(&r, 6, port.dat_index);
  PutLe16At(&r, 8, t);
  PutLe16At(&r, 10, (t + 0xEA60) & 0xffff);
  PutLe16At(&r, 12, (t + 0xF230) & 0xffff);
  PutLe16At(&r, 14, (t + 0xFA00) & 0xffff);
  PutLe32(&r, 16, 79000 + t);
  r[20] = 1;
  r[21] = 2;
  r[22] = source15_dat[22];
  r[23] = source15_dat[23];
  r[26] = source15_dat[26];
  std::copy(source15_dat.begin() + 36, source15_dat.begin() + 50, r.begin() + 36);
  Bytes dsbd(80, 0);
  for (uint32_t k = 1; k <= 4; ++k) {
    PutLe16At(&r, 120 + 2 * (k - 1), t * 100 + k);
    PutLe16At(&dsbd, 2 * (k - 1), t * 100 + k);
  }
  PutLe16At(&r, r.size() - 4, t);
  Bytes pro = donor14_pro;
  for (size_t o : {576, 578, 586, 592, 594, 602, 580, 582, 596, 598}) PutLe16At(&pro, o, t);
  if (port.donor_music)
    for (size_t o : {576, 592}) PutLe16At(&pro, o, port.donor_id);
  if (port.alternate && port.donor_music)  // an alternate with his own 2K15 theme keeps it (owner 11-16)
    for (size_t o : {576, 592}) PutLe16At(&pro, o, port.original14_id);
  for (size_t k = 0; k < 262; ++k) PutLe16At(&pro, 4 + 2 * k, port.slots[k]);
  for (size_t k = 0; k < 22; ++k) PutLe16At(&pro, 528 + 2 * k, port.extras[k]);
  output->dat = std::move(r);
  output->dsbd = std::move(dsbd);
  output->pro = std::move(pro);
  return true;
}

}  // namespace wwe2k14::launcher
