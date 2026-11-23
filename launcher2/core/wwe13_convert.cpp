#include "wwe13_convert.h"
#include "havok_convert.h"

#include "internal/game_codecs.h"
#include "internal/miniz_api.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>


namespace wwe2k14::launcher {
namespace {

using Bytes = WWE13ConvertBytes;

constexpr size_t kPacDataStart = 0x4000;
constexpr size_t kPacDirectoryStart = 0x800;
constexpr size_t kPacDirectoryEnd = 0x4000;
constexpr size_t kPacSector = 0x800;
constexpr size_t kPacSizeUnit = 0x100;
constexpr size_t kAttireColorRecordSize = 664;
constexpr uint32_t kStringTableSentinel = 60139;
constexpr std::array<uint32_t, 9> kCoreLeaves{0, 10, 12, 13, 40, 50, 80, 110, 20002};

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void PutLe32(Bytes* bytes, size_t at, uint32_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value);
  (*bytes)[at + 1] = static_cast<uint8_t>(value >> 8);
  (*bytes)[at + 2] = static_cast<uint8_t>(value >> 16);
  (*bytes)[at + 3] = static_cast<uint8_t>(value >> 24);
}

bool HasMagic(const Bytes& bytes, const char* magic) {
  return bytes.size() >= 4 && std::memcmp(bytes.data(), magic, 4) == 0;
}

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

std::string Id6(uint32_t id) {
  std::ostringstream value;
  value.width(6);
  value.fill('0');
  value << id;
  return value.str();
}

struct PacEntry {
  std::string group;
  std::array<uint8_t, 8> name{};
  Bytes data;
};

struct PacGroup {
  std::array<uint8_t, 4> name{};
  uint32_t reserved = 0;
  bool wide = false;
  std::vector<PacEntry> entries;
};

struct PachLeaf {
  uint32_t id = 0;
  Bytes data;
};

std::string TrimField(const uint8_t* bytes, size_t size) {
  while (size && (bytes[size - 1] == 0 || bytes[size - 1] == ' ')) --size;
  return std::string(reinterpret_cast<const char*>(bytes), size);
}

std::string NameString(const std::array<uint8_t, 8>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool DecodePac(const Bytes& input, Bytes* output, std::string* error) {
  if (!output) return Fail(error, "The character PAC output buffer is missing.");
  if (HasMagic(input, "\x0F\xF5\x12\xED")) {
    std::string detail;
    if (!internal::DecodeWweXCompress(input, *output, detail))
      return Fail(error, "The source character PAC could not be decoded: " + detail);
  } else {
    *output = input;
  }
  if (!HasMagic(*output, "EPK8") && !HasMagic(*output, "EPAC"))
    return Fail(error, "The source character PAC is not an EPAC or EPK8 archive.");
  return true;
}

bool ParsePac(const Bytes& bytes, std::vector<PacGroup>* groups, std::string* error) {
  if (!groups || bytes.size() < kPacDataStart ||
      (!HasMagic(bytes, "EPK8") && !HasMagic(bytes, "EPAC")))
    return Fail(error, "The character PAC directory header is invalid.");
  groups->clear();
  size_t at = kPacDirectoryStart;
  while (at + 12 <= kPacDirectoryEnd) {
    if (bytes[at] == 0 && bytes[at + 1] == 0 && bytes[at + 2] == 0 && bytes[at + 3] == 0) break;
    const uint32_t word = Le32(bytes.data() + at + 4);
    const bool wide = ((word >> 16) & 1) != 0;
    const size_t record_size = wide ? 16 : 12;
    const size_t name_size = wide ? 8 : 4;
    const uint32_t word_count = word & 0xffff;
    const size_t words_per_entry = record_size / 4;
    if (word_count % words_per_entry != 0) {
      std::ostringstream detail;
      detail << "The character PAC has an invalid entry count in group '"
             << TrimField(bytes.data() + at, 4) << "' (word count " << word_count << ").";
      return Fail(error, detail.str());
    }
    const size_t count = word_count / words_per_entry;
    at += 12;
    if (count > (kPacDirectoryEnd - at) / record_size)
      return Fail(error, "The character PAC directory is truncated.");

    PacGroup group;
    std::copy_n(bytes.data() + at - 12, 4, group.name.begin());
    group.reserved = Le32(bytes.data() + at - 4);
    group.wide = wide;
    group.entries.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* record = bytes.data() + at;
      const uint32_t sector = Le32(record + name_size);
      const uint32_t size_units = Le32(record + name_size + 4);
      const uint64_t offset = uint64_t(kPacDataStart) + uint64_t(sector) * kPacSector;
      const uint64_t size = uint64_t(size_units) * kPacSizeUnit;
      if (offset > bytes.size() || size > bytes.size() - offset)
        return Fail(error, "A character PAC entry points outside its archive.");
      PacEntry entry;
      entry.group = TrimField(group.name.data(), group.name.size());
      std::copy_n(record, std::min(name_size, entry.name.size()), entry.name.begin());
      entry.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
      group.entries.push_back(std::move(entry));
      at += record_size;
    }
    groups->push_back(std::move(group));
  }
  if (groups->empty()) return Fail(error, "The character PAC has no entries.");
  return true;
}

bool DecodePach(const Bytes& input, Bytes* output, std::string* error) {
  if (HasMagic(input, "BPE ")) {
    std::string detail;
    if (!internal::DecodeWweBpe(input, *output, detail))
      return Fail(error, "A character entry could not be decoded: " + detail);
  } else {
    *output = input;
  }
  if (!HasMagic(*output, "PACH"))
    return Fail(error, "A character EMD entry is not a PACH archive.");
  return true;
}

bool ParsePach(const Bytes& bytes, std::vector<PachLeaf>* leaves, std::string* error) {
  if (!leaves || !HasMagic(bytes, "PACH") || bytes.size() < 8)
    return Fail(error, "The character PACH header is invalid.");
  const uint32_t count = Le32(bytes.data() + 4);
  if (uint64_t(count) * 12 + 8 > bytes.size())
    return Fail(error, "The character PACH directory is truncated.");
  const size_t base = 8 + size_t(count) * 12;
  leaves->clear();
  leaves->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t record = 8 + size_t(i) * 12;
    const uint32_t id = Le32(bytes.data() + record);
    const uint32_t relative = Le32(bytes.data() + record + 4);
    const uint32_t size = Le32(bytes.data() + record + 8);
    if (relative > bytes.size() - base || size > bytes.size() - base - relative)
      return Fail(error, "A character leaf points outside its PACH archive.");
    PachLeaf leaf;
    leaf.id = id;
    const size_t begin = base + relative;
    leaf.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                     bytes.begin() + static_cast<std::ptrdiff_t>(begin + size));
    leaves->push_back(std::move(leaf));
  }
  return true;
}

