// Mods on/off switch (owner 2026-11-18): "an on/off switch for mods so it won't affect the core game".
//
// Mods off = the game starts from a CLEAN VIEW instead of the real folders, and every Mods-page option is off
// (EffectiveSettings / BuildLaunchPlan). Nothing in the real game folder, add-ons or saves is changed:
//  - game view    <launcher>/mods-off/game: the game folder mirrored with hard links (copies when the drive cannot
//    link), every file an import changed replaced by its saved original, files an import added left out.
//  - userdata view <launcher>/mods-off/userdata: the add-on content (0000000000000000/545408B2/00000002 and its
//    Headers) mirrored the same way on every Play; everything else (saves, profile) is copied ONCE when the view is
//    first made and then belongs to mods-off play (owner choice 11-18: separate saves).
// Rebuilt incrementally at each mods-off Play (unchanged links are kept), so it is quick after the first time.
#pragma once

#include "launcher_core.h"

#include <filesystem>
#include <string>

namespace wwe2k14::launcher {

struct ModsOffView {
  std::filesystem::path game;
  std::filesystem::path userdata;
  size_t linked = 0, copied = 0, originals = 0, removed = 0;  // what this Play's refresh did
};

std::filesystem::path ModsOffRoot(const Paths& paths);

// Builds / refreshes the clean view for game_folder. Fails (and Play must not start) when an original is missing or
// its hash does not match, or the disk is too full for a copy.
Result PrepareModsOffView(const Paths& paths, const std::filesystem::path& game_folder, ModsOffView* view);

// Points a launch plan at the clean view: the game folder argument and --user_data_root.
void UseModsOffView(const ModsOffView& view, LaunchPlan* plan);

}  // namespace wwe2k14::launcher
