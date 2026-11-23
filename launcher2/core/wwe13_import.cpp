#include "launcher_core.h"

#include "internal/stfs.h"
#include "internal/util.h"
#include "wwe13_pipeline.h"
#include "game_search.h"
#include "path_case.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace wwe2k14::launcher {
namespace {

constexpr uint32_t kWwe13TitleId = 0x545108B4;
constexpr uint32_t kDlcContentType = 0x00000002;
constexpr uint64_t kMaximumFiles = 1000000;  // packages opened by a chosen-folder scan

struct ExtractedPack {
  fs::path folder;
  uint32_t number = 0;
  std::set<uint32_t> wrestler_ids;
};

struct PackageRecord {
  WWE13ImportPack pack;
};

bool IsPacFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::array<uint8_t, 4> magic{};
  if (!input.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()))) return false;
  return magic == std::array<uint8_t, 4>{'E', 'P', 'K', '8'} ||
         magic == std::array<uint8_t, 4>{'E', 'P', 'A', 'C'} ||
         magic == std::array<uint8_t, 4>{'B', 'P', 'E', ' '} ||
         magic == std::array<uint8_t, 4>{0x0F, 0xF5, 0x12, 0xED};  // WWE '13 XMem/LZX wrapper.
}

bool IsWwe13Base(const fs::path& folder) {
  uint32_t title_id = 0;
  if (!internal::DecodeXexTitleId(ResolvePathCase(folder, "default.xex"), &title_id) || title_id != kWwe13TitleId) return false;
  return IsPacFile(ResolvePathCase(folder, "pac/ch/chEtc.pac")) &&
         IsPacFile(ResolvePathCase(folder, "pac/DLC.pac")) &&
         IsPacFile(ResolvePathCase(folder, "pac/string.pac"));
}

bool ParseDigits(std::string_view text, size_t* offset, uint32_t* value, size_t minimum, size_t maximum) {
  const size_t start = *offset;
  uint32_t parsed = 0;
  while (*offset < text.size() && text[*offset] >= '0' && text[*offset] <= '9') {
    if (parsed > 1000000) return false;
    parsed = parsed * 10 + static_cast<uint32_t>(text[*offset] - '0');
    ++*offset;
  }
  const size_t count = *offset - start;
  if (count < minimum || count > maximum) return false;
  *value = parsed;
  return true;
}

bool ParseDlcEtcName(std::string_view name, uint32_t* pack_number) {
  const std::string lower = internal::LowerAscii(name);
  constexpr std::string_view prefix = "chetc_dlc";
  if (lower.size() != prefix.size() + 2 + 4 || lower.substr(0, prefix.size()) != prefix ||
      lower.substr(lower.size() - 4) != ".pac") return false;
  size_t offset = prefix.size();
  uint32_t number = 0;
  if (!ParseDigits(lower, &offset, &number, 2, 2) || lower.substr(offset) != ".pac" || number == 0) return false;
  *pack_number = number;
  return true;
}

bool ParseDlcCharacterName(std::string_view name, uint32_t* character_id, uint32_t* pack_number) {
  const std::string lower = internal::LowerAscii(name);
  if (lower.size() < 11 || lower.substr(0, 2) != "ch" || lower.substr(lower.size() - 4) != ".pac") return false;
  size_t offset = 2;
  uint32_t id = 0;
  if (!ParseDigits(lower, &offset, &id, 1, 3) || lower.substr(offset, 4) != "_dlc") return false;
  offset += 4;
  uint32_t number = 0;
  if (!ParseDigits(lower, &offset, &number, 2, 2) || lower.substr(offset) != ".pac" || number == 0) return false;
  *character_id = id;
  *pack_number = number;
  return true;
}

std::vector<ExtractedPack> ReadExtractedPacks(const fs::path& folder,
                                               const std::set<uint32_t>& source_ids,
                                               const CancelFlag& cancel) {
  std::map<uint32_t, ExtractedPack> packs;
  std::error_code error;
  const fs::path character_dir = ResolvePathCase(folder, "pac/ch");
  fs::directory_iterator iterator(character_dir, fs::directory_options::skip_permission_denied, error);
  if (error) return {};
  for (const auto& entry : iterator) {
    if (cancel.load(std::memory_order_relaxed)) break;
    error.clear();
    if (!entry.is_regular_file(error) || error) continue;
    uint32_t pack_number = 0;
    const std::string filename = internal::PathToUtf8(entry.path().filename());
    // By name first: a game folder has hundreds of character files, and most need no opening at all.
    uint32_t named_id = 0, named_pack = 0;
    if (!ParseDlcEtcName(filename, &named_pack) && !ParseDlcCharacterName(filename, &named_id, &named_pack)) continue;
    if (!IsPacFile(entry.path())) continue;
    if (ParseDlcEtcName(filename, &pack_number)) {
      auto& pack = packs[pack_number];
      pack.folder = folder;
      pack.number = pack_number;
      continue;
    }
    uint32_t character_id = 0;
    if (ParseDlcCharacterName(filename, &character_id, &pack_number) && source_ids.contains(character_id)) {
      auto& pack = packs[pack_number];
      pack.folder = folder;
      pack.number = pack_number;
      pack.wrestler_ids.insert(character_id);
    }
  }
  std::vector<ExtractedPack> result;
  for (auto& [number, pack] : packs) result.push_back(std::move(pack));
  return result;
}

