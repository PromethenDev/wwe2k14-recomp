#include "../launcher_core.h"

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

    fs::remove_all(output, ignored);
    std::cout << "disc_image_test: PASS (plain/XGD3/XGD2 byte compare; ISO inside the WWE 2K14 folder; wrong title, unsafe path, cancellation, invalid image, GoD folder)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "disc_image_test: FAIL: " << error.what() << '\n';
    return 1;
  }
}
