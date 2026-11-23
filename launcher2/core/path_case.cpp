#include "path_case.h"

#include "internal/util.h"

#include <algorithm>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace wwe2k14::launcher {
namespace {

std::string Lower(const std::string& value) {
  std::string result(value);
  for (char& ch : result)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
  return result;
}

// Returns the existing entry of `current` whose name equals `name` (exact case first, then the
// first case-insensitive match in sorted name order), or an empty path when not found.
fs::path MatchComponent(const fs::path& current, const std::string& name) {
  std::error_code error;
  const fs::path exact = current / fs::u8path(name);
  if (fs::exists(exact, error) && !error) return exact;
  error.clear();
  if (!fs::is_directory(current, error)) return {};
  std::vector<std::string> names;
  fs::directory_iterator it(current, error);
  const fs::directory_iterator end;
  while (!error && it != end) {
    names.push_back(internal::PathToUtf8(it->path().filename()));
    it.increment(error);
  }
  if (error) return {};
  std::sort(names.begin(), names.end());
  const std::string wanted = Lower(name);
  for (const std::string& candidate : names)
    if (Lower(candidate) == wanted) return current / fs::u8path(candidate);
  return {};
}

}  // namespace

fs::path ResolvePathCase(const fs::path& base, const std::string& relative) {
  fs::path current = base;
  bool appending = false;
  std::string component;
  auto finish_component = [&]() {
    if (component.empty()) return;
    if (!appending) {
      const fs::path matched = MatchComponent(current, component);
      if (!matched.empty()) {
        current = std::move(matched);
        component.clear();
        return;
      }
      appending = true;
    }
    current /= fs::u8path(component);
    component.clear();
  };
  for (const char ch : relative) {
    if (ch == '/') {
      finish_component();
      continue;
    }
    component += ch;
  }
  finish_component();
  return current;
}

}  // namespace wwe2k14::launcher
