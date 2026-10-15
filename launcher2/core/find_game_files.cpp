#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace wwe2k14::launcher {
namespace {

bool HasExecutable(const fs::path& folder) {
  std::error_code error;
  return fs::is_regular_file(folder / "default.xex", error) && !error;
}

bool SkipDirectory(const fs::path& directory) {
  const std::string name = internal::LowerAscii(internal::PathToUtf8(directory.filename()));
  static const std::set<std::string> skipped = {
      ".git", ".hg", ".svn", ".cache", "node_modules", "proc", "sys", "dev", "run",
      "windows", "program files", "program files (x86)", "programdata", "system volume information",
      "$recycle.bin", "lost+found", "recovery", "appdata"};
  return skipped.contains(name);
}

void AddUnique(std::vector<fs::path>& paths, const fs::path& path) {
  if (path.empty()) return;
  std::error_code error;
  fs::path normalized = fs::absolute(path, error);
  if (error) normalized = path;
  normalized = normalized.lexically_normal();
  if (std::find(paths.begin(), paths.end(), normalized) == paths.end()) paths.push_back(std::move(normalized));
}

bool HasTestSearchRootOverride() {
#ifdef _WIN32
  const wchar_t* roots = _wgetenv(L"WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS");
#else
  const char* roots = std::getenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS");
#endif
  return roots && *roots;
}

fs::path HomeDirectory() {
#ifdef _WIN32
  if (const wchar_t* profile = _wgetenv(L"USERPROFILE")) return fs::path(profile);
  const wchar_t* drive = _wgetenv(L"HOMEDRIVE");
  const wchar_t* home = _wgetenv(L"HOMEPATH");
  if (drive && home) return fs::path(std::wstring(drive) + home);
#else
  if (const char* home = std::getenv("HOME")) return internal::PathFromUtf8(home);
#endif
  std::error_code error;
  return fs::current_path(error);
}

std::string UnescapeMount(std::string value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && i + 3 < value.size() && value[i + 1] == '0') {
      const std::string code = value.substr(i + 1, 3);
      if (code == "040") decoded.push_back(' ');
      else if (code == "011") decoded.push_back('\t');
      else if (code == "134") decoded.push_back('\\');
      else decoded.push_back(value[i]);
      if (code == "040" || code == "011" || code == "134") {
        i += 3;
        continue;
      }
    }
    decoded.push_back(value[i]);
  }
  return decoded;
}

std::vector<fs::path> SearchRoots(const fs::path& exe_dir, const fs::path& home) {
  std::vector<fs::path> roots;
#ifdef _WIN32
  if (const wchar_t* test_roots = _wgetenv(L"WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS");
      test_roots && *test_roots) {
    std::wstring_view remaining(test_roots);
    constexpr wchar_t separator = L';';
    while (!remaining.empty()) {
      const size_t end = remaining.find(separator);
      const std::wstring_view item = remaining.substr(0, end);
      if (!item.empty()) AddUnique(roots, fs::path(item));
      if (end == std::wstring_view::npos) break;
      remaining.remove_prefix(end + 1);
    }
    return roots;
  }
#else
  if (const char* test_roots = std::getenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS");
      test_roots && *test_roots) {
    constexpr char separator = ':';
    std::string_view remaining(test_roots);
    while (!remaining.empty()) {
      const size_t end = remaining.find(separator);
      const std::string_view item = remaining.substr(0, end);
      if (!item.empty()) AddUnique(roots, internal::PathFromUtf8(std::string(item)));
      if (end == std::string_view::npos) break;
      remaining.remove_prefix(end + 1);
    }
    return roots;
  }
#endif
  AddUnique(roots, exe_dir);
  AddUnique(roots, home);
#ifdef _WIN32
  const DWORD mask = GetLogicalDrives();
  for (unsigned drive = 0; drive < 26; ++drive) {
    if (mask & (1u << drive)) {
      std::string root;
      root.push_back(static_cast<char>('A' + drive));
      root += ":\\";
      AddUnique(roots, internal::PathFromUtf8(root));
    }
  }
#else
  std::ifstream mounts("/proc/self/mounts");
  std::string source, target, type, options;
  while (mounts >> source >> target >> type >> options) {
    if (type == "proc" || type == "sysfs" || type == "devtmpfs" || type == "devpts" ||
        type == "cgroup" || type == "cgroup2" || type == "securityfs" || type == "pstore" ||
        type == "debugfs" || type == "tracefs" || type == "mqueue" || type == "configfs" ||
        type == "fusectl") continue;
    AddUnique(roots, internal::PathFromUtf8(UnescapeMount(target)));
  }
  AddUnique(roots, "/");
  AddUnique(roots, "/mnt");
  AddUnique(roots, "/media");
#endif
  return roots;
}

}  // namespace

