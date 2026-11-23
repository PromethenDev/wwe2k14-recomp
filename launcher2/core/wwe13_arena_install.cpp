#include "wwe13_arena_install.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

#include "bink_convert.h"
#include "internal/util.h"
#include "path_case.h"
#include "wwe13_install.h"

namespace wwe2k14::launcher {
namespace fs = std::filesystem;
namespace {

using Bytes = WWE13ConvertBytes;

// Targets proven free of 2K14 arena files/menu/stage/movie entries in the all-25 fixture (G4-R13). The live lane record
// table has entries for 10, 18-20, 24-26 and 69; 57-63, 84 and 89-97 are appended by the guarded game hook.
const std::vector<uint32_t> kImportTargets{10, 18, 19, 20, 24, 25, 26, 57, 58, 59, 60, 61, 62, 63, 69,
                                           84, 89, 90, 91, 92, 93, 94, 95, 96, 97};
const std::vector<uint32_t> kImportable{5, 10, 15, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
                                        45, 46, 47, 48, 49};

std::string Pad(unsigned value, int width) {
  std::string s = std::to_string(value);
  return s.size() < size_t(width) ? std::string(size_t(width) - s.size(), '0') + s : s;
}

uint32_t Le32(const Bytes& b, size_t at) {
  return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
}

bool ReadFile(const fs::path& path, Bytes* out) { return internal::ReadBinaryFile(path, *out); }
// The WWE '13 base is a folder or (owner 11-13) the player's disc image, read in place.
bool IsDiscBase(const fs::path& base) { std::error_code ec; return fs::is_regular_file(base, ec) && !ec; }
bool ReadBaseFile(const fs::path& base, const std::string& relative, Bytes* out) {
  return IsDiscBase(base) ? ReadDiscImageFile(base, relative, out) : ReadFile(ResolvePathCase(base, relative), out);
}

bool WriteFile(const fs::path& path, const Bytes& bytes) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(f);
}

// English arena names from the '13 string.pac: SDB group, entry "0000", leaf 0, keys 40002 + id.
bool ReadWWE13ArenaNames(const Bytes& string_pac, std::map<uint32_t, std::string>* names, std::string* error) {
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(string_pac, &groups, error)) return false;
  for (const auto& g : groups) {
    if (g.name.rfind("SDB", 0) != 0) continue;
    for (const auto& e : g.entries) {
      std::string n = e.name;
      while (!n.empty() && (n.back() == '\0' || n.back() == ' ')) n.pop_back();
      if (n != "0000") continue;
      std::vector<WWE13PachLeafData> leaves;
      if (!ReadWWE13PachLeaves(e.data, &leaves, error)) return false;
      for (const auto& leaf : leaves) {
        if (leaf.id != 0) continue;
        Bytes zleaf = leaf.data, plain;
        if (zleaf.size() >= 4 && std::memcmp(zleaf.data(), "BPE ", 4) == 0 && !DecodeWWE13Bpe(leaf.data, &zleaf, error)) return false;
        if (!DecodeWWE13ZlibLeaf(zleaf, &plain, error)) return false;
        if (plain.size() < 8) return true;
        const uint32_t count = Le32(plain, 4);
        // '13 tables use 16-byte rows {offset, length, key, 0}; 2K14 12-byte rows. Detect by the first row's 4th word.
        const size_t row = (count > 0 && plain.size() >= 8 + 16 && Le32(plain, 8 + 12) == 0 && count * 16 + 8 <= plain.size()) ? 16 : 12;
        for (uint32_t i = 0; i < count && 8 + (i + 1) * row <= plain.size(); ++i) {
          const uint32_t offset = Le32(plain, 8 + i * row), length = Le32(plain, 8 + i * row + 4), key = Le32(plain, 8 + i * row + 8);
          if (key < 40002 || key > 40002 + 99 || uint64_t(offset) + length > plain.size()) continue;
          std::string value(reinterpret_cast<const char*>(plain.data() + offset), length);
          value = value.substr(0, value.find('\0'));
          (*names)[key - 40002] = value;
        }
      }
    }
  }
  return true;
}

}  // namespace

const std::vector<uint32_t>& WWE13ArenaImportTargets() { return kImportTargets; }

std::string WWE13ArenaPicture(uint32_t source_id) {
  static const std::map<uint32_t, std::string> kByMenuPosition{
      {0, "arena00"}, {1, "arena01"}, {2, "arena02"}, {3, "arena03"}, {7, "arena04"}, {11, "arena05"},
      {13, "arena06"}, {15, "arena07"}, {4, "arena08"}, {5, "arena09"}, {6, "arena10"}, {8, "arena11"},
      {9, "arena12"}, {10, "arena13"}, {12, "arena14"}, {14, "arena15"}, {16, "arena16"}};
  if (auto it = kByMenuPosition.find(source_id); it != kByMenuPosition.end()) return it->second;
  if (source_id >= 28 && source_id <= 49) return "AEM" + Pad(source_id - 27, 2);
  return {};
}

