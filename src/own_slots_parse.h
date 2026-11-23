// Parser for own-slots.txt: one decimal roster id per line, '#' starts a comment, valid ids 100..499.
// Header-only and free of game/SDK includes so it can be unit-tested on the host.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <istream>
#include <string>
#include <vector>

namespace ownslots {
constexpr uint32_t kMinId = 100, kMaxId = 499;

// Reads ids from a stream: blank/comment lines skipped, text after '#' ignored, out-of-range or malformed lines ignored,
// duplicates dropped (first occurrence order kept).
inline std::vector<uint32_t> Parse(std::istream& in) {
  std::vector<uint32_t> ids;
  std::string line;
  while (std::getline(in, line)) {
    const size_t h = line.find('#');
    if (h != std::string::npos) line.resize(h);
    size_t b = line.find_first_not_of(" \t\r\n"), e = line.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) continue;
    const std::string tok = line.substr(b, e - b + 1);
    bool digits = tok.size() <= 6;
    for (char c : tok) if (c < '0' || c > '9') digits = false;
    if (!digits) continue;
    const uint32_t v = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (v < kMinId || v > kMaxId) continue;
    bool dup = false;
    for (uint32_t x : ids) if (x == v) dup = true;
    if (!dup) ids.push_back(v);
  }
  return ids;
}

// Missing or unreadable file -> no ids: an unmodded game stays exactly as it is.
// A path, not a string: on Windows an install folder with characters outside the code page throws in path::string().
inline std::vector<uint32_t> LoadFile(const std::filesystem::path& path) {
  std::ifstream f(path);
  if (!f) return {};
  return Parse(f);
}
}  // namespace ownslots
