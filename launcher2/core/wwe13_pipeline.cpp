#include "wwe13_pipeline.h"
#include "havok_convert.h"
#include "grid_faces.h"
#include "internal/util.h"
#include "bink_convert.h"
#include "path_case.h"

#include <algorithm>
#include <thread>
#include <mutex>
#include <cstdlib>
#include <atomic>
#include <cstdio>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

#include "internal/miniz_api.h"

namespace wwe2k14::launcher {
namespace fs = std::filesystem;
namespace {

using Bytes = WWE13ConvertBytes;
thread_local const CancelFlag* g_import_cancel = nullptr;
thread_local const ProgressFn* g_import_progress = nullptr;

bool Fail(std::string* error, const std::string& message);

bool Cancelled(std::string* error) {
  if (!g_import_cancel || !g_import_cancel->load(std::memory_order_relaxed)) return false;
  Fail(error, "WWE '13 import cancelled.");
  return true;
}

void ReportStage(uint64_t stage, const std::string& label) {
  if (g_import_progress && *g_import_progress) (*g_import_progress)(stage, 9, label);
}

uint16_t Le16(const Bytes& bytes, size_t at) {
  return static_cast<uint16_t>(bytes.at(at) | (uint16_t(bytes.at(at + 1)) << 8));
}
uint32_t Le32(const Bytes& bytes, size_t at) {
  return uint32_t(bytes.at(at)) | (uint32_t(bytes.at(at + 1)) << 8) |
         (uint32_t(bytes.at(at + 2)) << 16) | (uint32_t(bytes.at(at + 3)) << 24);
}
void PutLe16(Bytes* bytes, size_t at, uint16_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value);
  (*bytes)[at + 1] = static_cast<uint8_t>(value >> 8);
}
void PutLe32(Bytes* bytes, size_t at, uint32_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value);
  (*bytes)[at + 1] = static_cast<uint8_t>(value >> 8);
  (*bytes)[at + 2] = static_cast<uint8_t>(value >> 16);
  (*bytes)[at + 3] = static_cast<uint8_t>(value >> 24);
}
bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}
std::string Trim(std::string text) {
  while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) text.pop_back();
  return text;
}
std::string Id(uint32_t value, size_t width = 0) {
  std::ostringstream stream;
  if (width) stream.width(static_cast<std::streamsize>(width));
  if (width) stream.fill('0');
  stream << value;
  return stream.str();
}
Bytes EncodedName(const std::string& value, size_t width, uint8_t pad = ' ') {
  Bytes result(width, pad);
  std::copy_n(value.begin(), std::min(value.size(), width), result.begin());
  return result;
}
std::string RawName(const std::string& value) { return Trim(value); }
bool StartsWith(const Bytes& value, const char* magic) {
  return value.size() >= 4 && std::equal(value.begin(), value.begin() + 4,
      reinterpret_cast<const uint8_t*>(magic));
}
bool ReadFile(const fs::path& path, Bytes* output, std::string* error) {
  if (Cancelled(error)) return false;
  if (!output) return Fail(error, "The file output buffer is missing.");
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec) return Fail(error, "Could not read input file '" + internal::PathToUtf8(path) + "': " + ec.message());
  if (size > 512ull * 1024 * 1024)
    return Fail(error, "Input file exceeds the 512 MiB safety limit: " + internal::PathToUtf8(path.filename()));
  std::ifstream input(path, std::ios::binary);
  if (!input) return Fail(error, "Could not open input file '" + internal::PathToUtf8(path) + "'.");
  Bytes data(static_cast<size_t>(size));
  if (size) input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
  if (!input && size) return Fail(error, "Could not read input file '" + internal::PathToUtf8(path) + "'.");
  if (Cancelled(error)) return false;
  *output = std::move(data);
  return true;
}
bool WriteFile(const fs::path& path, const Bytes& bytes, std::string* error) {
  if (Cancelled(error)) return false;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return Fail(error, "Could not create staging directory '" + internal::PathToUtf8(path.parent_path()) + "': " + ec.message());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return Fail(error, "Could not create staging file '" + internal::PathToUtf8(path) + "'.");
  if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!output) return Fail(error, "Could not write staging file '" + internal::PathToUtf8(path) + "'.");
  if (Cancelled(error)) return false;
  return true;
}

WWE13PacGroupData* FindGroup(std::vector<WWE13PacGroupData>* groups, const std::string& wanted) {
  for (auto& group : *groups) if (Trim(group.name) == wanted) return &group;
  return nullptr;
}
WWE13PacEntryData* FindEntry(WWE13PacGroupData* group, const std::string& wanted) {
  if (!group) return nullptr;
  for (auto& entry : group->entries) if (RawName(entry.name) == wanted) return &entry;
  return nullptr;
}
const WWE13PacEntryData* FindEntry(const WWE13PacGroupData* group, const std::string& wanted) {
  if (!group) return nullptr;
  for (const auto& entry : group->entries) if (RawName(entry.name) == wanted) return &entry;
  return nullptr;
}

bool DecodePachEntry(const WWE13PacEntryData& entry, std::vector<WWE13PachLeafData>* leaves,
                     std::string* error) {
  Bytes plain;
  if (!DecodeWWE13Bpe(entry.data, &plain, error)) return false;
  return ReadWWE13PachLeaves(plain, leaves, error);
}
std::string RegistryLeafContext(const char* table, uint32_t leaf_id,
                                const std::string& file_name) {
  return "The " + std::string(table) + " registry table (leaf " +
      std::to_string(leaf_id) + ") in " + file_name;
}

std::string InvalidRegistryRowCount(const char* table, uint32_t leaf_id,
                                    const std::string& file_name, size_t count,
                                    size_t header_size, size_t row_size,
                                    size_t actual_size) {
  const size_t required_size = header_size + count * row_size;
  return RegistryLeafContext(table, leaf_id, file_name) +
      " has an invalid row count: " + std::to_string(count) +
      (count == 1 ? " row needs " : " rows need ") +
      std::to_string(required_size) + " bytes, the table has " +
      std::to_string(actual_size) + " bytes.";
}

bool ParseLsd(const Bytes& raw, std::vector<std::array<uint16_t, 4>>* rows,
              const std::string& file_name, uint32_t leaf_id,
              std::string* error) {
  if (!rows) return Fail(error, "The LSD registry output rows are missing.");
  if (raw.size() < 2)
    return Fail(error, RegistryLeafContext("LSD", leaf_id, file_name) +
        " is truncated: it has fewer than 2 bytes for its row count.");
  const size_t count = Le16(raw, 0);
  const size_t required_size = 2 + count * 8;
  if (raw.size() < required_size ||
      !std::all_of(raw.begin() + static_cast<std::ptrdiff_t>(required_size), raw.end(),
                   [](uint8_t value) { return value == 0; }))
    return Fail(error, InvalidRegistryRowCount("LSD", leaf_id, file_name,
                                               count, 2, 8, raw.size()));
  std::vector<std::array<uint16_t, 4>> result;
  result.reserve(count);
  std::set<uint16_t> ids;
  for (size_t i = 0; i < count; ++i) {
    std::array<uint16_t, 4> row{};
    for (size_t c = 0; c < 4; ++c) row[c] = Le16(raw, 2 + i * 8 + c * 2);
    if (!ids.insert(row[0]).second)
      return Fail(error, RegistryLeafContext("LSD", leaf_id, file_name) +
          " contains duplicate ID " + std::to_string(row[0]) + ".");
    result.push_back(row);
  }
  *rows = std::move(result);
  return true;
}
Bytes MakeLsd(const std::vector<std::array<uint16_t, 4>>& rows) {
  Bytes output(2 + rows.size() * 8, 0);
  PutLe16(&output, 0, static_cast<uint16_t>(rows.size()));
  for (size_t i = 0; i < rows.size(); ++i)
    for (size_t c = 0; c < 4; ++c) PutLe16(&output, 2 + i * 8 + c * 2, rows[i][c]);
  return output;
}

}  // namespace

bool ConvertWWE13DlcRegistry(const WWE13ConvertBytes& source_pac,
                             const std::vector<WWE13RegistryChar>& chars,
                             const std::string& file_name,
                             WWE13ConvertBytes* output, std::string* error) {
  if (error) error->clear();
  if (!output || chars.empty()) return Fail(error, "The registry conversion requires output and at least one character.");
  std::set<uint32_t> target_ids;
  for (const WWE13RegistryChar& ch : chars) {
    if (ch.target_id < 100 || ch.target_id > 499 || ch.rank_lsd0 == 0 || ch.rank_lsd2 == 0)
      return Fail(error, "A DLC registry character has an unsupported ID or a zero rank.");
    if (!target_ids.insert(ch.target_id).second)
      return Fail(error, "The DLC registry character list contains a duplicate target ID.");
  }
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(source_pac, &groups, error)) return false;
  WWE13PacGroupData* enex = FindGroup(&groups, "ENEX");
  WWE13PacEntryData* lsd = FindEntry(enex, "LSD");
  WWE13PacEntryData* bin = FindEntry(enex, "BIN");
  if (!lsd || !bin) return Fail(error, "The DLC registry is missing ENEX/LSD or ENEX/BIN.");
  WWE13PacEntryData original_lsd = *lsd;
  WWE13PacEntryData original_bin = *bin;
  const auto lsd0 = [](const WWE13RegistryChar& ch) {
    return std::array<uint16_t, 4>{static_cast<uint16_t>(ch.target_id),
      static_cast<uint16_t>(ch.target_id + 1100), ch.rank_lsd0, 0};
  };
  const auto lsd2 = [](const WWE13RegistryChar& ch) {
    return std::array<uint16_t, 4>{static_cast<uint16_t>(ch.target_id),
      static_cast<uint16_t>(ch.target_id + 1100), ch.rank_lsd2, 0};
  };
  const auto ordinary_lsd = [](uint32_t offset) {
    return [offset](const WWE13RegistryChar& ch) {
      return std::array<uint16_t, 4>{static_cast<uint16_t>(ch.target_id + offset),
        static_cast<uint16_t>(ch.target_id + 1100),
        static_cast<uint16_t>(60000 + ch.target_id), 0};
    };
  };
  std::vector<WWE13PachLeafData> lsd_leaves;
  if (!DecodePachEntry(original_lsd, &lsd_leaves, error)) return false;
  std::set<uint32_t> found_lsd;
  for (WWE13PachLeafData& leaf : lsd_leaves) {
    std::function<std::array<uint16_t, 4>(const WWE13RegistryChar&)> make_row;
    if (leaf.id == 0) make_row = lsd0;
    else if (leaf.id == 2) make_row = lsd2;
    else if (leaf.id == 3 || leaf.id == 17) make_row = ordinary_lsd(0);
    else if (leaf.id == 8) make_row = ordinary_lsd(11000);
    else if (leaf.id == 9) make_row = ordinary_lsd(12000);
    else if (leaf.id == 10) make_row = ordinary_lsd(13000);
    else if (leaf.id == 11) make_row = ordinary_lsd(14000);
    else if (leaf.id == 16) make_row = ordinary_lsd(10000);
    else continue;
    found_lsd.insert(leaf.id);
    Bytes decoded;
    if (!DecodeWWE13Bpe(leaf.data, &decoded, error)) return false;
    std::vector<std::array<uint16_t, 4>> rows;
    if (!ParseLsd(decoded, &rows, file_name, leaf.id, error)) return false;
    std::set<uint16_t> have;
    for (const auto& row : rows) have.insert(row[0]);
    for (const WWE13RegistryChar& ch : chars) {
      const auto row = make_row(ch);
      if (!have.insert(row[0]).second)
        return Fail(error, "LSD registry already contains character ID " + std::to_string(row[0]) + ".");
      rows.push_back(row);
    }
    leaf.data = StoreWWE13BpeLiteral(MakeLsd(rows));
  }
  for (uint32_t id : {0u, 2u, 3u, 8u, 9u, 10u, 11u, 16u, 17u})
    if (!found_lsd.count(id)) return Fail(error, "The DLC registry LSD is missing required leaf " + std::to_string(id) + ".");
  Bytes patched_lsd;
  if (!BuildWWE13PachLeaves(lsd_leaves, &patched_lsd, true, error)) return false;
  lsd->data = std::move(patched_lsd);

  std::vector<WWE13PachLeafData> bin_leaves;
  if (!DecodePachEntry(original_bin, &bin_leaves, error)) return false;
  std::set<uint32_t> found_bin;
  for (WWE13PachLeafData& leaf : bin_leaves) {
    if (leaf.id > 4) continue;
    found_bin.insert(leaf.id);
    Bytes decoded;
    if (!DecodeWWE13Bpe(leaf.data, &decoded, error)) return false;
    const uint32_t base = 10000 + uint32_t(leaf.id) * 1000;
    // The five BIN leaves are numbered 0..4 and correspond to base IDs 10000..14000.
    if (decoded.size() < 16)
      return Fail(error, RegistryLeafContext("BIN", leaf.id, file_name) +
          " is truncated: it has fewer than 16 bytes for its row count and header.");
    const size_t count = Le16(decoded, 0);
    if (decoded.size() != 16 + count * 20)
      return Fail(error, InvalidRegistryRowCount("BIN", leaf.id, file_name,
                                                 count, 16, 20, decoded.size()));
    std::vector<std::array<uint8_t, 20>> rows(count);
    for (size_t i = 0; i < count; ++i)
      std::copy_n(decoded.begin() + static_cast<std::ptrdiff_t>(16 + i * 20), 20, rows[i].begin());
    std::optional<size_t> last;
    for (size_t i = 0; i < rows.size(); ++i) {
      const uint16_t pack = static_cast<uint16_t>(rows[i][16] | (uint16_t(rows[i][17]) << 8));
      if (pack >= 1100 && pack < 65535) last = i;
    }
    if (!last) return Fail(error, "A BIN registry table has no DLC insertion boundary.");
    std::vector<std::array<uint8_t, 20>> additions;
    additions.reserve(chars.size());
    for (const WWE13RegistryChar& ch : chars) {
      std::array<uint8_t, 20> row{};
      const uint32_t name_key = 60000 + ch.target_id;
      for (size_t b = 0; b < 4; ++b) row[4 + b] = static_cast<uint8_t>(name_key >> (b * 8));
      const uint16_t movie = static_cast<uint16_t>(base + ch.target_id);
      row[8] = static_cast<uint8_t>(movie); row[9] = static_cast<uint8_t>(movie >> 8);
      row[10] = 0x88; row[11] = 0x13;  // u16 5000
      row[14] = 100;
      const uint16_t pack = static_cast<uint16_t>(ch.target_id + 1100);
      row[16] = static_cast<uint8_t>(pack); row[17] = static_cast<uint8_t>(pack >> 8);
      additions.push_back(row);
    }
    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(*last + 1), additions.begin(), additions.end());
    if (rows.size() > 0xffff) return Fail(error, "A BIN registry table exceeds its 16-bit row limit.");
    Bytes table(decoded.begin(), decoded.begin() + 16);
    PutLe16(&table, 0, static_cast<uint16_t>(rows.size()));
    for (size_t i = 0; i < rows.size(); ++i) {
      const uint32_t ordinal = static_cast<uint32_t>(i);
      for (size_t b = 0; b < 4; ++b) rows[i][b] = static_cast<uint8_t>(ordinal >> (b * 8));
      table.insert(table.end(), rows[i].begin(), rows[i].end());
    }
    leaf.data = StoreWWE13BpeLiteral(table);
  }
  for (uint32_t id = 0; id <= 4; ++id)
    if (!found_bin.count(id)) return Fail(error, "The DLC registry BIN is missing required leaf " + std::to_string(id) + ".");
  Bytes patched_bin;
  if (!BuildWWE13PachLeaves(bin_leaves, &patched_bin, true, error)) return false;
  bin->data = std::move(patched_bin);
  return RebuildWWE13PacGroups(source_pac, groups, output, error);
}

