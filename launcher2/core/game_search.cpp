#include "game_search.h"

#include "internal/util.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <set>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace wwe2k14::launcher {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kMaximumFolders = 3000000;
constexpr unsigned kMaximumDepth = 12;
constexpr uint64_t kDiscImageMinimumBytes = 1024ull * 1024ull * 1024ull;

bool TestRootsSet() {
#ifdef _WIN32
  const wchar_t* roots = _wgetenv(L"WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS");
#else
  const char* roots = std::getenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS");
#endif
  return roots && *roots;
}

// System folders, other programs' data, and the inside of game folders (a game is recognized from the folder above
// its pac folder, so pac/movies/sound themselves are never walked).
bool SkipFolder(const std::string& lower_name) {
  static const std::set<std::string> skipped{
      ".git", ".hg", ".svn", ".cache", "node_modules", "proc", "sys", "dev", "run", "snap", "windows",
      "programdata", "system volume information", "$recycle.bin", "lost+found", "recovery", "appdata",
      "545408b2",  // WWE 2K14's own add-ons: same file names as WWE '13's, never a source
      "program files", "program files (x86)", "windowsapps", "$windows.~bt", "$windows.~ws", "$sysreset",
      "$winreagent", "pac", "movies", "sound", "shaders", "shaders32", "shadertools", "crowd-cards", "cache"};
  return skipped.contains(lower_name);
}

// Folder identity for "each folder once" (roots overlap: C:\ contains the Downloads root). Case-insensitive on Windows.
std::string FolderKey(const fs::path& folder) {
  std::string key = internal::PathToUtf8(folder.lexically_normal());
#ifdef _WIN32
  key = internal::LowerAscii(key);
#endif
  while (key.size() > 1 && (key.back() == '/' || key.back() == '\\')) key.pop_back();
  return key;
}

void AddRoot(std::vector<fs::path>& roots, const fs::path& root) {
  if (root.empty()) return;
  std::error_code error;
  fs::path absolute = fs::absolute(root, error);
  if (error) absolute = root;
  absolute = absolute.lexically_normal();
  const std::string key = FolderKey(absolute);
  for (const fs::path& existing : roots)
    if (FolderKey(existing) == key) return;
  roots.push_back(std::move(absolute));
}

}  // namespace

std::vector<fs::path> GameSearchRoots(const Paths& paths) {
  // InstallationSearchRoots: the launcher folder, home, Downloads/Desktop/Documents and every drive / mount
  // (or only the test roots). Reordered so the likely places come first, and limited to drives worth walking.
  const std::vector<fs::path> base = InstallationSearchRoots(paths);
  std::vector<fs::path> roots;
  if (TestRootsSet()) {
    for (const fs::path& root : base) AddRoot(roots, root);
    return roots;
  }
  std::vector<fs::path> personal, drives;
  for (const fs::path& root : base) {
    const fs::path normal = root.lexically_normal();
    const bool is_drive = normal == normal.root_path() || normal.relative_path().empty();
    (is_drive ? drives : personal).push_back(root);
  }
  // Videos is a common place for disc images too.
  for (const fs::path& root : personal) {
    if (root.filename() == "Downloads" || root.filename() == "Desktop" || root.filename() == "Documents") {
      AddRoot(roots, root);
      if (root.filename() == "Documents") AddRoot(roots, root.parent_path() / "Videos");
    }
  }
  for (const fs::path& root : personal) AddRoot(roots, root);
  for (const fs::path& root : drives) {
#ifdef _WIN32
    const UINT type = GetDriveTypeW(root.root_path().c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;  // no network shares, no optical drives
#endif
    AddRoot(roots, root);
  }
  return roots;
}

GameSearchStats WalkGameFiles(const std::vector<fs::path>& roots,
                              const std::function<bool(const fs::path&, const std::vector<GameSearchEntry>&)>& visit,
                              bool time_limited, const ProgressFn& progress, const CancelFlag* cancel) {
  GameSearchStats stats;
  const auto stopped = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kGameSearchTimeLimitSeconds);
  std::set<std::string> visited;
  std::vector<GameSearchEntry> entries;
  try {
    for (const fs::path& root : roots) {
      std::error_code error;
      if (!fs::is_directory(root, error) || error) continue;
      std::vector<std::pair<fs::path, unsigned>> pending{{root, 0}};
      while (!pending.empty()) {
        if (stopped()) { stats.cancelled = true; return stats; }
        if ((time_limited && std::chrono::steady_clock::now() >= deadline) || stats.folders >= kMaximumFolders) {
          stats.limit_reached = true;
          return stats;
        }
        auto [folder, depth] = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(FolderKey(folder)).second) continue;
        ++stats.folders;
        if (progress && stats.folders % 64 == 0) progress(stats.folders, kMaximumFolders, internal::PathToUtf8(folder));

        entries.clear();
        std::vector<fs::path> children;
        fs::directory_iterator iterator(folder, fs::directory_options::skip_permission_denied, error);
        if (error) continue;
        for (fs::directory_iterator end; iterator != end; iterator.increment(error)) {
          if (error) break;
          if (stopped()) { stats.cancelled = true; return stats; }
          const fs::directory_entry& entry = *iterator;
          std::error_code e;
          // symlink_status: links and Windows junctions ("Documents and Settings") are never followed.
          const fs::file_type type = entry.symlink_status(e).type();
          if (e) continue;
          GameSearchEntry item;
          item.path = entry.path();
          item.lower_name = internal::LowerAscii(internal::PathToUtf8(item.path.filename()));
          if (type == fs::file_type::directory) {
            item.directory = true;
            if (depth < kMaximumDepth && !SkipFolder(item.lower_name)) children.push_back(item.path);
          } else if (type == fs::file_type::regular) {
            item.size = entry.file_size(e);
            if (e) item.size = 0;
          } else {
            continue;
          }
          entries.push_back(std::move(item));
        }
        if (visit(folder, entries)) {
          stats.stopped_early = true;
          return stats;
        }
        // Stack: the last pushed is walked next, so likely names go last.
        std::stable_partition(children.begin(), children.end(), [](const fs::path& child) {
          const std::string name = internal::LowerAscii(internal::PathToUtf8(child.filename()));
          for (const char* word : {"wwe", "2k", "13", "15", "xbox", "360", "xenia", "game", "iso", "rom", "content", "dlc"})
            if (name.find(word) != std::string::npos) return false;
          return true;
        });
        for (fs::path& child : children) pending.emplace_back(std::move(child), depth + 1);
      }
    }
  } catch (const std::exception&) {
    // A folder that cannot be read ends the walk like "not found"; the player can still choose a folder.
  }
  return stats;
}

bool IsCloudOnlyFile(const fs::path& file) {
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(file.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return false;
  constexpr DWORD kRecallOnDataAccess = 0x00400000;  // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
  constexpr DWORD kRecallOnOpen = 0x00040000;        // FILE_ATTRIBUTE_RECALL_ON_OPEN
  return (attributes & (FILE_ATTRIBUTE_OFFLINE | kRecallOnDataAccess | kRecallOnOpen)) != 0;
#else
  (void)file;
  return false;
#endif
}

bool IsGameDiscImageCandidate(const GameSearchEntry& entry) {
  if (entry.directory) return false;
  const std::string& name = entry.lower_name;
  const bool image_name = name.size() > 4 && (name.ends_with(".iso") || name.ends_with(".img"));
  if (!image_name) return false;
  if (!TestRootsSet() && entry.size < kDiscImageMinimumBytes) return false;
  return !IsCloudOnlyFile(entry.path);
}

}  // namespace wwe2k14::launcher
