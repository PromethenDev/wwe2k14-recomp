#include "wwe15_moves.h"

#include <cstdlib>

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>
#include <sstream>
#include <tuple>

namespace wwe2k14::launcher {
namespace {

using Bytes = WWE15Bytes;

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}
uint16_t Le16(const Bytes& b, size_t at) { return uint16_t(b[at] | (b[at + 1] << 8)); }
uint32_t Le32(const Bytes& b, size_t at) {
  return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
}
void PutLe16(Bytes* b, size_t at, uint32_t v) { (*b)[at] = uint8_t(v); (*b)[at + 1] = uint8_t(v >> 8); }
void PutLe32(Bytes* b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) (*b)[at + i] = uint8_t(v >> (8 * i));
}
uint16_t RecordId(const Bytes& record) { return Le16(record, 152); }

bool SplitWaze(const Bytes& waze, const std::string& which, std::vector<Bytes>* records, size_t* end,
               std::string* error) {
  if (waze.size() < kWWE15WazeHeader)
    return Fail(error, which + " move list (WAZE) is " + std::to_string(waze.size()) + " bytes; at least " +
                           std::to_string(kWWE15WazeHeader) + " expected.");
  const uint32_t n = Le32(waze, 4);
  if (uint64_t(n) * kWWE15WazeRecord > waze.size() - kWWE15WazeHeader)
    return Fail(error, which + " move list (WAZE) says " + std::to_string(n) + " records (" +
                           std::to_string(kWWE15WazeHeader + uint64_t(n) * kWWE15WazeRecord) +
                           " bytes) but is " + std::to_string(waze.size()) + " bytes.");
  records->clear();
  for (uint32_t i = 0; i < n; ++i) {
    const size_t at = kWWE15WazeHeader + kWWE15WazeRecord * i;
    records->emplace_back(waze.begin() + static_cast<std::ptrdiff_t>(at),
                          waze.begin() + static_cast<std::ptrdiff_t>(at + kWWE15WazeRecord));
  }
  *end = kWWE15WazeHeader + kWWE15WazeRecord * size_t(n);
  return true;
}

}  // namespace

bool ReadWWE15MoveOwner(const WWE15Bytes& waze15, uint16_t id, uint16_t* owner) {
  std::vector<Bytes> records;
  size_t end = 0;
  std::string ignored;
  if (!SplitWaze(waze15, "WWE 2K15", &records, &end, &ignored)) return false;
  // python {rid: record} keeps the LAST record with an ID
  for (auto it = records.rbegin(); it != records.rend(); ++it)
    if (RecordId(*it) == id) { *owner = Le16(*it, 128); return true; }
  return false;
}

bool AddWWE15MoveRecords(const WWE15Bytes& waze14, const WWE15Bytes& waze15,
                         const std::vector<uint16_t>& ids, WWE15Bytes* output,
                         std::vector<std::string>* added, std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The move list output is missing.");
  std::vector<Bytes> records, all15;
  size_t end14 = 0, end15 = 0;
  if (!SplitWaze(waze14, "The WWE 2K14", &records, &end14, error) ||
      !SplitWaze(waze15, "The WWE 2K15", &all15, &end15, error))
    return false;
  std::vector<Bytes> named, empty;
  for (const Bytes& r : records) (r[24] ? named : empty).push_back(r);
  if (!std::equal(records.begin(), records.begin() + static_cast<std::ptrdiff_t>(named.size()), named.begin()))
    return Fail(error, "The WWE 2K14 move list (WAZE) does not list its named moves first.");
  std::vector<uint16_t> order15;
  std::map<uint16_t, const Bytes*> by15;
  for (const Bytes& r : all15) { order15.push_back(RecordId(r)); by15[RecordId(r)] = &r; }
  Bytes header(waze14.begin(), waze14.begin() + kWWE15WazeHeader);
  std::set<uint16_t> have;
  for (const Bytes& r : named) have.insert(RecordId(r));
  std::vector<uint16_t> sorted_ids = ids;
  std::sort(sorted_ids.begin(), sorted_ids.end());
  sorted_ids.erase(std::unique(sorted_ids.begin(), sorted_ids.end()), sorted_ids.end());
  std::vector<std::string> notes;
  for (uint16_t id : sorted_ids) {
    if (have.count(id)) { notes.push_back(std::to_string(id) + " already"); continue; }
    const auto source = by15.find(id);
    if (source == by15.end())
      return Fail(error, "Move " + std::to_string(id) + " has no record in the WWE 2K15 move list (WAZE, " +
                             std::to_string(all15.size()) + " records).");
    Bytes r = *source->second;
    std::fill(r.begin() + 154, r.begin() + 160, 0xff);
    const auto first = std::find(order15.begin(), order15.end(), id);
    size_t pos = named.size();
    for (auto k = first; k != order15.begin();) {
      --k;
      if (have.count(*k)) {
        const auto at = std::find_if(named.begin(), named.end(), [&](const Bytes& q) { return RecordId(q) == *k; });
        pos = static_cast<size_t>(at - named.begin()) + 1;
        break;
      }
    }
    named.insert(named.begin() + static_cast<std::ptrdiff_t>(pos), r);
    have.insert(id);
    notes.push_back(std::to_string(id) + " at " + std::to_string(pos));
    for (int b = 0; b < 163; ++b) {
      if (b >= 120 && b <= 123) continue;
      if (r[size_t(b / 64) * 8 + size_t(b % 64) / 8] >> (b % 8) & 1) {
        const uint32_t count = Le16(header, 8 + 2 * size_t(b)) + 1u;
        if (count > 0xffff)
          return Fail(error, "Move category " + std::to_string(b) + " already lists 65535 moves.");
        PutLe16(&header, 8 + 2 * size_t(b), count);
      }
    }
  }
  PutLe32(&header, 4, static_cast<uint32_t>(named.size() + empty.size()));
  Bytes out = header;
  for (const Bytes& r : named) out.insert(out.end(), r.begin(), r.end());
  for (const Bytes& r : empty) out.insert(out.end(), r.begin(), r.end());
  out.insert(out.end(), waze14.begin() + static_cast<std::ptrdiff_t>(end14), waze14.end());
  out.resize((out.size() + 0xff) & ~size_t(0xff), 0);
  *output = std::move(out);
  if (added) *added = std::move(notes);
  return true;
}

