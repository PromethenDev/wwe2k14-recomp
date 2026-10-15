#include "launcher_core.h"

#include "internal/xdvdfs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>

namespace wwe2k14::launcher {
namespace {

constexpr uint32_t kXex2Magic = 0x58455832;
constexpr uint32_t kExecutionInfoKey = 0x00040006;
constexpr uint32_t kWwe2k14TitleId = 0x545408B2;

uint32_t ReadBigEndianU32(const uint8_t* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

bool ReadXexTitleId(internal::XdvdfsReader& reader, const internal::XdvdfsEntry& xex,
                    uint32_t* title_id) {
  std::array<uint8_t, 0x18> fixed_header{};
  std::string read_error;
  if (xex.size < fixed_header.size() ||
      !reader.ReadRange(xex, 0, fixed_header.data(), fixed_header.size(), &read_error) ||
      ReadBigEndianU32(fixed_header.data()) != kXex2Magic) {
    return false;
  }
  const uint32_t header_size = ReadBigEndianU32(fixed_header.data() + 8);
  const uint32_t optional_header_count = ReadBigEndianU32(fixed_header.data() + 0x14);
  if (header_size < fixed_header.size() || header_size > xex.size ||
      optional_header_count > (header_size - fixed_header.size()) / 8) {
    return false;
  }

  bool found_execution_info = false;
  uint32_t execution_info_offset = 0;
  for (uint32_t index = 0; index < optional_header_count; ++index) {
    std::array<uint8_t, 8> optional_header{};
    const uint64_t offset = fixed_header.size() + static_cast<uint64_t>(index) * 8;
    if (!reader.ReadRange(xex, offset, optional_header.data(), optional_header.size(), &read_error)) {
      return false;
    }
    if (ReadBigEndianU32(optional_header.data()) != kExecutionInfoKey) {
      continue;
    }
    if (found_execution_info) {
      return false;
    }
    execution_info_offset = ReadBigEndianU32(optional_header.data() + 4);
    found_execution_info = true;
  }
  if (!found_execution_info || execution_info_offset > header_size ||
      header_size - execution_info_offset < 0x10) {
    return false;
  }
  std::array<uint8_t, 4> title_id_bytes{};
  if (!reader.ReadRange(xex, static_cast<uint64_t>(execution_info_offset) + 0x0C,
                        title_id_bytes.data(), title_id_bytes.size(), &read_error)) {
    return false;
  }
  *title_id = ReadBigEndianU32(title_id_bytes.data());
  return true;
}

void CollectEntries(const std::vector<internal::XdvdfsEntry>& entries,
                    std::vector<const internal::XdvdfsEntry*>* files,
                    std::vector<const internal::XdvdfsEntry*>* directories) {
  for (const auto& entry : entries) {
    if (entry.directory) {
      directories->push_back(&entry);
      CollectEntries(entry.children, files, directories);
    } else {
      files->push_back(&entry);
    }
  }
}

bool MakeSafeOutputPath(const fs::path& base, const fs::path& relative, fs::path* output) {
  if (relative.empty() || relative.is_absolute()) {
    return false;
  }
  const fs::path normalized_base = base.lexically_normal();
  const fs::path candidate = (normalized_base / relative).lexically_normal();
  const fs::path within = candidate.lexically_relative(normalized_base);
  if (within.empty() || within.is_absolute() || *within.begin() == fs::path("..")) {
    return false;
  }
  *output = candidate;
  return true;
}

void RemovePartial(const fs::path& partial) {
  std::error_code ignored;
  fs::remove_all(partial, ignored);
}

}  // namespace

Result ExtractDiscImage(const fs::path& iso, const fs::path& dest_folder,
                        const ProgressFn& progress, const CancelFlag& cancel) {
  fs::path partial;
  bool partial_created = false;
  auto fail = [&](std::string message) {
    if (partial_created) {
      RemovePartial(partial);
    }
    return Result::Fail(std::move(message));
  };

  try {
    if (cancel.load()) {
      return Result::Fail("The extraction was cancelled.");
    }
    internal::XdvdfsReader reader;
    std::string error;
    if (!reader.Open(iso, &error)) {
      return Result::Fail(error.empty() ? "This is not an Xbox 360 disc image." : error);
    }

    const internal::XdvdfsEntry* xex = reader.FindRootFile("default.xex");
    uint32_t title_id = 0;
    if (!xex || !ReadXexTitleId(reader, *xex, &title_id)) {
      return Result::Fail("This disc image does not contain a valid WWE 2K14 game executable.");
    }
    if (title_id != kWwe2k14TitleId) {
      return Result::Fail("This disc is not WWE 2K14.");
    }

    std::error_code filesystem_error;
    // The release ships an (almost) empty "WWE 2K14" folder, and players may drop the ISO itself, the title update or
    // DLC files into it. Extracting into it is fine as long as it holds no game yet and nothing on the disc would
    // overwrite a file that is already there; the extracted files are moved in next to what is there.
    bool merge_into_existing = false;
    if (fs::exists(dest_folder, filesystem_error) || filesystem_error) {
      if (filesystem_error || !fs::is_directory(dest_folder, filesystem_error)) {
        return Result::Fail("A file with the game folder's name is in the way.");
      }
      if (fs::exists(dest_folder / "default.xex", filesystem_error)) {
        return Result::Fail("The game folder already contains a game. Choose that folder instead, or empty it first.");
      }
      for (const auto& entry : reader.root_entries()) {
        if (fs::exists(dest_folder / entry.relative_path, filesystem_error)) {
          return Result::Fail("The game folder already contains files from the disc. Empty it first.");
        }
      }
      merge_into_existing = true;
    }
    fs::path parent = dest_folder.parent_path();
    if (parent.empty()) {
      parent = ".";
    }
    fs::create_directories(parent, filesystem_error);
    if (filesystem_error) {
      return Result::Fail("The destination folder could not be created.");
    }
    const auto available = fs::space(parent, filesystem_error);
    if (!filesystem_error && available.available < reader.total_file_bytes()) {
      return Result::Fail("There is not enough free space to extract the disc image.");
    }

    partial = dest_folder;
    partial += ".partial";
    RemovePartial(partial);
    if (!fs::create_directory(partial, filesystem_error) || filesystem_error) {
      return Result::Fail("The temporary extraction folder could not be created.");
    }
    partial_created = true;

    std::vector<const internal::XdvdfsEntry*> files;
    std::vector<const internal::XdvdfsEntry*> directories;
    CollectEntries(reader.root_entries(), &files, &directories);
    for (const auto* directory : directories) {
      if (cancel.load()) {
        return fail("The extraction was cancelled.");
      }
      fs::path output_path;
      if (!MakeSafeOutputPath(partial, directory->relative_path, &output_path)) {
        return fail("The disc image contains an invalid file path.");
      }
      filesystem_error.clear();
      fs::create_directories(output_path, filesystem_error);
      if (filesystem_error) {
        return fail("A folder could not be created while extracting the disc image.");
      }
    }

    uint64_t completed = 0;
    if (progress) {
      progress(0, reader.total_file_bytes(), "Preparing files");
    }
    if (cancel.load()) {
      return fail("The extraction was cancelled.");
    }
    for (const auto* file : files) {
      if (cancel.load()) {
        return fail("The extraction was cancelled.");
      }
      fs::path output_path;
      if (!MakeSafeOutputPath(partial, file->relative_path, &output_path)) {
        return fail("The disc image contains an invalid file path.");
      }
      filesystem_error.clear();
      fs::create_directories(output_path.parent_path(), filesystem_error);
      if (filesystem_error) {
        return fail("A folder could not be created while extracting the disc image.");
      }
      std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
      if (!output) {
        return fail("A file could not be created while extracting the disc image.");
      }
      const bool copied = reader.CopyFile(
          *file, output,
          [&](uint64_t count) {
            completed += count;
            if (progress) {
              progress(completed, reader.total_file_bytes(), file->relative_path.generic_string());
            }
            return !cancel.load();
          },
          &error);
      output.close();
      if (!copied) {
        return fail(cancel.load() ? "The extraction was cancelled."
                                  : (error.empty() ? "A file could not be read from the disc image."
                                                   : error));
      }
      if (!output) {
        return fail("A file could not be written while extracting the disc image.");
      }
    }

    if (cancel.load()) {
      return fail("The extraction was cancelled.");
    }
    filesystem_error.clear();
    if (!merge_into_existing) {
      fs::rename(partial, dest_folder, filesystem_error);
      if (filesystem_error) {
        return fail("The extracted game folder could not be installed.");
      }
      partial_created = false;
      return Result::Ok();
    }
    // Move every top-level extracted entry into the existing folder (same drive: renames, no copying).
    std::vector<fs::path> moved;
    for (const auto& entry : fs::directory_iterator(partial)) {
      const fs::path target = dest_folder / entry.path().filename();
      fs::rename(entry.path(), target, filesystem_error);
      if (filesystem_error) {
        for (const auto& done : moved) {  // undo, so a retry starts from the same state
          std::error_code ignored;
          fs::rename(done, partial / done.filename(), ignored);
        }
        return fail("The extracted game files could not be moved into the game folder.");
      }
      moved.push_back(target);
    }
    RemovePartial(partial);
    partial_created = false;
    return Result::Ok();
  } catch (const std::exception&) {
    return fail("The disc image could not be extracted.");
  } catch (...) {
    return fail("The disc image could not be extracted.");
  }
}

}  // namespace wwe2k14::launcher
