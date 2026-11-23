// Mods page data: the imported wrestlers and which of them are in the game.
//   mods/<game>/roster.tsv  installed with the mod: id <tab> name <tab> M or F ('#' comments). The folder says which game
//                           they came from; a '#game <label>' line sets the tag shown on the Mods page.
//   own-slots.txt          next to the game: the ids the game gives a roster slot (read by the game at startup)
// Removing a wrestler takes the id out of own-slots.txt; the files stay, so adding him back is instant.
#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace wwe2k14::launcher {
namespace {
// Folders installed before tags existed have no '#game' line.
std::string GameLabelFromFolder(std::string folder) {
  if (folder == "wwe13") return "WWE '13";
  for (char& c : folder) c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
  return folder;
}
fs::path OwnSlotsPath(const Paths& paths) { return paths.exe_dir / "own-slots.txt"; }

// Same rules as the game's reader: one decimal id per line, '#' starts a comment, valid ids 100..499.
std::set<uint32_t> ReadOwnSlots(const fs::path& path) {
  std::set<uint32_t> ids;
  std::string text;
  if (!internal::ReadTextFile(path, text)) return ids;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    const std::string token = internal::Trim(line);
    if (token.empty() || token.size() > 6 ||
        !std::all_of(token.begin(), token.end(), [](char c) { return c >= '0' && c <= '9'; }))
      continue;
    const uint32_t id = static_cast<uint32_t>(std::stoul(token));
    if (id >= 100 && id <= 499) ids.insert(id);
  }
  return ids;
}

Result WriteOwnSlots(const fs::path& path, const std::set<uint32_t>& ids) {
  std::string text = "# Written by the launcher's Mods page.\n";
  for (const uint32_t value : ids) text += std::to_string(value) + "\n";
  // Plain bytes, not WriteTextFileAtomic: that writes UTF-16, which the game's reader would see as no ids at all.
  fs::path temporary = path;
  temporary += ".tmp";
  std::error_code error;
  if (internal::WriteBinaryFile(temporary, text.data(), text.size())) {
    fs::rename(temporary, path, error);
    if (!error) return Result::Ok();
  }
  fs::remove(temporary, error);
  return Result::Fail("The mod list could not be saved. Check that this folder is not read-only, then try again.");
}
}  // namespace

std::vector<ModWrestler> LoadModWrestlers(const Paths& paths) {
  std::vector<ModWrestler> wrestlers;
  std::vector<fs::path> rosters;
  std::error_code error;
  for (fs::directory_iterator it(paths.exe_dir / "mods", error), end; !error && it != end; it.increment(error))
    if (fs::is_regular_file(it->path() / "roster.tsv", error)) rosters.push_back(it->path() / "roster.tsv");
  std::sort(rosters.begin(), rosters.end());  // an id in two folders stays with the first, every time
  const std::set<uint32_t> enabled = ReadOwnSlots(OwnSlotsPath(paths));
  std::set<uint32_t> seen;
  for (const fs::path& roster : rosters) {
    std::string text;
    if (!internal::ReadTextFile(roster, text)) continue;
    std::string game = GameLabelFromFolder(internal::PathToUtf8(roster.parent_path().filename()));
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.rfind("#game ", 0) == 0 && !internal::Trim(line.substr(6)).empty()) game = internal::Trim(line.substr(6));
      if (line.empty() || line[0] == '#') continue;
      std::istringstream fields(line);
      std::string id_text, name, gender;
      if (!std::getline(fields, id_text, '\t') || !std::getline(fields, name, '\t') || !std::getline(fields, gender, '\t'))
        continue;
      id_text = internal::Trim(id_text);
      if (id_text.empty() || id_text.size() > 6 ||
          !std::all_of(id_text.begin(), id_text.end(), [](char c) { return c >= '0' && c <= '9'; }))
        continue;
      const uint32_t id = static_cast<uint32_t>(std::stoul(id_text));
      if (id < 100 || id > 499 || !seen.insert(id).second) continue;
      wrestlers.push_back({id, internal::Trim(name), internal::Trim(gender) == "F", enabled.count(id) != 0, game});
    }
  }
  std::sort(wrestlers.begin(), wrestlers.end(), [](const ModWrestler& a, const ModWrestler& b) {
    return internal::LowerAscii(a.name) < internal::LowerAscii(b.name);
  });
  return wrestlers;
}

std::set<uint32_t> ReadOwnSlotIds(const fs::path& file) { return ReadOwnSlots(file); }

std::string MakeOwnSlotsText(const std::set<uint32_t>& ids) {
  std::string text = "# Written by the launcher's Mods page.\n";
  for (const uint32_t value : ids) text += std::to_string(value) + "\n";
  return text;
}