bool ReadEntryLeaves(const Bytes& raw_pac, const std::string& wanted_name,
                     std::vector<PachLeaf>* leaves, std::string* error) {
  Bytes plain;
  if (!DecodePac(raw_pac, &plain, error)) return false;
  std::vector<PacGroup> groups;
  if (!ParsePac(plain, &groups, error)) return false;
  for (const PacGroup& group : groups) {
    if (TrimField(group.name.data(), group.name.size()) != "EMD") continue;
    for (const PacEntry& entry : group.entries) {
      if (TrimField(entry.name.data(), entry.name.size()) != wanted_name) continue;
      Bytes pach;
      if (!DecodePach(entry.data, &pach, error)) return false;
      return ParsePach(pach, leaves, error);
    }
  }
  return Fail(error, "The requested character EMD entry is missing.");
}

bool ReadNamedEntryLeaves(const Bytes& raw_pac, const std::string& wanted_group,
                          const std::string& wanted_name, std::vector<PachLeaf>* leaves,
                          std::string* error) {
  Bytes plain;
  if (!DecodePac(raw_pac, &plain, error)) return false;
  std::vector<PacGroup> groups;
  if (!ParsePac(plain, &groups, error)) return false;
  for (const PacGroup& group : groups) {
    if (TrimField(group.name.data(), group.name.size()) != wanted_group) continue;
    for (const PacEntry& entry : group.entries) {
      if (TrimField(entry.name.data(), entry.name.size()) != wanted_name) continue;
      Bytes pach;
      if (!DecodePach(entry.data, &pach, error)) return false;
      return ParsePach(pach, leaves, error);
    }
  }
  return Fail(error, "The requested character EMD entry is missing.");
}

Bytes BuildAttirePach(const std::vector<PachLeaf>& leaves) {
  const size_t base = 8 + leaves.size() * 12;
  Bytes output(base, 0);
  std::memcpy(output.data(), "PACH", 4);
  PutLe32(&output, 4, static_cast<uint32_t>(leaves.size()));
  Bytes body;
  for (size_t i = 0; i < leaves.size(); ++i) {
    const size_t record = 8 + i * 12;
    PutLe32(&output, record, leaves[i].id);
    PutLe32(&output, record + 4, static_cast<uint32_t>(body.size()));
    PutLe32(&output, record + 8, static_cast<uint32_t>(leaves[i].data.size()));
    body.insert(body.end(), leaves[i].data.begin(), leaves[i].data.end());
  }
  output.insert(output.end(), body.begin(), body.end());
  output.resize((output.size() + 0xff) & ~size_t(0xff), 0);
  return output;
}

bool ParseAttireColorLeaf(const Bytes& data, std::map<uint32_t, Bytes>* records,
                          std::string* error) {
  if (!records || data.size() < 2)
    return Fail(error, "The attire-color leaf header is truncated.");
  const uint16_t count = static_cast<uint16_t>(data[0] | (uint16_t(data[1]) << 8));
  if (data.size() != 2 + size_t(count) * kAttireColorRecordSize)
    return Fail(error, "The attire-color leaf has an unexpected record size.");
  std::map<uint32_t, Bytes> parsed;
  uint32_t previous = 0;
  for (uint16_t i = 0; i < count; ++i) {
    const size_t begin = 2 + size_t(i) * kAttireColorRecordSize;
    const uint32_t id = Le32(data.data() + begin);
    if (i && id < previous)
      return Fail(error, "The attire-color records are not sorted by ID.");
    previous = id;
    parsed[id] = Bytes(data.begin() + static_cast<std::ptrdiff_t>(begin),
                       data.begin() + static_cast<std::ptrdiff_t>(begin + kAttireColorRecordSize));
  }
  *records = std::move(parsed);
  return true;
}

bool FindAttireColorEntry(const Bytes& plain, std::vector<PacGroup>* groups,
                          size_t* found_group, size_t* found_entry,
                          std::vector<PachLeaf>* found_leaves,
                          std::map<uint32_t, Bytes>* found_records, std::string* error) {
  if (!groups || !found_group || !found_entry || !found_leaves || !found_records)
    return Fail(error, "The attire-color lookup output is missing.");
  if (!ParsePac(plain, groups, error)) return false;
  bool found = false;
  for (size_t gi = 0; gi < groups->size(); ++gi) {
    for (size_t ei = 0; ei < (*groups)[gi].entries.size(); ++ei) {
      const PacEntry& entry = (*groups)[gi].entries[ei];
      Bytes pach;
      if (!HasMagic(entry.data, "PACH")) {
        if (!HasMagic(entry.data, "BPE ")) continue;
        std::string decode_error;
        if (!DecodePach(entry.data, &pach, &decode_error)) continue;
      } else {
        pach = entry.data;
      }
      std::vector<PachLeaf> leaves;
      std::string parse_error;
      if (!ParsePach(pach, &leaves, &parse_error)) continue;
      const auto color = std::find_if(leaves.begin(), leaves.end(), [](const PachLeaf& leaf) {
        return leaf.id == 600;
      });
      if (color == leaves.end()) continue;
      std::map<uint32_t, Bytes> records;
      if (!ParseAttireColorLeaf(color->data, &records, error)) return false;
      *found_group = gi;
      *found_entry = ei;
      *found_leaves = std::move(leaves);
      *found_records = std::move(records);
      found = true;
    }
  }
  if (!found) return Fail(error, "The PAC has no PACH entry with attire-color leaf 600.");
  return true;
}

bool ReplacePacEntries(const Bytes& plain, const std::vector<PacGroup>& groups,
                       Bytes* output, std::string* error) {
  if (!output || plain.size() < kPacDataStart)
    return Fail(error, "The attire-color PAC output buffer is missing or the header is truncated.");
  Bytes result(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(kPacDataStart));
  Bytes body;
  size_t directory = kPacDirectoryStart;
  for (size_t gi = 0; gi < groups.size(); ++gi) {
    const size_t record_size = groups[gi].wide ? 16 : 12;
    const size_t name_size = groups[gi].wide ? 8 : 4;
    directory += 12;
    for (const PacEntry& entry : groups[gi].entries) {
      const Bytes& data = entry.data;
      if (data.size() > std::numeric_limits<uint32_t>::max() - (kPacSizeUnit - 1))
        return Fail(error, "An attire-color PAC entry is too large.");
      if (directory + record_size > result.size())
        return Fail(error, "The attire-color PAC directory is truncated.");
      PutLe32(&result, directory + name_size, static_cast<uint32_t>(body.size() / kPacSector));
      PutLe32(&result, directory + name_size + 4,
              static_cast<uint32_t>((data.size() + kPacSizeUnit - 1) / kPacSizeUnit));
      body.insert(body.end(), data.begin(), data.end());
      body.resize((body.size() + kPacSector - 1) & ~(kPacSector - 1), 0);
      directory += record_size;
    }
  }
  if (body.size() > std::numeric_limits<uint32_t>::max())
    return Fail(error, "The attire-color PAC exceeds the supported size.");
  PutLe32(&result, 8, static_cast<uint32_t>(body.size()));
  result.insert(result.end(), body.begin(), body.end());
  result.resize(result.size() + kPacSector, 0);
  *output = std::move(result);
  return true;
}