bool IsStfsFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::array<char, 4> magic{};
  if (!input.read(magic.data(), static_cast<std::streamsize>(magic.size()))) return false;
  return magic == std::array<char, 4>{'C', 'O', 'N', ' '} ||
         magic == std::array<char, 4>{'L', 'I', 'V', 'E'} ||
         magic == std::array<char, 4>{'P', 'I', 'R', 'S'};
}

std::optional<WWE13ImportPack> ReadStfsPack(const fs::path& path,
                                           const std::set<uint32_t>& source_ids) {
  if (!IsStfsFile(path)) return std::nullopt;
  internal::StfsReader reader;
  std::string error;
  if (!reader.Open(path, &error)) return std::nullopt;
  const auto& metadata = reader.metadata();
  if (metadata.content_type != kDlcContentType || metadata.title_id != kWwe13TitleId) return std::nullopt;

  WWE13ImportPack pack;
  pack.folder = path;
  pack.display_name = metadata.display_name;
  pack.package_file = true;
  std::set<uint32_t> character_ids;
  for (const auto& entry : reader.entries()) {
    if (entry.directory) continue;
    uint32_t number = 0;
    if (ParseDlcEtcName(internal::PathToUtf8(entry.relative_path.filename()), &number)) {
      pack.pack_number = pack.pack_number == 0 ? number : std::min(pack.pack_number, number);
      continue;
    }
    uint32_t character_id = 0;
    if (ParseDlcCharacterName(internal::PathToUtf8(entry.relative_path.filename()), &character_id, &number) &&
        source_ids.contains(character_id)) {
      pack.pack_number = pack.pack_number == 0 ? number : std::min(pack.pack_number, number);
      character_ids.insert(character_id);
    }
  }
  if (pack.pack_number == 0) return std::nullopt;
  pack.wrestler_ids.assign(character_ids.begin(), character_ids.end());
  if (pack.display_name.empty()) pack.display_name = "DLC pack " + std::to_string(pack.pack_number);
  return pack;
}

std::string PackLabel(uint32_t number) {
  return "DLC pack " + (number < 10 ? std::string("0") : std::string()) + std::to_string(number);
}

// The WWE '13 add-on packages, by their fixed names, or anything in WWE '13's own add-on folder.
bool IsWwe13PackageName(const fs::path& file) {
  static const std::set<std::string> names{
      "b64239e5b81fc5efe8cef7e695f77223a9d39a0f54", "9faf6bd4eadad29a4aa3517843077fc877ddfa0wwe",
      "ae201cfdeffb0c988b4a1dc89edf2b4f7491fb5354"};
  if (names.contains(internal::LowerAscii(internal::PathToUtf8(file.filename())))) return true;
  const fs::path content = file.parent_path();
  return content.filename() == "00000002" &&
         internal::LowerAscii(internal::PathToUtf8(content.parent_path().filename())) == "545108b4";
}

bool IsDiscImageName(const fs::path& path) {
  const std::string extension = internal::LowerAscii(internal::PathToUtf8(path.extension()));
  return extension == ".iso" || extension == ".img";
}

// Lower-case paths of every file on a WWE '13 disc image (IsWWE13DiscImage); empty for any other file.
std::set<std::string> Wwe13DiscFileNames(const fs::path& iso) {
  std::set<std::string> names;
  if (ReadDiscImageTitleId(iso) != kWwe13TitleId) return names;
  for (const DiscImageFile& file : ListDiscImageFiles(iso)) names.insert(internal::LowerAscii(file.path));
  for (const char* required : {"pac/ch/chetc.pac", "pac/dlc.pac", "pac/string.pac"})
    if (!names.contains(required)) return {};
  return names;
}

}  // namespace