namespace {

constexpr const char* kHostPack = "745BEE31BC19100A3EE21606E089EC25F739099C54";
constexpr const char* kExtraOfferPack = "267E5576B180BEBEABD7BE8863B0F5D62D7D37DE54";
constexpr std::array<uint16_t, 11> kDeadOffers{105, 106, 317, 320, 277, 330, 331, 497, 424, 498, 499};
constexpr uint32_t kGameTitle = 0x545408B2;

struct PackageFileInfo {
  std::string relative;
  std::string kind;
  std::vector<WWE13PacGroupData> groups;
  fs::path output_path;
};

struct OutputWriter {
  fs::path files_root;
  std::set<std::string> files;
  std::string* error = nullptr;
  bool Put(const std::string& relative, const Bytes& bytes) {
    if (Cancelled(error)) return false;
    if (relative.empty() || relative.find("..") != std::string::npos || relative[0] == '/')
      return Fail(error, "The staging output has an unsafe relative path.");
    if (!WriteFile(files_root / fs::path(relative), bytes, error)) return false;
    files.insert(relative);
    return true;
  }
};

fs::path TitleRoot(const fs::path& userdata) {
  return userdata / "0000000000000000" / "545408B2" / "00000002";
}
std::vector<fs::path> ChildDirectories(const fs::path& directory) {
  std::vector<fs::path> result;
  std::error_code ec;
  if (!fs::is_directory(directory, ec) || ec) return result;
  for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code type_error;
    if (it->is_directory(type_error) && !type_error) result.push_back(it->path());
  }
  std::sort(result.begin(), result.end());
  return result;
}
std::string AsciiLower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}
std::vector<fs::path> FilesWithPrefix(const fs::path& directory, const std::string& prefix,
                                      const std::string& extension = ".pac") {
  std::vector<fs::path> result;
  std::error_code ec;
  if (!fs::is_directory(directory, ec) || ec) return result;
  for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code file_error;
    if (!it->is_regular_file(file_error) || file_error) continue;
    const std::string filename = internal::PathToUtf8(it->path().filename());
    if (AsciiLower(filename).rfind(AsciiLower(prefix), 0) == 0 &&
        (extension.empty() || AsciiLower(internal::PathToUtf8(it->path().extension())) == AsciiLower(extension)))
      result.push_back(it->path());
  }
  std::sort(result.begin(), result.end());
  return result;
}
bool HasStringOverlay(const fs::path& package) {
  return !FilesWithPrefix(ResolvePathCase(package, "pac"), "string_dlc").empty();
}
std::vector<fs::path> OverlayPackages(const WWE13ImportPaths& paths) {
  std::vector<fs::path> result;
  for (const fs::path& package : ChildDirectories(TitleRoot(paths.userdata_root)))
    if (HasStringOverlay(package)) result.push_back(package);
  return result;
}
std::string PackCode(const fs::path& package) { return internal::PathToUtf8(package.filename()).substr(0, 4); }
std::string UserdataPath(const std::string& package, const std::string& relative) {
  return "userdata/0000000000000000/545408B2/00000002/" + package + "/" + relative;
}
std::string HostOutputPack(const std::string& host = kHostPack) { return WWE13HostOutputPack(host); }
// Any version of pack 2 (offer pack): the standard 32-offer Add-On 2 (745BEE31..) or the 4-offer one (267E5576.., the
// pre-order bonuses only). #103/#111/#118/#120 (2026-11-10): players with only the 4-offer version could not import.
// Offers per add-on pack. The game itself locked pack positions 32+ (licence bit 1 << position is 0 there); the game
// build with src/dlc_offer_mask.cpp (2026-11-15) lists them, so imports may fill each pack to 64.
constexpr uint16_t kAddOnOfferCap = 64;
bool IsPack2Catalog(const Bytes& catalog) {
  if (catalog.size() < 0x1c || catalog[0] != 'D' || catalog[1] != 'L' || catalog[2] != 'C' || catalog[3] != 'C') return false;
  const uint16_t pack_id = Le16(catalog, 8), count = Le16(catalog, 12);
  return pack_id == 2 && count <= kAddOnOfferCap && uint64_t(0x1c) + uint64_t(count) * 0x40 <= catalog.size();
}
bool IsHostCatalog(const Bytes& catalog) {
  if (catalog.size() < 0x1c || catalog[0] != 'D' || catalog[1] != 'L' || catalog[2] != 'C' || catalog[3] != 'C') return false;
  const uint16_t pack_id = Le16(catalog, 8), count = Le16(catalog, 12);
  if (pack_id != 2 || uint64_t(0x1c) + uint64_t(count) * 0x40 > catalog.size()) return false;
  for (uint16_t i = 0; i < count; ++i)
    if (Le16(catalog, 0x1c + size_t(i) * 0x40 + 16) == 338) return true;
  return false;
}

bool ReadRows(const fs::path& file, const std::string& table, WWE13RawRowMap* rows,
              std::string* error) {
  Bytes bytes;
  if (!ReadFile(file, &bytes, error)) return false;
  if (!ReadWWE13PacRows(bytes, "CHAR", table, rows, error)) {
    const std::string detail = *error;
    return Fail(error, internal::PathToUtf8(file.filename()) + ": " + detail);
  }
  return true;
}
void AddDatIndexes(const WWE13RawRowMap& rows, std::set<uint16_t>* indexes) {
  for (const auto& [id, row] : rows) {
    (void)id;
    if (row.size() >= 8) indexes->insert(Le16(row, 6));
  }
}

bool FindSourceCharacter(const WWE13ImportPaths& paths, uint32_t id, fs::path* output) {
  if (!output) return false;
  std::vector<fs::path> packs = paths.wwe13_pack_roots;
  std::sort(packs.begin(), packs.end());
  if (id < 100) {
    for (const fs::path& pack : packs) {
      const fs::path directory = ResolvePathCase(pack, "pac/ch");
      const std::string prefix = "ch" + std::to_string(id) + "_dlc";
      const auto files = FilesWithPrefix(directory, prefix);
      if (!files.empty()) { *output = files.front(); return true; }
    }
  }
  const fs::path base = ResolvePathCase(paths.wwe13_root, "pac/ch/" + ("ch" + std::to_string(id) + ".pac"));
  std::error_code ec;
  if (fs::is_regular_file(base, ec) && !ec) { *output = base; return true; }
  return false;
}
bool FindSourceRoot(const WWE13ImportPaths& paths, uint32_t id, fs::path* root) {
  fs::path character;
  if (!FindSourceCharacter(paths, id, &character)) return false;
  *root = character.parent_path().parent_path().parent_path();
  return true;
}
bool ReadSourceRows(const WWE13ImportPaths& paths, const std::vector<WWE13ImportPort>& ports,
                    WWE13RowsInput* rows, std::string* error) {
  const fs::path source_etc = ResolvePathCase(paths.wwe13_root, "pac/ch/chEtc.pac");
  WWE13RawRowMap base_dat, base_pro;
  if (!ReadRows(source_etc, "DAT", &base_dat, error) ||
      !ReadRows(source_etc, "PRO", &base_pro, error)) return false;
  rows->training13_pro = base_pro;
  rows->source13_dat = base_dat;
  rows->source13_pro = base_pro;
  std::set<fs::path> source_pack_tables;
  for (const WWE13ImportPort& port : ports) {
    fs::path character;
    if (!FindSourceCharacter(paths, port.source_id, &character))
      return Fail(error, "The WWE '13 source PAC is missing for ID " + std::to_string(port.source_id) + ".");
    const std::string name = internal::PathToUtf8(character.filename());
    const size_t dlc = name.find("_dlc");
    if (dlc == std::string::npos || dlc + 6 > name.size()) continue;
    source_pack_tables.insert(ResolvePathCase(character.parent_path().parent_path().parent_path(),
                                              "pac/ch/" + ("chEtc" + name.substr(dlc, 6) + ".pac")));
  }
  for (const fs::path& path : source_pack_tables) {
    WWE13RawRowMap pack_dat, pack_pro;
    if (!ReadRows(path, "DAT", &pack_dat, error) || !ReadRows(path, "PRO", &pack_pro, error)) return false;
    for (const WWE13ImportPort& port : ports) {
      const auto dat = pack_dat.find(port.source_id);
      const auto pro = pack_pro.find(port.source_id);
      if (dat != pack_dat.end()) rows->source13_dat[port.source_id] = dat->second;
      if (pro != pack_pro.end()) rows->source13_pro[port.source_id] = pro->second;
    }
  }
  return true;
}
bool ReadGameRows(const WWE13ImportPaths& paths, const std::vector<fs::path>& overlays,
                  WWE13RowsInput* rows, std::string* error) {
  const fs::path base_etc = paths.wwe14_root / "pac/ch/chEtc.pac";
  if (!ReadRows(base_etc, "DAT", &rows->game14_dat, error) ||
      !ReadRows(base_etc, "DSBD", &rows->game14_dsbd, error) ||
      !ReadRows(base_etc, "PRO", &rows->game14_pro, error)) return false;
  AddDatIndexes(rows->game14_dat, &rows->occupied_dat_indices);
  for (const fs::path& package : overlays) {
    for (const fs::path& path : FilesWithPrefix(ResolvePathCase(package, "pac/ch"), "chEtc_dlc")) {
      WWE13RawRowMap dat;
      if (!ReadRows(path, "DAT", &dat, error)) return false;
      AddDatIndexes(dat, &rows->occupied_dat_indices);
    }
  }
  return true;
}

bool PatchChEtc(const Bytes& source, const std::vector<WWE13ConvertedRows>& rows,
                Bytes* output, std::string* error) {
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(source, &groups, error)) return false;
  if (!FindGroup(&groups, "CHAR")) return Fail(error, "The chEtc PAC is missing its CHAR group.");
  for (WWE13PacGroupData& group : groups) {
    const bool required_table = Trim(group.name) == "CHAR";
    for (WWE13PacEntryData& table : group.entries) {
      const std::string section = Trim(table.name);
      const size_t row_kind = section == "DAT" ? 0 : section == "DSBD" ? 1 : section == "PRO" ? 2 : 3;
      if (row_kind == 3) continue;
      Bytes pach;
      if (!DecodeWWE13Bpe(table.data, &pach, error)) return false;
      std::vector<WWE13PachLeafData> leaves;
      if (!ReadWWE13PachLeaves(pach, &leaves, error)) return false;
      for (const WWE13ConvertedRows& converted : rows) {
        const Bytes* replacement = row_kind == 0 ? &converted.dat : row_kind == 1 ? &converted.dsbd : &converted.pro;
        auto leaf = std::find_if(leaves.begin(), leaves.end(), [&](const WWE13PachLeafData& value) {
          return value.id == converted.target_id;
        });
        if (leaf == leaves.end()) {
          if (required_table)
            return Fail(error, "The CHAR/" + section + " table has no target row " + std::to_string(converted.target_id) + ".");
          continue;
        }
        leaf->data = StoreWWE13BpeLiteral(*replacement);
      }
      Bytes rebuilt;
      if (!BuildWWE13PachLeaves(leaves, &rebuilt, true, error)) return false;
      table.data = std::move(rebuilt);
    }
  }
  return RebuildWWE13PacGroups(source, groups, output, error);
}

bool WritePacFile(OutputWriter* writer, const std::string& relative, const Bytes& bytes,
                  std::string* error) {
  if (!writer) return Fail(error, "The staging writer is missing.");
  return writer->Put(relative, bytes);
}

bool ConvertRowsForPorts(const WWE13ImportPaths& paths, const std::vector<WWE13ImportPort>& ports,
                         const std::vector<fs::path>& overlays, bool own_codes,
                         std::vector<WWE13ConvertedRows>* converted, std::string* error) {
  WWE13RowsInput input;
  if (!ReadSourceRows(paths, ports, &input, error) || !ReadGameRows(paths, overlays, &input, error)) return false;
  std::vector<WWE13RowsPort> choices;
  choices.reserve(ports.size());
  for (const WWE13ImportPort& port : ports) {
    WWE13RowsPort spec;
    spec.source_id = port.source_id;
    spec.target_id = port.target_id;
    spec.donor_id = port.donor_id;
    spec.attrs = port.attrs;
    spec.index = port.index;
    spec.own_pro = port.own_pro;
    choices.push_back(std::move(spec));
  }
  WWE13RowsOptions options;
  options.own_attire_codes = own_codes;
  return ConvertWWE13Rows(choices, input, options, converted, error);
}

void MakeNameMap(const std::vector<WWE13ImportPort>& ports, WWE13StringKeyMap* names) {
  for (const WWE13ImportPort& port : ports) {
    const Bytes name(port.name.begin(), port.name.end());
    const Bytes handle(port.handle.begin(), port.handle.end());
    for (const uint32_t base : {60000u, 62000u, 64000u}) (*names)[base + port.target_id] = name;
    (*names)[79000 + port.target_id] = handle;
  }
}

bool ConvertStringPacFile(const fs::path& input, const WWE13StringKeyMap& names,
                          Bytes* output, std::string* error) {
  Bytes source;
  if (!ReadFile(input, &source, error)) return false;
  size_t inserted = 0;
  return ConvertWWE13StringKeys(source, names, output, &inserted, error);
}

bool ConvertAttirePacFile(const fs::path& input, const std::vector<WWE13AttireColorPort>& ports,
                          Bytes* output, std::string* error) {
  Bytes source;
  if (!ReadFile(input, &source, error)) return false;
  return ConvertWWE13AttireColors(source, ports, output, error);
}

}  // namespace

namespace {

bool IsNumber(const std::string& text, uint32_t* value) {
  if (text.empty()) return false;
  uint64_t parsed = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10 + static_cast<uint32_t>(ch - '0');
    if (parsed > std::numeric_limits<uint16_t>::max()) return false;
  }
  if (value) *value = static_cast<uint32_t>(parsed);
  return true;
}
std::string UpperAscii(std::string value) {
  for (char& ch : value) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  return value;
}
std::string BytesToString(const Bytes& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool CollectNames(const WWE13ImportPaths& paths, const std::vector<fs::path>& overlays,
                  std::map<uint32_t, std::string>* names, std::string* error) {
  std::vector<fs::path> paths_to_read{paths.wwe14_root / "pac/string.pac"};
  for (const fs::path& package : overlays)
    for (const fs::path& file : FilesWithPrefix(ResolvePathCase(package, "pac"), "string_dlc"))
      paths_to_read.push_back(file);
  for (const fs::path& file : paths_to_read) {
    Bytes pac;
    if (!ReadFile(file, &pac, error)) return false;
    WWE13StringKeyMap values;
    if (!ReadWWE13StringKeyValues(pac, &values, error)) return false;
    for (const auto& [key, value] : values) {
      if (key < 60000 || key >= 60500 || names->count(key)) continue;
      Bytes clean = value;
      while (!clean.empty() && clean.back() == 0) clean.pop_back();
      (*names)[key] = BytesToString(clean);
    }
  }
  return true;
}

bool ReadRegistryLsd(const Bytes& pac, const std::string& file_name, uint32_t leaf_id,
                     std::vector<std::array<uint16_t, 4>>* rows, std::string* error) {
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(pac, &groups, error)) return false;
  const WWE13PacGroupData* enex = nullptr;
  for (const auto& group : groups) if (Trim(group.name) == "ENEX") enex = &group;
  const WWE13PacEntryData* lsd = FindEntry(enex, "LSD");
  if (!lsd) return Fail(error, "The DLC registry is missing ENEX/LSD.");
  std::vector<WWE13PachLeafData> leaves;
  if (!DecodePachEntry(*lsd, &leaves, error)) return false;
  for (const auto& leaf : leaves) if (leaf.id == leaf_id) {
    Bytes decoded;
    if (!DecodeWWE13Bpe(leaf.data, &decoded, error)) return false;
    return ParseLsd(decoded, rows, file_name, leaf_id, error);
  }
  return Fail(error, "The DLC registry LSD table is missing leaf " + std::to_string(leaf_id) + ".");
}

uint16_t AutoRank(const std::vector<std::array<uint16_t, 4>>& rows,
                  const std::map<uint32_t, std::string>& names,
                  const std::string& wrestler_name, const std::set<uint16_t>& taken) {
  std::vector<std::pair<std::string, uint16_t>> named;
  for (const auto& row : rows) {
    const auto name = names.find(60000 + row[0]);
    if (name != names.end()) named.emplace_back(UpperAscii(name->second), row[2]);
  }
  std::stable_sort(named.begin(), named.end());
  const std::string wanted = UpperAscii(wrestler_name);
  std::optional<uint16_t> previous, next;
  for (const auto& [name, rank] : named) {
    if (name < wanted) previous = rank;
    else if (name > wanted) { next = rank; break; }
  }
  if (previous && next && *previous < *next) {
    for (uint32_t candidate = uint32_t(*previous) + 1; candidate < *next; ++candidate)
      if (!taken.count(static_cast<uint16_t>(candidate))) return static_cast<uint16_t>(candidate);
  }
  const uint32_t maximum = taken.empty() ? 0 : *taken.rbegin();
  return static_cast<uint16_t>(maximum + 1);
}

bool MakeRegistryChars(const WWE13ImportPaths& paths,
                       const std::vector<fs::path>& overlays,
                       const std::vector<WWE13ImportPort>& ports,
                       std::vector<WWE13RegistryChar>* chars, std::string* error) {
  std::map<uint32_t, std::string> names;
  if (!CollectNames(paths, overlays, &names, error)) return false;
  const fs::path base_path = paths.wwe14_root / "pac/DLC.pac";
  const std::string base_name = internal::PathToUtf8(base_path.filename());
  Bytes base;
  if (!ReadFile(base_path, &base, error)) return false;
  std::vector<std::array<uint16_t, 4>> rows0, rows2;
  if (!ReadRegistryLsd(base, base_name, 0, &rows0, error) ||
      !ReadRegistryLsd(base, base_name, 2, &rows2, error)) return false;
  std::set<uint16_t> taken0, taken2;
  for (const auto& row : rows0) taken0.insert(row[2]);
  for (const auto& row : rows2) taken2.insert(row[2]);
  for (const auto& port : ports) {
    uint32_t fixed = 0;
    if (IsNumber(port.rank0, &fixed)) taken0.insert(static_cast<uint16_t>(fixed));
    if (IsNumber(port.rank2, &fixed)) taken2.insert(static_cast<uint16_t>(fixed));
  }
  std::vector<WWE13RegistryChar> result;
  result.reserve(ports.size());
  for (const WWE13ImportPort& port : ports) {
    WWE13RegistryChar item;
    item.target_id = port.target_id;
    uint32_t fixed = 0;
    item.rank_lsd0 = IsNumber(port.rank0, &fixed)
        ? static_cast<uint16_t>(fixed) : AutoRank(rows0, names, port.name, taken0);
    item.rank_lsd2 = IsNumber(port.rank2, &fixed)
        ? static_cast<uint16_t>(fixed) : AutoRank(rows2, names, port.name, taken2);
    taken0.insert(item.rank_lsd0);
    taken2.insert(item.rank_lsd2);
    result.push_back(item);
  }
  *chars = std::move(result);
  return true;
}

}  // namespace