bool FixWWE15MoveOwners(WWE15Bytes* waze, const std::map<uint16_t, uint16_t>& owners,
                        std::vector<std::string>* changes, std::string* error) {
  if (error) error->clear();
  if (!waze) return Fail(error, "The move list to fix is missing.");
  std::vector<Bytes> records;
  size_t end = 0;
  if (!SplitWaze(*waze, "The WWE 2K14", &records, &end, error)) return false;
  std::vector<std::string> local;
  for (size_t i = 0; i < records.size(); ++i) {
    const size_t o = kWWE15WazeHeader + kWWE15WazeRecord * i;
    const auto hit = owners.find(Le16(*waze, o + 152));
    if (hit == owners.end()) continue;
    const uint16_t old = Le16(*waze, o + 128);
    if (old != hit->second) {
      PutLe16(waze, o + 128, hit->second);
      local.push_back(std::to_string(hit->first) + " " + std::to_string(old) + "->" + std::to_string(hit->second));
    }
    if (hit->second == 0x0200 && ((*waze)[o + 149] & 0x08)) {
      (*waze)[o + 149] &= 0xF7;
      local.push_back(std::to_string(hit->first) + " b149 " + std::to_string((*waze)[o + 149]));
    }
  }
  if (changes) *changes = std::move(local);
  return true;
}

namespace {

std::string TrimName(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  return s;
}
uint32_t Be32(const Bytes& b, size_t at) {
  return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | b[at + 3];
}
uint16_t Be16(const Bytes& b, size_t at) { return uint16_t((b[at] << 8) | b[at + 1]); }

bool Unz(const Bytes& raw, Bytes* out, std::string* error) {
  if (raw.size() >= 4 && std::memcmp(raw.data(), "ZLIB", 4) == 0) return DecodeWWE13ZlibLeaf(raw, out, error);
  return DecodeWWE13Bpe(raw, out, error);
}
// moves15.pach: entries 4-byte aligned from the end of the table, no tail padding.
Bytes Pach4(const std::vector<std::pair<uint32_t, Bytes>>& items) {
  Bytes head(8 + items.size() * 12, 0), data;
  std::memcpy(head.data(), "PACH", 4);
  PutLe32(&head, 4, static_cast<uint32_t>(items.size()));
  for (size_t i = 0; i < items.size(); ++i) {
    while (data.size() % 4) data.push_back(0);
    PutLe32(&head, 8 + 12 * i, items[i].first);
    PutLe32(&head, 12 + 12 * i, static_cast<uint32_t>(data.size()));
    PutLe32(&head, 16 + 12 * i, static_cast<uint32_t>(items[i].second.size()));
    data.insert(data.end(), items[i].second.begin(), items[i].second.end());
  }
  head.insert(head.end(), data.begin(), data.end());
  return head;
}
bool Leaves(const Bytes& pach, std::vector<std::pair<uint32_t, Bytes>>* out, const std::string& what,
            std::string* error) {
  std::vector<WWE13PachLeafData> leaves;
  std::string detail;
  if (!ReadWWE13PachLeaves(pach, &leaves, &detail)) return Fail(error, what + " is not a valid table: " + detail);
  out->clear();
  for (auto& l : leaves) out->emplace_back(l.id, std::move(l.data));
  return true;
}
const Bytes* Find(const std::vector<std::pair<uint32_t, Bytes>>& v, uint32_t id) {
  for (const auto& [k, d] : v)
    if (k == id) return &d;  // python dict(...)[k]: last wins; tables never repeat an ID
  return nullptr;
}
constexpr size_t RowSize(uint32_t leaf) { return leaf == 0 ? 40 : leaf == 1 ? 28 : leaf == 4 ? 12 : leaf == 6 ? 32 : 0; }

}  // namespace

bool ReadWWE15PrmTables(const WWE15Bytes& wwe15_m3_pac, std::map<uint32_t, WWE15Bytes>* tables, std::string* error) {
  if (error) error->clear();
  std::string detail;
  // --- WWE 2K15 m3.pac: group entry 'PRM' -> PACH -> leaf 0 (packed) -> PACH of packed leaves
  std::vector<WWE13PacGroupData> g15;
  if (!ReadWWE13PacGroups(wwe15_m3_pac, &g15, &detail)) return Fail(error, "WWE 2K15 m3.pac could not be read: " + detail);
  const Bytes* prm15 = nullptr;
  for (const auto& g : g15)
    for (const auto& e : g.entries)
      if (!prm15 && TrimName(e.name) == "PRM") prm15 = &e.data;
  if (!prm15) return Fail(error, "WWE 2K15 m3.pac has no PRM move table.");
  std::vector<std::pair<uint32_t, Bytes>> outer15, inner15;
  if (!Leaves(*prm15, &outer15, "WWE 2K15 m3.pac PRM", error)) return false;
  const Bytes* first15 = Find(outer15, 0);
  Bytes inner15_plain;
  if (!first15 || !Unz(*first15, &inner15_plain, &detail))
    return Fail(error, "WWE 2K15 m3.pac PRM part 0 is missing or could not be unpacked. " + detail);
  if (!Leaves(inner15_plain, &inner15, "WWE 2K15 m3.pac PRM part 0", error)) return false;
  std::map<uint32_t, Bytes>& t15 = *tables;
  t15.clear();
  for (const auto& [k, v] : inner15) {
    Bytes plain;
    if (!Unz(v, &plain, &detail)) return Fail(error, "WWE 2K15 PRM leaf " + std::to_string(k) + " could not be unpacked: " + detail);
    t15[k] = std::move(plain);
  }
  for (uint32_t k : {0u, 1u, 4u, 5u, 6u, 10u, 11u, 14u, 15u, 16u})
    if (!t15.count(k)) return Fail(error, "WWE 2K15 PRM leaf " + std::to_string(k) + " is missing.");

  return true;
}