// A new folder that missed the import carry-over (GitHub #132) has the wrestlers' files and mods/*/roster.tsv but no
// own-slots.txt, so the game gives them no roster slot. Only when the file is absent: an existing file, even an empty
// one, is the player's choice from the Mods page. Only ever adds ids.
size_t RepairOwnSlotsFromRosters(const Paths& paths) {
  std::error_code error;
  if (fs::exists(OwnSlotsPath(paths), error) || error) return 0;
  std::set<uint32_t> ids;
  for (const ModWrestler& wrestler : LoadModWrestlers(paths)) ids.insert(wrestler.id);
  if (ids.empty()) return 0;
  return WriteOwnSlots(OwnSlotsPath(paths), ids).ok ? ids.size() : 0;
}

bool HasEnabledModWrestlers(const Paths& paths) {
  return !ReadOwnSlots(OwnSlotsPath(paths)).empty();
}

Result SetModWrestlerEnabled(const Paths& paths, uint32_t id, bool enabled) {
  return SetModWrestlersEnabled(paths, {id}, enabled);
}

Result SetModWrestlersEnabled(const Paths& paths, const std::vector<uint32_t>& selected, bool enabled) {
  if (selected.empty()) return Result::Ok();
  std::set<uint32_t> ids = ReadOwnSlots(OwnSlotsPath(paths));
  for (const uint32_t id : selected) {
    if (id < 100 || id > 499) return Result::Fail("That wrestler has an invalid roster slot.");
    if (enabled) ids.insert(id);
    else ids.erase(id);
  }
  return WriteOwnSlots(OwnSlotsPath(paths), ids);
}

// Theme volumes. Layout read from the built pack and proven by capture on two songs (docs/tasks/PORTS-1107-music-volume-report.md):
// AKPK bank row -> bank 0x3017F079 -> event Play_MUS_<id>_0_0 -> play action -> container -> Sound object whose prop 0 is volume.
// All numbers big-endian. Anything that does not match exactly returns no entry, so an unfamiliar pack gets no slider.
namespace {
uint32_t Be32(const std::vector<uint8_t>& d, size_t at) {
  return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) | (uint32_t(d[at + 2]) << 8) | d[at + 3];
}
constexpr uint32_t kMusicBank = 0x3017F079;
uint32_t Fnv1(const std::string& text) {
  uint32_t h = 2166136261u;
  for (char c : text) {
    h *= 16777619u;
    h ^= static_cast<uint8_t>((c >= 'A' && c <= 'Z') ? c + 32 : c);
  }
  return h;
}
float BeFloat(uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
struct HircObject {
  uint8_t type;
  size_t body;
  size_t size;
};
}  // namespace

std::map<uint32_t, float> ReadModThemeLevels(const Paths& paths) {
  std::map<uint32_t, float> levels;
  for (const char* game : {"wwe13", "wwe15"}) {  // WWE '13 and WWE 2K15 imports use different slots
    std::ifstream file(paths.exe_dir / "mods" / game / "theme-levels.tsv", std::ios::binary);
    uint32_t id = 0;
    float db = 0.0f;
    while (file >> id >> db) levels[id] = db;
  }
  return levels;
}