fs::path WWE13ArenaListFile(const Paths& paths) { return paths.exe_dir / "mods" / "wwe13" / "arenas.txt"; }

std::vector<WWE13ArenaItem> ListWWE13Arenas(const fs::path& wwe13_base, std::string* error) {
  std::vector<WWE13ArenaItem> items;
  std::map<uint32_t, std::string> names;
  Bytes string_pac;
  if (ReadBaseFile(wwe13_base, "pac/string.pac", &string_pac)) {
    std::string name_error;
    if (!ReadWWE13ArenaNames(string_pac, &names, &name_error) && error) *error = name_error;
  }
  std::set<std::string> disc_files;  // lower-case paths, when the base is a disc image
  if (IsDiscBase(wwe13_base))
    for (const DiscImageFile& file : ListDiscImageFiles(wwe13_base)) disc_files.insert(internal::LowerAscii(file.path));
  for (uint32_t id : kImportable) {
    WWE13ArenaItem item;
    item.source_id = id;
    item.name = names.count(id) ? names[id] : "ARENA " + std::to_string(id);
    item.picture = WWE13ArenaPicture(id);
    std::error_code ec;
    item.available = IsDiscBase(wwe13_base)
        ? disc_files.contains("pac/bg/bg" + Pad(id, 2) + ".pac")
        : fs::is_regular_file(ResolvePathCase(wwe13_base, "pac/bg/bg" + Pad(id, 2) + ".pac"), ec) && !ec;
    if (!item.available) item.missing_reason = "pac/bg/bg" + Pad(id, 2) + ".pac is missing in the WWE '13 folder";
    items.push_back(std::move(item));
  }
  return items;
}

std::vector<uint32_t> FreeWWE13ArenaNumbers(const fs::path& game_folder) {
  std::vector<uint32_t> free;
  for (uint32_t id : kImportTargets) {
    std::error_code ec;
    if (!fs::exists(game_folder / "pac" / "bg" / ("bg" + Pad(id, 2) + ".pac"), ec)) free.push_back(id);
  }
  return free;
}