bool DecodeSdbLeaf(const Bytes& compressed, Bytes* plain, std::string* error) {
  if (!plain || !HasMagic(compressed, "ZLIB") || compressed.size() < 16)
    return Fail(error, "The SDB string-table compression header is invalid.");
  const uint32_t packed_size = Le32(compressed.data() + 8);
  const uint32_t plain_size = Le32(compressed.data() + 12);
  if (packed_size > compressed.size() - 16 || plain_size > 32 * 1024 * 1024)
    return Fail(error, "The SDB string-table compression sizes are invalid.");
  plain->resize(plain_size);
  wwe_mz_ulong output_size = plain_size;
  const int result = mz_uncompress(plain->data(), &output_size, compressed.data() + 16, packed_size);
  static constexpr uint8_t kEmptyPartialStream[] = {0x78, 0xda, 0x63, 0x60, 0x80, 0x00, 0x00, 0x00};
  if (result != kWweMzOk && plain_size == 8 && packed_size == sizeof(kEmptyPartialStream) &&
      compressed.size() == 16 + sizeof(kEmptyPartialStream) &&
      std::equal(std::begin(kEmptyPartialStream), std::end(kEmptyPartialStream), compressed.begin() + 16)) {
    plain->assign(8, 0);
    return true;
  }
  if (result != kWweMzOk || output_size != plain_size) {
    plain->clear();
    return Fail(error, "An SDB string table could not be decompressed.");
  }
  return true;
}

bool EncodeSdbLeaf(const Bytes& header, const Bytes& plain, Bytes* compressed, std::string* error) {
  if (!compressed || header.size() < 16 || plain.size() > std::numeric_limits<uint32_t>::max())
    return Fail(error, "The SDB string-table compression output is invalid.");
  const wwe_mz_ulong bound = mz_compressBound(static_cast<wwe_mz_ulong>(plain.size()));
  Bytes body(static_cast<size_t>(bound));
  wwe_mz_ulong body_size = bound;
  if (mz_compress2(body.data(), &body_size, plain.data(), static_cast<wwe_mz_ulong>(plain.size()), 9) != 0)
    return Fail(error, "An SDB string table could not be compressed.");
  Bytes result(16, 0);
  std::memcpy(result.data(), "ZLIB", 4);
  std::copy_n(header.data() + 4, 4, result.data() + 4);
  PutLe32(&result, 8, static_cast<uint32_t>(body_size));
  PutLe32(&result, 12, static_cast<uint32_t>(plain.size()));
  result.insert(result.end(), body.begin(), body.begin() + static_cast<std::ptrdiff_t>(body_size));
  *compressed = std::move(result);
  return true;
}

bool ParseSdbTable(const Bytes& plain, std::map<uint32_t, std::pair<uint32_t, uint32_t>>* entries,
                   uint32_t* row_count, std::string* error) {
  if (!entries || plain.size() < 20 || Le32(plain.data()) != 0)
    return Fail(error, "The SDB string table header is invalid.");
  const uint32_t count = Le32(plain.data() + 4);
  if (!count || uint64_t(count) * 12 + 8 > plain.size())
    return Fail(error, "The SDB string table directory is invalid.");
  const size_t table_end = 8 + size_t(count) * 12;
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> parsed;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = 8 + size_t(i) * 12;
    const uint32_t offset = Le32(plain.data() + at);
    const uint32_t length = Le32(plain.data() + at + 4);
    const uint32_t key = Le32(plain.data() + at + 8);
    if (offset > plain.size() || length > plain.size() - offset)
      return Fail(error, "An SDB string table value points outside its leaf.");
    (void)table_end;
    parsed[key] = {offset, length};
  }
  *entries = std::move(parsed);
  if (row_count) *row_count = count;
  return true;
}

bool InsertSdbKeys(const Bytes& plain, const WWE13StringKeyMap& keyed_strings,
                   Bytes* output, size_t* inserted, std::string* error,
                   uint32_t sentinel = kStringTableSentinel) {
  if (!output || !inserted) return Fail(error, "The SDB key insertion output is missing.");
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> entries;
  uint32_t row_count = 0;
  if (!ParseSdbTable(plain, &entries, &row_count, error)) return false;
  if (!entries.count(sentinel)) {
    *output = plain;
    *inserted = 0;
    return true;
  }
  std::vector<uint32_t> new_keys;
  for (const auto& [key, value] : keyed_strings) {
    (void)value;
    if (!entries.count(key)) new_keys.push_back(key);
  }
  const Bytes old_rest(plain.begin() + static_cast<std::ptrdiff_t>(8 + size_t(row_count) * 12), plain.end());
  const uint64_t shift = uint64_t(new_keys.size()) * 12;
  const uint64_t base = 8 + uint64_t(row_count + new_keys.size()) * 12 + old_rest.size();
  if (base > std::numeric_limits<uint32_t>::max())
    return Fail(error, "The SDB string table is too large to insert names.");
  for (auto& [key, location] : entries) {
    (void)key;
    const uint64_t offset = uint64_t(location.first) + shift;
    if (offset > std::numeric_limits<uint32_t>::max())
      return Fail(error, "An SDB string table offset exceeds the supported size.");
    location.first = static_cast<uint32_t>(offset);
  }
  Bytes tail;
  for (const auto& [key, value] : keyed_strings) {
    const uint64_t offset = base + tail.size();
    if (offset > std::numeric_limits<uint32_t>::max() || value.size() >= std::numeric_limits<uint32_t>::max())
      return Fail(error, "An inserted SDB name is too large.");
    entries[key] = {static_cast<uint32_t>(offset), static_cast<uint32_t>(value.size() + 1)};
    tail.insert(tail.end(), value.begin(), value.end());
    tail.push_back(0);
  }
  if (entries.size() > std::numeric_limits<uint32_t>::max())
    return Fail(error, "The SDB string table has too many entries.");
  Bytes rebuilt(8 + entries.size() * 12, 0);
  PutLe32(&rebuilt, 0, 0);
  PutLe32(&rebuilt, 4, static_cast<uint32_t>(entries.size()));
  size_t at = 8;
  for (const auto& [key, location] : entries) {
    PutLe32(&rebuilt, at, location.first);
    PutLe32(&rebuilt, at + 4, location.second);
    PutLe32(&rebuilt, at + 8, key);
    at += 12;
  }
  rebuilt.insert(rebuilt.end(), old_rest.begin(), old_rest.end());
  rebuilt.insert(rebuilt.end(), tail.begin(), tail.end());
  *output = std::move(rebuilt);
  *inserted = keyed_strings.size();
  return true;
}

bool ReadSdbKeyValues(const Bytes& plain, WWE13StringKeyMap* values, std::string* error) {
  if (!values) return Fail(error, "The SDB key-value output map is missing.");
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> entries;
  if (!ParseSdbTable(plain, &entries, nullptr, error)) return false;
  for (const auto& [key, location] : entries) {
    const size_t offset = location.first, length = location.second;
    values->try_emplace(key, Bytes(plain.begin() + static_cast<std::ptrdiff_t>(offset),
                                   plain.begin() + static_cast<std::ptrdiff_t>(offset + length)));
  }
  return true;
}