namespace {

struct ArcEntry {
  std::string name;
  uint32_t sector = 0;
  uint32_t size = 0;
};
struct ArcGroup {
  std::string name;
  bool wide = false;
  std::vector<ArcEntry> entries;
};
struct ArcBlock {
  std::vector<ArcGroup> groups;
  uint16_t index = 0xffff;
  bool has_index = false;
};
uint32_t Be32(const Bytes& bytes, size_t at) {
  return (uint32_t(bytes.at(at)) << 24) | (uint32_t(bytes.at(at + 1)) << 16) |
         (uint32_t(bytes.at(at + 2)) << 8) | uint32_t(bytes.at(at + 3));
}
void PutBe16(Bytes* bytes, size_t at, uint16_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value >> 8);
  (*bytes)[at + 1] = static_cast<uint8_t>(value);
}
void PutBe32(Bytes* bytes, size_t at, uint32_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value >> 24);
  (*bytes)[at + 1] = static_cast<uint8_t>(value >> 16);
  (*bytes)[at + 2] = static_cast<uint8_t>(value >> 8);
  (*bytes)[at + 3] = static_cast<uint8_t>(value);
}

bool ParseArc(const Bytes& bytes, std::vector<ArcBlock>* blocks, std::string* error) {
  if (!blocks || bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0 ||
      bytes[2] != 0 || bytes[3] != 0)
    return Fail(error, "The package arc header is invalid.");
  std::vector<ArcBlock> result;
  size_t at = 4;
  while (at < bytes.size()) {
    if (at + 4 <= bytes.size() && bytes[at] == 0xff && bytes[at + 1] == 0xff) {
      if (result.empty()) return Fail(error, "The package arc has an index without a block.");
      result.back().index = static_cast<uint16_t>((uint16_t(bytes[at + 2]) << 8) | bytes[at + 3]);
      result.back().has_index = true;
      at += 4;
      continue;
    }
    if (result.empty() || result.back().has_index) result.emplace_back();
    if (at + 12 > bytes.size()) return Fail(error, "The package arc group header is truncated.");
    ArcGroup group;
    group.name.assign(reinterpret_cast<const char*>(bytes.data() + at), 4);
    const uint32_t word = Be32(bytes, at + 4);
    const uint32_t count_words = word >> 20;
    const uint32_t width_code = (word >> 12) & 0xf;
    if (Be32(bytes, at + 8) != 0 || (word & 0xfff) != 0 || width_code > 1)
      return Fail(error, "The package arc group flags are invalid.");
    group.wide = width_code == 1;
    const size_t record_size = group.wide ? 16 : 12;
    if ((count_words * 4) % record_size != 0)
      return Fail(error, "The package arc entry count is invalid.");
    const size_t count = count_words * 4 / record_size;
    at += 12;
    if (count > (bytes.size() - at) / record_size)
      return Fail(error, "The package arc entries are truncated.");
    group.entries.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const size_t name_size = record_size - 8;
      ArcEntry entry;
      entry.name.assign(reinterpret_cast<const char*>(bytes.data() + at), name_size);
      entry.sector = Be32(bytes, at + name_size);
      entry.size = Be32(bytes, at + name_size + 4);
      group.entries.push_back(std::move(entry));
      at += record_size;
    }
    result.back().groups.push_back(std::move(group));
  }
  if (result.empty()) return Fail(error, "The package arc has no blocks.");
  *blocks = std::move(result);
  return true;
}

Bytes EmitArc(const std::vector<ArcBlock>& blocks) {
  Bytes output{0xff, 0, 0, 0};
  for (const ArcBlock& block : blocks) {
    for (const ArcGroup& group : block.groups) {
      const size_t record_size = group.wide ? 16 : 12;
      Bytes header(12, 0);
      std::copy_n(group.name.begin(), std::min<size_t>(4, group.name.size()), header.begin());
      const uint64_t words = uint64_t(group.entries.size()) * record_size / 4;
      PutBe32(&header, 4, static_cast<uint32_t>((words << 20) | (group.wide ? 0x1000 : 0)));
      output.insert(output.end(), header.begin(), header.end());
      for (const ArcEntry& entry : group.entries) {
        const size_t name_size = record_size - 8;
        const size_t position = output.size();
        output.resize(position + record_size, 0);
        std::copy_n(entry.name.begin(), std::min(name_size, entry.name.size()),
                    output.begin() + static_cast<std::ptrdiff_t>(position));
        PutBe32(&output, position + name_size, entry.sector);
        PutBe32(&output, position + name_size + 4, entry.size);
      }
    }
    const size_t position = output.size();
    output.resize(position + 4);
    output[position] = 0xff; output[position + 1] = 0xff;
    PutBe16(&output, position + 2, block.index);
  }
  return output;
}

bool DirectoryFromPac(const Bytes& pac, std::vector<ArcGroup>* groups, std::string* error) {
  std::vector<WWE13PacGroupData> source;
  if (!ReadWWE13PacGroups(pac, &source, error)) return false;
  std::vector<ArcGroup> result;
  size_t body_offset = 0;
  for (const WWE13PacGroupData& group : source) {
    ArcGroup arc_group;
    arc_group.name = group.name;
    arc_group.name.resize(4, ' ');
    arc_group.wide = group.wide;
    for (const WWE13PacEntryData& entry : group.entries) {
      ArcEntry arc_entry;
      arc_entry.name = entry.name;
      arc_entry.sector = static_cast<uint32_t>(body_offset / 0x800);
      arc_entry.size = static_cast<uint32_t>((entry.data.size() + 0xff) / 0x100);
      body_offset += entry.data.size();
      body_offset = (body_offset + 0x7ff) & ~size_t(0x7ff);
      arc_group.entries.push_back(std::move(arc_entry));
    }
    result.push_back(std::move(arc_group));
  }
  *groups = std::move(result);
  return true;
}

bool SameArcDirectory(const std::vector<ArcGroup>& a, const std::vector<ArcGroup>& b,
                      bool loose) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].name != b[i].name || a[i].wide != b[i].wide || a[i].entries.size() != b[i].entries.size()) return false;
    for (size_t j = 0; j < a[i].entries.size(); ++j) {
      const ArcEntry& x = a[i].entries[j];
      const ArcEntry& y = b[i].entries[j];
      if (x.name != y.name || x.sector != y.sector || (!loose && x.size != y.size)) return false;
    }
  }
  return true;
}

bool SyncArc(Bytes* arc, const Bytes& old_pac, const Bytes& new_pac, bool loose,
             std::string* error) {
  std::vector<ArcBlock> blocks;
  if (!ParseArc(*arc, &blocks, error)) return false;
  std::vector<ArcGroup> before, after;
  if (!DirectoryFromPac(old_pac, &before, error) || !DirectoryFromPac(new_pac, &after, error)) return false;
  std::vector<size_t> matches;
  for (size_t i = 0; i < blocks.size(); ++i)
    if (SameArcDirectory(blocks[i].groups, before, loose)) matches.push_back(i);
  if (matches.size() != 1)
    return Fail(error, "Could not uniquely match a rewritten PAC in its catalog arc (matches=" + std::to_string(matches.size()) + ").");
  blocks[matches[0]].groups = std::move(after);
  *arc = EmitArc(blocks);
  return true;
}

bool BuildArc(const std::vector<PackageFileInfo>& files, const std::string& kind,
              Bytes* output, std::string* error) {
  std::vector<ArcBlock> blocks;
  for (const PackageFileInfo& file : files) {
    if (file.kind == "mov" || (file.kind != kind && file.kind != "both")) continue;
    Bytes pac;
    std::vector<ArcGroup> groups;
    if (!ReadFile(file.output_path, &pac, error) || !DirectoryFromPac(pac, &groups, error)) return false;
    ArcBlock block;
    block.groups = std::move(groups);
    blocks.push_back(std::move(block));
  }
  if (blocks.empty()) return Fail(error, "The package catalog arc would contain no PAC files.");
  for (size_t i = 0; i < blocks.size(); ++i) {
    blocks[i].index = i + 1 < blocks.size() ? static_cast<uint16_t>(i + 1) : 0xffff;
    blocks[i].has_index = true;
  }
  *output = EmitArc(blocks);
  return true;
}

struct PacCache {
  std::map<fs::path, std::vector<WWE13PacGroupData>> archives;
  bool Load(const fs::path& path, const std::vector<WWE13PacGroupData>** groups,
            std::string* error) {
    auto found = archives.find(path);
    if (found == archives.end()) {
      Bytes bytes;
      if (!ReadFile(path, &bytes, error)) return false;
      std::vector<WWE13PacGroupData> parsed;
      if (!ReadWWE13PacGroups(bytes, &parsed, error)) return false;
      found = archives.emplace(path, std::move(parsed)).first;
    }
    *groups = &found->second;
    return true;
  }
  bool Entry(const fs::path& path, const std::string& group, const std::string& name,
             Bytes* data, std::string* error) {
    const std::vector<WWE13PacGroupData>* groups = nullptr;
    if (!Load(path, &groups, error)) return false;
    for (const auto& candidate : *groups) {
      if (Trim(candidate.name) != group) continue;
      const auto* entry = FindEntry(&candidate, name);
      if (!entry) return false;
      *data = entry->data;
      return true;
    }
    return false;
  }
};

std::vector<fs::path> EntranceFiles(const fs::path& root) {
  return FilesWithPrefix(ResolvePathCase(root, "pac/evt"), "Entrance");
}
std::vector<fs::path> WinningFiles(const fs::path& root) {
  return FilesWithPrefix(ResolvePathCase(root, "pac/evt"), "Winning");
}
std::vector<fs::path> EventMoveFiles(const fs::path& root) {
  return FilesWithPrefix(ResolvePathCase(root, "pac/evt"), "evtmov");
}
bool FindEntry(const std::vector<fs::path>& files, PacCache* cache,
               const std::string& group, const std::string& name,
               Bytes* data, std::string* error) {
  for (const fs::path& file : files) {
    std::string ignored;
    if (cache->Entry(file, group, name, data, &ignored)) return true;
    if (ignored.find("Could not read") != std::string::npos ||
        ignored.find("could not be decoded") != std::string::npos) {
      if (error) *error = ignored;
      return false;
    }
  }
  return false;
}

struct GageBlock {
  std::array<uint8_t, 256> left{};
  std::array<uint8_t, 256> right{};
  Bytes coded;
  size_t end = 0;
};
bool ReadGageBlock(const Bytes& data, size_t position, GageBlock* block, std::string* error) {
  if (!block) return Fail(error, "The EVP decoder output is missing.");
  size_t at = position;
  size_t symbol = 0;
  for (size_t i = 0; i < 256; ++i) { block->left[i] = static_cast<uint8_t>(i); block->right[i] = 0; }
  while (symbol < 256) {
    if (at >= data.size()) return Fail(error, "The EVP pair table is truncated.");
    uint8_t run = data[at++];
    if (run > 127) { symbol += run - 127; run = 0; }
    if (symbol == 256) break;
    for (uint16_t i = 0; i <= run; ++i) {
      if (at >= data.size()) return Fail(error, "The EVP pair table is truncated.");
      const uint8_t left = data[at++];
      block->left[symbol] = left;
      if (left != symbol) {
        if (at >= data.size()) return Fail(error, "The EVP pair table is truncated.");
        block->right[symbol] = data[at++];
      }
      ++symbol;
    }
  }
  if (at + 2 > data.size()) return Fail(error, "The EVP coded-size field is truncated.");
  const size_t coded_size = Le16(data, at);
  at += 2;
  if (coded_size > data.size() - at) return Fail(error, "The EVP coded block is truncated.");
  block->coded.assign(data.begin() + static_cast<std::ptrdiff_t>(at),
                      data.begin() + static_cast<std::ptrdiff_t>(at + coded_size));
  block->end = at + coded_size;
  return true;
}
bool ExpandGage(const GageBlock& block, Bytes* output, std::string* error) {
  if (!output) return Fail(error, "The EVP expansion output is missing.");
  Bytes result;
  std::vector<uint8_t> stack;
  for (uint8_t byte : block.coded) {
    stack.push_back(byte);
    while (!stack.empty()) {
      const uint8_t current = stack.back(); stack.pop_back();
      if (block.left[current] == current) {
        result.push_back(current);
        if (result.size() > 16 * 1024 * 1024) return Fail(error, "The EVP first block expands beyond its safety limit.");
      } else {
        stack.push_back(block.right[current]);
        stack.push_back(block.left[current]);
      }
    }
  }
  *output = std::move(result);
  return true;
}

Bytes EncodeGage(const Bytes& source, std::string* error) {
  std::vector<uint8_t> data(source.begin(), source.end());
  std::array<bool, 256> used{};
  for (uint8_t byte : data) used[byte] = true;
  std::vector<uint8_t> free_symbols;
  for (int i = 255; i >= 0; --i) if (!used[static_cast<size_t>(i)]) free_symbols.push_back(static_cast<uint8_t>(i));
  std::array<uint8_t, 256> left{}, right{}, depth{};
  for (size_t i = 0; i < 256; ++i) left[i] = static_cast<uint8_t>(i);
  while (!free_symbols.empty() && data.size() > 1) {
    struct PairCount { uint16_t pair = 0; size_t count = 0; };
    std::vector<PairCount> order;
    std::map<uint16_t, size_t> locations;
    for (size_t i = 0; i + 1 < data.size(); ++i) {
      const uint16_t pair = static_cast<uint16_t>((uint16_t(data[i]) << 8) | data[i + 1]);
      auto [it, inserted] = locations.emplace(pair, order.size());
      if (inserted) order.push_back({pair, 0});
      ++order[it->second].count;
    }
    std::stable_sort(order.begin(), order.end(), [](const PairCount& a, const PairCount& b) {
      return a.count > b.count;
    });
    std::optional<uint16_t> selected;
    for (const PairCount& item : order) {
      if (item.count < 4) break;
      const uint8_t a = static_cast<uint8_t>(item.pair >> 8);
      const uint8_t b = static_cast<uint8_t>(item.pair);
      if (std::max(depth[a], depth[b]) + 1 <= 16) { selected = item.pair; break; }
    }
    if (!selected) break;
    const uint8_t code = free_symbols.front(); free_symbols.erase(free_symbols.begin());
    const uint8_t a = static_cast<uint8_t>(*selected >> 8);
    const uint8_t b = static_cast<uint8_t>(*selected);
    left[code] = a; right[code] = b;
    depth[code] = static_cast<uint8_t>(std::max(depth[a], depth[b]) + 1);
    Bytes reduced;
    reduced.reserve(data.size());
    for (size_t i = 0; i < data.size();) {
      if (i + 1 < data.size() && data[i] == a && data[i + 1] == b) {
        reduced.push_back(code); i += 2;
      } else reduced.push_back(data[i++]);
    }
    data = std::move(reduced);
  }
  if (data.size() > 0xffff) { if (error) *error = "The EVP first block is too large to encode."; return {}; }
  Bytes result;
  size_t symbol = 0;
  while (symbol < 256) {
    size_t run = 0;
    while (symbol + run < 256 && left[symbol + run] == symbol + run && run < 128) ++run;
    if (run) {
      result.push_back(static_cast<uint8_t>(127 + run));
      symbol += run;
      if (symbol == 256) break;
      result.push_back(left[symbol]);
      if (left[symbol] != symbol) result.push_back(right[symbol]);
      ++symbol;
    } else {
      size_t count = 0;
      while (symbol + count < 256 && left[symbol + count] != symbol + count && count < 128) ++count;
      if (!count) { if (error) *error = "The EVP pair table could not be encoded."; return {}; }
      result.push_back(static_cast<uint8_t>(count - 1));
      for (size_t i = symbol; i < symbol + count; ++i) { result.push_back(left[i]); result.push_back(right[i]); }
      symbol += count;
    }
  }
  result.push_back(static_cast<uint8_t>(data.size()));
  result.push_back(static_cast<uint8_t>(data.size() >> 8));
  result.insert(result.end(), data.begin(), data.end());
  return result;
}

uint16_t Be16(const Bytes& bytes, size_t at) {
  return static_cast<uint16_t>((uint16_t(bytes.at(at)) << 8) | bytes.at(at + 1));
}
bool RetargetEvp(const Bytes& raw, uint32_t new_key, uint32_t old_key,
                 bool drop_timed, Bytes* output, std::string* error) {
  if (!StartsWith(raw, "BPE ") || raw.size() < 16)
    return Fail(error, "An entrance EVP family item is not BPE compressed.");
  const uint32_t packed = Le32(raw, 8);
  if (packed > raw.size() - 16 || new_key > 0xffff || old_key > 0xffff)
    return Fail(error, "An entrance EVP header or key is invalid.");
  Bytes body(raw.begin() + 16, raw.begin() + static_cast<std::ptrdiff_t>(16 + packed));
  GageBlock first;
  if (!ReadGageBlock(body, 0, &first, error)) return false;
  Bytes expanded;
  if (!ExpandGage(first, &expanded, error)) return false;
  if (expanded.size() < 34 || Be16(expanded, 32) != old_key)
    return Fail(error, "An entrance EVP own-number field did not match its source key.");
  expanded[32] = static_cast<uint8_t>(new_key >> 8);
  expanded[33] = static_cast<uint8_t>(new_key);
  if (drop_timed) {
    if (expanded.size() < 0x1a) return Fail(error, "An entrance EVP timed-list field is truncated.");
    expanded[0x18] = 0; expanded[0x19] = 0;
  }
  Bytes encoded = EncodeGage(expanded, error);
  if (encoded.empty() && !expanded.empty()) return false;
  encoded.insert(encoded.end(), body.begin() + static_cast<std::ptrdiff_t>(first.end), body.end());
  Bytes result(raw.begin(), raw.begin() + 16);
  PutLe32(&result, 8, static_cast<uint32_t>(encoded.size()));
  result.insert(result.end(), encoded.begin(), encoded.end());
  result.resize((result.size() + 0xff) & ~size_t(0xff), 0);
  *output = std::move(result);
  return true;
}