bool ExtendWWE15Prm6(const WWE15Bytes& m3_patch_pac, const WWE15Bytes& wwe15_m3_pac,
                     const std::vector<uint16_t>& ids, WWE15Bytes* output, WWE15PrmReport* report,
                     std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The m3_patch.pac output is missing.");
  std::string detail;
  std::map<uint32_t, Bytes> t15;
  if (!ReadWWE15PrmTables(wwe15_m3_pac, &t15, error)) return false;

  // --- WWE 2K14 m3_patch.pac MDAT/PRM6
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(m3_patch_pac, &groups, &detail)) return Fail(error, "m3_patch.pac could not be read: " + detail);
  WWE13PacEntryData* prm6 = nullptr;
  if (!groups.empty() && TrimName(groups[0].name) == "MDAT" && !groups[0].entries.empty() &&
      TrimName(groups[0].entries[0].name) == "PRM6")
    prm6 = &groups[0].entries[0];
  if (!prm6) return Fail(error, "m3_patch.pac does not start with the MDAT/PRM6 move table.");
  std::vector<std::pair<uint32_t, Bytes>> outer, inner;
  if (!Leaves(prm6->data, &outer, "m3_patch.pac PRM6", error)) return false;
  const Bytes* first = Find(outer, 0);
  if (!first) return Fail(error, "m3_patch.pac PRM6 part 0 is missing.");
  if (!Leaves(*first, &inner, "m3_patch.pac PRM6 part 0", error)) return false;
  std::map<uint32_t, Bytes> lv;
  for (const auto& [k, v] : inner) {
    Bytes plain;
    if (!Unz(v, &plain, &detail)) return Fail(error, "PRM6 leaf " + std::to_string(k) + " could not be unpacked: " + detail);
    lv[k] = std::move(plain);
  }
  for (uint32_t k : {0u, 1u, 4u, 5u, 6u, 10u, 11u, 14u, 15u, 16u})
    if (!lv.count(k)) return Fail(error, "PRM6 leaf " + std::to_string(k) + " is missing.");

  // rows of a move in a table (in table order)
  auto rows_of = [](const Bytes& t, size_t size, const std::set<uint16_t>& want) {
    std::vector<size_t> at;
    for (size_t o = 0; o + size <= t.size(); o += size)
      if (want.count(Be16(t, o))) at.push_back(o);
    return at;
  };
  // already: ids whose PRM6 leaf-0 rows exist; they must equal 2K15's rows (sorted)
  WWE15PrmReport rep;
  std::set<uint16_t> in6;
  for (size_t o = 0; o + 40 <= lv[0].size(); o += 40) in6.insert(Be16(lv[0], o));
  std::vector<uint16_t> uniq = ids;
  std::sort(uniq.begin(), uniq.end());
  uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
  std::set<uint16_t> todo;
  for (uint16_t id : uniq) {
    if (!in6.count(id)) { todo.insert(id); continue; }
    std::vector<Bytes> a, b;
    for (size_t o : rows_of(lv[0], 40, {id})) a.emplace_back(lv[0].begin() + o, lv[0].begin() + o + 40);
    for (size_t o : rows_of(t15[0], 40, {id})) b.emplace_back(t15[0].begin() + o, t15[0].begin() + o + 40);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b)
      return Fail(error, "Move " + std::to_string(id) + " already has " + std::to_string(a.size()) +
                             " PRM6 rows in WWE 2K14 that differ from its " + std::to_string(b.size()) +
                             " WWE 2K15 rows; it cannot be added safely.");
    rep.already.push_back(id);
  }

  std::map<uint32_t, Bytes> nw;
  const size_t base5 = lv[5].size() / 4;
  Bytes add5, add15;
  for (uint32_t leaf : {0u, 1u, 4u, 6u}) {
    const size_t sz = RowSize(leaf);
    const Bytes& old = lv[leaf];
    const Bytes& oldle = lv[leaf + 10];
    if (old.size() % sz != 0 || oldle.size() != old.size())
      return Fail(error, "PRM6 leaf " + std::to_string(leaf) + " is " + std::to_string(old.size()) +
                             " bytes (mirror " + std::to_string(oldle.size()) + "); a multiple of " +
                             std::to_string(sz) + " with an equal mirror expected.");
    std::map<uint32_t, std::pair<Bytes, Bytes>> rows;  // key (BE u32 of the first 4 bytes) -> (be, le); last wins
    for (size_t o = 0; o < old.size(); o += sz)
      rows[Be32(old, o)] = {Bytes(old.begin() + o, old.begin() + o + sz), Bytes(oldle.begin() + o, oldle.begin() + o + sz)};
    const Bytes& x = t15[leaf];
    const Bytes& y = t15[leaf + 10];
    size_t got = 0;
    for (size_t o = 0; o + sz <= x.size(); o += sz) {
      if (!todo.count(Be16(x, o))) continue;
      if (o + sz > y.size()) return Fail(error, "WWE 2K15 PRM mirror leaf " + std::to_string(leaf + 10) + " is shorter than leaf " + std::to_string(leaf) + ".");
      Bytes be(x.begin() + o, x.begin() + o + sz), le(y.begin() + o, y.begin() + o + sz);
      if (leaf == 4) {
        const uint32_t off = Be32(be, 4);
        const uint16_t cnt = Be16(be, 8);
        if (Le32(le, 4) != off || Le16(le, 8) != cnt)
          return Fail(error, "Move " + std::to_string(Be16(be, 0)) + ": WWE 2K15 PRM leaf 4 and its mirror disagree.");
        if ((uint64_t(off) + cnt) * 4 > t15[5].size() || (uint64_t(off) + cnt) * 4 > t15[15].size())
          return Fail(error, "Move " + std::to_string(Be16(be, 0)) + ": WWE 2K15 PRM leaf 4 points past leaf 5.");
        const uint32_t now = static_cast<uint32_t>(base5 + add5.size() / 4);
        add5.insert(add5.end(), t15[5].begin() + off * 4, t15[5].begin() + (off + cnt) * 4);
        add15.insert(add15.end(), t15[15].begin() + off * 4, t15[15].begin() + (off + cnt) * 4);
        be[4] = uint8_t(now >> 24); be[5] = uint8_t(now >> 16); be[6] = uint8_t(now >> 8); be[7] = uint8_t(now);
        PutLe32(&le, 4, now);
      }
      if (rows.count(Be32(be, 0)))
        return Fail(error, "Move " + std::to_string(Be16(be, 0)) + ": a PRM6 leaf " + std::to_string(leaf) +
                               " row with the same key is already there.");
      rows[Be32(be, 0)] = {be, le};
      ++got;
    }
    rep.rows_added[leaf] = got;
    Bytes a, b;
    for (const auto& [key, r] : rows) { a.insert(a.end(), r.first.begin(), r.first.end()); b.insert(b.end(), r.second.begin(), r.second.end()); }
    nw[leaf] = std::move(a);
    nw[leaf + 10] = std::move(b);
  }
  nw[5] = lv[5]; nw[5].insert(nw[5].end(), add5.begin(), add5.end());
  nw[15] = lv[15]; nw[15].insert(nw[15].end(), add15.begin(), add15.end());
  rep.rows_added[5] = add5.size() / 4;

  std::vector<std::pair<uint32_t, Bytes>> items;
  for (const auto& [k, v] : inner) items.emplace_back(k, nw.count(k) ? StoreWWE13BpeLiteral(nw[k]) : v);
  std::vector<std::pair<uint32_t, Bytes>> outer_items{{0, Pach4(items)}};
  for (const auto& [k, v] : outer)
    if (k != 0) outer_items.emplace_back(k, v);
  prm6->data = Pach4(outer_items);
  if (!RebuildWWE13PacGroups(m3_patch_pac, groups, output, &detail))
    return Fail(error, "m3_patch.pac could not be rebuilt: " + detail);
  if (report) *report = std::move(rep);
  return true;
}