bool ReadLeaf(const Bytes& pac, const std::string& emd_name, uint32_t leaf_id,
              Bytes* output, std::string* error) {
  std::vector<PachLeaf> leaves;
  if (!ReadEntryLeaves(pac, emd_name, &leaves, error)) return false;
  for (const PachLeaf& leaf : leaves) {
    if (leaf.id == leaf_id) {
      if (output) *output = leaf.data;
      return output != nullptr || Fail(error, "The character leaf output buffer is missing.");
    }
  }
  std::ostringstream message;
  message << "The requested character leaf " << leaf_id << " is missing.";
  return Fail(error, message.str());
}

Bytes BuildPach(const std::vector<PachLeaf>& leaves) {
  const size_t base = 8 + leaves.size() * 12;
  Bytes output(base, 0);
  std::memcpy(output.data(), "PACH", 4);
  PutLe32(&output, 4, static_cast<uint32_t>(leaves.size()));
  Bytes body;
  for (size_t i = 0; i < leaves.size(); ++i) {
    while (body.size() % 16) body.push_back(0);
    const size_t record = 8 + i * 12;
    PutLe32(&output, record, leaves[i].id);
    PutLe32(&output, record + 4, static_cast<uint32_t>(body.size()));
    PutLe32(&output, record + 8, static_cast<uint32_t>(leaves[i].data.size()));
    body.insert(body.end(), leaves[i].data.begin(), leaves[i].data.end());
  }
  output.insert(output.end(), body.begin(), body.end());
  return output;
}

bool HasLeaf(const std::vector<PachLeaf>& leaves, uint32_t id) {
  return std::any_of(leaves.begin(), leaves.end(), [id](const PachLeaf& leaf) { return leaf.id == id; });
}

std::set<uint32_t> MissingCoreLeaves(const std::vector<PachLeaf>& leaves) {
  std::set<uint32_t> missing;
  for (uint32_t id : kCoreLeaves)
    if (!HasLeaf(leaves, id)) missing.insert(id);
  return missing;
}

bool ReadSourceEntryLeaves(const Bytes* pac, uint32_t id, std::vector<PachLeaf>* leaves,
                           std::string* error, const char* source_name) {
  if (!pac) return Fail(error, std::string("The recipe requires ") + source_name + " PAC bytes.");
  return ReadEntryLeaves(*pac, Id6(id) + "02", leaves, error);
}

bool BuildEpk8(const std::vector<PacEntry>& entries, uint32_t header_directory_size,
               uint32_t reserved, Bytes* output, std::string* error) {
  if (!output || entries.size() > (kPacDirectoryEnd - kPacDirectoryStart - 12) / 16)
    return Fail(error, "The converted character PAC directory is too large.");
  Bytes result(kPacDataStart, 0);
  std::memcpy(result.data(), "EPK8", 4);
  PutLe32(&result, 4, header_directory_size);
  PutLe32(&result, 12, 7);
  size_t directory = kPacDirectoryStart;
  std::memcpy(result.data() + directory, "EMD ", 4);
  PutLe32(&result, directory + 4, static_cast<uint32_t>(entries.size() * 4) | 0x10000);
  PutLe32(&result, directory + 8, reserved);
  directory += 12;
  Bytes body;
  for (const PacEntry& entry : entries) {
    std::copy(entry.name.begin(), entry.name.end(), result.begin() + static_cast<std::ptrdiff_t>(directory));
    PutLe32(&result, directory + 8, static_cast<uint32_t>(body.size() / kPacSector));
    if (entry.data.size() > std::numeric_limits<uint32_t>::max() - (kPacSizeUnit - 1))
      return Fail(error, "A converted character entry is too large.");
    PutLe32(&result, directory + 12,
            static_cast<uint32_t>((entry.data.size() + kPacSizeUnit - 1) / kPacSizeUnit));
    body.insert(body.end(), entry.data.begin(), entry.data.end());
    body.resize((body.size() + kPacSector - 1) & ~(kPacSector - 1), 0);
    directory += 16;
  }
  if (body.size() > std::numeric_limits<uint32_t>::max())
    return Fail(error, "The converted character PAC exceeds the supported size.");
  PutLe32(&result, 8, static_cast<uint32_t>(body.size()));
  result.insert(result.end(), body.begin(), body.end());
  result.resize(result.size() + kPacSector, 0);
  *output = std::move(result);
  return true;
}

bool ConvertEntry(const PacEntry& source, const WWE13CharacterPacRecipe& recipe,
                  const std::vector<PachLeaf>* leaf10000_source,
                  const std::vector<PachLeaf>* stock_model,
                  const std::vector<PachLeaf>* stock_body,
                  const Bytes* shared_leaf50, PacEntry* converted,
                  bool* omit, std::string* error) {
  if (source.group != "EMD")
    return Fail(error, "The source character PAC contains a non-EMD group.");
  const std::string source_name = NameString(source.name);
  if (source_name.substr(0, 6) != Id6(recipe.source_id))
    return Fail(error, "A source character EMD entry does not match the recipe's source ID.");
  Bytes pach;
  if (!DecodePach(source.data, &pach, error)) return false;
  std::vector<PachLeaf> leaves;
  if (!ParsePach(pach, &leaves, error)) return false;

  leaves.erase(std::remove_if(leaves.begin(), leaves.end(), [](const PachLeaf& leaf) {
    return leaf.id == 100;
  }), leaves.end());

  if (stock_model && source_name.substr(6, 2) == "02") {
    leaves = *stock_model;
    leaves.erase(std::remove_if(leaves.begin(), leaves.end(), [](const PachLeaf& leaf) {
      return leaf.id == 100;
    }), leaves.end());
  } else if (stock_body && source_name.substr(6, 2) == "02") {
    const auto body_leaf = std::find_if(stock_body->begin(), stock_body->end(), [](const PachLeaf& leaf) {
      return leaf.id == 40;
    });
    if (body_leaf == stock_body->end()) return Fail(error, "The stock body PAC has no leaf 40.");
    auto source_leaf = std::find_if(leaves.begin(), leaves.end(), [](const PachLeaf& leaf) { return leaf.id == 40; });
    if (source_leaf == leaves.end()) return Fail(error, "The WWE '13 primary entry has no leaf 40.");
    source_leaf->data = body_leaf->data;
  }

  if (recipe.target_id != recipe.source_id && source_name.substr(6, 2) != "02") {
    const std::set<uint32_t> missing = MissingCoreLeaves(leaves);
    if (!missing.empty() && !(recipe.waive_missing_20002 && missing == std::set<uint32_t>{20002})) {
      *omit = true;
      return true;
    }
  }

  if (recipe.leaf_10000_source_id) {
    if (!leaf10000_source) return Fail(error, "The recipe requires the selected leaf 10000 source PAC.");
    const auto source_leaf = std::find_if(leaf10000_source->begin(), leaf10000_source->end(), [](const PachLeaf& leaf) {
      return leaf.id == 10000;
    });
    if (source_leaf == leaf10000_source->end()) return Fail(error, "The selected leaf 10000 source has no such leaf.");
    auto target_leaf = std::find_if(leaves.begin(), leaves.end(), [](const PachLeaf& leaf) { return leaf.id == 10000; });
    if (target_leaf == leaves.end()) return Fail(error, "The source entry has no leaf 10000 to replace.");
    target_leaf->data = source_leaf->data;
  }

  if (recipe.add_shared_leaf50 && source_name.substr(6, 2) == "02") {
    if (!shared_leaf50) return Fail(error, "The recipe requires the verified shared leaf 50.");
    if (HasLeaf(leaves, 50)) return Fail(error, "The source primary entry already has leaf 50.");
    if (shared_leaf50->size() != 303)
      return Fail(error, "The shared leaf 50 is not the surveyed 303-byte record.");
    leaves.push_back({50, *shared_leaf50});
    std::stable_sort(leaves.begin(), leaves.end(), [](const PachLeaf& left, const PachLeaf& right) {
      return left.id < right.id;
    });
  }

  // WWE '13 Havok cloth (hk_2011.3) -> WWE 2K14 (hk_2012.2): without it 2K14 cannot simulate the cloth and a cape or coat
  // stays in its modelled shape (owner 11-16, cape-1..5). Kept as is when no template or the conversion fails.
  if (recipe.cloth_templates) {
    for (PachLeaf& leaf : leaves) {
      if (leaf.id < 9000 || leaf.id >= 9100) continue;
      const auto templ = recipe.cloth_templates->find(leaf.id);
      if (templ == recipe.cloth_templates->end()) continue;
      Bytes decoded;
      std::string detail;
      if (HasMagic(leaf.data, "BPE ")) {
        if (!internal::DecodeWweBpe(leaf.data, decoded, detail)) continue;
      } else {
        decoded = leaf.data;
      }
      if (HavokPackfileVersion(decoded).rfind("hk_2011", 0) != 0) continue;
      Bytes converted_leaf;
      if (!ConvertHavokPackfile(decoded, templ->second, &converted_leaf, &detail)) continue;
      leaf.data = StoreWWE13BpeLiteral(converted_leaf);
    }
  }

  converted->group = "EMD";
  converted->name = source.name;
  if (recipe.target_id != recipe.source_id) {
    const std::string target = Id6(recipe.target_id);
    std::copy(target.begin(), target.end(), converted->name.begin());
  }
  converted->data = BuildPach(leaves);
  *omit = false;
  return true;
}

}  // namespace