WWE13ImportScan ScanWWE13ImportSources(const Paths& paths, const fs::path& selected_folder,
                                       const ProgressFn& progress, const CancelFlag& cancel) {
  WWE13ImportScan result;
  std::vector<WWE13ImportPort> recipes;
  if (!ReadWWE13ImportRecipes(paths.exe_dir / "data" / "wwe13-ports.tsv", &recipes, &result.error)) return result;
  std::set<uint32_t> source_ids;
  for (const WWE13ImportPort& recipe : recipes) {
    source_ids.insert(recipe.source_id);
    result.wrestlers.push_back({recipe.source_id, recipe.target_id, recipe.name, false, false, {}, {}});
  }
  try {
    const bool whole_pc = selected_folder.empty();
    std::vector<fs::path> roots;
    fs::path chosen_disc;  // the player picked the disc image file itself (owner 11-13)
    if (whole_pc) {
      roots = GameSearchRoots(paths);
    } else {
      std::error_code absolute_error;
      const fs::path absolute = fs::absolute(selected_folder, absolute_error);
      const fs::path selected = (absolute_error ? selected_folder : absolute).lexically_normal();
      std::error_code file_error;
      if (fs::is_regular_file(selected, file_error)) {
        if (!IsDiscImageName(selected) || Wwe13DiscFileNames(selected).empty()) {
          result.error = internal::PathToUtf8(selected.filename()) + " is not a WWE '13 (Xbox 360) disc image. Choose your "
                         "WWE '13 .iso or .img file, or Choose a folder with the extracted WWE '13 files.";
          return result;
        }
        chosen_disc = selected;
        // Its own folder is still checked for WWE '13 add-on packages kept beside it.
        roots.push_back(selected.parent_path());
      } else {
        roots.push_back(selected);
      }
    }
    std::vector<fs::path> base_folders;
    std::vector<fs::path> disc_bases;  // WWE '13 disc images (.iso/.img), read in place
    std::set<std::string> disc_names;  // the first disc image's file list (lower case)
    std::vector<ExtractedPack> extracted_packs;
    std::vector<WWE13ImportPack> stfs_packs;
    std::set<fs::path> visited_packages;
    uint64_t files_examined = 0;
    // Stop early only when every wrestler in the list has a source, not just when some of each pack turned up.
    std::set<uint32_t> covered;
    // Each folder is judged by the names in it (game_search.h): only a folder with default.xex beside pac, a folder
    // with pac (extracted add-on), a large .iso/.img, or an add-on package name is ever opened. A chosen folder also
    // opens its other files, because players rename add-on packages.
    const GameSearchStats stats = WalkGameFiles(roots, [&](const fs::path& folder, const std::vector<GameSearchEntry>& entries) {
      bool has_pac = false, has_xex = false;
      for (const GameSearchEntry& entry : entries) {
        if (entry.directory && entry.lower_name == "pac") has_pac = true;
        if (!entry.directory && entry.lower_name == "default.xex") has_xex = true;
      }
      if (has_pac && has_xex && IsWwe13Base(folder)) {
        base_folders.push_back(folder);
        for (const uint32_t id : source_ids)
          if (IsPacFile(ResolvePathCase(folder, "pac/ch/ch" + std::to_string(id) + ".pac"))) covered.insert(id);
      }
      if (has_pac) {
        // An extracted pack, recognized from its PAC signatures and DLC file names wherever it is stored.
        std::vector<ExtractedPack> found = ReadExtractedPacks(folder, source_ids, cancel);
        for (const ExtractedPack& pack : found) covered.insert(pack.wrestler_ids.begin(), pack.wrestler_ids.end());
        extracted_packs.insert(extracted_packs.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
      }
      for (const GameSearchEntry& entry : entries) {
        if (entry.directory) continue;
        if (cancel.load(std::memory_order_relaxed)) return true;
        // A WWE '13 disc image is a base game too (owner 11-13: no need to extract the disc).
        if (IsDiscImageName(entry.path)) {
          // The PC search opens only large ones; in a chosen folder any disc image is checked.
          if (whole_pc ? !IsGameDiscImageCandidate(entry) : IsCloudOnlyFile(entry.path)) continue;
          if (progress) progress(result.directories_visited, 0, "Checking " + internal::PathToUtf8(entry.path));
          if (std::set<std::string> names = Wwe13DiscFileNames(entry.path); !names.empty()) {
            for (const uint32_t id : source_ids)
              if (names.contains("pac/ch/ch" + std::to_string(id) + ".pac")) covered.insert(id);
            if (disc_bases.empty()) disc_names = std::move(names);
            disc_bases.push_back(entry.path);
          }
          continue;
        }
        // The automatic search knows what it wants: the add-on packages' fixed names.
        if (whole_pc && !IsWwe13PackageName(entry.path)) continue;
        if (files_examined >= kMaximumFiles || IsCloudOnlyFile(entry.path)) continue;
        ++files_examined;
        if (!visited_packages.insert(entry.path.lexically_normal()).second) continue;
        if (auto package = ReadStfsPack(entry.path, source_ids)) {
          covered.insert(package->wrestler_ids.begin(), package->wrestler_ids.end());
          stfs_packs.push_back(std::move(*package));
        }
      }
      return whole_pc && (!base_folders.empty() || !disc_bases.empty()) && covered.size() >= source_ids.size();
    }, whole_pc || !chosen_disc.empty(), progress, &cancel);  // a picked disc: its folder is only a quick extra look
    result.directories_visited = stats.folders;
    result.search_limit_reached = stats.limit_reached;

    result.cancelled = cancel.load(std::memory_order_relaxed);
    if (result.cancelled) return result;  // stop means stop: no more checking of what was found
    std::sort(base_folders.begin(), base_folders.end());
    base_folders.erase(std::unique(base_folders.begin(), base_folders.end()), base_folders.end());
    if (!chosen_disc.empty()) {  // a picked disc image wins over anything found beside it
      result.base_folder = chosen_disc;
      result.base_found = true;
      result.base_is_disc_image = true;
      disc_names = Wwe13DiscFileNames(chosen_disc);
    } else if (!base_folders.empty()) {
      result.base_folder = base_folders.front();
      result.base_found = true;
    } else if (!disc_bases.empty()) {  // extracted files win; a disc image is used when it is all there is
      result.base_folder = disc_bases.front();
      result.base_found = true;
      result.base_is_disc_image = true;
    }

    std::set<std::pair<uint32_t, fs::path>> seen_packs;
    for (const ExtractedPack& found : extracted_packs) {
      const auto key = std::make_pair(found.number, found.folder.lexically_normal());
      if (!seen_packs.insert(key).second) continue;
      WWE13ImportPack pack;
      pack.pack_number = found.number;
      pack.folder = found.folder;
      pack.display_name = PackLabel(found.number);
      pack.wrestler_ids.assign(found.wrestler_ids.begin(), found.wrestler_ids.end());
      result.packs.push_back(std::move(pack));
    }
    for (WWE13ImportPack& pack : stfs_packs) {
      const auto key = std::make_pair(pack.pack_number, pack.folder.lexically_normal());
      if (seen_packs.insert(key).second) result.packs.push_back(std::move(pack));
    }
    std::sort(result.packs.begin(), result.packs.end(), [](const WWE13ImportPack& left, const WWE13ImportPack& right) {
      if (left.pack_number != right.pack_number) return left.pack_number < right.pack_number;
      return left.folder < right.folder;
    });

    if (result.base_found) {
      const fs::path character_dir = ResolvePathCase(result.base_folder, "pac/ch");
      for (WWE13ImportWrestler& wrestler : result.wrestlers) {
        const std::string name = "ch" + std::to_string(wrestler.source_id) + ".pac";
        if (result.base_is_disc_image ? disc_names.contains("pac/ch/" + name) : IsPacFile(ResolvePathCase(character_dir, name))) {
          wrestler.available = true;
          wrestler.source_label = "Base game";
          wrestler.missing_reason.clear();
        }
      }
    }
    for (const WWE13ImportPack& pack : result.packs) {
      for (const uint32_t source_id : pack.wrestler_ids) {
        auto wrestler = std::find_if(result.wrestlers.begin(), result.wrestlers.end(),
                                     [source_id](const WWE13ImportWrestler& candidate) {
                                       return candidate.source_id == source_id;
                                     });
        if (wrestler == result.wrestlers.end()) continue;
        if (!wrestler->available) {
          wrestler->available = true;
          wrestler->source_label = PackLabel(pack.pack_number);
          wrestler->missing_reason.clear();
        }
      }
    }
    for (WWE13ImportWrestler& wrestler : result.wrestlers) {
      wrestler.selected = wrestler.available;
      if (!wrestler.available) {
        wrestler.missing_reason = result.base_found
            ? "No character PAC was found in the base game or detected add-ons."
            : "WWE '13 base game files were not found; a base game is required to import this wrestler.";
      }
    }
    if (progress) progress(result.directories_visited, result.directories_visited,
                           result.base_found ? "WWE '13 files found" : "Search complete");
  } catch (...) {
    result.error = "The WWE '13 folders could not be scanned completely.";
  }
  return result;
}

bool IsWWE13DiscImage(const fs::path& iso) { return !Wwe13DiscFileNames(iso).empty(); }

}  // namespace wwe2k14::launcher