bool AddWWE15D004Motc(const WWE15Bytes& m, const std::vector<uint16_t>& ids_in, WWE15Bytes* output,
                      size_t* updated_out, std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The MOTC output is missing.");
  auto need = [&](size_t at, size_t n) {
    if (at > m.size() || m.size() - at < n)
      throw std::string("the moveset motion table (MOTC, " + std::to_string(m.size()) + " bytes) ends at byte " +
                        std::to_string(at) + " where " + std::to_string(n) + " more bytes were expected");
  };
  std::vector<uint16_t> ids = ids_in;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  auto blob = [](const std::vector<uint16_t>& v) {
    Bytes b(4, 0);
    PutLe32(&b, 0, static_cast<uint32_t>(v.size()));
    for (uint16_t x : v) { b.push_back(uint8_t(x)); b.push_back(uint8_t(x >> 8)); }
    if (v.size() & 1) { b.push_back(0); b.push_back(0); }
    return b;
  };
  const Bytes route = {'D', '0', '0', '4'};
  constexpr size_t kSlot = 21;
  size_t updated = 0;
  try {
    size_t p = 248;
    for (int k = 0; k < 2; ++k) {
      need(p, 4);
      const uint32_t n = Le32(m, p);
      p += (size_t(n) + (n & 1) + 2) * 2;
    }
    need(p, 4);
    const uint32_t tables = Le32(m, p);
    p += 4;
    Bytes out(m.begin(), m.begin() + static_cast<std::ptrdiff_t>(p));
    for (uint32_t t = 0; t < tables; ++t) {
      need(p, 4);
      const uint32_t nsets = Le32(m, p);
      out.insert(out.end(), m.begin() + p, m.begin() + p + 4);
      p += 4;
      for (uint32_t set = 0; set < nsets; ++set) {
        need(p, 248);
        Bytes head(m.begin() + p, m.begin() + p + 248);
        p += 248;
        std::vector<Bytes> names;
        for (size_t i = 0; i < 31; ++i) names.emplace_back(head.begin() + 4 * i, head.begin() + 4 * i + 4);
        bool eligible = false;
        std::vector<size_t> route_slots;
        for (size_t i = 0; i < 31; ++i) {
          const Bytes& nm = names[i];
          if (nm == Bytes{'D', '0', '0', '2'} || nm == Bytes{'D', '0', '0', '3'} || nm == route ||
              nm == Bytes{'D', '0', '0', '5'})
            eligible = true;
          if (nm == route) route_slots.push_back(i);
        }
        if (route_slots.size() > 1 || (!route_slots.empty() && route_slots[0] != kSlot))
          throw std::string("table " + std::to_string(t) + " set " + std::to_string(set) +
                            " lists D004 outside its slot 21");
        const bool inserted = eligible && route_slots.empty();
        if (inserted) {
          if (names[kSlot] != Bytes(4, 0))
            throw std::string("table " + std::to_string(t) + " set " + std::to_string(set) + " has slot 21 in use");
          names[kSlot] = route;
          std::copy(route.begin(), route.end(), head.begin() + 4 * kSlot);
        }
        Bytes blocks;
        for (size_t i = 0; i < 31; ++i) {
          if (names[i] == Bytes(4, 0)) continue;
          if (inserted && i == kSlot) {
            const Bytes b = blob(ids);
            blocks.insert(blocks.end(), b.begin(), b.end());
            ++updated;
            continue;
          }
          need(p, 4);
          const uint32_t count = Le32(m, p);
          const size_t size = 4 + 2 * (size_t(count) + (count & 1));
          need(p, size);
          Bytes raw(m.begin() + p, m.begin() + p + size);
          p += size;
          if (eligible && names[i] == route) {
            std::vector<uint16_t> prior;
            for (uint32_t c = 0; c < count; ++c) prior.push_back(Le16(raw, 4 + 2 * c));
            const std::set<uint16_t> prior_set(prior.begin(), prior.end());
            if (prior_set.size() != prior.size())
              throw std::string("table " + std::to_string(t) + " set " + std::to_string(set) + " repeats a stock D004 ID");
            for (uint16_t id : ids)
              if (!prior_set.count(id)) prior.push_back(id);
            raw = blob(prior);
            ++updated;
          }
          blocks.insert(blocks.end(), raw.begin(), raw.end());
        }
        out.insert(out.end(), head.begin(), head.end());
        out.insert(out.end(), blocks.begin(), blocks.end());
      }
    }
    out.insert(out.end(), m.begin() + static_cast<std::ptrdiff_t>(std::min(p, m.size())), m.end());
    *output = std::move(out);
  } catch (const std::string& message) {
    return Fail(error, "The moveset motion table could not be updated: " + message + ".");
  }
  if (updated_out) *updated_out = updated;
  return true;
}

// ------------------------------------------------------------------ motion archives
namespace {

bool IsPackedMagic(const Bytes& b) {
  return b.size() >= 4 && (std::memcmp(b.data(), "BPE ", 4) == 0 || std::memcmp(b.data(), "ZLIB", 4) == 0);
}
bool IsPach(const Bytes& b) { return b.size() >= 4 && std::memcmp(b.data(), "PACH", 4) == 0; }
// _unz: unpack only BPE/ZLIB, otherwise as is.
bool MaybeUnz(const Bytes& b, Bytes* out, std::string* error) {
  if (!IsPackedMagic(b)) { *out = b; return true; }
  return Unz(b, out, error);
}
}  // namespace

bool IsWWE15PlaceholderPac(const WWE15Bytes& raw) {
  return raw.size() >= 0x830 && (std::memcmp(raw.data(), "EPK8", 4) == 0 || std::memcmp(raw.data(), "EPAC", 4) == 0) &&
         std::memcmp(raw.data() + 0x800, "DMMY    ", 8) == 0 &&
         std::all_of(raw.begin() + 0x810, raw.begin() + 0x830, [](uint8_t x) { return x == 0; });
}