bool ReadWWE13PacGroups(const WWE13ConvertBytes& pac,
                        std::vector<WWE13PacGroupData>* groups, std::string* error) {
  if (error) error->clear();
  if (!groups) return Fail(error, "The PAC group output is missing.");
  Bytes plain;
  if (!DecodePac(pac, &plain, error)) return false;
  std::vector<PacGroup> parsed;
  if (!ParsePac(plain, &parsed, error)) return false;
  std::vector<WWE13PacGroupData> result;
  result.reserve(parsed.size());
  for (const PacGroup& source : parsed) {
    WWE13PacGroupData group;
    group.name.assign(reinterpret_cast<const char*>(source.name.data()), source.name.size());
    group.wide = source.wide;
    group.reserved = source.reserved;
    group.entries.reserve(source.entries.size());
    for (const PacEntry& source_entry : source.entries) {
      WWE13PacEntryData entry;
      const size_t name_size = source.wide ? 8 : 4;
      entry.name.assign(reinterpret_cast<const char*>(source_entry.name.data()), name_size);
      entry.data = source_entry.data;
      group.entries.push_back(std::move(entry));
    }
    result.push_back(std::move(group));
  }
  *groups = std::move(result);
  return true;
}

namespace {

bool WritePacGroups(const Bytes& source_header,
                    const std::vector<WWE13PacGroupData>& source_groups,
                    bool new_archive, Bytes* output, std::string* error) {
  if (!output) return Fail(error, "The PAC archive output is missing.");
  if (!new_archive && source_header.size() < kPacDataStart)
    return Fail(error, "The source PAC header is truncated.");
  Bytes result(new_archive ? kPacDataStart : kPacDataStart, 0);
  if (!new_archive)
    std::copy_n(source_header.begin(), kPacDataStart, result.begin());
  else {
    std::memcpy(result.data(), "EPK8", 4);
    PutLe32(&result, 12, 7);
  }

  size_t directory = kPacDirectoryStart;
  size_t directory_size = 0;
  Bytes body;
  for (const WWE13PacGroupData& source_group : source_groups) {
    const size_t name_size = source_group.wide ? 8 : 4;
    const size_t record_size = name_size + 8;
    if (source_group.name.size() > 4 || source_group.entries.size() > 0xffff ||
        directory + 12 + source_group.entries.size() * record_size > kPacDirectoryEnd)
      return Fail(error, "The PAC directory exceeds its supported size.");
    std::fill(result.begin() + static_cast<std::ptrdiff_t>(directory),
              result.begin() + static_cast<std::ptrdiff_t>(directory + 12), 0);
    std::copy(source_group.name.begin(), source_group.name.end(),
              result.begin() + static_cast<std::ptrdiff_t>(directory));
    PutLe32(&result, directory + 4,
            static_cast<uint32_t>(source_group.entries.size() * record_size / 4) |
                (source_group.wide ? 0x10000u : 0u));
    // Match the Python arcmodel.epac_rebuild byte stream: it reads each reserved word at
    // the current output cursor, so an earlier group's inserted entries shift later reads.
    const uint32_t reserved = !new_archive && directory + 12 <= source_header.size()
        ? Le32(source_header.data() + directory + 8)
        : source_group.reserved;
    PutLe32(&result, directory + 8, reserved);
    directory += 12;
    directory_size += 12;
    for (const WWE13PacEntryData& source_entry : source_group.entries) {
      if (source_entry.name.size() != name_size)
        return Fail(error, "A PAC entry name does not match its group's width.");
      if (body.size() % kPacSector != 0)
        return Fail(error, "A PAC entry begins at an unaligned sector.");
      std::copy(source_entry.name.begin(), source_entry.name.end(),
                result.begin() + static_cast<std::ptrdiff_t>(directory));
      PutLe32(&result, directory + name_size,
              static_cast<uint32_t>(body.size() / kPacSector));
      if (source_entry.data.size() > std::numeric_limits<uint32_t>::max() - (kPacSizeUnit - 1))
        return Fail(error, "A PAC entry is too large.");
      PutLe32(&result, directory + name_size + 4,
              static_cast<uint32_t>((source_entry.data.size() + kPacSizeUnit - 1) / kPacSizeUnit));
      body.insert(body.end(), source_entry.data.begin(), source_entry.data.end());
      body.resize((body.size() + kPacSector - 1) & ~(kPacSector - 1), 0);
      directory += record_size;
      directory_size += record_size;
    }
  }
  if (directory > kPacDirectoryEnd || body.size() > std::numeric_limits<uint32_t>::max())
    return Fail(error, "The rebuilt PAC exceeds its supported size.");
  std::fill(result.begin() + static_cast<std::ptrdiff_t>(directory),
            result.begin() + static_cast<std::ptrdiff_t>(kPacDirectoryEnd), 0);
  if (new_archive) PutLe32(&result, 4, static_cast<uint32_t>(directory_size));
  PutLe32(&result, 8, static_cast<uint32_t>(body.size()));
  result.insert(result.end(), body.begin(), body.end());
  result.resize(result.size() + kPacSector, 0);
  *output = std::move(result);
  return true;
}

}  // namespace

