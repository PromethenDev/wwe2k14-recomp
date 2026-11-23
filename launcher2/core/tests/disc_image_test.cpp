#include "../launcher_core.h"
#include "../wwe15_install.h"
#include "../wwe13_arena_install.h"

#include <cctype>
#include <cstdlib>
#include <set>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {

constexpr uint64_t kSectorSize = 0x800;
constexpr uint64_t kDescriptorOffset = 32 * kSectorSize;

struct Node {
  std::string name;
  bool directory = false;
  std::vector<Node> children;
  std::vector<uint8_t> contents;
  uint32_t directory_sector = 0;
  uint32_t directory_size = 0;
  uint32_t file_sector = 0;
};

void Check(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string PathToUtf8(const fs::path& path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  Check(static_cast<bool>(input), "could not read fixture: " + path.string());
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

void AddFile(Node* root, const fs::path& relative, std::vector<uint8_t> contents) {
  Node* directory = root;
  auto component = relative.begin();
  for (; component != relative.end(); ++component) {
    const auto next = component;
    const bool last = std::next(next) == relative.end();
    const std::string name = PathToUtf8(*component);
    auto found = std::find_if(directory->children.begin(), directory->children.end(),
                              [&](const Node& child) { return child.name == name; });
    if (found == directory->children.end()) {
      directory->children.push_back(Node{});
      found = std::prev(directory->children.end());
      found->name = name;
      found->directory = !last;
    }
    if (last) {
      found->directory = false;
      found->contents = std::move(contents);
    } else {
      Check(found->directory, "file/directory fixture name collision");
      directory = &*found;
    }
  }
}

uint32_t TreeSize(Node* directory) {
  for (auto& child : directory->children) {
    if (child.directory) {
      TreeSize(&child);
    }
  }
  uint64_t size = 0;
  for (const auto& child : directory->children) {
    size += (14 + child.name.size() + 3) & ~uint64_t(3);
  }
  Check(size <= UINT32_MAX, "synthetic directory too large");
  directory->directory_size = static_cast<uint32_t>(size);
  return directory->directory_size;
}

uint32_t AssignDirectorySectors(Node* directory, uint32_t next_sector) {
  directory->directory_sector = next_sector;
  next_sector += (directory->directory_size + kSectorSize - 1) / kSectorSize;
  for (auto& child : directory->children) {
    if (child.directory && child.directory_size) {
      next_sector = AssignDirectorySectors(&child, next_sector);
    }
  }
  return next_sector;
}

void AssignFileSectors(Node* directory, uint32_t* next_sector) {
  for (auto& child : directory->children) {
    if (child.directory) {
      AssignFileSectors(&child, next_sector);
    } else {
      child.file_sector = *next_sector;
      *next_sector += static_cast<uint32_t>((child.contents.size() + kSectorSize - 1) / kSectorSize);
    }
  }
}

std::vector<uint8_t> SerializeDirectory(const Node& directory) {
  std::vector<const Node*> sorted;
  for (const auto& child : directory.children) {
    sorted.push_back(&child);
  }
  std::sort(sorted.begin(), sorted.end(), [](const Node* left, const Node* right) {
    return left->name < right->name;
  });
  std::vector<uint8_t> output;
  std::function<uint16_t(size_t, size_t)> append = [&](size_t begin, size_t end) -> uint16_t {
    if (begin == end) {
      return 0;
    }
    const size_t middle = begin + (end - begin) / 2;
    const Node& entry = *sorted[middle];
    const uint16_t ordinal = static_cast<uint16_t>(output.size() / 4);
    const size_t record_size = (14 + entry.name.size() + 3) & ~size_t(3);
    const size_t position = output.size();
    output.resize(position + record_size, 0);
    const uint16_t left = append(begin, middle);
    const uint16_t right = append(middle + 1, end);
    uint8_t* record = output.data() + position;
    const uint32_t sector = entry.directory ? entry.directory_sector : entry.file_sector;
    const uint32_t size = entry.directory ? entry.directory_size : static_cast<uint32_t>(entry.contents.size());
    record[0] = static_cast<uint8_t>(left);
    record[1] = static_cast<uint8_t>(left >> 8);
    record[2] = static_cast<uint8_t>(right);
    record[3] = static_cast<uint8_t>(right >> 8);
    for (unsigned i = 0; i < 4; ++i) {
      record[4 + i] = static_cast<uint8_t>(sector >> (i * 8));
      record[8 + i] = static_cast<uint8_t>(size >> (i * 8));
    }
    record[12] = entry.directory ? 0x10 : 0;
    record[13] = static_cast<uint8_t>(entry.name.size());
    std::copy(entry.name.begin(), entry.name.end(), record + 14);
    return ordinal;
  };
  if (!sorted.empty()) {
    append(0, sorted.size());
  }
  return output;
}

void WriteAt(std::fstream& output, uint64_t offset, const void* bytes, size_t size) {
  output.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  Check(static_cast<bool>(output), "could not seek synthetic image");
  output.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
  Check(static_cast<bool>(output), "could not write synthetic image");
}

void WriteTreePayload(std::fstream& output, const Node& directory, uint64_t partition_offset) {
  const auto bytes = SerializeDirectory(directory);
  WriteAt(output, partition_offset + static_cast<uint64_t>(directory.directory_sector) * kSectorSize,
          bytes.data(), bytes.size());
  for (const auto& child : directory.children) {
    if (child.directory && child.directory_size) {
      WriteTreePayload(output, child, partition_offset);
    } else if (!child.directory && !child.contents.empty()) {
      WriteAt(output, partition_offset + static_cast<uint64_t>(child.file_sector) * kSectorSize,
              child.contents.data(), child.contents.size());
    }
  }
}

void BuildImage(const fs::path& path, uint64_t partition_offset,
                const std::vector<std::pair<fs::path, std::vector<uint8_t>>>& files) {
  Node root;
  root.directory = true;
  for (const auto& file : files) {
    AddFile(&root, file.first, file.second);
  }
  TreeSize(&root);
  uint32_t next_sector = AssignDirectorySectors(&root, 40);
  AssignFileSectors(&root, &next_sector);
  fs::create_directories(path.parent_path());
  std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  Check(static_cast<bool>(output), "could not create synthetic image");
  std::array<uint8_t, 28> descriptor{};
  std::memcpy(descriptor.data(), "MICROSOFT*XBOX*MEDIA", 20);
  auto write_le32 = [&](size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
      descriptor[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    }
  };
  write_le32(20, root.directory_sector);
  write_le32(24, root.directory_size);
  WriteAt(output, partition_offset + kDescriptorOffset, descriptor.data(), descriptor.size());
  WriteTreePayload(output, root, partition_offset);
  output.close();
  Check(static_cast<bool>(output), "could not close synthetic image");
}

bool SameFile(const fs::path& left, const fs::path& right) {
  std::error_code error;
  if (fs::file_size(left, error) != fs::file_size(right, error) || error) {
    return false;
  }
  std::ifstream first(left, std::ios::binary);
  std::ifstream second(right, std::ios::binary);
  std::array<char, 65536> a{}, b{};
  while (first && second) {
    first.read(a.data(), a.size());
    second.read(b.data(), b.size());
    if (first.gcount() != second.gcount() ||
        !std::equal(a.begin(), a.begin() + first.gcount(), b.begin())) {
      return false;
    }
  }
  return first.eof() && second.eof();
}

fs::path OutputFor(const fs::path& base, const std::string& name) {
  return base / name;
}

void CompareExtracted(const fs::path& expected, const fs::path& output) {
  for (const std::string name : {"default.xex", "plist360.def", "plist360_4x3.def"}) {
    Check(SameFile(expected / name, output / name), "extracted file mismatch: " + name);
  }
  Check(SameFile(expected / "plist360.def", output / "nested" / "PLIST360.def"),
        "nested extracted file mismatch");
}

}  // namespace

// A copy of `xex` whose execution-info title ID is `title_id`.
std::vector<uint8_t> WithTitleId(std::vector<uint8_t> xex, uint32_t title_id) {
  auto be32 = [&](size_t at) {
    return (static_cast<uint32_t>(xex[at]) << 24) | (static_cast<uint32_t>(xex[at + 1]) << 16) |
           (static_cast<uint32_t>(xex[at + 2]) << 8) | xex[at + 3];
  };
  const uint32_t count = be32(0x14);
  for (uint32_t index = 0; index < count; ++index) {
    const size_t offset = 0x18 + static_cast<size_t>(index) * 8;
    if (be32(offset) != 0x00040006) continue;
    const uint32_t exec_info = be32(offset + 4);
    for (unsigned i = 0; i < 4; ++i) xex[exec_info + 0x0C + i] = static_cast<uint8_t>(title_id >> (24 - 8 * i));
    return xex;
  }
  throw std::runtime_error("synthetic XEX has no execution info");
}

// WWE 2K15 import from a disc image (owner 11-13): recognized by title ID + contents, read in place, and only the files
// the import reads are copied out.
void TestWWE15DiscImage(const fs::path& output, const std::vector<uint8_t>& xex2k14) {
  const fs::path root = output / "wwe15-disc";
  std::vector<std::pair<fs::path, std::vector<uint8_t>>> files = {
      {"default.xex", WithTitleId(xex2k14, kWwe2k15TitleId)}};
  auto add = [&](const char* path) {
    const std::string name(path);
    std::vector<uint8_t> bytes(name.begin(), name.end());
    bytes.resize(bytes.size() + 3000, 0x5A);  // more than one sector, path-unique contents
    files.emplace_back(path, std::move(bytes));
  };
  for (const char* path : {"pac/m3.pac", "pac/createmode.pac", "pac/gm.pac", "pac/prtcls.pac", "pac/mot00.pac",
                           "pac/ch/chEtc.pac", "pac/ch/ch424.pac", "pac/ch/ch398.pac", "pac/ch/ch301.pac",
                           "pac/ch/ch302.pac", "pac/menu/menuHD2.pac", "pac/menu/menuSD2.pac", "pac/menu/menuHD.pac",
                           "pac/evt/Entrance03.pac", "pac/evt/entrance09.pac", "pac/evt/Winning01.pac",
                           "pac/evt/evtobj.pac", "pac/evt/evtmov.pac", "pac/edit/edit.pac", "movies/intro.bik"})
    add(path);
  const fs::path iso = root / "My Games" / "WWE 2K15 (USA).iso";
  BuildImage(iso, 0x02080000, files);  // XGD3 layout, as WWE 2K15 discs are

  Check(ReadDiscImageTitleId(iso) == kWwe2k15TitleId, "2K15 title ID not read from the disc image");
  Check(IsWWE15DiscImage(iso), "WWE 2K15 disc image not recognized");
  Check(FindWWE15DiscImage(root) == iso, "WWE 2K15 disc image not found under the chosen folder");
  Check(FindWWE15DiscImage(iso) == iso, "WWE 2K15 disc image not accepted when chosen itself");
  // A WWE 2K14 disc with the same files is not WWE 2K15.
  auto files14 = files;
  files14[0].second = xex2k14;
  const fs::path iso14 = output / "wwe15-not" / "WWE 2K14.iso";
  BuildImage(iso14, 0, files14);
  Check(!IsWWE15DiscImage(iso14) && FindWWE15DiscImage(iso14.parent_path()).empty(), "a WWE 2K14 disc passed as WWE 2K15");
  // A WWE 2K15 disc missing a file the contents check needs is refused.
  auto files_short = files;
  files_short.erase(std::remove_if(files_short.begin(), files_short.end(),
                                   [](const auto& file) { return file.first == fs::path("pac/menu/menuSD2.pac"); }),
                    files_short.end());
  const fs::path iso_short = output / "wwe15-short" / "short.iso";
  BuildImage(iso_short, 0, files_short);
  Check(!IsWWE15DiscImage(iso_short), "a WWE 2K15 disc without menuSD2.pac passed the contents check");

  // Copy-out: the shared pac files, the entrance/victory files, the two portrait packs and the chosen models only.
  const fs::path dest = output / "wwe15-copy";
  uint64_t last_done = 0, last_total = 0;
  std::string error;
  CancelFlag cancel{false};
  Check(ExtractWWE15DiscFiles(iso, dest, {301}, [&](uint64_t done, uint64_t total, const std::string&) {
          last_done = done;
          last_total = total;
        }, &cancel, &error), "copy-out failed: " + error);
  std::set<std::string> copied;
  for (const auto& entry : fs::recursive_directory_iterator(dest))
    if (entry.is_regular_file()) copied.insert(entry.path().lexically_relative(dest).generic_string());
  const std::set<std::string> expected{
      "pac/m3.pac", "pac/createmode.pac", "pac/gm.pac", "pac/prtcls.pac", "pac/mot00.pac", "pac/ch/chEtc.pac",
      "pac/ch/ch424.pac", "pac/ch/ch398.pac", "pac/ch/ch301.pac", "pac/menu/menuHD2.pac", "pac/menu/menuSD2.pac",
      "pac/evt/Entrance03.pac", "pac/evt/entrance09.pac", "pac/evt/Winning01.pac", "pac/evt/evtobj.pac"};
  Check(copied == expected, "copy-out did not copy exactly the files the import reads");
  for (const auto& file : files)
    if (copied.count(file.first.generic_string()))
      Check(ReadFile(dest / file.first) == file.second, "copied file differs: " + file.first.generic_string());
  Check(last_total > 0 && last_done == last_total, "copy-out progress did not reach its total");
  Check(IsWWE15PacFolder(dest / "pac") && FindWWE15PacFolder(dest) == dest / "pac",
        "the copied files do not pass the folder check the import runs on them");
  // Stop means stop.
  cancel.store(true);
  Check(!ExtractWWE15DiscFiles(iso, output / "wwe15-cancel", {301}, {}, &cancel, &error) &&
            error.find("cancelled") != std::string::npos, "a stopped copy-out did not report cancellation");

  // The whole-PC search finds the disc image by itself.
#ifdef _WIN32
  _wputenv_s(L"WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", root.wstring().c_str());
#else
  setenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", root.string().c_str(), 1);
#endif
  Paths paths;
  paths.exe_dir = output / "launcher";
  CancelFlag no_cancel{false};
  bool limit = true;
  uint64_t visited = 0;
  Check(SearchWWE15PacFolder(paths, {}, &no_cancel, &limit, &visited) == iso, "the PC search did not find the disc image");
#ifdef _WIN32
  _wputenv_s(L"WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", L"");
#else
  unsetenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS");
#endif
}

// WWE '13 import from a disc image (owner 11-13): recognized, listed, read and copied in place; the scan uses it as the base.
void TestWWE13DiscImage(const fs::path& output, const std::vector<uint8_t>& xex2k14) {
  const fs::path root = output / "wwe13-disc";
  // The first wrestler in the shipped recipe list, so the scan can mark him available from the disc.
  uint32_t source_id = 0;
#if defined(WWE2K14_WWE13_RECIPE)
  {
    std::ifstream recipes(WWE2K14_WWE13_RECIPE);
    std::string line;
    while (std::getline(recipes, line))
      if (!line.empty() && std::isdigit(static_cast<unsigned char>(line[0]))) { source_id = std::stoul(line); break; }
  }
#endif
  Check(source_id != 0, "no WWE '13 recipe line to test with");
  const std::string model = "pac/ch/ch" + std::to_string(source_id) + ".pac";
  std::vector<std::pair<fs::path, std::vector<uint8_t>>> files = {{"default.xex", WithTitleId(xex2k14, 0x545108B4)}};
  auto add = [&](const std::string& path) {
    std::vector<uint8_t> bytes(path.begin(), path.end());
    bytes.resize(bytes.size() + 2500, 0x13);
    files.emplace_back(path, std::move(bytes));
  };
  for (const std::string& path : {std::string("pac/ch/chEtc.pac"), std::string("pac/DLC.pac"), std::string("pac/string.pac"),
                                  model, std::string("pac/bg/bg10.pac"), std::string("sound/RA.pck")})
    add(path);
  const fs::path iso = root / "WWE 13.iso";
  BuildImage(iso, 0x0FD90000, files);  // XGD2 layout, as WWE '13 discs are

  Check(IsWWE13DiscImage(iso), "WWE '13 disc image not recognized");
  Check(!IsWWE15DiscImage(iso), "a WWE '13 disc passed as WWE 2K15");
  const std::vector<DiscImageFile> listing = ListDiscImageFiles(iso);
  Check(listing.size() == files.size(), "disc listing does not hold every file");
  std::vector<uint8_t> read;
  Check(ReadDiscImageFile(iso, "PAC/STRING.PAC", &read) && read == files[3].second, "case-insensitive read of pac/string.pac failed");
  Check(!ReadDiscImageFile(iso, "pac/missing.pac", &read), "a missing disc file was read");
  CancelFlag cancel{false};
  const Result copied = CopyDiscImageFiles(iso, {model, "sound/ra.pck"}, output / "wwe13-copy", {}, &cancel);
  Check(copied.ok, "copy-out failed: " + copied.error);
  Check(ReadFile(output / "wwe13-copy" / model) == files[4].second &&
            ReadFile(output / "wwe13-copy" / "sound" / "RA.pck") == files.back().second,
        "copied files differ or lost the disc's letter case");
  const Result missing = CopyDiscImageFiles(iso, {"pac/nothere.pac"}, output / "wwe13-copy2", {}, &cancel);
  Check(!missing.ok && missing.error.find("pac/nothere.pac") != std::string::npos, "a missing file did not fail the copy by name");
  // A WWE 2K14 disc with the same files is not WWE '13.
  auto files14 = files;
  files14[0].second = xex2k14;
  BuildImage(output / "wwe13-not" / "x.iso", 0, files14);
  Check(!IsWWE13DiscImage(output / "wwe13-not" / "x.iso"), "a WWE 2K14 disc passed as WWE '13");

  // The scan of a chosen folder finds the disc image as the base game and lists the wrestler from it.
  Paths paths;
  paths.exe_dir = output / "wwe13-launcher";
#if defined(WWE2K14_WWE13_RECIPE)
  fs::create_directories(paths.exe_dir / "data");
  fs::copy_file(WWE2K14_WWE13_RECIPE, paths.exe_dir / "data" / "wwe13-ports.tsv", fs::copy_options::overwrite_existing);
#endif
  const WWE13ImportScan scan = ScanWWE13ImportSources(paths, root, {}, cancel);
  Check(scan.base_found && scan.base_is_disc_image && scan.base_folder == iso, "the scan did not use the disc image as the base: " + scan.error);
  const auto wrestler = std::find_if(scan.wrestlers.begin(), scan.wrestlers.end(),
                                     [&](const WWE13ImportWrestler& w) { return w.source_id == source_id; });
  Check(wrestler != scan.wrestlers.end() && wrestler->available && wrestler->source_label == "Base game",
        "the wrestler on the disc image is not available from the base game");
  // The disc image file itself can be picked (a folder picker hides files on Windows; owner 11-13).
  const WWE13ImportScan picked = ScanWWE13ImportSources(paths, iso, {}, cancel);
  Check(picked.base_found && picked.base_is_disc_image && picked.base_folder == iso && picked.error.empty(),
        "a picked WWE '13 disc image was not used as the base: " + picked.error);
  Check(std::any_of(picked.wrestlers.begin(), picked.wrestlers.end(),
                    [&](const WWE13ImportWrestler& w) { return w.source_id == source_id && w.available; }),
        "the wrestler on a picked disc image is not available");
  // Picking a disc that is not WWE '13 says so in plain words and finds nothing.
  const WWE13ImportScan wrong = ScanWWE13ImportSources(paths, output / "wwe13-not" / "x.iso", {}, cancel);
  Check(!wrong.base_found && wrong.error.find("is not a WWE '13") != std::string::npos,
        "a picked non-WWE '13 disc was not refused clearly: " + wrong.error);
  const auto arenas = ListWWE13Arenas(iso, nullptr);
  Check(std::any_of(arenas.begin(), arenas.end(), [](const WWE13ArenaItem& a) { return a.source_id == 10 && a.available; }),
        "arena 10 on the disc image is not available");
}

int main(int argc, char** argv) {
  try {
#if defined(WWE2K14_LAUNCHER_TESTDATA)
    const fs::path default_testdata = WWE2K14_LAUNCHER_TESTDATA;
#else
    const fs::path default_testdata = "out/l1/testdata";
#endif
    Check(argc == 1 || argc == 3, "usage: disc_image_test [TESTDATA_DIR OUTPUT_DIR]");
    const fs::path testdata = fs::absolute(argc == 3 ? fs::path(argv[1]) : default_testdata);
    const fs::path output_root = fs::absolute(argc == 3 ? fs::path(argv[2])
                                                        : testdata.parent_path() / "formats");
    const fs::path output = output_root / "disc-test";
    std::error_code ignored;
    fs::remove_all(output, ignored);
    fs::create_directories(output);
    const fs::path game = testdata / "tu-folder-min";
    const auto xex = ReadFile(game / "default.xex");
    const auto def = ReadFile(game / "plist360.def");
    const auto def43 = ReadFile(game / "plist360_4x3.def");
    const std::vector<std::pair<fs::path, std::vector<uint8_t>>> files = {
        {"default.xex", xex}, {"plist360.def", def}, {"plist360_4x3.def", def43},
        {"nested/PLIST360.def", def}};

    for (const auto& layout : std::array<std::pair<const char*, uint64_t>, 3>{{
             {"plain", 0}, {"xgd3", 0x02080000}, {"xgd2", 0x0FD90000}}}) {
      const fs::path image = output / (std::string(layout.first) + ".iso");
      BuildImage(image, layout.second, files);
      const fs::path extracted = output / (std::string(layout.first) + "-extracted");
      const fs::path stale_partial = fs::path(extracted.string() + ".partial");
      fs::create_directories(stale_partial / "stale-data");
      {
        std::ofstream stale(stale_partial / "stale-data" / "marker.txt");
        stale << "interrupted extraction";
      }
      uint64_t observed_total = 0;
      uint64_t observed_done = 0;
      CancelFlag cancel{false};
      const Result result = ExtractDiscImage(
          image, extracted,
          [&](uint64_t done, uint64_t total, const std::string&) {
            observed_done = done;
            observed_total = total;
          },
          cancel);
      Check(result.ok, std::string(layout.first) + " image failed: " + result.error);
      CompareExtracted(game, extracted);
      Check(!fs::exists(stale_partial), "stale extraction stage was not replaced");
      Check(observed_total == xex.size() + def.size() + def43.size() + def.size(),
            "progress total is not the extracted byte count");
      Check(observed_done == observed_total, "progress did not reach the total byte count");
      fs::remove_all(extracted, ignored);
      fs::remove(image, ignored);
    }

    // Release layout: the ISO sits inside the existing "WWE 2K14" folder next to the placeholder note. Extraction
    // fills that folder (keeping both files) and the folder shows up as a game in FindInstallableFiles first.
    {
      const fs::path game_folder = output / "WWE 2K14";
      fs::create_directories(game_folder);
      { std::ofstream note(game_folder / "PUT-GAME-FILES-HERE.txt"); note << "note"; }
      const fs::path inside_image = game_folder / "WWE 2K14.iso";
      BuildImage(inside_image, 0x02080000, files);
      const FolderInstallables found = FindInstallableFiles(game_folder);
      Check(found.disc_images.size() == 1 && found.disc_images[0] == inside_image && found.packages.empty(),
            "the ISO in the game folder was not found");
      CancelFlag cancel{false};
      const Result merged = ExtractDiscImage(inside_image, game_folder, nullptr, cancel);
      Check(merged.ok, "extracting into the existing game folder failed: " + merged.error);
      for (const auto& file : files) Check(fs::exists(game_folder / file.first), "an extracted file is missing");
      Check(fs::exists(game_folder / "PUT-GAME-FILES-HERE.txt") && fs::exists(inside_image),
            "files already in the game folder were removed");
      Check(!fs::exists(fs::path(game_folder.string() + ".partial")), "the extraction stage was left behind");
      // A folder that already holds a game is never extracted into.
      const Result again = ExtractDiscImage(inside_image, game_folder, nullptr, cancel);
      Check(!again.ok, "extracting over an existing game was allowed");
      fs::remove_all(game_folder, ignored);
    }

    const fs::path wrong_xex_image = output / "wrong-title.iso";
    auto wrong_xex = xex;
    const uint32_t header_size = (static_cast<uint32_t>(wrong_xex[8]) << 24) |
                                (static_cast<uint32_t>(wrong_xex[9]) << 16) |
                                (static_cast<uint32_t>(wrong_xex[10]) << 8) | wrong_xex[11];
    const uint32_t optional_count = (static_cast<uint32_t>(wrong_xex[0x14]) << 24) |
                                    (static_cast<uint32_t>(wrong_xex[0x15]) << 16) |
                                    (static_cast<uint32_t>(wrong_xex[0x16]) << 8) | wrong_xex[0x17];
    uint32_t exec_info = 0;
    for (uint32_t i = 0; i < optional_count; ++i) {
      const size_t offset = 0x18 + i * 8;
      const uint32_t key = (static_cast<uint32_t>(wrong_xex[offset]) << 24) |
                           (static_cast<uint32_t>(wrong_xex[offset + 1]) << 16) |
                           (static_cast<uint32_t>(wrong_xex[offset + 2]) << 8) |
                           wrong_xex[offset + 3];
      if (key == 0x00040006) {
        exec_info = (static_cast<uint32_t>(wrong_xex[offset + 4]) << 24) |
                    (static_cast<uint32_t>(wrong_xex[offset + 5]) << 16) |
                    (static_cast<uint32_t>(wrong_xex[offset + 6]) << 8) |
                    wrong_xex[offset + 7];
      }
    }
    Check(exec_info && exec_info + 0x10 <= header_size, "fixture executable has no execution info");
    wrong_xex[exec_info + 0x0C] ^= 0x01;
    BuildImage(wrong_xex_image, 0, {{"default.xex", wrong_xex}, {"plist360.def", def}});
    const fs::path wrong_dest = output / "wrong-title-game";
    CancelFlag no_cancel{false};
    const Result wrong = ExtractDiscImage(wrong_xex_image, wrong_dest, {}, no_cancel);
    Check(!wrong.ok && wrong.error.find("not WWE 2K14") != std::string::npos,
          "wrong-title image was not rejected clearly");
    Check(!fs::exists(wrong_dest) && !fs::exists(fs::path(wrong_dest.string() + ".partial")),
          "wrong-title image wrote an output folder");

    const fs::path unsafe_image = output / "unsafe-path.iso";
    BuildImage(unsafe_image, 0, {{"..", def}});
    const fs::path unsafe_dest = output / "unsafe-game";
    const Result unsafe = ExtractDiscImage(unsafe_image, unsafe_dest, {}, no_cancel);
    Check(!unsafe.ok && unsafe.error.find("invalid file name") != std::string::npos,
          "disc image path traversal was not rejected");
    Check(!fs::exists(unsafe_dest) && !fs::exists(fs::path(unsafe_dest.string() + ".partial")),
          "unsafe disc path created an output folder");

    const fs::path cancel_image = output / "cancel.iso";
    BuildImage(cancel_image, 0, files);
    const fs::path cancelled_dest = output / "cancelled-game";
    CancelFlag cancel{false};
    const Result cancelled = ExtractDiscImage(
        cancel_image, cancelled_dest,
        [&](uint64_t done, uint64_t, const std::string&) {
          if (done) {
            cancel.store(true);
          }
        },
        cancel);
    Check(!cancelled.ok && cancelled.error.find("cancelled") != std::string::npos,
          "disc extraction cancellation was not reported");
    Check(!fs::exists(cancelled_dest) &&
              !fs::exists(fs::path(cancelled_dest.string() + ".partial")),
          "cancelled extraction left a partial folder");

    const fs::path bad_image = output / "not-xbox.iso";
    {
      std::ofstream invalid(bad_image, std::ios::binary);
      invalid << "not an Xbox 360 image";
    }
    const Result bad = ExtractDiscImage(bad_image, output / "bad-game", {}, no_cancel);
    Check(!bad.ok && bad.error.find("not an Xbox 360") != std::string::npos,
          "invalid image error was not clear");

    const fs::path god_folder = output / "god-folder";
    fs::create_directories(god_folder / "00007000" / "content");
    const Result god = ExtractDiscImage(god_folder, output / "god-game", {}, no_cancel);
    Check(!god.ok && god.error.find("not supported yet") != std::string::npos,
          "Games-on-Demand folder was not clearly reported as unsupported");

    TestWWE15DiscImage(output, xex);
    TestWWE13DiscImage(output, xex);

    fs::remove_all(output, ignored);
    std::cout << "disc_image_test: PASS (plain/XGD3/XGD2 byte compare; ISO inside the WWE 2K14 folder; wrong title, unsafe path, cancellation, invalid image, GoD folder; WWE 2K15 disc image: recognize, find, copy-out, cancel, PC search; WWE '13 disc image: recognize, list, read, copy, scan base, arenas)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "disc_image_test: FAIL: " << error.what() << '\n';
    return 1;
  }
}