namespace {
// moves index rule: unpack BPE/ZLIB (an unreadable node is a leaf), recurse into readable PACH.
void WalkMotionIds(const Bytes& node, std::vector<uint32_t>* path, std::set<uint16_t>* ids) {
  Bytes unpacked;
  std::string ignored;
  const Bytes* d = &node;
  if (IsPackedMagic(node)) {
    if (!Unz(node, &unpacked, &ignored)) { if (path->size() == 3) ids->insert(uint16_t((*path)[2])); return; }
    d = &unpacked;
  }
  if (IsPach(*d)) {
    std::vector<WWE13PachLeafData> leaves;
    if (ReadWWE13PachLeaves(*d, &leaves, &ignored)) {
      for (const auto& l : leaves) {
        path->push_back(l.id);
        WalkMotionIds(l.data, path, ids);
        path->pop_back();
      }
      return;
    }
  }
  if (path->size() == 3) ids->insert(uint16_t((*path)[2]));
}
}  // namespace

bool ScanWWE15MotionIds(const WWE15Bytes& pac, std::set<uint16_t>* ids, std::string* error) {
  if (error) error->clear();
  Bytes plain;
  std::string detail;
  if (!DecodeWWE13PacPlain(pac, &plain, &detail)) return Fail(error, "A motion archive could not be read: " + detail);
  if (IsWWE15PlaceholderPac(plain)) return true;
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(plain, &groups, &detail)) return Fail(error, "A motion archive could not be read: " + detail);
  for (const auto& g : groups)
    for (const auto& e : g.entries) {
      std::vector<uint32_t> path;
      WalkMotionIds(e.data, &path, ids);
    }
  return true;
}

bool ScanWWE15MotionRoutes(const WWE15Bytes& pac, const std::set<uint16_t>& ids,
                           std::vector<WWE15MotionHit>* hits, std::string* error) {
  if (error) error->clear();
  std::vector<WWE13PacGroupData> groups;
  std::string detail;
  if (!ReadWWE13PacGroups(pac, &groups, &detail)) return Fail(error, "A motion archive could not be read: " + detail);
  for (const auto& g : groups) {
    const std::string group = TrimName(g.name);
    if (group != "ZMOT" && group != "ZMVM") continue;
    for (const auto& e : g.entries) {
      Bytes top;
      if (!MaybeUnz(e.data, &top, &detail) || !IsPach(top)) continue;
      std::vector<WWE13PachLeafData> banks;
      if (!ReadWWE13PachLeaves(top, &banks, &detail)) continue;
      for (const auto& bank : banks) {
        Bytes body;
        if (!MaybeUnz(bank.data, &body, &detail) || !IsPach(body)) continue;
        std::vector<WWE13PachLeafData> parts;
        if (!ReadWWE13PachLeaves(body, &parts, &detail)) continue;
        for (const auto& part : parts) {
          Bytes leaf;
          if (!MaybeUnz(part.data, &leaf, &detail) || !IsPach(leaf)) continue;
          std::vector<WWE13PachLeafData> recs;
          if (!ReadWWE13PachLeaves(leaf, &recs, &detail)) continue;
          for (const auto& r : recs)
            if (ids.count(uint16_t(r.id)) && r.id <= 0xffff)
              hits->push_back({group, TrimName(e.name), bank.id, part.id, uint16_t(r.id)});
        }
      }
    }
  }
  return true;
}

bool CollectWWE15MotionRecords(const WWE15Bytes& pac, const std::set<uint16_t>& ids,
                               std::map<uint16_t, WWE15MotionRecord>* records, std::string* error) {
  if (error) error->clear();
  std::vector<WWE13PacGroupData> groups;
  std::string detail;
  if (!ReadWWE13PacGroups(pac, &groups, &detail)) return Fail(error, "A WWE 2K15 motion archive could not be read: " + detail);
  for (const auto& g : groups)
    for (const auto& e : g.entries) {
      Bytes top;
      if (!MaybeUnz(e.data, &top, &detail) || !IsPach(top)) continue;
      std::vector<WWE13PachLeafData> banks;
      if (!ReadWWE13PachLeaves(top, &banks, &detail)) return Fail(error, "A WWE 2K15 motion bank list is damaged: " + detail);
      for (const auto& bank : banks) {
        Bytes body;
        if (!MaybeUnz(bank.data, &body, &detail) || !IsPach(body)) continue;
        std::vector<WWE13PachLeafData> parts;
        if (!ReadWWE13PachLeaves(body, &parts, &detail)) return Fail(error, "A WWE 2K15 motion part list is damaged: " + detail);
        for (const auto& part : parts) {
          Bytes leaf;
          if (!MaybeUnz(part.data, &leaf, &detail) || !IsPach(leaf)) continue;
          std::vector<WWE13PachLeafData> recs;
          if (!ReadWWE13PachLeaves(leaf, &recs, &detail)) return Fail(error, "A WWE 2K15 motion record list is damaged: " + detail);
          for (auto& r : recs) {
            if (r.id > 0xffff || !ids.count(uint16_t(r.id))) continue;
            WWE15MotionRecord& m = (*records)[uint16_t(r.id)];
            if (part.id % 2 == 0) { if (!m.has_even) { m.even = r.data; m.has_even = true; } }
            else if (!m.has_odd) { m.odd = r.data; m.has_odd = true; }
          }
        }
      }
    }
  return true;
}

namespace {
using RecordMap = std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Bytes>;
bool MotionRecordMap(const Bytes& blob, RecordMap* out, const std::string& what, std::string* error) {
  Bytes top;
  std::string d;
  if (!MaybeUnz(blob, &top, &d) || !IsPach(top)) return Fail(error, what + " is not a motion table. " + d);
  std::vector<WWE13PachLeafData> banks;
  if (!ReadWWE13PachLeaves(top, &banks, &d)) return Fail(error, what + ": " + d);
  for (const auto& bank : banks) {
    Bytes body;
    if (!MaybeUnz(bank.data, &body, &d) || !IsPach(body)) return Fail(error, what + " bank " + std::to_string(bank.id) + " is not a motion bank.");
    std::vector<WWE13PachLeafData> parts;
    if (!ReadWWE13PachLeaves(body, &parts, &d)) return Fail(error, what + ": " + d);
    for (const auto& part : parts) {
      Bytes leaf;
      if (!MaybeUnz(part.data, &leaf, &d) || !IsPach(leaf))
        return Fail(error, what + " bank " + std::to_string(bank.id) + " part " + std::to_string(part.id) + " is not a record list.");
      std::vector<WWE13PachLeafData> recs;
      if (!ReadWWE13PachLeaves(leaf, &recs, &d)) return Fail(error, what + ": " + d);
      for (auto& r : recs)
        if (!out->emplace(std::make_tuple(bank.id, part.id, r.id), r.data).second)
          return Fail(error, what + " repeats motion record " + std::to_string(r.id) + " in bank " + std::to_string(bank.id) + ".");
    }
  }
  return true;
}
}  // namespace