bool RebuildWWE13PacGroups(const WWE13ConvertBytes& source_pac,
                           const std::vector<WWE13PacGroupData>& groups,
                           WWE13ConvertBytes* output, std::string* error) {
  if (error) error->clear();
  Bytes plain;
  if (!DecodePac(source_pac, &plain, error)) return false;
  return WritePacGroups(plain, groups, false, output, error);
}

bool BuildWWE13PacGroups(const std::vector<WWE13PacGroupData>& groups,
                         WWE13ConvertBytes* output, std::string* error) {
  if (error) error->clear();
  return WritePacGroups({}, groups, true, output, error);
}

bool ReadWWE13Pach(const WWE13ConvertBytes& raw, WWE13RawRowMap* leaves,
                   std::string* error) {
  if (error) error->clear();
  if (!leaves) return Fail(error, "The PACH leaf output map is missing.");
  std::vector<WWE13PachLeafData> parsed;
  if (!ReadWWE13PachLeaves(raw, &parsed, error)) return false;
  WWE13RawRowMap result;
  for (WWE13PachLeafData& leaf : parsed) {
    if (!result.emplace(leaf.id, std::move(leaf.data)).second)
      return Fail(error, "The PACH archive contains a duplicate leaf ID.");
  }
  *leaves = std::move(result);
  return true;
}

bool ReadWWE13PachLeaves(const WWE13ConvertBytes& raw,
                         std::vector<WWE13PachLeafData>* leaves,
                         std::string* error) {
  if (error) error->clear();
  if (!leaves) return Fail(error, "The PACH leaf output is missing.");
  std::vector<PachLeaf> parsed;
  if (!ParsePach(raw, &parsed, error)) return false;
  std::vector<WWE13PachLeafData> result;
  result.reserve(parsed.size());
  for (PachLeaf& leaf : parsed) result.push_back({leaf.id, std::move(leaf.data)});
  *leaves = std::move(result);
  return true;
}

bool BuildWWE13PachLeaves(const std::vector<WWE13PachLeafData>& leaves,
                          WWE13ConvertBytes* output, bool pad_to_256,
                          std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The PACH archive output is missing.");
  const uint64_t header_size = 8 + uint64_t(leaves.size()) * 12;
  if (header_size > std::numeric_limits<uint32_t>::max())
    return Fail(error, "The PACH archive has too many leaves.");
  Bytes result(static_cast<size_t>(header_size), 0);
  std::memcpy(result.data(), "PACH", 4);
  PutLe32(&result, 4, static_cast<uint32_t>(leaves.size()));
  Bytes body;
  for (size_t index = 0; index < leaves.size(); ++index) {
    const size_t record = 8 + index * 12;
    const auto& leaf = leaves[index];
    if (body.size() > std::numeric_limits<uint32_t>::max() ||
        leaf.data.size() > std::numeric_limits<uint32_t>::max())
      return Fail(error, "A PACH leaf exceeds its supported size.");
    PutLe32(&result, record, leaf.id);
    PutLe32(&result, record + 4, static_cast<uint32_t>(body.size()));
    PutLe32(&result, record + 8, static_cast<uint32_t>(leaf.data.size()));
    body.insert(body.end(), leaf.data.begin(), leaf.data.end());
  }
  result.insert(result.end(), body.begin(), body.end());
  if (pad_to_256) result.resize((result.size() + 0xff) & ~size_t(0xff), 0);
  *output = std::move(result);
  return true;
}

bool BuildWWE13Pach(const WWE13RawRowMap& leaves, WWE13ConvertBytes* output,
                    bool pad_to_256, std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The PACH archive output is missing.");
  const uint64_t header_size = 8 + uint64_t(leaves.size()) * 12;
  if (header_size > std::numeric_limits<uint32_t>::max())
    return Fail(error, "The PACH archive has too many leaves.");
  Bytes result(static_cast<size_t>(header_size), 0);
  std::memcpy(result.data(), "PACH", 4);
  PutLe32(&result, 4, static_cast<uint32_t>(leaves.size()));
  Bytes body;
  size_t index = 0;
  for (const auto& [id, data] : leaves) {
    const size_t record = 8 + index++ * 12;
    if (body.size() > std::numeric_limits<uint32_t>::max() ||
        data.size() > std::numeric_limits<uint32_t>::max())
      return Fail(error, "A PACH leaf exceeds its supported size.");
    PutLe32(&result, record, id);
    PutLe32(&result, record + 4, static_cast<uint32_t>(body.size()));
    PutLe32(&result, record + 8, static_cast<uint32_t>(data.size()));
    body.insert(body.end(), data.begin(), data.end());
  }
  result.insert(result.end(), body.begin(), body.end());
  if (pad_to_256) result.resize((result.size() + 0xff) & ~size_t(0xff), 0);
  *output = std::move(result);
  return true;
}

bool DecodeWWE13Bpe(const WWE13ConvertBytes& raw, WWE13ConvertBytes* output,
                    std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The BPE output is missing.");
  if (HasMagic(raw, "BPE ")) {
    std::string detail;
    if (!internal::DecodeWweBpe(raw, *output, detail))
      return Fail(error, "A BPE archive could not be decoded: " + detail);
  } else {
    *output = raw;
  }
  return true;
}

WWE13ConvertBytes StoreWWE13BpeLiteral(const WWE13ConvertBytes& decoded) {
  Bytes output(16, 0);
  std::memcpy(output.data(), "BPE ", 4);
  PutLe32(&output, 4, 0x100);
  PutLe32(&output, 12, static_cast<uint32_t>(decoded.size()));
  Bytes body;
  size_t position = 0;
  while (position < decoded.size()) {
    const size_t count = std::min<size_t>(0xffff, decoded.size() - position);
    body.insert(body.end(), {0xff, 0x80, 0xfe,
                             static_cast<uint8_t>(count),
                             static_cast<uint8_t>(count >> 8)});
    body.insert(body.end(), decoded.begin() + static_cast<std::ptrdiff_t>(position),
                decoded.begin() + static_cast<std::ptrdiff_t>(position + count));
    position += count;
  }
  PutLe32(&output, 8, static_cast<uint32_t>(body.size()));
  output.insert(output.end(), body.begin(), body.end());
  return output;
}

bool ReadWWE13PacLeaf(const WWE13ConvertBytes& pac, const std::string& group,
                      const std::string& entry, uint32_t leaf_id,
                      WWE13ConvertBytes* output, std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The PAC leaf output buffer is missing.");
  std::vector<PachLeaf> leaves;
  if (!ReadNamedEntryLeaves(pac, group, entry, &leaves, error)) return false;
  const auto found = std::find_if(leaves.begin(), leaves.end(), [leaf_id](const PachLeaf& leaf) {
    return leaf.id == leaf_id;
  });
  if (found == leaves.end()) return Fail(error, "The requested PAC leaf is missing.");
  *output = found->data;
  return true;
}