std::vector<FoundGame> FindGameFiles(const Paths& paths, const ProgressFn& progress,
                                     const CancelFlag& cancel) {
  try {
  struct Candidate {
    fs::path folder;
    size_t rank = 0;
  };
  std::vector<Candidate> candidates;
  std::vector<fs::path> visited;
  uint64_t visited_count = 0;
  auto inspect = [&](const fs::path& folder) {
    if (folder.empty() || cancel.load(std::memory_order_relaxed)) return;
    std::error_code error;
    if (!fs::is_directory(folder, error) || error || !HasExecutable(folder)) return;
    fs::path normalized = fs::absolute(folder, error);
    if (error) normalized = folder;
    normalized = normalized.lexically_normal();
    if (std::find(visited.begin(), visited.end(), normalized) != visited.end()) return;
    visited.push_back(normalized);
    candidates.push_back({normalized, candidates.size()});
    if (progress) progress(visited_count, 0, internal::PathToUtf8(normalized));
  };

  if (!HasTestSearchRootOverride()) {
    inspect(paths.default_game_folder);
    const Settings saved = LoadSettings(paths);
    inspect(saved.game_folder);
    const fs::path home = HomeDirectory();
    const fs::path title_content = fs::path("545408B2") / "00000000";
    for (const fs::path& content_root : {
             home / "Documents" / "Xenia" / "content",
             home / "Documents" / "Xenia Canary" / "content",
             home / "Documents" / "xenia-canary" / "content",
             paths.exe_dir / "content",
             paths.exe_dir / "xenia-canary" / "content",
             paths.exe_dir / "Xenia Canary" / "content"}) {
      inspect(content_root / title_content);
      inspect(content_root / "545408B2");
    }
  }

  const fs::path home = HomeDirectory();
  for (const fs::path& root : SearchRoots(paths.exe_dir, home)) {
    if (cancel.load(std::memory_order_relaxed)) break;
    std::error_code error;
    if (!fs::is_directory(root, error) || error) continue;
    std::vector<std::pair<fs::path, unsigned>> pending{{root, 0}};
    while (!pending.empty() && !cancel.load(std::memory_order_relaxed)) {
      auto [folder, depth] = std::move(pending.back());
      pending.pop_back();
      inspect(folder);
      ++visited_count;
      if (progress) progress(visited_count, 0, internal::PathToUtf8(folder));
      if (depth >= 4 || cancel.load(std::memory_order_relaxed)) continue;
      fs::directory_iterator iterator(folder, fs::directory_options::skip_permission_denied, error);
      if (error) continue;
      for (const auto& entry : iterator) {
        if (cancel.load(std::memory_order_relaxed)) break;
        error.clear();
        const bool symlink = entry.is_symlink(error);
        if (error || !entry.is_directory(error) || error) continue;
        if (symlink) {
          // Xenia game folders are often linked into a user's library. Inspect
          // the link target as a candidate, but never recurse through it.
          inspect(entry.path());
          continue;
        }
        if (SkipDirectory(entry.path())) continue;
        pending.emplace_back(entry.path(), depth + 1);
      }
    }
  }

  std::vector<FoundGame> found;
  found.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    found.push_back({candidate.folder, CheckGameFolder(candidate.folder, paths)});
  }
  std::stable_sort(found.begin(), found.end(), [&](const FoundGame& left, const FoundGame& right) {
    if (left.status.ready() != right.status.ready()) return left.status.ready();
    const auto rank_of = [&](const fs::path& path) {
      auto it = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& c) {
        return c.folder == path;
      });
      return it == candidates.end() ? candidates.size() : it->rank;
    };
    return rank_of(left.folder) < rank_of(right.folder);
  });
  return found;
  } catch (...) {
    return {};
  }
}

}  // namespace wwe2k14::launcher