bool MergeWWE15D004Archive(const WWE15Bytes& pac, const std::vector<uint16_t>& ids_in,
                           const std::map<uint16_t, WWE15MotionRecord>& records, WWE15Bytes* output,
                           std::string* error) {
  if (error) error->clear();
  std::vector<uint16_t> ids = ids_in;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::vector<WWE13PacGroupData> groups;
  std::string d;
  if (!ReadWWE13PacGroups(pac, &groups, &d)) return Fail(error, "Add-on 4's dlc_mot04.pac could not be read: " + d);
  WWE13PacEntryData* zmot = nullptr;
  WWE13PacEntryData* zmvm = nullptr;
  for (auto& g : groups)
    for (auto& e : g.entries) {
      if (TrimName(g.name) == "ZMOT" && TrimName(e.name) == "DLC4" && !zmot) zmot = &e;
      if (TrimName(g.name) == "ZMVM" && TrimName(e.name) == "D004" && !zmvm) zmvm = &e;
    }
  if (!zmot || !zmvm) return Fail(error, "Add-on 4's dlc_mot04.pac has no ZMOT/DLC4 and ZMVM/D004 motion groups.");
  RecordMap a, b;
  if (!MotionRecordMap(zmot->data, &a, "dlc_mot04.pac ZMOT/DLC4", error) ||
      !MotionRecordMap(zmvm->data, &b, "dlc_mot04.pac ZMVM/D004", error))
    return false;
  if (a != b) return Fail(error, "Add-on 4's two motion groups (ZMOT/DLC4, ZMVM/D004) differ; this copy of Add-On 4 is not the known one.");
  for (uint16_t id : ids) {
    for (const auto& [key, v] : a)
      if (std::get<2>(key) == id)
        return Fail(error, "Move " + std::to_string(id) + " is already in Add-on 4's motion archive.");
    const auto r = records.find(id);
    if (r == records.end() || !r->second.has_even || !r->second.has_odd)
      return Fail(error, "Move " + std::to_string(id) + " lacks one of its two motion parts in the WWE 2K15 archives.");
  }
  Bytes top;
  MaybeUnz(zmot->data, &top, &d);
  std::vector<WWE13PachLeafData> bank_list;
  ReadWWE13PachLeaves(top, &bank_list, &d);
  std::map<uint32_t, Bytes> banks;
  for (auto& x : bank_list) banks[x.id] = x.data;  // dict(): last wins
  if (!banks.count(399)) return Fail(error, "Add-on 4's motion archive has no bank 399.");
  Bytes bank_body;
  if (!MaybeUnz(banks[399], &bank_body, &d)) return Fail(error, "Add-on 4 bank 399: " + d);
  std::vector<WWE13PachLeafData> part_list;
  if (!ReadWWE13PachLeaves(bank_body, &part_list, &d)) return Fail(error, "Add-on 4 bank 399: " + d);
  std::map<uint32_t, Bytes> parts;
  for (auto& x : part_list) parts[x.id] = x.data;
  if (!parts.count(0) || !parts.count(1)) return Fail(error, "Add-on 4 bank 399 lacks part 0 or 1.");
  for (uint16_t id : ids)
    for (uint32_t part : {0u, 1u}) {
      Bytes leaf;
      if (!MaybeUnz(parts[part], &leaf, &d)) return Fail(error, "Add-on 4 bank 399 part " + std::to_string(part) + ": " + d);
      std::vector<WWE13PachLeafData> rl;
      if (!ReadWWE13PachLeaves(leaf, &rl, &d)) return Fail(error, "Add-on 4 bank 399 part " + std::to_string(part) + ": " + d);
      std::map<uint32_t, Bytes> recs;
      for (auto& x : rl) recs[x.id] = x.data;
      const WWE15MotionRecord& src = records.at(id);
      recs[id] = part == 0 ? src.even : src.odd;
      std::vector<std::pair<uint32_t, Bytes>> items(recs.begin(), recs.end());
      parts[part] = Pach4(items);
    }
  std::vector<std::pair<uint32_t, Bytes>> pitems(parts.begin(), parts.end());
  banks[399] = Pach4(pitems);
  std::vector<std::pair<uint32_t, Bytes>> bitems(banks.begin(), banks.end());
  const Bytes merged = Pach4(bitems);
  RecordMap after;
  if (!MotionRecordMap(merged, &after, "the merged motion archive", error)) return false;
  for (const auto& [key, v] : a) {
    const auto hit = after.find(key);
    if (hit == after.end() || hit->second != v) return Fail(error, "Merging changed a stock Add-on 4 motion record.");
  }
  if (after.size() != a.size() + 2 * ids.size()) return Fail(error, "The merged motion archive has unexpected records.");
  zmot->data = merged;
  zmvm->data = merged;
  if (!RebuildWWE13PacGroups(pac, groups, output, &d)) return Fail(error, "dlc_mot04.pac could not be rebuilt: " + d);
  return true;
}

bool ScanWWE15MotionParts(const WWE15Bytes& pac, std::set<uint16_t>* even, std::set<uint16_t>* odd, std::string* error) {
  if (error) error->clear();
  std::vector<WWE13PacGroupData> groups;
  std::string d;
  if (!ReadWWE13PacGroups(pac, &groups, &d)) return Fail(error, "A WWE 2K15 motion archive could not be read: " + d);
  for (const auto& g : groups)
    for (const auto& e : g.entries) {
      Bytes top;
      if (!MaybeUnz(e.data, &top, &d) || !IsPach(top)) continue;
      std::vector<WWE13PachLeafData> banks;
      if (!ReadWWE13PachLeaves(top, &banks, &d)) continue;
      for (const auto& bank : banks) {
        Bytes body;
        if (!MaybeUnz(bank.data, &body, &d) || !IsPach(body)) continue;
        std::vector<WWE13PachLeafData> parts;
        if (!ReadWWE13PachLeaves(body, &parts, &d)) continue;
        for (const auto& part : parts) {
          Bytes leaf;
          if (!MaybeUnz(part.data, &leaf, &d) || !IsPach(leaf)) continue;
          std::vector<WWE13PachLeafData> recs;
          if (!ReadWWE13PachLeaves(leaf, &recs, &d)) continue;
          for (const auto& r : recs)
            if (r.id <= 0xffff) (part.id % 2 == 0 ? even : odd)->insert(uint16_t(r.id));
        }
      }
    }
  return true;
}