bool ReadWWE13AttireColorRecords(const WWE13ConvertBytes& pac,
                                 std::map<uint32_t, WWE13ConvertBytes>* records,
                                 std::string* error) {
  if (error) error->clear();
  if (!records) return Fail(error, "The attire-color record output map is missing.");
  Bytes plain;
  if (!DecodePac(pac, &plain, error)) return false;
  std::vector<PacGroup> groups;
  size_t group_index = 0, entry_index = 0;
  std::vector<PachLeaf> leaves;
  std::map<uint32_t, Bytes> parsed;
  if (!FindAttireColorEntry(plain, &groups, &group_index, &entry_index, &leaves, &parsed, error)) return false;
  (void)group_index;
  (void)entry_index;
  (void)leaves;
  *records = std::move(parsed);
  return true;
}

bool ConvertWWE13AttireColors(const WWE13ConvertBytes& source_pac,
                              const std::vector<WWE13AttireColorPort>& ports,
                              WWE13ConvertBytes* output, std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The attire-color PAC output buffer is missing.");
  Bytes plain;
  if (!DecodePac(source_pac, &plain, error)) return false;
  std::vector<PacGroup> groups;
  size_t group_index = 0, entry_index = 0;
  std::vector<PachLeaf> leaves;
  std::map<uint32_t, Bytes> records;
  if (!FindAttireColorEntry(plain, &groups, &group_index, &entry_index,
                            &leaves, &records, error)) return false;

  for (const WWE13AttireColorPort& port : ports) {
    const auto donor = records.find(port.donor_id);
    if (donor == records.end()) {
      std::ostringstream message;
      message << "The attire-color donor record " << port.donor_id << " is missing.";
      return Fail(error, message.str());
    }
    Bytes clone = donor->second;
    PutLe32(&clone, 0, port.target_id);
    records[port.target_id] = std::move(clone);
  }
  if (records.size() > std::numeric_limits<uint16_t>::max())
    return Fail(error, "The attire-color leaf has too many records.");

  Bytes color_leaf(2, 0);
  color_leaf[0] = static_cast<uint8_t>(records.size());
  color_leaf[1] = static_cast<uint8_t>(records.size() >> 8);
  for (const auto& [id, record] : records) {
    (void)id;
    if (record.size() != kAttireColorRecordSize)
      return Fail(error, "An attire-color record has an unexpected size.");
    color_leaf.insert(color_leaf.end(), record.begin(), record.end());
  }
  auto color = std::find_if(leaves.begin(), leaves.end(), [](const PachLeaf& leaf) {
    return leaf.id == 600;
  });
  if (color == leaves.end()) return Fail(error, "The attire-color leaf disappeared during conversion.");
  color->data = std::move(color_leaf);

  const Bytes pach = BuildAttirePach(leaves);
  const PacEntry& target_entry = groups[group_index].entries[entry_index];
  const std::string wanted_group = TrimField(groups[group_index].name.data(), groups[group_index].name.size());
  const std::string wanted_entry = TrimField(target_entry.name.data(), target_entry.name.size());
  bool patched = false;
  for (size_t gi = 0; gi < groups.size(); ++gi) {
    if (TrimField(groups[gi].name.data(), groups[gi].name.size()) != wanted_group) continue;
    for (size_t ei = 0; ei < groups[gi].entries.size(); ++ei) {
      if (TrimField(groups[gi].entries[ei].name.data(), groups[gi].entries[ei].name.size()) != wanted_entry)
        continue;
      groups[gi].entries[ei].data = pach;
      patched = true;
    }
  }
  if (!patched) return Fail(error, "The attire-color PACH entry could not be located for replacement.");
  return ReplacePacEntries(plain, groups, output, error);
}

bool ConvertWWE13StringKeys(const WWE13ConvertBytes& source_pac,
                            const WWE13StringKeyMap& keyed_strings,
                            WWE13ConvertBytes* output, size_t* inserted_keys,
                            std::string* error) {
  return ConvertWWE13StringKeysAt(source_pac, keyed_strings, kStringTableSentinel, output,
                                  inserted_keys, error);
}

bool ConvertWWE13StringKeysAt(const WWE13ConvertBytes& source_pac,
                              const WWE13StringKeyMap& keyed_strings, uint32_t sentinel,
                              WWE13ConvertBytes* output, size_t* inserted_keys,
                              std::string* error) {
  if (error) error->clear();
  if (!output || !inserted_keys) return Fail(error, "The SDB string output buffer is missing.");
  Bytes plain;
  if (!DecodePac(source_pac, &plain, error)) return false;
  std::vector<PacGroup> groups;
  if (!ParsePac(plain, &groups, error)) return false;
  size_t total_inserted = 0;
  for (PacGroup& group : groups) {
    for (PacEntry& entry : group.entries) {
      if (!HasMagic(entry.data, "PACH")) continue;
      std::vector<PachLeaf> leaves;
      if (!ParsePach(entry.data, &leaves, error)) return false;
      bool touched = false;
      for (PachLeaf& leaf : leaves) {
        if (!HasMagic(leaf.data, "ZLIB")) continue;
        Bytes sdb_plain;
        if (!DecodeSdbLeaf(leaf.data, &sdb_plain, error)) return false;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> table_entries;
        std::string table_error;
        if (!ParseSdbTable(sdb_plain, &table_entries, nullptr, &table_error) ||
            !table_entries.count(sentinel))
          continue;
        Bytes patched_plain;
        size_t inserted = 0;
        if (!InsertSdbKeys(sdb_plain, keyed_strings, &patched_plain, &inserted, error, sentinel)) return false;
        if (inserted == 0) continue;
        Bytes compressed;
        if (!EncodeSdbLeaf(leaf.data, patched_plain, &compressed, error)) return false;
        leaf.data = std::move(compressed);
        total_inserted += inserted;
        touched = true;
      }
      if (touched) entry.data = BuildAttirePach(leaves);
    }
  }
  if (!ReplacePacEntries(plain, groups, output, error)) return false;
  *inserted_keys = total_inserted;
  return true;
}

bool ReadWWE13StringKeyValues(const WWE13ConvertBytes& pac, WWE13StringKeyMap* values,
                              std::string* error) {
  if (error) error->clear();
  if (!values) return Fail(error, "The SDB key-value output map is missing.");
  Bytes plain;
  if (!DecodePac(pac, &plain, error)) return false;
  std::vector<PacGroup> groups;
  if (!ParsePac(plain, &groups, error)) return false;
  WWE13StringKeyMap result;
  for (const PacGroup& group : groups) {
    for (const PacEntry& entry : group.entries) {
      if (!HasMagic(entry.data, "PACH")) continue;
      std::vector<PachLeaf> leaves;
      if (!ParsePach(entry.data, &leaves, error)) return false;
      for (const PachLeaf& leaf : leaves) {
        if (!HasMagic(leaf.data, "ZLIB")) continue;
        Bytes sdb_plain;
        std::string decode_error;
        if (!DecodeSdbLeaf(leaf.data, &sdb_plain, &decode_error)) continue;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> table_entries;
        if (!ParseSdbTable(sdb_plain, &table_entries, nullptr, &decode_error)) continue;
        if (!ReadSdbKeyValues(sdb_plain, &result, error)) return false;
      }
    }
  }
  *values = std::move(result);
  return true;
}