bool IsFamilyName(const std::string& name, uint32_t id) {
  if (name == std::to_string(id)) return true;
  const std::string tail = Id(id, 3);
  return name.size() == 5 && name.substr(2) == tail;
}
std::string RenamedFamilyName(const std::string& name, uint32_t old_id, uint32_t new_id) {
  if (name == std::to_string(old_id)) return std::to_string(new_id);
  return name.substr(0, 2) + Id(new_id, 3);
}

bool EntranceFamily(const WWE13ImportPaths& paths, const WWE13ImportPort& port,
                    PacCache* cache, const std::string& group,
                    std::map<std::string, Bytes>* output, std::string* error) {
  const bool donor = port.ent == "donor";
  const uint32_t old_id = donor ? port.donor_id : port.source_id;
  std::vector<fs::path> roots;
  if (donor) roots.push_back(paths.wwe14_root);
  else if (port.target_id != port.source_id) {
    fs::path source_root;
    if (!FindSourceRoot(paths, port.source_id, &source_root))
      return Fail(error, "The WWE '13 entrance root is missing for source " + std::to_string(port.source_id) + ".");
    roots.push_back(source_root);
  } else {
    roots.push_back(paths.wwe14_root);
    fs::path source_root;
    if (FindSourceRoot(paths, port.source_id, &source_root)) roots.push_back(source_root);
  }
  fs::path source_file;
  Bytes source_entry;
  for (const fs::path& root : roots) {
    for (const fs::path& file : EntranceFiles(root)) {
      if (cache->Entry(file, "EVP", std::to_string(old_id), &source_entry, nullptr)) {
        source_file = file;
        break;
      }
    }
    if (!source_file.empty()) break;
  }
  if (source_file.empty()) return Fail(error, "No entrance EVP anchor exists for wrestler " + std::to_string(port.target_id) + ".");
  const std::vector<WWE13PacGroupData>* source_groups = nullptr;
  if (!cache->Load(source_file, &source_groups, error)) return false;
  const WWE13PacGroupData* wanted_group = nullptr;
  for (const auto& candidate : *source_groups) if (Trim(candidate.name) == group) wanted_group = &candidate;
  if (!wanted_group) return Fail(error, "The entrance PAC is missing group " + group + ".");
  std::vector<std::pair<uint32_t, std::string>> matches;
  for (const WWE13PacEntryData& entry : wanted_group->entries) {
    const std::string key = RawName(entry.name);
    if (!IsFamilyName(key, old_id)) continue;
    uint32_t numeric = 0;
    if (!IsNumber(key, &numeric)) numeric = old_id;
    matches.emplace_back(numeric, key);
  }
  std::sort(matches.begin(), matches.end());
  if (port.ent_keep != "all") {
    uint32_t count = 0;
    if (!IsNumber(port.ent_keep, &count)) return Fail(error, "The entrance family limit is not numeric.");
    if (matches.size() > count) matches.resize(count);
  }
  for (const auto& [numeric, old_name] : matches) {
    (void)numeric;
    const WWE13PacEntryData* source = FindEntry(wanted_group, old_name);
    if (!source) return Fail(error, "An entrance family item disappeared during lookup.");
    const std::string new_name = RenamedFamilyName(old_name, old_id, port.target_id);
    Bytes data = source->data;
    const bool drop_timed = port.ent_video == "off" && group == "EVP" && new_name.size() <= 3;
    if (((donor || port.target_id != port.source_id) && group == "EVP") || drop_timed) {
      Bytes changed;
      if (!RetargetEvp(data, static_cast<uint32_t>(std::stoul(new_name)),
                       static_cast<uint32_t>(std::stoul(old_name)), drop_timed, &changed, error)) return false;
      data = std::move(changed);
    }
    (*output)[new_name] = std::move(data);
  }
  return true;
}

}  // namespace

bool SyncWWE13PacArchiveIfListed(WWE13ConvertBytes* arc, const WWE13ConvertBytes& original_pac,
                                 const WWE13ConvertBytes& rewritten_pac, bool* synced,
                                 std::string* error) {
  if (!arc || !synced) return Fail(error, "The PAC archive is missing.");
  *synced = false;
  std::vector<ArcBlock> blocks;
  if (!ParseArc(*arc, &blocks, error)) return false;
  std::vector<ArcGroup> before;
  if (!DirectoryFromPac(original_pac, &before, error)) return false;
  size_t matches = 0;
  for (const ArcBlock& block : blocks)
    if (SameArcDirectory(block.groups, before, false)) ++matches;
  if (matches == 0) return true;  // this list does not carry the PAC (16:9 lists HD pacs, 4:3 lists SD pacs)
  if (!SyncArc(arc, original_pac, rewritten_pac, false, error)) return false;
  *synced = true;
  return true;
}

bool InsertWWE13ArcBlock(WWE13ConvertBytes* arc, uint16_t index, const WWE13ConvertBytes& pac,
                         std::string* error) {
  if (!arc) return Fail(error, "The PAC list archive is missing.");
  std::vector<ArcBlock> blocks;
  if (!ParseArc(*arc, &blocks, error)) return false;
  ArcBlock added;
  if (!DirectoryFromPac(pac, &added.groups, error)) return false;
  added.index = index;
  added.has_index = true;
  // Blocks name their pac-list line; 0xffff is the sentinel block. Later lines move down by one.
  size_t position = blocks.size();
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (blocks[i].index != 0xffff && blocks[i].index >= index) ++blocks[i].index;
    if (position == blocks.size() && blocks[i].index != 0xffff && blocks[i].index > index) position = i;
  }
  if (position == blocks.size()) return Fail(error, "The PAC list has no line after the new arena.");
  blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(position), std::move(added));
  *arc = EmitArc(blocks);
  return true;
}

bool SyncWWE13PacArchives(const WWE13ConvertBytes& original_pac,
                          const WWE13ConvertBytes& rewritten_pac,
                          std::vector<WWE13ConvertBytes>* arcs, std::string* error, bool loose) {
  if (!arcs) return Fail(error, "The PAC archive list is missing.");
  for (WWE13ConvertBytes& arc : *arcs)
    if (!SyncArc(&arc, original_pac, rewritten_pac, loose, error)) return false;
  return true;
}

namespace {

bool BuildPortraitPac(const WWE13ImportPaths& paths, const std::vector<WWE13ImportPort>& ports, const std::string& kind,
                      Bytes* output, std::string* error);

// Main select grid faces (owner 2026-11-16; core/grid_faces.h): each import's own WWE '13 grid face (menu/menuHD.pac
// MENU/SSFD, the 2K14 format) goes into the 2K14 menuHD/SD.pac under his slot; WWE '13 add-on people (no face there) get a
// head crop of their SSFC select portrait. Each menu PAC is synced in the one pac list that carries it. Cosmetic: problems
// become notices, never a failed import.
void AddWWE13GridFaces(const WWE13ImportPaths& paths, const std::vector<WWE13ImportPort>& ports, OutputWriter* writer,
                       std::vector<Bytes>* arcs, std::vector<std::string>* notices) {
  std::string detail;
  std::map<uint32_t, Bytes> own13;
  Bytes m13;
  if (!ReadFile(ResolvePathCase(paths.wwe13_root, "pac/menu/menuHD.pac"), &m13, &detail) || !ReadGridFaces(m13, &own13, &detail))
    own13.clear();
  std::map<uint32_t, Bytes> ssfc;  // target id -> portrait (only built when a face is missing)
  bool ssfc_ready = false;
  size_t own = 0, cropped = 0;
  for (const std::string kind : {"HD", "SD"}) {
    const std::string rel = "pac/menu/menu" + kind + ".pac";
    Bytes old, now;
    std::map<uint32_t, Bytes> stock, faces;
    if (!ReadFile(ResolvePathCase(paths.wwe14_root, rel), &old, &detail) || !ReadGridFaces(old, &stock, &detail) || stock.empty()) {
      notices->push_back("Select-grid faces skipped (" + rel + ": " + detail + ").");
      continue;
    }
    own = cropped = 0;
    const GridFaceBytes backdrop = GridFaceBackdrop(stock);  // opaque tile background for portrait crops (owner 11-16)
    for (const WWE13ImportPort& port : ports) {
      if (stock.count(port.target_id)) continue;
      const auto mine = own13.find(port.source_id);
      if (mine != own13.end() && IsGridFace(mine->second)) { faces[port.target_id] = mine->second; ++own; continue; }
      if (!ssfc_ready) {
        ssfc_ready = true;
        Bytes pac;
        std::vector<WWE13PacGroupData> groups;
        if (BuildPortraitPac(paths, ports, "HD", &pac, &detail) && ReadWWE13PacGroups(pac, &groups, &detail))
          for (const auto& g : groups)
            if (Trim(g.name) == "SSFC")
              for (const auto& e : g.entries) ssfc[uint32_t(std::stoul(RawName(e.name)))] = e.data;
      }
      const auto portrait = ssfc.find(port.target_id);
      Bytes face;
      if (portrait != ssfc.end() && MakeGridFaceFromPortrait(portrait->second, stock.begin()->second, &face, &detail, &backdrop)) {
        faces[port.target_id] = face;
        ++cropped;
      }
    }
    size_t added = 0;
    Bytes old_plain;
    if (!AddGridFaces(old, faces, &now, &added, &detail) || !DecodeWWE13PacPlain(old, &old_plain, &detail)) {
      notices->push_back("Select-grid faces skipped (" + rel + ": " + detail + ").");
      continue;
    }
    if (!added) continue;
    std::vector<Bytes> next = *arcs;
    bool any = false, ok = true;
    for (Bytes& arc : next) {
      bool synced = false;
      if (!SyncWWE13PacArchiveIfListed(&arc, old_plain, now, &synced, &detail)) { ok = false; break; }
      any = any || synced;
    }
    if (!ok || !any) {
      notices->push_back("Select-grid faces skipped (" + rel + " is not in the game's pac lists).");
      continue;
    }
    if (!WritePacFile(writer, "game/" + rel, now, &detail)) {
      notices->push_back("Select-grid faces skipped (" + rel + ": " + detail + ").");
      continue;
    }
    *arcs = std::move(next);
  }
  notices->push_back("Select-grid faces: " + std::to_string(own) + " from WWE '13, " + std::to_string(cropped) +
                     " made from the select portrait.");
}

bool ModifyBase(const WWE13ImportPaths& paths, OutputWriter* writer,
                const std::string& relative, const Bytes& source, const Bytes& output,
                std::vector<Bytes>* arcs, std::string* error) {
  if (arcs) for (Bytes& arc : *arcs)
    if (!SyncArc(&arc, source, output, false, error)) return false;
  return WritePacFile(writer, "game/" + relative, output, error);
}

bool AddHostPackageFile(OutputWriter* writer, std::vector<PackageFileInfo>* files,
                        const std::string& host_pack, const std::string& relative,
                        const std::string& kind, const Bytes& data,
                        std::string* error) {
  const std::string output = UserdataPath(host_pack, relative);
  if (!writer->Put(output, data)) return false;
  PackageFileInfo info;
  info.relative = relative;
  info.kind = kind;
  info.output_path = writer->files_root / output;
  files->push_back(std::move(info));
  return true;
}

WWE13PacGroupData MakeGroup(const std::string& name, bool wide,
                            const std::vector<std::pair<std::string, Bytes>>& entries,
                            uint8_t padding = ' ') {
  WWE13PacGroupData group;
  group.name.assign(4, ' ');
  std::copy_n(name.begin(), std::min<size_t>(name.size(), group.name.size()), group.name.begin());
  group.wide = wide;
  const size_t name_size = wide ? 8 : 4;
  for (const auto& [entry_name, data] : entries) {
    WWE13PacEntryData entry;
    entry.name.assign(name_size, static_cast<char>(padding));
    std::copy_n(entry_name.begin(), std::min(entry_name.size(), name_size), entry.name.begin());
    entry.data = data;
    group.entries.push_back(std::move(entry));
  }
  return group;
}

bool BuildPortraitPac(const WWE13ImportPaths& paths,
                      const std::vector<WWE13ImportPort>& ports,
                      const std::string& kind, Bytes* output, std::string* error) {
  std::set<std::string> wanted;
  for (const WWE13ImportPort& port : ports)
    for (const char* group : {"SSFA", "SSFB", "SSFC"})
      wanted.insert(std::string(group) + "/" + Id(port.source_id, 4));
  std::vector<fs::path> sources;
  if (std::any_of(ports.begin(), ports.end(), [](const WWE13ImportPort& port) { return port.source_id < 100; }))
    sources.push_back(ResolvePathCase(paths.wwe13_root, "pac/" + ("DLC_" + kind + ".pac")));
  std::set<fs::path> pack_roots;
  for (const WWE13ImportPort& port : ports) {
    if (port.source_id >= 100) continue;
    fs::path root;
    if (!FindSourceRoot(paths, port.source_id, &root))
      return Fail(error, "The WWE '13 portrait pack is missing for source " + std::to_string(port.source_id) + ".");
    if (root != paths.wwe13_root) pack_roots.insert(root);
  }
  for (const fs::path& root : pack_roots) {
    // By contents, not the folder's name: a player's add-on folder can be called anything.
    const auto paths_in_root = FilesWithPrefix(ResolvePathCase(root, "pac"), "DLC_" + kind + "_dlc");
    if (!paths_in_root.empty()) sources.push_back(paths_in_root.front());
  }
  sources.push_back(ResolvePathCase(paths.wwe13_root, "pac/menu/" + ("menu" + kind + ".pac")));
  std::map<std::string, Bytes> portraits;
  for (const fs::path& source_path : sources) {
    std::error_code ec;
    if (!fs::is_regular_file(source_path, ec) || ec) continue;
    Bytes data;
    if (!ReadFile(source_path, &data, error)) return false;
    std::vector<WWE13PacGroupData> groups;
    if (!ReadWWE13PacGroups(data, &groups, error)) return false;
    for (const WWE13PacGroupData& group : groups) {
      const std::string type = Trim(group.name);
      if (type != "SSFA" && type != "SSFB" && type != "SSFC") continue;
      for (const WWE13PacEntryData& entry : group.entries) {
        const std::string key = type + "/" + RawName(entry.name);
        if (wanted.count(key)) portraits[key] = entry.data;
      }
    }
  }
  std::vector<WWE13PacGroupData> generated;
  for (const char* name : {"SSFA", "SSFB", "SSFC"}) {
    std::vector<std::pair<std::string, Bytes>> entries;
    for (const WWE13ImportPort& port : ports) {
      const std::string key = std::string(name) + "/" + Id(port.source_id, 4);
      const auto found = portraits.find(key);
      if (found == portraits.end())
        return Fail(error, "A WWE '13 portrait is missing: " + key + ".");
      entries.emplace_back(Id(port.target_id, 4), found->second);
    }
    generated.push_back(MakeGroup(name, false, entries, '\0'));
  }
  return BuildWWE13PacGroups(generated, output, error);
}

bool ReplaceOrAppendEntries(const Bytes& source, const std::map<std::string,
                            std::vector<std::pair<std::string, Bytes>>>& additions,
                            const std::map<std::pair<std::string, std::string>, Bytes>& replacements,
                            const std::set<std::pair<std::string, std::string>>& drops,
                            Bytes* output, std::string* error) {
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(source, &groups, error)) return false;
  for (WWE13PacGroupData& group : groups) {
    const std::string type = Trim(group.name);
    std::vector<WWE13PacEntryData> entries;
    entries.reserve(group.entries.size());
    for (WWE13PacEntryData& entry : group.entries) {
      const std::string key = RawName(entry.name);
      if (drops.count({type, key})) continue;
      const auto replacement = replacements.find({type, key});
      if (replacement != replacements.end()) entry.data = replacement->second;
      entries.push_back(std::move(entry));
    }
    const auto add = additions.find(type);
    if (add != additions.end()) {
      const size_t width = group.wide ? 8 : 4;
      for (const auto& [name, data] : add->second) {
        WWE13PacEntryData entry;
        entry.name.assign(width, ' ');
        std::copy_n(name.begin(), std::min(name.size(), width), entry.name.begin());
        entry.data = data;
        entries.push_back(std::move(entry));
      }
      std::sort(entries.begin(), entries.end(), [](const WWE13PacEntryData& a, const WWE13PacEntryData& b) {
        return a.name < b.name;
      });
    }
    group.entries = std::move(entries);
  }
  return RebuildWWE13PacGroups(source, groups, output, error);
}

bool BuildFamilyPac(const std::map<std::string, Bytes>& evp,
                    const std::map<std::string, Bytes>& evz, Bytes* output,
                    std::string* error) {
  std::vector<std::pair<std::string, Bytes>> a(evp.begin(), evp.end());
  std::vector<std::pair<std::string, Bytes>> b(evz.begin(), evz.end());
  auto numeric = [](const auto& x, const auto& y) { return std::stoul(x.first) < std::stoul(y.first); };
  std::sort(a.begin(), a.end(), numeric);
  std::sort(b.begin(), b.end(), numeric);
  return BuildWWE13PacGroups({MakeGroup("EVP", true, a), MakeGroup("EVZ", true, b)}, output, error);
}

bool SourceRoot(const WWE13ImportPaths& paths, const WWE13ImportPort& port, fs::path* root,
                std::string* error) {
  if (!FindSourceRoot(paths, port.source_id, root))
    return Fail(error, "The WWE '13 source package root is missing for ID " + std::to_string(port.source_id) + ".");
  return true;
}