bool ReadWWE15MoveRules(const std::string& text, WWE15MoveRules* rules, std::string* error) {
  if (error) error->clear();
  *rules = {};
  std::istringstream in(text);
  std::string line;
  size_t number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    std::istringstream cols(line);
    std::string c;
    while (std::getline(cols, c, '\t')) f.push_back(c);
    auto bad = [&](const std::string& why) {
      return Fail(error, "data/wwe2k15-moves.tsv line " + std::to_string(number) + ": " + why + ".");
    };
    try {
      if (f.size() == 3 && f[0] == "unsupported") rules->unsupported[uint16_t(std::stoul(f[1]))] = f[2];
      else if (f.size() == 2 && f[0] == "stall-donor-212") rules->stall_donor_212.insert(std::stoul(f[1]));
      else if (f.size() == 2 && f[0] == "allow-loop") rules->allow_loop.insert(uint16_t(std::stoul(f[1])));
      else if (f.size() == 3 && f[0] == "stall-slot") {
        std::istringstream items(f[2]);
        std::string item;
        while (std::getline(items, item, ',')) {
          const size_t colon = item.find(':');
          if (colon == std::string::npos) return bad("expected SLOT:MOVE pairs");
          rules->stall_slot[std::stoul(f[1])][std::stoul(item.substr(0, colon))] = uint16_t(std::stoul(item.substr(colon + 1)));
        }
      } else return bad("unknown row '" + (f.empty() ? std::string() : f[0]) + "' or wrong column count");
    } catch (const std::exception&) {
      return bad("a number could not be read");
    }
  }
  return true;
}

namespace {

struct Planner {
  const WWE15MoveFacts& k;
  const WWE15MoveRules& rules;
  std::map<uint16_t, std::vector<size_t>> rows15;  // id -> offsets of its PRM leaf-0 rows
  std::map<uint16_t, std::pair<int, std::string>> cache;
  enum { kBase, kPort, kFallback, kNotMotion };

  Planner(const WWE15MoveFacts& facts, const WWE15MoveRules& r) : k(facts), rules(r) {
    for (size_t o = 0; o + 40 <= k.prm15_rows.size(); o += 40) rows15[Be16(k.prm15_rows, o)].push_back(o);
  }
  bool LockRisk(uint16_t id, std::string* loops_text) {
    std::map<uint8_t, std::set<uint8_t>> tracks;
    std::map<std::pair<uint8_t, uint8_t>, std::pair<uint16_t, uint8_t>> next;
    const auto it = rows15.find(id);
    if (it == rows15.end()) return false;
    for (size_t o : it->second) {
      const uint8_t ph = k.prm15_rows[o + 2], tr = k.prm15_rows[o + 3];
      tracks[ph].insert(tr);
      next[{ph, tr}] = {Be16(k.prm15_rows, o + 20), k.prm15_rows[o + 22]};
    }
    bool pair = false;
    std::vector<int> loops;
    for (const auto& [ph, t] : tracks) {
      if (t.count(1)) pair = true;
      if (t == std::set<uint8_t>{0}) {
        const auto n = next.at({ph, 0});
        if (n.first == id && n.second == ph) loops.push_back(ph);
      }
    }
    std::ostringstream text;
    text << "[";
    for (size_t i = 0; i < loops.size(); ++i) text << (i ? ", " : "") << loops[i];
    text << "]";
    *loops_text = text.str();
    return pair && !loops.empty();
  }
  std::pair<int, std::string> Classify(uint16_t v) {
    const auto hit = cache.find(v);
    if (hit != cache.end()) return hit->second;
    std::pair<int, std::string> r;
    if (v < 1500 || k.base14.count(v)) r = {kBase, ""};
    else if (k.all15.count(v)) {
      std::vector<std::string> bad;
      const auto u = rules.unsupported.find(v);
      if (u != rules.unsupported.end()) bad.push_back(u->second);
      if (!k.even15.count(v) || !k.odd15.count(v)) bad.push_back("a part missing in 2K15");
      std::string loops;
      if (!rules.allow_loop.count(v) && LockRisk(v, &loops)) bad.push_back("attacker-only phase loops on itself " + loops + " (lock like SA2)");
      if (!rows15.count(v)) bad.push_back("no PRM row in 2K15");
      if (!k.waze15.count(v)) bad.push_back("no WAZE record in 2K15");
      std::string why;
      for (size_t i = 0; i < bad.size(); ++i) why += (i ? "; " : "") + bad[i];
      r = bad.empty() ? std::make_pair(int(kPort), std::string(k.addon14.count(v) ? "addon" : ""))
                      : std::make_pair(int(kFallback), why);
    } else if (k.addon14.count(v)) r = {kFallback, "ADDON-ONLY, no 2K15 copy"};
    else r = {kNotMotion, ""};
    cache[v] = r;
    return r;
  }
};

}  // namespace