std::map<uint32_t, ModTheme> ReadModThemes(const fs::path& pack, const std::vector<uint32_t>& ids) {
  std::map<uint32_t, ModTheme> themes;
  std::ifstream file(pack, std::ios::binary);
  if (!file) return themes;
  auto read_at = [&file](uint64_t at, size_t size, std::vector<uint8_t>& out) {
    out.resize(size);
    file.clear();
    file.seekg(static_cast<std::streamoff>(at));
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return static_cast<size_t>(file.gcount()) == size;
  };
  std::vector<uint8_t> head;
  if (!read_at(0, 0x20, head) || std::memcmp(head.data(), "AKPK", 4) != 0) return themes;
  const uint32_t strings = Be32(head, 12), banks = Be32(head, 16);
  std::vector<uint8_t> table;
  if (banks < 4 || banks > (1u << 24) || !read_at(0x1cull + strings, banks, table)) return themes;
  const uint32_t rows = Be32(table, 0);
  if (rows > (banks - 4) / 20) return themes;
  uint64_t bank_start = 0;
  uint32_t bank_size = 0;
  for (uint32_t i = 0; i < rows; ++i) {
    const size_t row = 4 + size_t(i) * 20;
    if (Be32(table, row) != kMusicBank) continue;
    bank_start = uint64_t(Be32(table, row + 12)) * Be32(table, row + 4);
    bank_size = Be32(table, row + 8);
  }
  std::vector<uint8_t> bank;
  if (bank_size < 8 || bank_size > (1u << 28) || !read_at(bank_start, bank_size, bank)) return themes;

  std::map<uint32_t, HircObject> objects;
  std::map<uint32_t, uint32_t> sound_of_container;  // container id -> sound id; 0 = more than one
  for (size_t at = 0; at + 8 <= bank.size();) {
    const uint32_t section = Be32(bank, at + 4);
    if (section > bank.size() - at - 8) return themes;
    if (std::memcmp(&bank[at], "HIRC", 4) == 0 && section >= 4) {
      const uint32_t count = Be32(bank, at + 8);
      size_t object = at + 12;
      const size_t end = at + 8 + section;
      for (uint32_t i = 0; i < count; ++i) {
        if (object + 9 > end) return themes;
        const uint32_t length = Be32(bank, object + 1);
        if (length < 4 || length > end - object - 5) return themes;
        const uint32_t id = Be32(bank, object + 5);
        const size_t body = object + 9, body_size = length - 4;
        const uint8_t type = bank[object];
        objects[id] = {type, body, body_size};
        if (type == 2 && body_size >= 44 && bank[body + 26] == 0) {
          const uint32_t parent = Be32(bank, body + 31);
          const auto [it, added] = sound_of_container.emplace(parent, id);
          if (!added) it->second = 0;
        }
        object = body + body_size;
      }
    }
    at += 8 + size_t(section);
  }
  for (const uint32_t wrestler : ids) {
    char event_name[32];
    std::snprintf(event_name, sizeof(event_name), "play_mus_%04u_0_0", wrestler);
    const auto event = objects.find(Fnv1(event_name));
    if (event == objects.end() || event->second.type != 4 || event->second.size < 4) continue;
    const uint32_t action_count = Be32(bank, event->second.body);
    if (action_count > (event->second.size - 4) / 4) continue;
    uint32_t container = 0;
    int plays = 0;
    for (uint32_t i = 0; i < action_count; ++i) {
      const auto action = objects.find(Be32(bank, event->second.body + 4 + size_t(i) * 4));
      if (action == objects.end() || action->second.type != 3 || action->second.size < 6) continue;
      if (bank[action->second.body] != 0x04 || bank[action->second.body + 1] != 0x03) continue;
      container = Be32(bank, action->second.body + 2);
      ++plays;
    }
    if (plays != 1) continue;
    const auto sound_id = sound_of_container.find(container);
    if (sound_id == sound_of_container.end() || sound_id->second == 0) continue;
    const HircObject& sound = objects[sound_id->second];
    if (sound.type != 2 || sound.size < 44) continue;  // a damaged pack: the id was reused by a shorter object
    // Prop count 2, ids volume then 6. Older packs have only prop 6: no room for a volume, so no slider.
    if (bank[sound.body + 37] != 2 || bank[sound.body + 38] != 0 || bank[sound.body + 39] != 6) continue;
    themes[wrestler] = {bank_start + sound.body + 40, sound_id->second, BeFloat(Be32(bank, sound.body + 40))};
  }
  return themes;
}

Result SetModThemeVolume(const fs::path& pack, uint64_t offset, float db) {
  if (db < kModThemeMinDb || db > kModThemeMaxDb) return Result::Fail("That volume is outside the allowed range.");
  std::error_code ec;
  if (fs::hard_link_count(pack, ec) > 1 && !ec) {  // an in-place write would change every linked copy
    fs::path copy = pack;
    copy += ".tmp";
    if (!fs::copy_file(pack, copy, fs::copy_options::overwrite_existing, ec) || ec)
      return Result::Fail("The music file could not be prepared. Check free disk space, then try again.");
    fs::rename(copy, pack, ec);
    if (ec) return Result::Fail("The music file could not be prepared. Close the game, then try again.");
  }
  std::fstream file(pack, std::ios::in | std::ios::out | std::ios::binary);
  if (!file) return Result::Fail("The music file could not be opened. Close the game, then try again.");
  if (std::uintmax_t size = fs::file_size(pack, ec); ec || offset + 4 > size)
    return Result::Fail("The music file has changed. Close and reopen this page, then try again.");
  uint32_t bits = 0;
  std::memcpy(&bits, &db, sizeof(bits));
  const char bytes[4] = {char(bits >> 24), char(bits >> 16), char(bits >> 8), char(bits)};
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(bytes, 4);
  file.flush();
  if (!file) return Result::Fail("The music file could not be saved. Check that this folder is not read-only, then try again.");
  return Result::Ok();
}
}  // namespace wwe2k14::launcher