Bytes EntryNameBytes(const std::string& name, size_t width, uint8_t padding = ' ') {
  return EncodedName(name, width, padding);
}

bool BuildWinningPac(const WWE13ImportPaths& paths,
                     const std::vector<WWE13ImportPort>& ports, PacCache* cache,
                     Bytes* output, std::string* error) {
  std::map<std::string, std::map<std::string, Bytes>> entries;
  std::map<std::string, std::set<std::string>> reference_entries;
  std::map<std::string, std::set<std::string>> fallback_entries;
  for (const WWE13ImportPort& port : ports) {
    fs::path source_root;
    if (!SourceRoot(paths, port, &source_root, error)) return false;
    for (const std::string group : {"EVP", "EVZ"}) {
      Bytes stock;
      if (port.source_id == port.target_id &&
          FindEntry(WinningFiles(paths.wwe14_root), cache, group,
                    "7" + std::to_string(port.target_id), &stock, nullptr)) {
        const std::string stock_key = "7" + std::to_string(port.target_id);
        entries[group][stock_key] = std::move(stock);
        reference_entries[group].insert(stock_key);
      }

      Bytes source13;
      bool has_source13 = false;
      if (port.win == "13")
        has_source13 = FindEntry(WinningFiles(source_root), cache, group,
                                 "6" + Id(port.source_id, 3), &source13, error);
      if (has_source13) {
        const std::string target_key = "6" + std::to_string(port.target_id);
        if (port.source_id != port.target_id && group == "EVP") {
          Bytes converted;
          if (!RetargetEvp(source13, 6000 + port.target_id, 6000 + port.source_id,
                           false, &converted, error)) return false;
          source13 = std::move(converted);
        }
        entries[group][target_key] = std::move(source13);
        reference_entries[group].insert(target_key);
      } else {
        Bytes donor;
        if (port.win == "13" && group == "EVP") {
          // The reference tool falls back to the donor only after a missing source family.
        }
        if (FindEntry(WinningFiles(paths.wwe14_root), cache, group,
                      "6" + std::to_string(port.donor_id), &donor, error)) {
          const std::string donor_key = "6" + std::to_string(port.target_id);
          // #142 (11-15): the donor's EVP carries its own number 6000+donor at expanded +32; filed under the target's key
          // unchanged, the victory lookup misses and the game plays its per-gender default (Brodus Clay / AJ Lee). The
          // WWE 2K15 path retargets the same word (wwe15_pipeline.cpp WWE13SharedRetargetEvp); do the same here.
          if (group == "EVP" && port.donor_id != port.target_id) {
            Bytes retargeted;
            if (!RetargetEvp(donor, 6000 + port.target_id, 6000 + port.donor_id, false, &retargeted, error)) return false;
            donor = std::move(retargeted);
          }
          entries[group][donor_key] = std::move(donor);
          fallback_entries[group].insert(donor_key);
        }
      }
    }
  }
  uint32_t second_group_reserved = 0;
  if (!fallback_entries["EVP"].empty()) {
    std::vector<WWE13PacGroupData> phase_one_groups;
    for (const std::string group : {"EVP", "EVZ"}) {
      std::vector<std::pair<std::string, Bytes>> items;
      for (const std::string& key : reference_entries[group])
        if (entries[group].count(key)) items.emplace_back(key, entries[group].at(key));
      phase_one_groups.push_back(MakeGroup(group, true, items));
    }
    Bytes phase_one_pac;
    if (!BuildWWE13PacGroups(phase_one_groups, &phase_one_pac, error)) return false;
    const size_t second_group_at = 0x800 + 12 + entries["EVP"].size() * 16;
    if (second_group_at + 12 > phase_one_pac.size())
      return Fail(error, "The Winning PAC reference group cursor is outside the header.");
    second_group_reserved = Le32(phase_one_pac, second_group_at + 8);
  }
  std::vector<WWE13PacGroupData> groups;
  for (const std::string group : {"EVP", "EVZ"}) {
    std::vector<std::pair<std::string, Bytes>> items;
    for (auto& [key, data] : entries[group]) items.emplace_back(key, std::move(data));
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    groups.push_back(MakeGroup(group, true, items));
  }
  groups[1].reserved = second_group_reserved;
  if (!BuildWWE13PacGroups(groups, output, error)) return false;
  const size_t reference_directory_size = 24 +
      16 * (reference_entries["EVP"].size() + reference_entries["EVZ"].size());
  if (reference_directory_size > std::numeric_limits<uint32_t>::max() || output->size() < 8)
    return Fail(error, "The Winning PAC directory size is invalid.");
  PutLe32(output, 4, static_cast<uint32_t>(reference_directory_size));
  return true;
}

bool BuildEventMovePac(const WWE13ImportPaths& paths,
                       const std::vector<WWE13ImportPort>& ports, PacCache* cache,
                       Bytes* output, std::string* error) {
  std::map<std::string, std::map<std::string, Bytes>> entries;
  std::map<std::string, std::set<std::string>> reference_entries;
  std::map<std::string, std::set<std::string>> fallback_entries;
  for (const WWE13ImportPort& port : ports) {
    fs::path source_root;
    if (!SourceRoot(paths, port, &source_root, error)) return false;
    for (const std::string group : {"MVSW", "MVSB"}) {
      Bytes source13;
      const bool has_source13 = port.mv == "13" &&
          FindEntry(EventMoveFiles(source_root), cache, group,
                    "M" + Id(port.source_id, 3), &source13, error);
      if (has_source13) {
        const std::string key = "M" + std::to_string(port.target_id);
        entries[group][key] = std::move(source13);
        reference_entries[group].insert(key);
      }
      else {
        Bytes donor;
        if (FindEntry(EventMoveFiles(paths.wwe14_root), cache, group,
                      "M" + std::to_string(port.donor_id), &donor, error)) {
          const std::string donor_key = "M" + std::to_string(port.target_id);
          entries[group][donor_key] = std::move(donor);
          fallback_entries[group].insert(donor_key);
        }
      }
    }
  }
  uint32_t second_group_reserved = 0;
  if (!fallback_entries["MVSW"].empty()) {
    std::vector<WWE13PacGroupData> phase_one_groups;
    for (const std::string group : {"MVSW", "MVSB"}) {
      std::vector<std::pair<std::string, Bytes>> items;
      for (const std::string& key : reference_entries[group])
        if (entries[group].count(key)) items.emplace_back(key, entries[group].at(key));
      phase_one_groups.push_back(MakeGroup(group, false, items, '\0'));
    }
    Bytes phase_one_pac;
    if (!BuildWWE13PacGroups(phase_one_groups, &phase_one_pac, error)) return false;
    const size_t second_group_at = 0x800 + 12 + entries["MVSW"].size() * 12;
    if (second_group_at + 12 > phase_one_pac.size())
      return Fail(error, "The event-move PAC reference group cursor is outside the header.");
    second_group_reserved = Le32(phase_one_pac, second_group_at + 8);
  }
  std::vector<WWE13PacGroupData> groups;
  for (const std::string group : {"MVSW", "MVSB"}) {
    std::vector<std::pair<std::string, Bytes>> items;
    for (auto& [key, data] : entries[group]) items.emplace_back(key, std::move(data));
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    groups.push_back(MakeGroup(group, false, items, '\0'));
  }
  groups[1].reserved = second_group_reserved;
  if (!BuildWWE13PacGroups(groups, output, error)) return false;
  const size_t reference_directory_size = 24 +
      12 * (reference_entries["MVSW"].size() + reference_entries["MVSB"].size());
  if (reference_directory_size > std::numeric_limits<uint32_t>::max() || output->size() < 8)
    return Fail(error, "The event-move PAC directory size is invalid.");
  PutLe32(output, 4, static_cast<uint32_t>(reference_directory_size));
  return true;
}

bool BuildGmPac(const WWE13ImportPaths& paths,
                const std::vector<WWE13ImportPort>& ports, PacCache* cache,
                Bytes* output, std::string* error) {
  std::vector<std::pair<std::string, Bytes>> gmev, gmch;
  for (const WWE13ImportPort& port : ports) {
    const std::string target_key = Id(port.target_id, 4);
    const std::string target_ch = "5" + std::to_string(port.target_id);
    const std::string source_key = Id(port.source_id, 4);
    Bytes data;
    bool found = port.target_id == port.source_id &&
        FindEntry({paths.wwe14_root / "pac/gm.pac"}, cache, "GMEV", target_key, &data, error);
    fs::path source_root;
    if (!SourceRoot(paths, port, &source_root, error)) return false;
    if (!found) found = FindEntry(FilesWithPrefix(ResolvePathCase(source_root, "pac"), "gm"), cache,
                                  "GMEV", source_key, &data, error);
    if (!found) {
      if (!FindEntry({paths.wwe14_root / "pac/gm.pac"}, cache, "GMEV",
                     Id(port.donor_id, 4), &data, error))
        return Fail(error, "No GMEV source is available for target " + std::to_string(port.target_id) + ".");
    }
    gmev.emplace_back(target_key, std::move(data));

    if (port.target_id == port.source_id) {
      Bytes body;
      if (FindEntry({paths.wwe14_root / "pac/gm.pac"}, cache, "GMCH", target_ch, &body, error) ||
          FindEntry({ResolvePathCase(paths.wwe13_root, "pac/gm.pac")}, cache, "GMCH", target_ch, &body, error))
        gmch.emplace_back(target_ch, std::move(body));
    }
  }
  std::vector<WWE13PacGroupData> groups;
  groups.push_back(MakeGroup("GMEV", false, gmev, '\0'));
  if (!gmch.empty()) groups.push_back(MakeGroup("GMCH", false, gmch, '\0'));
  return BuildWWE13PacGroups(groups, output, error);
}

bool BuildPrtclsPac(const WWE13ImportPaths& paths,
                    const std::vector<WWE13ImportPort>& ports, PacCache* cache,
                    Bytes* output, std::string* error) {
  std::vector<std::pair<std::string, Bytes>> pce, pcch;
  for (const WWE13ImportPort& port : ports) {
    const std::string target_key = Id(port.target_id, 4);
    const std::string source_key = Id(port.source_id, 4);
    const std::string target_ch = "5" + std::to_string(port.target_id);
    Bytes data;
    bool found = port.target_id == port.source_id &&
        FindEntry({paths.wwe14_root / "pac/prtcls.pac"}, cache, "PCE6", target_key, &data, error);
    fs::path source_root;
    if (!SourceRoot(paths, port, &source_root, error)) return false;
    if (!found) {
      const fs::path donor_root = port.pce == "13" ? source_root : paths.wwe14_root;
      const std::vector<fs::path> source_files = port.pce == "13"
          ? FilesWithPrefix(ResolvePathCase(donor_root, "pac"), "prtcls")
          : std::vector<fs::path>{donor_root / "pac/prtcls.pac"};
      const std::string source_key_name = port.pce == "13" ? source_key : Id(port.donor_id, 4);
      if (!FindEntry(source_files, cache, "PCE6", source_key_name, &data, error))
        return Fail(error, "No PCE6 source is available for target " + std::to_string(port.target_id) + ".");
    }
    pce.emplace_back(target_key, std::move(data));
    if (port.target_id == port.source_id) {
      Bytes item;
      if (FindEntry({paths.wwe14_root / "pac/prtcls.pac"}, cache, "PCCH", target_ch, &item, error) ||
          FindEntry({ResolvePathCase(paths.wwe13_root, "pac/prtcls.pac")}, cache, "PCCH", target_ch, &item, error))
        pcch.emplace_back(target_ch, std::move(item));
    }
  }
  std::vector<WWE13PacGroupData> groups;
  groups.push_back(MakeGroup("PCE6", false, pce, '\0'));
  if (!pcch.empty()) groups.push_back(MakeGroup("PCCH", false, pcch, '\0'));
  return BuildWWE13PacGroups(groups, output, error);
}

bool BuildExclusiveAiPac(const WWE13ImportPaths& paths,
                         const std::vector<WWE13ImportPort>& ports,
                         Bytes* output, bool* has_entries, std::string* error) {
  Bytes source;
  if (!ReadFile(paths.wwe14_root / "pac/ExclusiveAI.pac", &source, error)) return false;
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(source, &groups, error)) return false;
  std::vector<WWE13PacGroupData> generated;
  for (const WWE13PacGroupData& group : groups) {
    const std::string type = Trim(group.name);
    if (type != "EXCB" && type != "EXCL") continue;
    std::vector<std::pair<std::string, Bytes>> entries;
    for (const WWE13ImportPort& port : ports) {
      const std::string donor = std::to_string(port.donor_id);
      for (const WWE13PacEntryData& entry : group.entries) {
        const std::string old_name = RawName(entry.name);
        if (old_name != donor) continue;
        std::string new_name = old_name;
        const size_t at = new_name.find(donor);
        if (at == std::string::npos) continue;
        new_name.replace(at, donor.size(), std::to_string(port.target_id));
        entries.emplace_back(new_name, entry.data);
      }
    }
    if (!entries.empty()) generated.push_back(MakeGroup(type, group.wide, entries));
  }
  *has_entries = !generated.empty();
  if (!*has_entries) { output->clear(); return true; }
  return BuildWWE13PacGroups(generated, output, error);
}

}  // namespace

namespace {

struct RaRow { uint32_t id = 0; uint32_t key = 0; };
bool ReadRass(const Bytes& bytes, std::vector<RaRow>* rows, std::string* error) {
  if (!rows || bytes.size() < 16 || std::memcmp(bytes.data(), "RASS1001", 8) != 0)
    return Fail(error, "The sound PAC has an invalid ring-announcer RASS table.");
  const uint32_t size = Le32(bytes, 8), count = Le32(bytes, 12);
  if (size != count * 8 || bytes.size() != 16ull + size)
    return Fail(error, "The ring-announcer RASS table has invalid counts.");
  std::vector<RaRow> result;
  result.reserve(count);
  uint32_t previous = 0;
  for (uint32_t i = 0; i < count; ++i) {
    RaRow row{Le32(bytes, 16 + i * 8), Le32(bytes, 20 + i * 8)};
    if (i && row.id <= previous) return Fail(error, "The ring-announcer rows are not sorted and unique.");
    previous = row.id;
    result.push_back(row);
  }
  *rows = std::move(result);
  return true;
}
Bytes MakeRass(const std::vector<RaRow>& rows) {
  Bytes bytes(16 + rows.size() * 8, 0);
  std::memcpy(bytes.data(), "RASS1001", 8);
  PutLe32(&bytes, 8, static_cast<uint32_t>(rows.size() * 8));
  PutLe32(&bytes, 12, static_cast<uint32_t>(rows.size()));
  for (size_t i = 0; i < rows.size(); ++i) {
    PutLe32(&bytes, 16 + i * 8, rows[i].id);
    PutLe32(&bytes, 20 + i * 8, rows[i].key);
  }
  return bytes;
}
bool PatchRingAnnouncer(const Bytes& source, const std::vector<WWE13ImportPort>& ports,
                        Bytes* output, std::string* error) {
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(source, &groups, error)) return false;
  WWE13PacGroupData* sond = FindGroup(&groups, "SOND");
  WWE13PacEntryData* dupl = FindEntry(sond, "DUPL");
  if (!dupl) return Fail(error, "The sound PAC is missing SOND/DUPL.");
  Bytes dupl_plain;
  if (!DecodeWWE13Bpe(dupl->data, &dupl_plain, error)) return false;
  std::vector<WWE13PachLeafData> outer;
  if (!ReadWWE13PachLeaves(dupl_plain, &outer, error)) return false;
  auto leaf200 = std::find_if(outer.begin(), outer.end(), [](const WWE13PachLeafData& leaf) { return leaf.id == 200; });
  if (leaf200 == outer.end() || !StartsWith(leaf200->data, "ZLIB") || leaf200->data.size() < 16)
    return Fail(error, "The sound PAC's DUPL RASS compression is missing.");
  const uint32_t packed = Le32(leaf200->data, 8), plain_size = Le32(leaf200->data, 12);
  if (packed > leaf200->data.size() - 16 || plain_size > 32 * 1024 * 1024)
    return Fail(error, "The ring-announcer compression sizes are invalid.");
  Bytes inner_plain(plain_size);
  wwe_mz_ulong actual_size = plain_size;
  if (mz_uncompress(inner_plain.data(), &actual_size, leaf200->data.data() + 16, packed) != 0 || actual_size != plain_size)
    return Fail(error, "The ring-announcer RASS data could not be decompressed.");
  std::vector<WWE13PachLeafData> inner;
  if (!ReadWWE13PachLeaves(inner_plain, &inner, error)) return false;
  auto rass = std::find_if(inner.begin(), inner.end(), [](const WWE13PachLeafData& leaf) { return leaf.id == 21; });
  if (rass == inner.end()) return Fail(error, "The ring-announcer table is missing PACH leaf 21.");
  std::vector<RaRow> rows;
  if (!ReadRass(rass->data, &rows, error)) return false;
  std::map<uint32_t, uint32_t> have;
  for (const RaRow& row : rows) have[row.id] = row.key;
  std::map<uint32_t, uint32_t> additions;
  for (const WWE13ImportPort& port : ports) {
    if (port.ra_name.empty()) continue;
    uint32_t source_id = 0;
    if (!IsNumber(port.ra_name, &source_id)) return Fail(error, "A ring-announcer source ID is not numeric.");
    const auto source_row = have.find(source_id);
    if (source_row == have.end()) return Fail(error, "The ring-announcer source ID has no RASS row.");
    const uint32_t key = source_row->second;
    if ((key & 0xffffff00u) == 0x8d7b6600u || (key & 0xffffff00u) == 0x8c7b6400u || key == 0)
      return Fail(error, "The selected ring-announcer value is a slot key or zero.");
    const auto existing = have.find(port.target_id);
    if (existing != have.end() && existing->second != key)
      return Fail(error, "A target already has a different ring-announcer value.");
    additions[port.target_id] = key;
  }
  for (const auto& [id, key] : additions) have[id] = key;
  rows.clear();
  for (const auto& [id, key] : have) rows.push_back({id, key});
  rass->data = MakeRass(rows);
  Bytes rebuilt_inner;
  if (!BuildWWE13PachLeaves(inner, &rebuilt_inner, false, error)) return false;
  rebuilt_inner.resize((rebuilt_inner.size() + 3) & ~size_t(3), 0);
  const uint64_t bound = mz_compressBound(static_cast<wwe_mz_ulong>(rebuilt_inner.size()));
  Bytes compressed(static_cast<size_t>(bound));
  wwe_mz_ulong compressed_size = static_cast<wwe_mz_ulong>(bound);
  if (mz_compress2(compressed.data(), &compressed_size, rebuilt_inner.data(),
                static_cast<wwe_mz_ulong>(rebuilt_inner.size()), 9) != 0)
    return Fail(error, "The ring-announcer RASS data could not be compressed.");
  Bytes zlib(16, 0);
  std::memcpy(zlib.data(), "ZLIB", 4);
  std::copy_n(leaf200->data.begin() + 4, 4, zlib.begin() + 4);
  PutLe32(&zlib, 8, static_cast<uint32_t>(compressed_size));
  PutLe32(&zlib, 12, static_cast<uint32_t>(rebuilt_inner.size()));
  zlib.insert(zlib.end(), compressed.begin(), compressed.begin() + static_cast<std::ptrdiff_t>(compressed_size));
  leaf200->data = std::move(zlib);
  Bytes rebuilt_outer;
  if (!BuildWWE13PachLeaves(outer, &rebuilt_outer, false, error)) return false;
  rebuilt_outer.resize((rebuilt_outer.size() + 0xff) & ~size_t(0xff), 0);
  dupl->data = std::move(rebuilt_outer);
  return RebuildWWE13PacGroups(source, groups, output, error);
}

