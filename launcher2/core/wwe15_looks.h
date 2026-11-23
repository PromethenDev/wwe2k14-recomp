#pragma once

#include "wwe15_convert.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

struct WWE15LooksFiles {
  // Game-relative paths (for example, pac/ch/ch100.pac); plist arcs are returned separately.
  std::map<std::string, WWE15Bytes> files;
  std::vector<WWE15LooksReport> reports;
};

// The 47 2K15 IDs whose looks belong to existing WWE 2K14 roster slots.
const std::vector<uint32_t>& WWE15ExistingLookIds();

// Rebuild changed base-game ch PACs and synchronize their records in the supplied retail pac-list arcs.
// `wwe15_root` may be the extracted game root or its pac folder. Inputs are read-only.
bool BuildWWE15LooksFiles(const std::filesystem::path& wwe14_root,
                          const std::filesystem::path& wwe15_root,
                          const std::vector<uint32_t>& ids,
                          std::vector<WWE15Bytes>* arcs,
                          WWE15LooksFiles* output, std::string* error);

// Host probe writer: creates the same files/game tree plus report.json as the owner reference script.
bool WriteWWE15LooksProbe(const std::filesystem::path& wwe14_root,
                          const std::filesystem::path& wwe15_root,
                          const std::filesystem::path& output_directory,
                          const std::vector<uint32_t>& ids, std::string* error);

}  // namespace wwe2k14::launcher
