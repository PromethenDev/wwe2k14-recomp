#include "launcher_core.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>

using namespace wwe2k14::launcher;

namespace {
uint64_t HashFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return 0;
  uint64_t hash = 14695981039346656037ull;
  std::array<char, 65536> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    for (std::streamsize i = 0; i < file.gcount(); ++i) {
      hash ^= static_cast<uint8_t>(buffer[static_cast<size_t>(i)]);
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

std::vector<fs::path> SnapshotPaths(const std::vector<fs::path>& roots, const std::set<uint32_t>& ids) {
  std::vector<fs::path> paths;
  for (const fs::path& root : roots) {
    const fs::path pac = root.filename() == "pac" ? root : root / "pac";
    std::error_code error;
    for (fs::recursive_directory_iterator it(pac, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
      if (!it->is_regular_file(error)) {
        error.clear();
        continue;
      }
      const std::string filename = it->path().filename().string();
      bool include = filename == "string.pac" || filename.starts_with("string_dlc");
      for (const uint32_t id : ids) {
        const std::string prefix = "ch" + std::to_string(id);
        if (filename.starts_with(prefix) && it->path().parent_path().filename() == "ch") include = true;
      }
      if (include) paths.push_back(it->path());
    }
  }
  return paths;
}

struct FileState {
  uint64_t hash = 0;
  uintmax_t size = 0;
  fs::file_time_type modified{};
};

std::map<fs::path, FileState> Snapshot(const std::vector<fs::path>& paths) {
  std::map<fs::path, FileState> state;
  for (const fs::path& path : paths) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) continue;
    const auto modified = fs::last_write_time(path, error);
    if (error) continue;
    state[path] = {HashFile(path), size, modified};
  }
  return state;
}

bool SameSnapshot(const std::map<fs::path, FileState>& before, const std::map<fs::path, FileState>& after) {
  if (before.size() != after.size()) return false;
  for (const auto& [path, state] : before) {
    const auto current = after.find(path);
    if (current == after.end() || current->second.hash != state.hash || current->second.size != state.size ||
        current->second.modified != state.modified)
      return false;
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "Usage: %s GAME_FOLDER [DLC_FOLDER ...] --list\n", argv[0]);
    std::fprintf(stderr, "   or: %s GAME_FOLDER [DLC_FOLDER ...] --export OUTPUT_FOLDER ID [ID ...]\n", argv[0]);
    return 2;
  }
  std::vector<fs::path> roots;
  int marker = 1;
  while (marker < argc && std::string(argv[marker]) != "--list" && std::string(argv[marker]) != "--export")
    roots.emplace_back(argv[marker++]);
  if (roots.empty() || marker == argc) return 2;
  const std::vector<StockWrestler> roster = LoadStockWrestlers(roots);
  std::printf("WWE 2K14 roster rows: %zu\n", roster.size());
  for (const StockWrestler& wrestler : roster)
    std::printf("%u\t%s\t%s\n", wrestler.id, wrestler.name.c_str(), wrestler.game.c_str());
  if (std::string(argv[marker]) == "--list") return roster.empty() ? 1 : 0;
  if (std::string(argv[marker]) != "--export" || marker + 2 >= argc) return 2;
  const fs::path output_folder = argv[++marker];
  std::vector<uint32_t> ids;
  std::set<uint32_t> selected;
  for (++marker; marker < argc; ++marker) {
    uint32_t id = 0;
    const std::string text(argv[marker]);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), id);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !selected.insert(id).second) {
      std::fprintf(stderr, "Invalid or repeated wrestler id: %s\n", text.c_str());
      return 2;
    }
    ids.push_back(id);
  }
  if (ids.size() != 4) {
    std::fprintf(stderr, "The read-only real-data probe expects exactly 3 stock IDs and 1 DLC ID.\n");
    return 2;
  }
  const std::set<uint32_t> roster_ids([&] {
    std::set<uint32_t> result;
    for (const auto& wrestler : roster) result.insert(wrestler.id);
    return result;
  }());
  for (const uint32_t id : ids) {
    if (!roster_ids.contains(id)) {
      std::fprintf(stderr, "Selected id %u is absent from the parsed WWE 2K14 roster.\n", id);
      return 2;
    }
  }
  const auto paths = SnapshotPaths(roots, selected);
  const auto before = Snapshot(paths);
  CancelFlag cancel{false};
  const Result exported = ExportStockWrestlers(roots, ids, output_folder,
      [](uint64_t done, uint64_t total, const std::string& label) {
        std::printf("%s (%llu / %llu bytes)\n", label.c_str(),
                    static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
      }, cancel);
  if (!exported.ok) {
    std::fprintf(stderr, "Export failed: %s\n", exported.error.c_str());
    return 1;
  }
  const auto after = Snapshot(paths);
  if (!SameSnapshot(before, after)) {
    std::fprintf(stderr, "Read-only check failed: a source archive changed during export.\n");
    return 1;
  }
  std::printf("Read-only source check: %zu PAC files unchanged.\n", before.size());
  std::printf("Export complete: %zu folders under %s\n", ids.size(), output_folder.string().c_str());
  return 0;
}