bool PatchDscPackages(const WWE13ImportPaths& paths,
                      const std::vector<WWE13ImportPort>& ports,
                      OutputWriter* writer, std::vector<Bytes>* arcs,
                      std::string* error) {
  const auto source_files = FilesWithPrefix(paths.wwe14_root / "pac/evtdsc", "evtdsc");
  std::map<fs::path, std::map<std::string, std::vector<std::pair<std::string, Bytes>>>> additions;
  PacCache cache;
  for (const WWE13ImportPort& port : ports) {
    std::optional<std::tuple<fs::path, std::string, Bytes>> selected;
    for (const fs::path& path : source_files) {
      const std::vector<WWE13PacGroupData>* groups = nullptr;
      if (!cache.Load(path, &groups, error)) return false;
      for (const WWE13PacGroupData& group : *groups) {
        const std::string type = Trim(group.name);
        if (type.rfind("DSC", 0) != 0) continue;
        const WWE13PacEntryData* entry = FindEntry(&group, "M" + std::to_string(port.donor_id));
        if (entry) selected = std::make_tuple(path, type, entry->data);
      }
    }
    if (!selected) continue;  // The reference reports this as a warning and leaves DSC untouched.
    additions[std::get<0>(*selected)][std::get<1>(*selected)].emplace_back(
        "M" + std::to_string(port.target_id), std::get<2>(*selected));
  }
  std::vector<fs::path> modified;
  for (const auto& [path, groups] : additions) modified.push_back(path);
  std::reverse(modified.begin(), modified.end());
  for (const fs::path& path : modified) {
    Bytes source, output;
    if (!ReadFile(path, &source, error) ||
        !ReplaceOrAppendEntries(source, additions[path], {}, {}, &output, error)) return false;
    for (Bytes& arc : *arcs) if (!SyncArc(&arc, source, output, false, error)) return false;
    if (!writer->Put("game/pac/evtdsc/" + internal::PathToUtf8(path.filename()), output)) return false;
  }
  return true;
}

std::string TitleCase(const std::string& text) {
  std::string result = text;
  bool start = true;
  for (char& c : result) {
    const unsigned char value = static_cast<unsigned char>(c);
    if (std::isalnum(value)) {
      c = static_cast<char>(start ? std::toupper(value) : std::tolower(value));
      start = false;
    } else {
      start = true;
    }
  }
  return result;
}

bool RetargetCatalogOffers(const Bytes& source, const std::vector<WWE13ImportPort>& ports,
                           Bytes* output, std::vector<uint32_t>* remaining,
                           std::string* error) {
  uint16_t count = 0;
  if (!output || !remaining || source.size() < 0x1c)
    return Fail(error, "The host DLC catalog is truncated.");
  count = Le16(source, 12);
  if (uint64_t(0x1c) + uint64_t(count) * 0x40 > source.size())
    return Fail(error, "The host DLC catalog offer list is truncated.");
  std::set<uint16_t> have;
  for (uint16_t i = 0; i < count; ++i) have.insert(Le16(source, 0x1c + size_t(i) * 0x40 + 16));
  std::vector<uint32_t> missing;
  for (const WWE13ImportPort& port : ports) if (!have.count(static_cast<uint16_t>(port.target_id))) missing.push_back(port.target_id);
  std::vector<size_t> dead_slots;
  for (uint16_t i = 0; i < count; ++i) {
    const uint16_t id = Le16(source, 0x1c + size_t(i) * 0x40 + 16);
    if (std::find(kDeadOffers.begin(), kDeadOffers.end(), id) != kDeadOffers.end()) dead_slots.push_back(i);
  }
  Bytes offers(source.begin() + 0x1c, source.begin() + 0x1c + size_t(count) * 0x40);
  const std::map<uint32_t, const WWE13ImportPort*> by_id = [&] {
    std::map<uint32_t, const WWE13ImportPort*> result;
    for (const WWE13ImportPort& port : ports) result[port.target_id] = &port;
    return result;
  }();
  const size_t replacements = std::min(dead_slots.size(), missing.size());
  for (size_t i = 0; i < replacements; ++i) {
    const uint32_t target = missing[i];
    const size_t at = dead_slots[i] * 0x40;
    PutLe16(&offers, at + 16, static_cast<uint16_t>(target));
    const std::string title = TitleCase(by_id.at(target)->name);
    std::fill(offers.begin() + static_cast<std::ptrdiff_t>(at + 32),
              offers.begin() + static_cast<std::ptrdiff_t>(at + 64), 0);
    std::copy_n(title.begin(), std::min<size_t>(title.size(), 31),
                offers.begin() + static_cast<std::ptrdiff_t>(at + 32));
  }
  *remaining = std::vector<uint32_t>(missing.begin() + static_cast<std::ptrdiff_t>(replacements), missing.end());
  output->assign(source.begin(), source.begin() + 0x1c);
  output->insert(output->end(), offers.begin(), offers.end());
  return true;
}

// A wrestler offer to copy for new offers: the host's wrestler-338 offer (standard Add-On 2, unchanged behaviour), else
// the first wrestler offer (type 1 at +4, a real id at +16) in the host, then in packs 3-5. Wrestler offers share one
// layout; the copies get their own ordinal, pack, index, id and title (#118/#120: 4-offer pack 2 has no 338).
std::optional<Bytes> FindOfferTemplate(const WWE13ImportPaths& paths, const Bytes& host_catalog) {
  auto from = [](const Bytes& catalog, bool want_338) -> std::optional<Bytes> {
    if (catalog.size() < 0x1c) return std::nullopt;
    const uint16_t count = Le16(catalog, 12);
    if (uint64_t(0x1c) + uint64_t(count) * 0x40 > catalog.size()) return std::nullopt;
    for (uint16_t i = 0; i < count; ++i) {
      const size_t at = 0x1c + size_t(i) * 0x40;
      const uint16_t id = Le16(catalog, at + 16);
      const bool wrestler = Le16(catalog, at) == 0x40 && Le16(catalog, at + 4) == 1 && id != 0 && Le16(catalog, at + 18) == 0;
      if (want_338 ? id == 338 : wrestler)
        return Bytes(catalog.begin() + static_cast<std::ptrdiff_t>(at), catalog.begin() + static_cast<std::ptrdiff_t>(at + 0x40));
    }
    return std::nullopt;
  };
  if (auto found = from(host_catalog, true)) return found;
  if (auto found = from(host_catalog, false)) return found;
  for (const fs::path& package : ChildDirectories(TitleRoot(paths.userdata_root))) {
    Bytes source;
    std::string ignored;
    std::error_code ec;
    const fs::path catalog = ResolvePathCase(package, "info/catalog.dlc");
    if (!fs::is_regular_file(catalog, ec) || ec || !ReadFile(catalog, &source, &ignored)) continue;
    if (source.size() < 0x1c) continue;
    const uint16_t pack_id = Le16(source, 8);
    if (pack_id != 3 && pack_id != 4 && pack_id != 5) continue;
    if (auto found = from(source, false)) return found;
  }
  return std::nullopt;
}

// Fills the host catalog (header + offers, no file records yet) up to kAddOnOfferCap offers. The standard Add-On 2
// already has 32, so this adds 32 more there; the 4-offer pack 2 takes up to 60 imported wrestlers here.
bool AppendHostOffers(const WWE13ImportPaths& paths, Bytes* catalog, const Bytes& template_record,
                      const std::vector<WWE13ImportPort>& ports, std::vector<uint32_t>* remaining, std::string* error) {
  if (remaining->empty()) return true;
  if (catalog->size() < 0x1c) return Fail(error, "The host DLC catalog is truncated.");
  const uint16_t count = Le16(*catalog, 12);
  if (catalog->size() != 0x1c + size_t(count) * 0x40) return Fail(error, "The host DLC catalog has an unexpected size.");
  if (count >= kAddOnOfferCap) return true;
  std::map<uint32_t, const WWE13ImportPort*> by_id;
  for (const WWE13ImportPort& port : ports) by_id[port.target_id] = &port;
  // New host ordinals start above every ordinal already in the add-ons (the standard Add-On 2 numbers its offers from 50;
  // the 4-offer version uses 1-4, packs 3-5 use 5-12). Ordinals need not be unique across packs: the offers the import has
  // always appended to packs 3-5 restart per pack and overlap each other (v1.5.2, owner-tested 106 import), and a 4-offer
  // import with overlapping pack-2/pack-3 ordinals loaded its wrestlers in game (dlc-small-1). V153 review note.
  uint16_t next_ordinal = 0;
  auto raise = [&](const Bytes& c) {
    if (c.size() < 0x1c) return;
    const uint16_t n = Le16(c, 12);
    if (uint64_t(0x1c) + uint64_t(n) * 0x40 > c.size()) return;
    for (uint16_t i = 0; i < n; ++i)
      next_ordinal = std::max<uint16_t>(next_ordinal, static_cast<uint16_t>(Le16(c, 0x1c + size_t(i) * 0x40 + 2) + 1));
  };
  raise(*catalog);
  for (const fs::path& package : ChildDirectories(TitleRoot(paths.userdata_root))) {
    Bytes other;
    std::string ignored;
    std::error_code ec;
    const fs::path catalog = ResolvePathCase(package, "info/catalog.dlc");
    if (fs::is_regular_file(catalog, ec) && !ec && ReadFile(catalog, &other, &ignored)) raise(other);
  }
  const size_t added = std::min<size_t>(kAddOnOfferCap - count, remaining->size());
  for (size_t j = 0; j < added; ++j) {
    const uint32_t target = (*remaining)[j];
    Bytes record = template_record;
    PutLe16(&record, 2, static_cast<uint16_t>(next_ordinal + j));
    PutLe16(&record, 6, 2);
    PutLe16(&record, 8, static_cast<uint16_t>(count + j));
    PutLe16(&record, 10, static_cast<uint16_t>(next_ordinal + j));
    PutLe16(&record, 16, static_cast<uint16_t>(target));
    const std::string title = TitleCase(by_id.at(target)->name);
    std::fill(record.begin() + 32, record.end(), 0);
    std::copy_n(title.begin(), std::min<size_t>(title.size(), 31), record.begin() + 32);
    catalog->insert(catalog->end(), record.begin(), record.end());
  }
  PutLe16(catalog, 12, static_cast<uint16_t>(count + added));
  remaining->erase(remaining->begin(), remaining->begin() + static_cast<std::ptrdiff_t>(added));
  return true;
}

bool AppendRetailOffers(const WWE13ImportPaths& paths,
                        const std::vector<WWE13ImportPort>& ports,
                        const Bytes& template_record,
                        std::vector<uint32_t>* remaining, OutputWriter* writer,
                        std::string* error) {
  if (remaining->empty()) return true;
  std::map<uint32_t, const WWE13ImportPort*> by_id;
  for (const WWE13ImportPort& port : ports) by_id[port.target_id] = &port;
  for (const fs::path& package : ChildDirectories(TitleRoot(paths.userdata_root))) {
    if (remaining->empty()) break;
    const std::string package_name = internal::PathToUtf8(package.filename());
    Bytes source;
    const fs::path catalog_path = ResolvePathCase(package, "info/catalog.dlc");
    std::error_code ec;
    if (!fs::is_regular_file(catalog_path, ec) || ec) continue;
    if (!ReadFile(catalog_path, &source, error)) return false;
    if (source.size() < 0x1c) return Fail(error, "A retail package catalog header is truncated.");
    const uint16_t pack_id = Le16(source, 8), count = Le16(source, 12);
    if ((pack_id != 3 && pack_id != 4 && pack_id != 5) || count >= kAddOnOfferCap) continue;
    if (uint64_t(0x1c) + uint64_t(count) * 0x40 > source.size())
      return Fail(error, "A retail package offer list is truncated.");
    uint16_t next_ordinal = 0;
    for (uint16_t i = 0; i < count; ++i)
      next_ordinal = std::max<uint16_t>(next_ordinal, static_cast<uint16_t>(Le16(source, 0x1c + size_t(i) * 0x40 + 2) + 1));
    const size_t capacity = kAddOnOfferCap - count;
    const size_t added = std::min(capacity, remaining->size());
    if (!added) continue;
    Bytes records;
    records.reserve(added * 0x40);
    for (size_t j = 0; j < added; ++j) {
      const uint32_t target = (*remaining)[j];
      Bytes record = template_record;
      PutLe16(&record, 2, static_cast<uint16_t>(next_ordinal + j));
      PutLe16(&record, 6, pack_id);
      PutLe16(&record, 8, static_cast<uint16_t>(count + j));
      PutLe16(&record, 10, static_cast<uint16_t>(next_ordinal + j));
      PutLe16(&record, 16, static_cast<uint16_t>(target));
      const std::string title = TitleCase(by_id.at(target)->name);
      std::fill(record.begin() + 32, record.end(), 0);
      std::copy_n(title.begin(), std::min<size_t>(title.size(), 31), record.begin() + 32);
      records.insert(records.end(), record.begin(), record.end());
    }
    Bytes output(source.begin(), source.begin() + 0x1c);
    PutLe16(&output, 12, static_cast<uint16_t>(count + added));
    output.insert(output.end(), source.begin() + 0x1c,
                  source.begin() + static_cast<std::ptrdiff_t>(0x1c + size_t(count) * 0x40));
    output.insert(output.end(), records.begin(), records.end());
    output.insert(output.end(), source.begin() + static_cast<std::ptrdiff_t>(0x1c + size_t(count) * 0x40), source.end());
    if (!writer->Put(UserdataPath(package_name, "info/catalog.dlc"), output)) return false;
    remaining->erase(remaining->begin(), remaining->begin() + static_cast<std::ptrdiff_t>(added));
  }
  if (!remaining->empty())
    return Fail(error, "Your WWE 2K14 add-ons have no room for " + std::to_string(remaining->size()) +
                           " of the chosen wrestlers (each add-on holds 64 unlock entries; first one without room: " +
                           std::to_string(remaining->front()) + "). Import fewer wrestlers, or add more WWE 2K14 add-ons "
                           "on the Game files page. Nothing has been changed.");
  return true;
}