bool PlanWWE15Moves(uint32_t source_id, const WWE15Bytes& pro15, const WWE15Bytes& donor_pro14,
                    const WWE15MoveFacts& facts, const WWE15MoveRules& rules, WWE15MovePlan* plan,
                    std::string* error) {
  if (error) error->clear();
  if (!plan) return Fail(error, "The move plan output is missing.");
  const std::string who = "WWE 2K15 wrestler " + std::to_string(source_id);
  if (pro15.size() < 572) return Fail(error, who + ": the WWE 2K15 PRO row is " + std::to_string(pro15.size()) + " bytes; at least 572 expected.");
  if (donor_pro14.size() < 572) return Fail(error, who + ": the donor's WWE 2K14 PRO row is " + std::to_string(donor_pro14.size()) + " bytes; at least 572 expected.");
  Planner pl(facts, rules);
  std::vector<uint16_t> vals(262), ext(22), dvals(262), dext(22);
  for (size_t s = 0; s < 262; ++s) { vals[s] = Le16(pro15, 4 + 2 * s); dvals[s] = Le16(donor_pro14, 4 + 2 * s); }
  for (size_t s = 0; s < 22; ++s) { ext[s] = Le16(pro15, 528 + 2 * s); dext[s] = Le16(donor_pro14, 528 + 2 * s); }
  std::set<uint16_t> port;
  std::vector<std::string> notes;
  auto pick = [&](uint16_t v, uint16_t dv, const std::string& slot, const uint16_t* other) -> uint16_t {
    if (!v) return 0;
    const auto [c, why] = pl.Classify(v);
    if (c == Planner::kBase) return v;
    if (c == Planner::kPort) { port.insert(v); return v; }
    if (c == Planner::kNotMotion) {
      if (facts.playable14.count(v)) return v;
      notes.push_back("slot " + slot + " " + std::to_string(v) + " has no motion in either game: zeroed");
      return 0;
    }
    bool have_alt = false;
    uint16_t alt = 0;
    if (other && *other != v) {
      const int oc = pl.Classify(*other).first;
      if (oc == Planner::kBase || oc == Planner::kPort) { have_alt = true; alt = *other; }
    }
    const uint16_t nv = have_alt ? alt : dv;
    notes.push_back("fallback slot " + slot + " " + std::to_string(v) + ": " + why + " -> " + std::to_string(nv) +
                    (have_alt ? " (his other finisher)" : " (donor slot)"));
    if (have_alt && pl.Classify(alt).first == Planner::kPort) port.insert(alt);
    return nv;
  };
  for (size_t s = 0; s < 3; ++s) vals[s] = 0;  // 2K15 template marks: 0 in every playable 2K14 row
  std::vector<uint16_t> out(262), outx(22);
  for (size_t s = 0; s < 262; ++s) {
    const uint16_t* other = s == 17 ? &vals[18] : s == 18 ? &vals[17] : nullptr;
    out[s] = pick(vals[s], dvals[s], std::to_string(s), other);
  }
  for (size_t s = 0; s < 22; ++s) outx[s] = pick(ext[s], dext[s], "x" + std::to_string(s), nullptr);
  if (rules.stall_donor_212.count(source_id))
    for (size_t s : {212u, 213u}) {
      if (out[s] != dvals[s])
        notes.push_back("stall rule slot " + std::to_string(s) + " " + std::to_string(vals[s]) + " -> " +
                        std::to_string(dvals[s]) + " (donor slot; CPU v CPU freeze fix)");
      out[s] = dvals[s];
    }
  const auto set = rules.stall_slot.find(source_id);
  if (set != rules.stall_slot.end())
    for (const auto& [s, v] : set->second) {
      if (s >= 262) return Fail(error, who + ": stall rule slot " + std::to_string(s) + " is out of range.");
      out[s] = pick(v, dvals[s], std::to_string(s), nullptr);
    }
  // chain: next motions of ported moves that 2K14 cannot load come along
  std::set<uint16_t> have = facts.base14;
  have.insert(port.begin(), port.end());
  std::set<uint16_t> seen;
  std::vector<uint16_t> todo(port.begin(), port.end());
  std::map<uint16_t, std::set<uint16_t>> chain;
  while (!todo.empty()) {
    const uint16_t i = todo.back();
    todo.pop_back();
    if (!seen.insert(i).second) continue;
    std::set<uint16_t> nx;
    const auto rows = pl.rows15.find(i);
    if (rows != pl.rows15.end())
      for (size_t o : rows->second) nx.insert(Be16(facts.prm15_rows, o + 20));
    nx.erase(0); nx.erase(0xffff); nx.erase(i);
    chain[i] = nx;
    for (uint16_t n : nx) if (!have.count(n)) todo.push_back(n);
  }
  std::set<uint16_t> extra;
  for (const auto& [a, nx] : chain)
    for (uint16_t n : nx)
      if (!facts.base14.count(n) && !port.count(n)) {
        if (pl.Classify(n).first == Planner::kPort) extra.insert(n);
        else notes.push_back("CHECK: " + std::to_string(a) + " chains to " + std::to_string(n) + " which cannot be brought over");
      }
  port.insert(extra.begin(), extra.end());
#ifndef WWE2K14_LAUNCHER_NO_TEST_TOOLS
  // Test fixtures only (never set by the launcher): WWE2K14_P15_TEST_FORCE="SOURCE:MOVE,..." puts MOVE into every slot
  // that holds a 50000-59999 move, so a CPU wrestler uses it at once (as add_bray.py --force did for bray-m4/m5).
  if (const char* e = std::getenv("WWE2K14_P15_TEST_FORCE")) {
    std::istringstream items(e);
    std::string item;
    while (std::getline(items, item, ',')) {
      const size_t colon = item.find(':');
      if (colon == std::string::npos || std::stoul(item.substr(0, colon)) != source_id) continue;
      const uint16_t mv = uint16_t(std::stoul(item.substr(colon + 1)));
      size_t n = 0;
      for (auto& v : out) if (v >= 50000 && v <= 59999 && v != mv) { v = mv; ++n; }
      if (pl.Classify(mv).first == Planner::kPort) port.insert(mv);
      notes.push_back("TEST FORCE " + std::to_string(mv) + " in " + std::to_string(n) + " slots");
    }
  }
#endif
  plan->slots = std::move(out);
  plan->extras = std::move(outx);
  plan->port.assign(port.begin(), port.end());
  plan->notes = std::move(notes);
  return true;
}

bool UpdateWWE15MoveMenus(const WWE15Bytes& pac, const WWE15Bytes& waze15, const std::vector<uint16_t>& ids,
                          const std::map<uint16_t, uint16_t>& owners, const std::vector<uint16_t>& d004_ids,
                          WWE15Bytes* output, size_t* d004_sets, std::string* error) {
  if (error) error->clear();
  std::vector<WWE13PacGroupData> groups;
  std::string d;
  if (!ReadWWE13PacGroups(pac, &groups, &d)) return Fail(error, "A move menu PAC could not be read: " + d);
  WWE13PacEntryData* waze = nullptr;
  WWE13PacEntryData* motc = nullptr;
  for (auto& g : groups)
    if (TrimName(g.name) == "MOVS")
      for (auto& e : g.entries) {
        if (TrimName(e.name) == "WAZE" && !waze) waze = &e;
        if (TrimName(e.name) == "MOTC" && !motc) motc = &e;
      }
  if (!waze || !motc) return Fail(error, "A move menu PAC has no MOVS/WAZE and MOVS/MOTC tables.");
  Bytes w14, w, m, m2;
  if (!MaybeUnz(waze->data, &w14, &d) || !MaybeUnz(motc->data, &m, &d)) return Fail(error, "A move menu table could not be unpacked: " + d);
  std::vector<std::string> notes;
  if (!AddWWE15MoveRecords(w14, waze15, ids, &w, &notes, error) || !FixWWE15MoveOwners(&w, owners, &notes, error)) return false;
  size_t sets = 0;
  if (d004_ids.empty()) m2 = m;
  else if (!AddWWE15D004Motc(m, d004_ids, &m2, &sets, error)) return false;
  if (!d004_ids.empty() && sets == 0) return Fail(error, "No moveset menu set accepts the Add-on 4 motion route (MOTC).");
  m2.resize((m2.size() + 0xff) & ~size_t(0xff), 0);
  waze->data = std::move(w);
  motc->data = std::move(m2);
  if (!RebuildWWE13PacGroups(pac, groups, output, &d)) return Fail(error, "A move menu PAC could not be rebuilt: " + d);
  if (d004_sets) *d004_sets = sets;
  return true;
}

}  // namespace wwe2k14::launcher
