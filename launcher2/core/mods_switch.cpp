// Mods on/off switch (owner 2026-11-18). See mods_switch.h.
#include "mods_switch.h"

#include "wwe13_install.h"
#include "internal/util.h"

#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace wwe2k14::launcher {
namespace fs = std::filesystem;
namespace {

// The add-on content the imports change, relative to the userdata root (wwe13_install.cpp kContentRoot/kHeaderRoot).
const char* const kContentRoots[] = {"0000000000000000/545408B2/00000002", "0000000000000000/545408B2/Headers/00000002"};

std::string Key(const fs::path& relative) {
  std::string text = internal::PathToUtf8(relative);
  for (char& c : text) if (c == '\\') c = '/';
  return text;
}
std::string Lower(std::string text) {
  for (char& c : text) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return text;
}

bool SameFile(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  const bool same = fs::equivalent(a, b, ec);
  return !ec && same;
}

// A copy counts as current when size and write time match its source (copies get the source's write time).
bool SameCopy(const fs::path& view, const fs::path& source) {
  std::error_code ec1, ec2, ec3, ec4;
  const auto vs = fs::file_size(view, ec1), ss = fs::file_size(source, ec2);
  const auto vt = fs::last_write_time(view, ec3), st = fs::last_write_time(source, ec4);
  return !ec1 && !ec2 && !ec3 && !ec4 && vs == ss && vt == st;
}

struct Mirror {
  ModsOffView* view;
  std::string error;

  // view_file becomes a hard link to source (or a copy when linking fails); unchanged entries are kept.
  bool Place(const fs::path& source, const fs::path& view_file) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(view_file, ec);
    const bool exists = !ec && status.type() != fs::file_type::not_found;
    if (exists && fs::is_regular_file(status) && (SameFile(view_file, source) || SameCopy(view_file, source))) return true;
    if (exists) {
      fs::remove_all(view_file, ec);
      if (ec) { error = "Could not refresh " + internal::PathToUtf8(view_file) + "."; return false; }
    }
    fs::create_directories(view_file.parent_path(), ec);
    ec.clear();
    fs::create_hard_link(source, view_file, ec);
    if (!ec) { ++view->linked; return true; }
    ec.clear();
    fs::copy_file(source, view_file, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      error = "Mods off needs a clean copy of " + internal::PathToUtf8(source.filename()) +
              " and it could not be made (" + ec.message() + "). Check the free space on the launcher's drive.";
      return false;
    }
    fs::last_write_time(view_file, fs::last_write_time(source, ec), ec);
    ++view->copied;
    return true;
  }

  // Mirrors source_root into view_root. Files whose key ("<prefix>/<relative>") is in skip are left to the caller.
  // Extra files in the view (no longer in the source, not in skip) are removed.
  // skip holds lower-case keys (record keys and disk names may differ in case on Linux); actual receives
  // lower-case key -> view path for every source file, so originals land on the real spelling.
  bool Tree(const fs::path& source_root, const fs::path& view_root, const std::string& prefix,
            const std::set<std::string>& skip, std::map<std::string, fs::path>* actual) {
    std::error_code ec;
    std::set<std::string> present;
    if (fs::is_directory(source_root, ec)) {
      for (auto it = fs::recursive_directory_iterator(source_root, fs::directory_options::skip_permission_denied, ec);
           !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path relative = it->path().lexically_relative(source_root);
        const std::string key = Lower(prefix + "/" + Key(relative));
        (*actual)[key] = view_root / relative;
        std::error_code sec;
        const fs::file_status status = it->symlink_status(sec);
        if (fs::is_symlink(status)) {  // a linked folder or file (Linux test installs): recreate the same link
          present.insert(key);
          if (skip.count(key)) continue;
          const fs::path target = fs::read_symlink(it->path(), sec);
          const fs::path view_file = view_root / relative;
          std::error_code vec;
          if (fs::is_symlink(fs::symlink_status(view_file, vec)) && fs::read_symlink(view_file, vec) == target) {
            if (fs::is_directory(it->path(), sec)) it.disable_recursion_pending();
            continue;
          }
          fs::remove_all(view_file, vec);
          fs::create_directories(view_file.parent_path(), vec);
          if (fs::is_directory(it->path(), sec)) {
            fs::create_directory_symlink(target, view_file, vec);
            it.disable_recursion_pending();
          } else {
            fs::create_symlink(target, view_file, vec);
          }
          if (vec) { error = "Could not mirror the link " + internal::PathToUtf8(it->path()) + "."; return false; }
          continue;
        }
        if (fs::is_directory(status)) { present.insert(key); continue; }
        if (!fs::is_regular_file(status)) continue;
        present.insert(key);
        if (skip.count(key)) continue;
        if (!Place(it->path(), view_root / relative)) return false;
      }
      if (ec) { error = "Could not read " + internal::PathToUtf8(source_root) + " (" + ec.message() + ")."; return false; }
    }
    // Remove what is no longer in the source (deepest first), except the files the caller owns.
    std::vector<fs::path> extras;
    if (fs::is_directory(view_root, ec)) {
      for (auto it = fs::recursive_directory_iterator(view_root, ec); !ec && it != fs::recursive_directory_iterator();
           it.increment(ec)) {
        const std::string key = Lower(prefix + "/" + Key(it->path().lexically_relative(view_root)));
        std::error_code sec;
        if (fs::is_symlink(it->symlink_status(sec)) && fs::is_directory(it->path(), sec)) it.disable_recursion_pending();
        if (!present.count(key) && !skip.count(key)) {
          extras.push_back(it->path());
          if (fs::is_directory(it->symlink_status(sec))) it.disable_recursion_pending();
        }
      }
    }
    for (const fs::path& extra : extras) {
      std::error_code rec;
      fs::remove_all(extra, rec);
      if (!rec) ++view->removed;
    }
    return true;
  }