bool BuildHostCatalog(const WWE13ImportPaths& paths,
                      const std::vector<WWE13ImportPort>& ports,
                      const std::vector<PackageFileInfo>& files,
                      OutputWriter* writer, Bytes* hd_arc,
                      Bytes* sd_arc, std::string* error) {
  Bytes original;
  const std::string host_pack = FindWWE2K14HostPack(paths.userdata_root);
  if (host_pack.empty()) return Fail(error, "WWE 2K14 Add-On 2 was not found in the userdata folder.");
  if (!ReadFile(ResolvePathCase(TitleRoot(paths.userdata_root) / fs::u8path(host_pack), "info/catalog.dlc"), &original, error)) return false;
  Bytes catalog;
  std::vector<uint32_t> remaining;
  if (!RetargetCatalogOffers(original, ports, &catalog, &remaining, error)) return false;
  if (!remaining.empty()) {
    const std::optional<Bytes> offer_template = FindOfferTemplate(paths, original);
    if (!offer_template) return Fail(error, "No wrestler unlock entry was found in your WWE 2K14 add-ons to copy for the imported wrestlers.");
    if (!AppendHostOffers(paths, &catalog, *offer_template, ports, &remaining, error)) return false;
    if (!AppendRetailOffers(paths, ports, *offer_template, &remaining, writer, error)) return false;
  }
  if (files.size() > 0xffff) return Fail(error, "The host package contains too many file records.");
  Bytes records;
  uint16_t hd_index = 0, sd_index = 0;
  for (const PackageFileInfo& file : files) {
    const std::string path = [&] { std::string p = file.relative; std::replace(p.begin(), p.end(), '/', '\\'); return p; }();
    if (path.size() > 255 || 7 + path.size() > 0xffff)
      return Fail(error, "A host catalog path exceeds its byte-length field.");
    if (file.kind == "mov") {
      const size_t at = records.size();
      records.resize(at + 7, 0);
      PutLe16(&records, at, static_cast<uint16_t>(7 + path.size()));
      PutLe32(&records, at + 2, 0xffffffffu);
      records[at + 6] = static_cast<uint8_t>(path.size());
      records.insert(records.end(), path.begin(), path.end());
      continue;
    }
    uint16_t hdi = 0xffff, sdi = 0xffff;
    if (file.kind == "hd" || file.kind == "both") { hdi = hd_index++; }
    if (file.kind == "sd" || file.kind == "both") { sdi = sd_index++; }
    const size_t at = records.size();
    records.resize(at + 7, 0);
    PutLe16(&records, at, static_cast<uint16_t>(7 + path.size()));
    PutLe16(&records, at + 2, hdi); PutLe16(&records, at + 4, sdi);
    records[at + 6] = static_cast<uint8_t>(path.size());
    records.insert(records.end(), path.begin(), path.end());
  }
  PutLe16(&catalog, 18, static_cast<uint16_t>(files.size()));
  catalog.insert(catalog.end(), records.begin(), records.end());
  catalog.resize(catalog.size() + 4, 0);
  if (!writer->Put(UserdataPath(HostOutputPack(host_pack), "info/catalog.dlc"), catalog)) return false;
  if (!BuildArc(files, "hd", hd_arc, error) || !BuildArc(files, "sd", sd_arc, error)) return false;
  return true;
}

bool SyncOverlayFile(const fs::path& package, const fs::path& source_path,
                     const Bytes& source, const Bytes& converted,
                     std::map<std::string, Bytes>* arcs,
                     OutputWriter* writer, std::string* error) {
  for (auto& [relative, arc] : *arcs) {
    (void)relative;
    if (!SyncArc(&arc, source, converted, true, error)) return false;
  }
  const fs::path relative = source_path.lexically_relative(package);
  if (relative.empty() || internal::PathToUtf8(relative).rfind("..", 0) == 0)
    return Fail(error, "An overlay PAC is outside its source package.");
  return writer->Put(UserdataPath(internal::PathToUtf8(package.filename()), relative.generic_string()), converted);
}

bool WriteOverlayChanges(const WWE13ImportPaths& paths,
                         const std::vector<fs::path>& overlays,
                         const std::vector<WWE13ImportPort>& ports,
                         const std::vector<WWE13ConvertedRows>& rows,
                         const WWE13StringKeyMap& names,
                         const std::vector<WWE13RegistryChar>& registry_chars,
                         const std::vector<WWE13AttireColorPort>& attire,
                         PacCache* cache, OutputWriter* writer, std::string* error) {
  for (const fs::path& package : overlays) {
    std::map<std::string, Bytes> arcs;
    for (const std::string& arc_name : {"info/catalog_hd.arc", "info/catalog_sd.arc"})
      if (!ReadFile(ResolvePathCase(package, arc_name), &arcs[arc_name], error)) return false;
    std::vector<std::pair<fs::path, Bytes>> todo;
    for (const fs::path& path : FilesWithPrefix(ResolvePathCase(package, "pac"), "string_dlc")) {
      Bytes old, next;
      if (!ReadFile(path, &old, error)) return false;
      size_t inserted = 0;
      if (!ConvertWWE13StringKeys(old, names, &next, &inserted, error)) return false;
      todo.emplace_back(path, std::move(next));
    }
    for (const fs::path& path : FilesWithPrefix(ResolvePathCase(package, "pac"), "DLC_dlc")) {
      Bytes old, next;
      if (!ReadFile(path, &old, error) ||
          !ConvertWWE13DlcRegistry(old, registry_chars,
                                   internal::PathToUtf8(path.filename()), &next, error)) return false;
      todo.emplace_back(path, std::move(next));
    }
    for (const fs::path& path : FilesWithPrefix(ResolvePathCase(package, "pac/ch"), "chEtc_dlc")) {
      Bytes old, next;
      if (!ReadFile(path, &old, error) || !PatchChEtc(old, rows, &next, error)) return false;
      todo.emplace_back(path, std::move(next));
    }
    for (const fs::path& path : FilesWithPrefix(ResolvePathCase(package, "pac"), "misc_dlc")) {
      Bytes old, next;
      if (!ReadFile(path, &old, error) || !ConvertWWE13AttireColors(old, attire, &next, error)) return false;
      todo.emplace_back(path, std::move(next));
    }
    const std::vector<WWE13ImportPort> guests = [&] {
      std::vector<WWE13ImportPort> result;
      for (const WWE13ImportPort& port : ports) if (port.ent_host == PackCode(package)) result.push_back(port);
      return result;
    }();
    if (!guests.empty()) for (const fs::path& path : FilesWithPrefix(ResolvePathCase(package, "pac/evt"), "Entrance_dlc")) {
      Bytes old, next;
      if (!ReadFile(path, &old, error)) return false;
      std::map<std::string, std::vector<std::pair<std::string, Bytes>>> additions;
      for (const std::string group : {"EVP", "EVZ"}) {
        std::map<std::string, Bytes> family;
        for (const WWE13ImportPort& port : guests)
          if (!EntranceFamily(paths, port, cache, group, &family, error)) return false;
        for (const auto& [key, data] : family) additions[group].emplace_back(key, data);
      }
      if (!ReplaceOrAppendEntries(old, additions, {}, {}, &next, error)) return false;
      todo.emplace_back(path, std::move(next));
    }
    for (const auto& [path, next] : todo) {
      Bytes old;
      if (!ReadFile(path, &old, error) || !SyncOverlayFile(package, path, old, next, &arcs, writer, error)) return false;
    }
    for (const auto& [arc_name, arc] : arcs)
      if (!writer->Put(UserdataPath(internal::PathToUtf8(package.filename()), arc_name), arc)) return false;
  }
  return true;
}

bool BuildPortCharacterFiles(const WWE13ImportPaths& paths,
                             const std::vector<WWE13ImportPort>& ports,
                             OutputWriter* writer, const std::string& host_pack,
                             std::string* error) {
  // Layout templates for WWE '13 cloth: WWE 2K14's own Undertaker (ch103, attire 04 = hk_2012.2 cloth) from the player's
  // game folder. Missing (other game version): the cloth leaves stay as they are.
  std::map<uint32_t, Bytes> cloth_templates;
  {
    Bytes stock;
    std::string ignored;
    if (ReadFile(paths.wwe14_root / "pac/ch/ch103.pac", &stock, &ignored)) {
      for (const uint32_t id : {9000u, 9002u, 9010u, 9012u}) {
        Bytes leaf, decoded;
        if (!ReadWWE13CharacterLeaf(stock, "00010304", id, &leaf, &ignored)) continue;
        if (!DecodeWWE13Bpe(leaf, &decoded, &ignored)) decoded = leaf;
        if (HavokPackfileVersion(decoded).rfind("hk_2012", 0) == 0) cloth_templates[id] = std::move(decoded);
      }
    }
  }
  for (const WWE13ImportPort& port : ports) {
    fs::path source_path;
    if (!FindSourceCharacter(paths, port.source_id, &source_path))
      return Fail(error, "The WWE '13 character PAC is missing for source " + std::to_string(port.source_id) + ".");
    Bytes source, converted;
    if (!ReadFile(source_path, &source, error)) return false;
    WWE13CharacterPacRecipe recipe;
    recipe.source_id = port.source_id;
    recipe.target_id = port.target_id;
    recipe.leaf_10000_source_id = port.leaf10000;
    recipe.stock_model_source_id = port.stock_model_source_id;
    recipe.stock_body_source_id = port.stock_body_source_id;
    recipe.add_shared_leaf50 = port.leaf_fix == "add50";
    recipe.waive_missing_20002 = port.leaf_fix == "waive20002";
    recipe.cloth_templates = &cloth_templates;
    Bytes leaf10000, model, body, donor, shared_leaf;
    const Bytes* leaf_ptr = nullptr; const Bytes* model_ptr = nullptr;
    const Bytes* body_ptr = nullptr; const Bytes* shared_ptr = nullptr;
    const auto read_stock_character = [&](uint32_t id, Bytes* bytes, const Bytes** pointer) {
      if (!ReadFile(paths.wwe14_root / "pac/ch" / ("ch" + std::to_string(id) + ".pac"), bytes, error)) return false;
      *pointer = bytes; return true;
    };
    if (port.leaf10000) {
      if (!ReadFile(ResolvePathCase(paths.wwe13_root, "pac/ch/" + ("ch" + std::to_string(*port.leaf10000) + ".pac")), &leaf10000, error)) return false;
      leaf_ptr = &leaf10000;
    }
    if (port.stock_model_source_id && !read_stock_character(*port.stock_model_source_id, &model, &model_ptr)) return false;
    if (port.stock_body_source_id && !read_stock_character(*port.stock_body_source_id, &body, &body_ptr)) return false;
    if (recipe.add_shared_leaf50) {
      if (!read_stock_character(port.donor_id, &donor, &shared_ptr) ||
          !ReadWWE13CharacterLeaf(donor, Id(port.donor_id, 6) + "02", 50, &shared_leaf, error)) return false;
      shared_ptr = &shared_leaf;
    }
    if (!ConvertWWE13CharacterPac(recipe, source, leaf_ptr, model_ptr, body_ptr,
                                  shared_ptr, &converted, error)) return false;
    if (!writer->Put(UserdataPath(host_pack, "pac/ch/ch" + std::to_string(port.target_id) + "_dlc02.pac"), converted)) return false;
  }
  return true;
}

bool SafeToStage(const fs::path& staging, const WWE13ImportPaths& paths,
                 std::string* error) {
  std::error_code ec;
  const fs::path target = fs::absolute(staging, ec).lexically_normal();
  if (ec) return Fail(error, "The staging directory path could not be resolved: " + ec.message());
  std::vector<fs::path> roots{paths.wwe13_root, paths.wwe14_root, paths.userdata_root};
  roots.insert(roots.end(), paths.wwe13_pack_roots.begin(), paths.wwe13_pack_roots.end());
  for (const fs::path& root : roots) {
    if (root.empty()) continue;
    const fs::path input = fs::absolute(root, ec).lexically_normal();
    if (ec) return Fail(error, "An input root path could not be resolved: " + ec.message());
    auto prefix = input.begin();
    auto candidate = target.begin();
    for (; prefix != input.end() && candidate != target.end() && *prefix == *candidate; ++prefix, ++candidate) {}
    if (prefix == input.end()) return Fail(error, "The staging directory may not be inside a read-only source tree.");
  }
  return true;
}

bool ReadTitantron(const WWE13ImportPaths& paths, const WWE13ImportPort& port,
                   Bytes* data, std::string* error) {
  uint32_t stand_in = 0;
  if (!IsNumber(port.tron, &stand_in))
    return Fail(error, "The titantron for target " + std::to_string(port.target_id) + " must name a numeric WWE 2K14 stand-in.");
  if (!ReadFile(paths.wwe14_root / "movies/titantron" / (std::to_string(stand_in) + ".bik2"), data, error)) return false;
  if (!StartsWith(*data, "KB2f"))
    return Fail(error, "The WWE 2K14 titantron stand-in is not a KB2f movie for target " + std::to_string(port.target_id) + ".");
  return true;
}

void RegisterHostExistingFile(OutputWriter* writer, std::vector<PackageFileInfo>* files,
                              const std::string& host_pack, const std::string& relative,
                              const std::string& kind) {
  PackageFileInfo info;
  info.relative = relative;
  info.kind = kind;
  info.output_path = writer->files_root / UserdataPath(host_pack, relative);
  files->push_back(std::move(info));
}

}  // namespace

namespace {
// What the launcher saw in the add-on folder, for the "Add-On 2 missing" message (#103: players with Add-On 2
// installed still got it, and a screenshot of this line tells which case it is).
std::string DescribeAddOns(const fs::path& title) {
  std::string text = "Checked: " + internal::PathToUtf8(title) + ". ";
  const std::vector<fs::path> packages = ChildDirectories(title);
  if (packages.empty()) return text + "No add-on folders are there.";
  text += "Add-ons there:";
  for (const fs::path& package : packages) {
    const std::string name = internal::PathToUtf8(package.filename());
    text += " " + (name.size() > 12 ? name.substr(0, 8) + "..(" + std::to_string(name.size()) + ")" : name);
    std::error_code ec;
    const fs::path catalog_path = ResolvePathCase(package, "info/catalog.dlc");
    Bytes catalog;
    std::string ignored;
    if (!fs::is_regular_file(catalog_path, ec) || ec || !ReadFile(catalog_path, &catalog, &ignored)) { text += " [no catalog];"; continue; }
    if (catalog.size() < 0x1c || catalog[0] != 'D' || catalog[1] != 'L' || catalog[2] != 'C' || catalog[3] != 'C') {
      text += " [catalog " + std::to_string(catalog.size()) + " bytes, unknown];";
      continue;
    }
    const uint16_t count = Le16(catalog, 12);
    text += " [pack " + std::to_string(Le16(catalog, 8)) + ", " + std::to_string(count) + " offers" +
            (IsHostCatalog(catalog) ? ", is Add-On 2" : IsPack2Catalog(catalog) ? ", can host" : "") + "];";
  }
  std::error_code ec;
  for (const char* pack : {kHostPack, kExtraOfferPack}) {
    const fs::path header = title.parent_path() / "Headers" / "00000002" / (std::string(pack) + ".header");
    text += " " + std::string(pack).substr(0, 8) + (fs::exists(header, ec) ? " header present." : " header absent.");
  }
  return text;
}
}  // namespace

// The import's output: the host pack renamed so it loads first (first character '0'). A host under a name that is not a
// 42-character content id (added under another file name, #103) keeps the standard output name, as before.
std::string WWE13HostOutputPack(const std::string& host) {
  if (host.size() != 42) return HostOutputPack(kHostPack);
  std::string result = host;
  result[0] = '0';
  return result;
}

std::string FindWWE2K14HostPack(const std::filesystem::path& userdata_root) {
  const fs::path title = TitleRoot(userdata_root);
  auto is_host = [](const fs::path& package) {
    std::error_code ec;
    const fs::path catalog_path = ResolvePathCase(package, "info/catalog.dlc");
    if (!fs::is_regular_file(catalog_path, ec) || ec) return false;
    Bytes catalog;
    std::string ignored;
    return ReadFile(catalog_path, &catalog, &ignored) && IsHostCatalog(catalog);
  };
  if (is_host(title / kHostPack)) return kHostPack;
  // Never the import's own output (a 42-character name starting with '0': the host renamed), a half-installed add-on,
  // or a name the add-on header cannot hold. Retail pack-2 names (745BEE31.., 267E5576..) do not start with '0'.
  auto candidate = [](const std::string& name) {
    return !(name.size() > 42 || (name.size() == 42 && name[0] == '0') ||
             (name.size() >= 8 && name.compare(name.size() - 8, 8, ".partial") == 0));
  };
  const std::vector<fs::path> packages = ChildDirectories(title);
  for (const fs::path& package : packages) {
    const std::string name = internal::PathToUtf8(package.filename());
    if (candidate(name) && is_host(package)) return name;
  }
  // No standard Add-On 2: any other version of pack 2 hosts the import (#103 #111 #118 #120, owner 2026-11-10).
  for (const fs::path& package : packages) {
    const std::string name = internal::PathToUtf8(package.filename());
    if (!candidate(name)) continue;
    std::error_code ec;
    Bytes catalog;
    std::string ignored;
    const fs::path catalog_path = ResolvePathCase(package, "info/catalog.dlc");
    if (fs::is_regular_file(catalog_path, ec) && !ec &&
        ReadFile(catalog_path, &catalog, &ignored) && IsPack2Catalog(catalog))
      return name;
  }
  return {};
}

