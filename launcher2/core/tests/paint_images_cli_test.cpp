// Developer tool disguised as a test (no arguments: passes). Usage:
//   paint_images_cli_test <exe_dir> list | add <picture>... | remove <slot 1-20> | dump <slot 1-20> <out.png>
// <exe_dir> is a launcher folder whose userdata/ holds the save to change.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "launcher_core.h"

#include "internal/paint_tool.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace wwe2k14::launcher;

int main(int argc, char** argv) {
  if (argc < 3) return 0;
  Paths paths;
  paths.exe_dir = argv[1];
  paths.userdata_dir = paths.exe_dir / "userdata";
  paths.backups_dir = paths.exe_dir / "backups";
  paths.logs_dir = paths.exe_dir / "logs";
  const std::string cmd = argv[2];
  if (cmd == "list" || cmd == "dump") {
    const auto list = ListLogoImages(paths);
    if (!list) { std::cout << "unsupported save\n"; return 1; }
    std::cout << (list->file_present ? "file present" : "no file") << ", " << list->logos.size() << " used\n";
    for (const auto& l : list->logos) {
      std::cout << "slot " << l.slot_index + 1 << " id " << std::hex << l.id << std::dec << "\n";
      if (cmd == "dump" && argc >= 5 && l.slot_index + 1 == static_cast<uint32_t>(std::atoi(argv[3])))
        internal::WritePngRgba(argv[4], l.rgba.data(), 256, 256);
    }
    return 0;
  }
  if (cmd == "add") {
    for (int i = 3; i < argc; ++i) {
      uint32_t slot = 0;
      const Result r = AddLogoImage(paths, argv[i], &slot);
      std::cout << argv[i] << ": " << (r.ok ? "slot " + std::to_string(slot + 1) : r.error) << "\n";
      if (!r.ok) return 1;
    }
    return 0;
  }
  if (cmd == "remove" && argc >= 4) {
    const Result r = RemoveLogoImage(paths, static_cast<uint32_t>(std::atoi(argv[3])) - 1);
    std::cout << (r.ok ? "removed" : r.error) << "\n";
    return r.ok ? 0 : 1;
  }
  return 2;
}