Result RunWWE13ArenaImport(const Paths& paths, const fs::path& game_folder, const fs::path& wwe13_base,
                           const std::vector<uint32_t>& source_ids, const fs::path& bink_converter,
                           const ProgressFn& progress, CancelFlag& cancel, WWE13ArenaImportSummary* summary) {
  if (source_ids.empty()) return Result::Fail("Choose at least one WWE '13 arena.");
  if (HasWWE13ArenaImport(paths)) return Result::Fail("Undo the imported WWE '13 arenas before importing arenas again.");
  // Same depth rule as the wrestler import: Windows stops at 260 characters.
  if (internal::PathToUtf8(paths.exe_dir).size() > 125)
    return Result::Fail("This game folder's location is too long for Windows. Move the folder closer to the top of the drive, for example C:\\Games, and try again. Nothing has been changed.");
  std::string error;
  const std::vector<WWE13ArenaItem> items = ListWWE13Arenas(wwe13_base, &error);
  const std::vector<uint32_t> free = FreeWWE13ArenaNumbers(game_folder);
  if (source_ids.size() > free.size())
    return Result::Fail("Only " + std::to_string(free.size()) + " arena slots are free; choose fewer arenas.");

  std::vector<WWE13ArenaPort> ports;
  std::set<uint32_t> seen;
  for (size_t i = 0; i < source_ids.size(); ++i) {
    const uint32_t id = source_ids[i];
    auto item = std::find_if(items.begin(), items.end(), [&](const auto& x) { return x.source_id == id; });
    if (item == items.end()) return Result::Fail("WWE '13 arena " + std::to_string(id) + " is not one WWE 2K14 lacks.");
    if (!item->available) return Result::Fail(item->name + ": " + item->missing_reason + ".");
    if (!seen.insert(id).second) return Result::Fail(item->name + " was chosen twice.");
    WWE13ArenaPort port;
    port.source_id = id;
    port.target_id = free[i];
    port.tag = "W13_" + Pad(free[i], 2);
    port.picture = item->picture;
    port.name = item->name;
    ports.push_back(port);
  }

  // Inputs (read only): the game's current files and the '13 files.
  WWE13ArenaInput input;
  const auto game_files = WWE13ArenaGameFiles();
  const auto source_files = WWE13ArenaSourceFiles(ports);
  uint64_t step = 0;
  const uint64_t steps = game_files.size() + source_files.size() + ports.size() * 2 + 2;
  for (const auto& rel : game_files) {
    if (cancel.load(std::memory_order_relaxed)) return Result::Fail("WWE '13 arena import cancelled.");
    if (progress) progress(++step, steps, "Reading " + rel);
    if (!ReadFile(game_folder / rel, &input.game[rel])) return Result::Fail("Could not read the WWE 2K14 file " + rel + ".");
  }
  for (const auto& rel : source_files) {
    if (cancel.load(std::memory_order_relaxed)) return Result::Fail("WWE '13 arena import cancelled.");
    if (progress) progress(++step, steps, "Reading WWE '13 " + rel);
    if (!ReadBaseFile(wwe13_base, rel, &input.source[rel])) return Result::Fail("Could not read the WWE '13 file " + rel + ".");
  }
  {
    std::error_code ec;
    for (fs::directory_iterator it(game_folder / "pac" / "bg", ec), end; !ec && it != end; it.increment(ec)) {
      const std::string n = internal::PathToUtf8(it->path().filename());
      if (n.size() == 8 && n.rfind("bg", 0) == 0 && std::isdigit(static_cast<unsigned char>(n[2])) &&
          std::isdigit(static_cast<unsigned char>(n[3])) && n.substr(4) == ".pac")
        input.existing_bg.push_back(static_cast<uint32_t>(std::stoul(n.substr(2, 2))));
    }
  }

  // Temporary space: the system temp folder (shortest path), cleaned on every exit.
  std::error_code ec;
  fs::path temp = fs::temp_directory_path(ec);
  if (ec) temp = paths.exe_dir / "w13tmp";
  const fs::path work = temp / ("w13a-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{work};
  fs::create_directories(work, ec);
  if (ec) return Result::Fail("Could not create temporary space for the WWE '13 arena import.");

  // Stage movies: the '13 MOV / MVAB (Bink 2) through the launcher's converter (Bink 1 out, as for titantrons).
  WWE13ArenaImportSummary result_summary;
  {
    std::vector<WWE13PacGroupData> movie_groups;
    if (!ReadWWE13PacGroups(input.source.at("pac/evt/evtmov.pac"), &movie_groups, &error)) return Result::Fail(error);
    std::vector<WWE13PacGroupData> game_movie_groups;
    if (!ReadWWE13PacGroups(input.game.at("pac/evt/evtmov.pac"), &game_movie_groups, &error)) return Result::Fail(error);
    for (const auto& port : ports) {
      if (progress) progress(++step, steps, "Stage movies for " + port.name);
      for (const char* group : {"MOV ", "MVAB"}) {
        const std::string name = "M" + Pad(port.source_id, 3);
        const WWE13PacEntryData* source = nullptr;
        for (const auto& g : movie_groups)
          if (g.name == group)
            for (const auto& e : g.entries) if (e.name.substr(0, 4) == name) source = &e;
        if (!source) continue;
        if (bink_converter.empty()) {
          result_summary.lines.push_back(port.name + ": no Bink converter, the stage movie (" + std::string(group, 4) + ") is left out.");
          continue;
        }
        const fs::path in = work / ("m" + Pad(port.source_id, 2) + group[1] + ".bk2");
        const fs::path out = work / ("m" + Pad(port.source_id, 2) + group[1] + ".bik");
        if (!WriteFile(in, source->data)) return Result::Fail("Could not write a temporary stage movie.");
        WWE13TitantronResult converted;
        const Result r = ConvertWWE13TitantronOrStandIn(bink_converter, in, work / "no-stand-in", out, port.name, {}, cancel, &converted);
        Bytes movie;
        if (!r.ok || !ReadFile(out, &movie)) {
          if (cancel.load(std::memory_order_relaxed)) return Result::Fail("WWE '13 arena import cancelled.");
          result_summary.lines.push_back(port.name + ": its stage movie could not be converted and is left out.");
          continue;
        }
        WWE13PacEntryData entry;
        entry.name = "M" + Pad(port.target_id, 3);
        entry.name.resize(source->name.size(), source->name.back() == ' ' ? ' ' : '\0');
        movie.resize((movie.size() + 0xff) & ~size_t(0xff), 0);   // pac entries are whole 256-byte units
        entry.data = std::move(movie);
        input.evtmov_added.emplace_back(std::string(group).substr(0, 4), std::move(entry));
      }
    }
  }

  if (progress) progress(++step, steps, "Converting the arenas…");
  WWE13ArenaOutput output;
  if (!ConvertWWE13Arenas(ports, input, &output, &error)) return Result::Fail(error);
  input = {};   // free the inputs before staging

  const fs::path staged = work / "stage";
  for (const auto& [rel, bytes] : output.files)
    if (!WriteFile(staged / "game" / fs::u8path(rel), bytes)) return Result::Fail("Could not stage " + rel + ".");
  if (!WriteFile(staged / "launcher" / "mods" / "wwe13" / "arenas.txt", Bytes(output.arena_list.begin(), output.arena_list.end())))
    return Result::Fail("Could not stage the arena list.");
  if (progress) progress(++step, steps, "Applying…");
  const Result applied = ApplyWWE13ArenaStagedFiles(paths, game_folder, staged, progress, cancel);
  if (!applied.ok) return applied;
  for (const auto& port : ports)
    result_summary.lines.insert(result_summary.lines.begin(), port.name + " -> arena " + std::to_string(port.target_id));
  if (summary) *summary = std::move(result_summary);
  return Result::Ok();
}

}  // namespace wwe2k14::launcher