bool RunWWE13ImportStages2To9(const WWE13ImportPaths& paths,
                             const std::vector<WWE13ImportPort>& requested_ports,
                             const std::filesystem::path& fresh_staging_directory,
                             const WWE13ImportOptions& options,
                             WWE13ImportRunResult* result, std::string* error,
                             const ProgressFn& progress, const CancelFlag* cancel) {
  if (error) error->clear();
  const CancelFlag* previous_cancel = g_import_cancel;
  const ProgressFn* previous_progress = g_import_progress;
  g_import_cancel = cancel;
  g_import_progress = &progress;
  struct CallbackRestore {
    const CancelFlag* cancel;
    const ProgressFn* progress;
    ~CallbackRestore() { g_import_cancel = cancel; g_import_progress = progress; }
  } callback_restore{previous_cancel, previous_progress};
  if (!result) return Fail(error, "The import run result output is missing.");
  if (Cancelled(error)) return false;
  if (requested_ports.empty()) return Fail(error, "The import run requires at least one port.");
  std::error_code ec;
  if (fresh_staging_directory.empty() || fs::exists(fresh_staging_directory, ec) || ec)
    return Fail(error, "The requested staging directory must be fresh and must not already exist.");
  if (paths.wwe13_root.empty() || paths.wwe14_root.empty() || paths.userdata_root.empty())
    return Fail(error, "The WWE '13, WWE 2K14, and userdata input roots are required.");
  if (!SafeToStage(fresh_staging_directory, paths, error)) return false;
  for (const fs::path& root : {paths.wwe13_root, paths.wwe14_root, paths.userdata_root}) {
    if (!fs::is_directory(root, ec) || ec)
      return Fail(error, "An input root is not an accessible directory: " + internal::PathToUtf8(root));
  }
  if (!fs::create_directories(fresh_staging_directory, ec) || ec)
    return Fail(error, "The fresh staging directory could not be created: " + ec.message());

  bool keep_staging = false;
  struct Cleanup {
    fs::path path;
    bool* keep;
    ~Cleanup() { if (!*keep) { std::error_code ec; fs::remove_all(path, ec); } }
  } cleanup{fresh_staging_directory, &keep_staging};
  const auto fail = [&](const std::string& message) {
    if (error) *error = message;
    return false;
  };

  std::vector<WWE13ImportPort> ports = requested_ports;
  std::sort(ports.begin(), ports.end(), [](const auto& a, const auto& b) { return a.target_id < b.target_id; });
  std::set<uint32_t> targets;
  for (const WWE13ImportPort& port : ports) {
    if (port.target_id < 100 || port.target_id > 499 || port.source_id > 499 || port.donor_id > 499)
      return fail("A port contains an ID outside the supported 2K14 roster range.");
    if (!targets.insert(port.target_id).second) return fail("The chosen port list contains a duplicate target ID.");
    if (port.name.empty()) return fail("Every chosen port needs a wrestler name.");
    if (port.source_id != port.target_id && !port.own_pro)
      return fail("A remapped wrestler must use the converted WWE '13 PRO row.");
  }

  const std::vector<fs::path> overlays = OverlayPackages(paths);
  if (overlays.empty()) return fail("Importing needs your WWE 2K14 DLC packs: add them on the Game files page first (Add TU or DLC). Nothing has been changed.");
  {
    // The imported wrestlers are offered through one add-on folder; check it before any work so the player gets a
    // clear message instead of a file error (player report 2026-11-07).
    // It is found by its contents, so a copy added under another file name works too (#103).
    const fs::path title = TitleRoot(paths.userdata_root);
    if (FindWWE2K14HostPack(paths.userdata_root).empty()) {
      for (const std::string& output : {HostOutputPack(kHostPack), HostOutputPack(kExtraOfferPack)}) {
        std::error_code moved_error;
        if (fs::is_directory(title / output, moved_error))
          return fail("This userdata folder already holds an earlier WWE '13 import that the launcher has no record of "
                      "(for example, copied from another install): the folder " + output + " is there and the add-on it "
                      "was made from is not. Delete that folder, add your WWE 2K14 add-ons again on the Game files page "
                      "(Add TU or DLC), then import. Nothing has been changed.");
      }
      return fail("Importing needs one version of WWE 2K14 Add-On 2 (the add-on with the pre-order bonuses, folder " +
                  std::string(kHostPack) + " or " + kExtraOfferPack + ") and none was found. Add it on the Game files "
                  "page first (Add TU or DLC). Nothing has been changed. " + DescribeAddOns(title));
    }
  }
  ReportStage(1, "Reading wrestler rows and add-on tables…");
  std::vector<WWE13ConvertedRows> rows;
  if (!ConvertRowsForPorts(paths, ports, overlays, options.own_attire_codes, &rows, error)) return false;
  WWE13StringKeyMap names;
  MakeNameMap(ports, &names);
  std::vector<WWE13RegistryChar> registry_chars;
  if (!MakeRegistryChars(paths, overlays, ports, &registry_chars, error)) return false;
  const fs::path base_dlc_path = paths.wwe14_root / "pac/DLC.pac";
  Bytes base_dlc, registry;
  if (!ReadFile(base_dlc_path, &base_dlc, error) ||
      !ConvertWWE13DlcRegistry(base_dlc, registry_chars,
                               internal::PathToUtf8(base_dlc_path.filename()),
                               &registry, error)) return false;
  std::vector<WWE13AttireColorPort> attire;
  attire.reserve(ports.size());
  for (const WWE13ImportPort& port : ports) attire.push_back({port.target_id, port.donor_id});

  OutputWriter writer;
  writer.files_root = fresh_staging_directory / "files";
  writer.error = error;
  if (!fs::create_directories(writer.files_root, ec) || ec) return fail("The staging files directory could not be created: " + ec.message());
  const std::string host_pack = HostOutputPack(FindWWE2K14HostPack(paths.userdata_root));
  ReportStage(2, "Stage 2/9 · building character files…");
  if (!BuildPortCharacterFiles(paths, ports, &writer, host_pack, error)) return false;

  std::vector<Bytes> base_arcs;
  for (const std::string& name : {"plist360.arc", "plist360_4x3.arc"}) {
    Bytes arc;
    if (!ReadFile(paths.wwe14_root / name, &arc, error)) return false;
    base_arcs.push_back(std::move(arc));
  }
  Bytes old_string, new_string;
  ReportStage(3, "Stages 3–5/9 · building rows, strings, and attire…");
  if (!ReadFile(paths.wwe14_root / "pac/string.pac", &old_string, error)) return false;
  size_t inserted_strings = 0;
  if (!ConvertWWE13StringKeys(old_string, names, &new_string, &inserted_strings, error) ||
      !ModifyBase(paths, &writer, "pac/string.pac", old_string, new_string, &base_arcs, error)) return false;
  if (!ModifyBase(paths, &writer, "pac/DLC.pac", base_dlc, registry, &base_arcs, error)) return false;

  Bytes old_etc, new_etc;
  ReportStage(6, "Stage 6/9 · updating registries and roster tables…");
  if (!ReadFile(paths.wwe14_root / "pac/ch/chEtc.pac", &old_etc, error) ||
      !PatchChEtc(old_etc, rows, &new_etc, error) ||
      !ModifyBase(paths, &writer, "pac/ch/chEtc.pac", old_etc, new_etc, &base_arcs, error)) return false;
  Bytes old_misc, new_misc;
  if (!ReadFile(paths.wwe14_root / "pac/misc01_Start.pac", &old_misc, error) ||
      !ConvertWWE13AttireColors(old_misc, attire, &new_misc, error) ||
      !ModifyBase(paths, &writer, "pac/misc01_Start.pac", old_misc, new_misc, &base_arcs, error)) return false;
  if (!PatchDscPackages(paths, ports, &writer, &base_arcs, error)) return false;
  if (std::any_of(ports.begin(), ports.end(), [](const WWE13ImportPort& p) { return !p.ra_name.empty(); })) {
    Bytes old_sound, new_sound;
    if (!ReadFile(paths.wwe14_root / "pac/sound.pac", &old_sound, error) ||
        !PatchRingAnnouncer(old_sound, ports, &new_sound, error) ||
        !ModifyBase(paths, &writer, "pac/sound.pac", old_sound, new_sound, &base_arcs, error)) return false;
  }

  PacCache cache;
  ReportStage(7, "Stage 7/9 · building entrances and move data…");
  if (!WriteOverlayChanges(paths, overlays, ports, rows, names, registry_chars, attire,
                           &cache, &writer, error)) return false;

  std::vector<PackageFileInfo> package_files;
  std::vector<std::string> notices;
  AddWWE13GridFaces(paths, ports, &writer, &base_arcs, &notices);
  CancelFlag never_cancel{false};
  const CancelFlag& conversion_cancel = cancel ? *cancel : never_cancel;
  ReportStage(8, "Stage 8/9 · converting titantrons…");
  // Several videos at once, one thread each: output bytes do not depend on thread count, and one video
  // barely gains from more threads (106 videos on 12 threads: 71-84 s this way against several minutes).
  struct MovieJob {
    const WWE13ImportPort* port = nullptr;
    fs::path source, stand_in, staged;
    std::string label;
    WWE13TitantronResult conversion;
    Result result = Result::Ok();
  };
  std::vector<MovieJob> movie_jobs;
  if (!paths.bink_converter_executable.empty()) {
    for (const WWE13ImportPort& port : ports) {
      uint32_t stand_in = 0;
      if (!IsNumber(port.tron, &stand_in)) return fail("The titantron for target " + std::to_string(port.target_id) + " must name a numeric WWE 2K14 stand-in.");
      MovieJob job;
      job.port = &port;
      // Base game first, then the add-ons. Names under 100 are written with leading zeros (050.bik2).
      char padded[16]{};
      std::snprintf(padded, sizeof(padded), "%03u.bik2", port.source_id);
      std::vector<fs::path> movie_roots{paths.wwe13_root};
      movie_roots.insert(movie_roots.end(), paths.wwe13_pack_roots.begin(), paths.wwe13_pack_roots.end());
      for (const fs::path& root : movie_roots) {
        std::error_code movie_error;
        const fs::path candidate = root / "movies/titantron" / padded;
        if (fs::is_regular_file(candidate, movie_error) && !movie_error) { job.source = candidate; break; }
      }
      job.stand_in = paths.wwe14_root / "movies/titantron" / (std::to_string(stand_in) + ".bik2");
      job.staged = fresh_staging_directory / ("titantron-" + std::to_string(port.target_id) + ".bik2");
      job.label = port.name.empty() ? "Wrestler " + std::to_string(port.target_id) : port.name;
      movie_jobs.push_back(std::move(job));
    }
#ifdef _WIN32
    _putenv_s("WWE_BINK_THREADS", "1");
#else
    setenv("WWE_BINK_THREADS", "1", 1);
#endif
    std::atomic<size_t> next_job{0};
    std::mutex progress_mutex;
    size_t movies_done = 0;
    const auto work = [&] {
      for (;;) {
        const size_t index = next_job.fetch_add(1);
        if (index >= movie_jobs.size() || conversion_cancel.load(std::memory_order_relaxed)) return;
        MovieJob& job = movie_jobs[index];
        // Kept by source number and size: retail files don't change, so a retry or "add more" skips the work.
        std::error_code cache_error;
        fs::path cached;
        if (!paths.titantron_cache.empty() && !job.source.empty()) {
          const uint64_t size = fs::file_size(job.source, cache_error);
          if (!cache_error) cached = paths.titantron_cache / (std::to_string(job.port->source_id) + "-" + std::to_string(size) + ".bik2");
        }
        cache_error.clear();
        bool from_cache = false;
        if (!cached.empty() && fs::is_regular_file(cached, cache_error) && !cache_error && fs::file_size(cached, cache_error) > 0 && !cache_error)
          from_cache = fs::copy_file(cached, job.staged, fs::copy_options::overwrite_existing, cache_error) && !cache_error;
        if (!from_cache) {
          job.result = ConvertWWE13TitantronOrStandIn(paths.bink_converter_executable, job.source, job.stand_in, job.staged,
                                                      job.label, {}, conversion_cancel, &job.conversion);
          if (job.result.ok && !job.conversion.used_stand_in && !cached.empty()) {
            cache_error.clear();
            fs::create_directories(cached.parent_path(), cache_error);
            fs::path part = cached;
            part += ".part";
            cache_error.clear();
            if (fs::copy_file(job.staged, part, fs::copy_options::overwrite_existing, cache_error) && !cache_error) fs::rename(part, cached, cache_error);
            if (cache_error) { std::error_code ignored; fs::remove(part, ignored); }
          }
        }
        std::lock_guard lock(progress_mutex);
        ++movies_done;
        if (progress) progress(movies_done, movie_jobs.size(), "Converting titantrons: " + std::to_string(movies_done) + " of " + std::to_string(movie_jobs.size()));
      }
    };
    const unsigned hardware = std::max(2u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < std::max(2u, hardware / 2); ++i) workers.emplace_back(work);
    for (std::thread& worker : workers) worker.join();
    if (conversion_cancel.load(std::memory_order_relaxed)) return fail("The import was canceled.");
    for (const MovieJob& job : movie_jobs) if (!job.result.ok) return fail(job.result.error);
  }
  for (size_t i = 0; i < ports.size(); ++i) {
    const WWE13ImportPort& port = ports[i];
    if (cancel && cancel->load(std::memory_order_relaxed)) return fail("The import was canceled.");
    Bytes movie;
    if (paths.bink_converter_executable.empty()) {
      if (!ReadTitantron(paths, port, &movie, error)) return false;
    } else {
      const MovieJob& job = movie_jobs[i];
      if (!job.conversion.message.empty()) notices.push_back(job.conversion.message);
      if (!ReadFile(job.staged, &movie, error)) return false;
      std::error_code remove_error;
      fs::remove(job.staged, remove_error);
    }
    if (!AddHostPackageFile(&writer, &package_files, host_pack,
                            "movies/titantron/" + std::to_string(port.target_id) + ".bik2", "mov", movie, error)) return false;
  }
  for (const std::string kind : {"HD", "SD"}) {
    Bytes portraits;
    if (!BuildPortraitPac(paths, ports, kind, &portraits, error)) return false;
    const std::string relative = "pac/DLC_" + kind + "_dlc02.pac";
    if (!AddHostPackageFile(&writer, &package_files, host_pack, relative,
                            kind == "HD" ? "hd" : "sd", portraits, error)) return false;
  }
  for (const WWE13ImportPort& port : ports) {
    const std::string relative = "pac/ch/ch" + std::to_string(port.target_id) + "_dlc02.pac";
    RegisterHostExistingFile(&writer, &package_files, host_pack, relative, "both");
  }
  std::map<std::string, Bytes> host_evp, host_evz;
  for (const WWE13ImportPort& port : ports) {
    if (!port.ent_host.empty()) continue;
    if (!EntranceFamily(paths, port, &cache, "EVP", &host_evp, error) ||
        !EntranceFamily(paths, port, &cache, "EVZ", &host_evz, error)) return false;
  }
  Bytes entrance;
  if (!BuildFamilyPac(host_evp, host_evz, &entrance, error) ||
      !AddHostPackageFile(&writer, &package_files, host_pack, "pac/evt/Entrance_dlc02.pac", "both", entrance, error)) return false;

  Bytes winning, event_move, gm, prtcls;
  if (!BuildWinningPac(paths, ports, &cache, &winning, error) ||
      !BuildEventMovePac(paths, ports, &cache, &event_move, error) ||
      !BuildGmPac(paths, ports, &cache, &gm, error) ||
      !BuildPrtclsPac(paths, ports, &cache, &prtcls, error)) return false;
  if (!AddHostPackageFile(&writer, &package_files, host_pack, "pac/evt/Winning_dlc02.pac", "both", winning, error) ||
      !AddHostPackageFile(&writer, &package_files, host_pack, "pac/evt/evtmov_dlc02.pac", "both", event_move, error) ||
      !AddHostPackageFile(&writer, &package_files, host_pack, "pac/gm_dlc02.pac", "both", gm, error) ||
      !AddHostPackageFile(&writer, &package_files, host_pack, "pac/prtcls_dlc02.pac", "both", prtcls, error)) return false;

  if (!AddHostPackageFile(&writer, &package_files, host_pack, "pac/DLC_dlc02.pac", "both", registry, error) ||
      !AddHostPackageFile(&writer, &package_files, host_pack, "pac/string_dlc02.pac", "both", new_string, error) ||
      !AddHostPackageFile(&writer, &package_files, host_pack, "pac/ch/chEtc_dlc02.pac", "both", new_etc, error)) return false;
  Bytes exclusive_ai;
  bool has_exclusive_ai = false;
  if (!BuildExclusiveAiPac(paths, ports, &exclusive_ai, &has_exclusive_ai, error)) return false;
  if (has_exclusive_ai &&
      !AddHostPackageFile(&writer, &package_files, host_pack, "pac/ExclusiveAI_dlc02.pac", "both", exclusive_ai, error)) return false;

  Bytes hd_arc, sd_arc;
  ReportStage(9, "Stage 9/9 · finishing add-on packages and indexes…");
  if (!BuildHostCatalog(paths, ports, package_files, &writer, &hd_arc, &sd_arc, error)) return false;
  if (!writer.Put(UserdataPath(host_pack, "info/catalog_hd.arc"), hd_arc) ||
      !writer.Put(UserdataPath(host_pack, "info/catalog_sd.arc"), sd_arc)) return false;
  for (const std::string& name : {"plist360.arc", "plist360_4x3.arc"}) {
    const size_t index = name == "plist360.arc" ? 0 : 1;
    if (!writer.Put("game/" + name, base_arcs[index])) return false;
  }

  result->staging_directory = fresh_staging_directory;
  result->notices = std::move(notices);
  result->files.assign(writer.files.begin(), writer.files.end());
  keep_staging = true;
  if (progress) progress(9, 9, "WWE '13 files are ready to apply.");
  return true;
}

}  // namespace wwe2k14::launcher

// Wrappers for the WWE 2K15 import (wwe13_shared.h).
namespace wwe2k14::launcher {
bool WWE13SharedPatchChEtc(const WWE13ConvertBytes& source, const std::vector<WWE13ConvertedRows>& rows,
                           WWE13ConvertBytes* output, std::string* error) {
  return PatchChEtc(source, rows, output, error);
}
bool WWE13SharedRetargetEvp(const WWE13ConvertBytes& raw, uint32_t new_key, uint32_t old_key,
                            WWE13ConvertBytes* output, std::string* error) {
  return RetargetEvp(raw, new_key, old_key, false, output, error);
}
bool WWE13SharedRegistryRows(const WWE13ConvertBytes& pac, const std::string& file_name, uint32_t leaf_id,
                             std::vector<std::array<uint16_t, 4>>* rows, std::string* error) {
  return ReadRegistryLsd(pac, file_name, leaf_id, rows, error);
}
uint16_t WWE13SharedAutoRank(const std::vector<std::array<uint16_t, 4>>& rows,
                             const std::map<uint32_t, std::string>& names, const std::string& wrestler_name,
                             const std::set<uint16_t>& taken) {
  return AutoRank(rows, names, wrestler_name, taken);
}
}  // namespace wwe2k14::launcher