bool ReadWWE13CharacterLeaf(const WWE13ConvertBytes& pac, const std::string& emd_name,
                            uint32_t leaf_id, WWE13ConvertBytes* output, std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The character leaf output buffer is missing.");
  return ReadLeaf(pac, emd_name, leaf_id, output, error);
}

bool ReadWWE13PacRows(const WWE13ConvertBytes& pac, const std::string& wanted_group,
                      const std::string& wanted_table, WWE13RawRowMap* rows, std::string* error) {
  if (error) error->clear();
  if (!rows) return Fail(error, "The chEtc row output map is missing.");
  Bytes plain;
  if (!DecodePac(pac, &plain, error)) return false;
  std::vector<PacGroup> groups;
  if (!ParsePac(plain, &groups, error)) return false;
  for (const PacGroup& group : groups) {
    if (TrimField(group.name.data(), group.name.size()) != wanted_group) continue;
    for (const PacEntry& entry : group.entries) {
      if (TrimField(entry.name.data(), entry.name.size()) != wanted_table) continue;
      Bytes pach;
      if (!DecodePach(entry.data, &pach, error)) return false;
      std::vector<PachLeaf> leaves;
      if (!ParsePach(pach, &leaves, error)) return false;
      WWE13RawRowMap result;
      for (const PachLeaf& leaf : leaves) {
        Bytes row;
        if (HasMagic(leaf.data, "BPE ")) {
          std::string detail;
          if (!internal::DecodeWweBpe(leaf.data, row, detail))
            return Fail(error, "A chEtc row could not be decoded: " + detail);
        } else {
          row = leaf.data;
        }
        if (!result.emplace(leaf.id, std::move(row)).second)
          return Fail(error, "The chEtc table contains a duplicate row ID.");
      }
      *rows = std::move(result);
      return true;
    }
  }
  return Fail(error, "The requested chEtc table is missing.");
}

bool ConvertWWE13CharacterPac(const WWE13CharacterPacRecipe& recipe,
                              const WWE13ConvertBytes& source_wwe13_pac,
                              const WWE13ConvertBytes* leaf_10000_source_wwe13_pac,
                              const WWE13ConvertBytes* stock_model_pac,
                              const WWE13ConvertBytes* stock_body_pac,
                              const WWE13ConvertBytes* shared_leaf50,
                              WWE13ConvertBytes* output, std::string* error) {
  if (error) error->clear();
  if (!output) return Fail(error, "The converted character PAC output buffer is missing.");
  if (recipe.source_id == 0 || recipe.target_id == 0 || recipe.source_id > 999999 || recipe.target_id > 999999)
    return Fail(error, "The character PAC recipe has an invalid source or target ID.");
  if (recipe.leaf_10000_source_id && !leaf_10000_source_wwe13_pac)
    return Fail(error, "The recipe requires the selected leaf 10000 source PAC.");
  if (recipe.stock_model_source_id && !stock_model_pac)
    return Fail(error, "The recipe requires the selected stock model PAC.");
  if (recipe.stock_body_source_id && !stock_body_pac)
    return Fail(error, "The recipe requires the selected stock body PAC.");

  Bytes source_plain;
  if (!DecodePac(source_wwe13_pac, &source_plain, error)) return false;
  std::vector<PacGroup> source_groups;
  if (!ParsePac(source_plain, &source_groups, error)) return false;
  std::vector<PacEntry> entries;
  for (const PacGroup& group : source_groups) {
    if (TrimField(group.name.data(), group.name.size()) != "EMD")
      return Fail(error, "The source character PAC contains a non-EMD group.");
    entries.insert(entries.end(), group.entries.begin(), group.entries.end());
  }

  std::vector<PachLeaf> replacement_leaves;
  if (recipe.leaf_10000_source_id &&
      !ReadSourceEntryLeaves(leaf_10000_source_wwe13_pac, *recipe.leaf_10000_source_id,
                             &replacement_leaves, error, "leaf 10000 source")) return false;
  std::vector<PachLeaf> model_leaves;
  if (recipe.stock_model_source_id &&
      !ReadSourceEntryLeaves(stock_model_pac, *recipe.stock_model_source_id,
                             &model_leaves, error, "stock model")) return false;
  std::vector<PachLeaf> body_leaves;
  if (recipe.stock_body_source_id &&
      !ReadSourceEntryLeaves(stock_body_pac, *recipe.stock_body_source_id,
                             &body_leaves, error, "stock body")) return false;

  std::vector<PacEntry> converted_entries;
  converted_entries.reserve(entries.size() + 1);
  for (const PacEntry& entry : entries) {
    PacEntry converted;
    bool omit = false;
    if (!ConvertEntry(entry, recipe,
                      recipe.leaf_10000_source_id ? &replacement_leaves : nullptr,
                      recipe.stock_model_source_id ? &model_leaves : nullptr,
                      recipe.stock_body_source_id ? &body_leaves : nullptr,
                      shared_leaf50, &converted, &omit, error)) return false;
    if (!omit) converted_entries.push_back(std::move(converted));
  }

  std::ostringstream primary_name;
  primary_name.width(6); primary_name.fill('0'); primary_name << recipe.target_id << "02";
  auto primary = std::find_if(converted_entries.begin(), converted_entries.end(), [&](const PacEntry& entry) {
    return NameString(entry.name) == primary_name.str();
  });
  if (primary == converted_entries.end())
    return Fail(error, "The converted character PAC has no primary 02 entry.");

  const uint32_t initial_directory_size = static_cast<uint32_t>(12 + converted_entries.size() * 16);
  std::ostringstream secondary_name;
  secondary_name.width(6); secondary_name.fill('0'); secondary_name << recipe.target_id << "04";
  const std::string name04 = secondary_name.str();
  const bool added_secondary = std::none_of(converted_entries.begin(), converted_entries.end(), [&](const PacEntry& entry) {
    return NameString(entry.name) == name04;
  });
  if (added_secondary) {
    PacEntry copy = *primary;
    std::copy(name04.begin(), name04.end(), copy.name.begin());
    converted_entries.push_back(std::move(copy));
    std::stable_sort(converted_entries.begin(), converted_entries.end(), [](const PacEntry& left, const PacEntry& right) {
      return left.name < right.name;
    });
  }

  return BuildEpk8(converted_entries, initial_directory_size, 0, output, error);
}


bool DecodeWWE13PacPlain(const WWE13ConvertBytes& pac, WWE13ConvertBytes* plain, std::string* error) {
  if (error) error->clear();
  return DecodePac(pac, plain, error);
}

bool DecodeWWE13ZlibLeaf(const WWE13ConvertBytes& leaf, WWE13ConvertBytes* plain, std::string* error) {
  if (error) error->clear();
  return DecodeSdbLeaf(leaf, plain, error);
}

bool EncodeWWE13ZlibLeaf(const WWE13ConvertBytes& original_leaf, const WWE13ConvertBytes& plain,
                         WWE13ConvertBytes* leaf, std::string* error) {
  if (error) error->clear();
  return EncodeSdbLeaf(original_leaf, plain, leaf, error);
}

}  // namespace wwe2k14::launcher
