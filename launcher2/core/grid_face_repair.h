#pragma once
// Select-grid face repair for imported wrestlers (owner 2026-11-16). Imports made before v1.6.2 added no grid faces
// (the grid then shows the game's dark generic head), and v1.6.2 test builds made see-through portrait crops (dark tiles).
// Before each Play the launcher adds the missing faces and replaces see-through ones, cropping each wrestler's own select
// portrait from the add-on files the import installed (no WWE '13 / WWE 2K15 files needed). Stock faces are never touched.
// The menu PACs, the two pac-list arcs and the import undo records are updated together, so Undo keeps working.

#include <cstddef>
#include <filesystem>
#include <string>

#include "launcher_core.h"

namespace wwe2k14::launcher {

struct GridFaceRepairReport {
  bool unchanged = false;   // nothing to check: same files and wrestlers as the last successful check
  size_t added = 0;         // faces added (HD + SD)
  size_t replaced = 0;      // see-through faces replaced (HD + SD)
  size_t no_portrait = 0;   // wrestlers that needed a face but have no installed select portrait
};

// True on success, including "nothing to do". False + *error when a file could not be read or written; then nothing in
// the game folder or the undo records has changed.
bool RepairImportGridFaces(const Paths& paths, const std::filesystem::path& game_folder, GridFaceRepairReport* report,
                           std::string* error);

}  // namespace wwe2k14::launcher