  // The view file for one import record: the saved original, or nothing when the import added the file.
  bool Original(const ImportOriginal& original, const fs::path& view_file) {
    std::error_code ec;
    if (!original.exists) {
      if (fs::exists(fs::symlink_status(view_file, ec))) { fs::remove_all(view_file, ec); ++view->removed; }
      return true;
    }
    if (original.kind == 'L') {
      std::ifstream input(original.artifact, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      if (text.empty()) { error = "A saved original link is missing from the import backups."; return false; }
      fs::remove_all(view_file, ec);
      fs::create_directories(view_file.parent_path(), ec);
      fs::create_symlink(internal::PathFromUtf8(text), view_file, ec);
      if (ec) { error = "Could not restore a linked original file for Mods off."; return false; }
      ++view->originals;
      return true;
    }
    if (!fs::is_regular_file(original.artifact, ec)) {
      error = "An original game file is missing from the import backups, so Mods off cannot start a clean game. "
              "Undo the imports on the Mods page or switch Mods back on.";
      return false;
    }
    const bool current = fs::is_regular_file(view_file, ec) &&
                         (SameFile(view_file, original.artifact) || SameCopy(view_file, original.artifact));
    if (current) return true;
    if (ImportFileHash(original.artifact) != original.hash) {
      error = "A saved original game file does not match its fingerprint, so Mods off cannot start a clean game. "
              "Undo the imports on the Mods page or switch Mods back on.";
      return false;
    }
    if (!Place(original.artifact, view_file)) return false;
    ++view->originals;
    return true;
  }
};

bool CopyOnce(const fs::path& source, const fs::path& target, std::string* error) {
  // Saves and profile (everything except the add-on content) become the mods-off copy, made once. Real copies:
  // mods-off play must never write into the player's modded saves.
  std::error_code ec;
  fs::create_directories(target, ec);
  if (!fs::is_directory(source, ec)) return true;
  for (auto it = fs::recursive_directory_iterator(source, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    const fs::path relative = it->path().lexically_relative(source);
    const std::string key = Key(relative);
    bool content = false;
    for (const char* root : kContentRoots) {
      const std::string r(root);
      if (key == r || key.rfind(r + "/", 0) == 0) content = true;
    }
    if (content) { it.disable_recursion_pending(); continue; }
    std::error_code sec;
    if (it->is_directory(sec)) { fs::create_directories(target / relative, sec); continue; }
    if (!it->is_regular_file(sec)) continue;
    fs::create_directories((target / relative).parent_path(), sec);
    fs::copy_file(it->path(), target / relative, fs::copy_options::overwrite_existing, sec);
    if (sec) {
      if (error) *error = "Could not copy your saves for Mods off (" + sec.message() + ").";
      return false;
    }
  }
  if (ec) { if (error) *error = "Could not read your saves for Mods off."; return false; }
  return true;
}

}  // namespace

fs::path ModsOffRoot(const Paths& paths) { return paths.exe_dir / "mods-off"; }

Result PrepareModsOffView(const Paths& paths, const fs::path& game_folder, ModsOffView* view) {
  *view = {};
  view->game = ModsOffRoot(paths) / "game";
  view->userdata = ModsOffRoot(paths) / "userdata";
  std::map<std::string, ImportOriginal> originals;
  std::string error;
  if (!CollectImportOriginals(paths, &originals, &error))
    return Result::Fail(error.empty() ? "The import records could not be read, so Mods off cannot start." : error);

  std::error_code ec;
  fs::create_directories(ModsOffRoot(paths), ec);
  // First mods-off Play: the saves become their own copy (owner 11-18: separate saves).
  const fs::path saves_marker = ModsOffRoot(paths) / "saves-created.txt";
  if (!fs::exists(saves_marker, ec)) {
    if (!CopyOnce(paths.userdata_dir, view->userdata, &error)) return Result::Fail(error);
    std::ofstream(saves_marker) << "Mods-off saves were copied from userdata on first use. They are separate from the "
                                   "saves used with Mods on.\n";
  }

  std::set<std::string> skip;
  for (const auto& entry : originals) skip.insert(Lower(entry.first));
  std::map<std::string, fs::path> actual;
  Mirror mirror{view, {}};
  if (!mirror.Tree(game_folder, view->game, "game", skip, &actual)) return Result::Fail(mirror.error);
  for (const char* root : kContentRoots) {
    if (!mirror.Tree(paths.userdata_dir / fs::u8path(root), view->userdata / fs::u8path(root),
                     std::string("userdata/") + root, skip, &actual))
      return Result::Fail(mirror.error);
  }
  for (const auto& [key, original] : originals) {
    fs::path view_file;
    if (auto it = actual.find(Lower(key)); it != actual.end()) view_file = it->second;
    else if (key.rfind("game/", 0) == 0) view_file = view->game / fs::u8path(key.substr(5));
    else if (key.rfind("userdata/", 0) == 0) view_file = view->userdata / fs::u8path(key.substr(9));
    else continue;  // launcher/ state (own-slots, rosters): mods off simply does not pass it to the game
    if (!mirror.Original(original, view_file)) return Result::Fail(mirror.error);
  }
  return Result::Ok();
}

void UseModsOffView(const ModsOffView& view, LaunchPlan* plan) {
  if (!plan->arguments.empty()) plan->arguments[0] = internal::PathToUtf8(view.game);
  for (std::string& argument : plan->arguments) {
    if (argument.rfind("--user_data_root=", 0) == 0) argument = "--user_data_root=" + internal::PathToUtf8(view.userdata);
  }
}

}  // namespace wwe2k14::launcher
