#include "update.h"

#include <nlohmann/json.hpp>
#include "sha256.h"
#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
#undef MINIZ_HEADER_FILE_ONLY

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

namespace wwe2k14::updates {
namespace {
constexpr uint64_t kMaxDownload = 512ull * 1024 * 1024;
constexpr uint64_t kMaxExpanded = 1024ull * 1024 * 1024;
std::array<unsigned, 3> Version(std::string text) {
  if (text.starts_with('v')) text.erase(0, 1);
  std::array<unsigned, 3> result{};
  for (size_t i = 0; i < 3; ++i) {
    const size_t dot = text.find('.');
    const std::string part = text.substr(0, dot);
    if (part.empty() || (part.size() > 1 && part.front() == '0'))
      throw std::runtime_error("The release version is invalid.");
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), result[i]);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() ||
        (i < 2 ? dot == std::string::npos : dot != std::string::npos))
      throw std::runtime_error("The release version is invalid.");
    if (dot != std::string::npos) text.erase(0, dot + 1);
  }
  return result;
}
bool SafePath(const std::string& text) {
  if (text.empty() || text.size() > 230 || text.front() == '/' || text.back() == '/' ||
      text.find('\\') != std::string::npos || text.find(':') != std::string::npos) return false;
  for (unsigned char c : text) if (c < 32 || c >= 127 || std::string_view("<>\"|?*").find(c) != std::string_view::npos) return false;
  for (const auto& part : fs::path(text)) {
    const auto s = part.string();
    if (s.empty() || s == "." || s == ".." || s.back() == '.' || s.back() == ' ') return false;
    std::string base = s.substr(0, s.find('.'));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
        (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '0' && base[3] <= '9')) return false;
  }
  return text.find("//") == std::string::npos;
}
bool Digest(const std::string& value) {
  return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::string Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("The update file could not be read.");
  return std::string(std::istreambuf_iterator<char>(input), {});
}
fs::path NewDirectory(const fs::path& parent) {
  fs::create_directories(parent);
  for (unsigned i = 0; i < 1000; ++i) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = parent / ("update-" + std::to_string(stamp) + "-" + std::to_string(i));
    if (fs::create_directory(dir)) {
      fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
      return dir;
    }
  }
  throw std::runtime_error("A private update folder could not be created.");
}
struct Archive {
  mz_zip_archive zip{};
  std::vector<char> bytes;
  explicit Archive(const fs::path& path) {
    if (fs::file_size(path) > kMaxDownload) throw std::runtime_error("The update archive is too large.");
    auto text = Read(path);
    bytes.assign(text.begin(), text.end());
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) throw std::runtime_error("The update ZIP is invalid.");
  }
  ~Archive() { mz_zip_reader_end(&zip); }
};
}

bool Newer(const std::string& candidate, const std::string& installed) {
  return Version(candidate) > Version(installed);
}
Release ParseRelease(const std::string& text, const std::string& installed) {
  const auto json = nlohmann::json::parse(text);
  Release result;
  if (json.value("draft", true) || json.value("prerelease", true)) return result;
  const std::string tag = json.at("tag_name").get<std::string>();
  Version(tag);
  result.version = tag.starts_with('v') ? tag.substr(1) : tag;
  if (!Newer(result.version, installed)) return result;
  if (json.contains("body") && json["body"].is_string()) result.notes = json["body"].get<std::string>().substr(0, 16000);
  const std::string name = "WWE2K14-Recomp-v" + result.version + "-x64.zip";
  for (const auto& asset : json.at("assets")) {
    if (asset.value("name", "") != name || asset.value("state", "") != "uploaded") continue;
    if (result.available) throw std::runtime_error("The release has duplicate update archives.");
    result.url = asset.at("browser_download_url").get<std::string>();
    const std::string expected = "https://github.com/" + std::string(kRepository) + "/releases/download/" + tag + "/" + name;
    if (result.url != expected) throw std::runtime_error("The update archive is not from the official repository.");
    if (!asset.contains("digest") || !asset["digest"].is_string())
      throw std::runtime_error("This release has no GitHub SHA-256 checksum. Please use the release page.");
    const std::string hash = asset["digest"].get<std::string>();
    if (!hash.starts_with("sha256:") || !Digest(hash.substr(7)))
      throw std::runtime_error("The release checksum is invalid.");
    result.sha256 = hash.substr(7);
    result.size = asset.at("size").get<uint64_t>();
    if (!result.size || result.size > kMaxDownload) throw std::runtime_error("The update archive size is invalid.");
    result.available = true;
  }
  if (!result.available) throw std::runtime_error("The new release does not include a supported update archive.");
  return result;
}
Release Check(const std::string& installed, std::atomic<bool>& cancel) {
  const fs::path temp = NewDirectory(fs::temp_directory_path());
  struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{temp};
  const int status = detail::Get("https://api.github.com/repos/" + std::string(kRepository) + "/releases/latest",
                                 temp / "release.json", 4 * 1024 * 1024, cancel);
  if (status == 404) return {};
  if (status != 200) throw std::runtime_error(status == 403 || status == 429 ?
    "GitHub is limiting update checks. Please try again later." : "Updates could not be checked. You can still play offline.");
  return ParseRelease(Read(temp / "release.json"), installed);
}
std::string HashFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("The update archive could not be read.");
  sha256::SHA256 hash;
  std::array<char, 65536> bytes{};
  while (input.read(bytes.data(), bytes.size()) || input.gcount()) hash.add(bytes.data(), static_cast<size_t>(input.gcount()));
  if (!input.eof()) throw std::runtime_error("The update archive could not be read.");
  return hash.getHash();
}
bool AllowedFile(const std::string& relative) {
  if (!SafePath(relative)) return false;
  if (relative.starts_with("licenses/") || relative.starts_with("third-party-licenses/")) return true;
  std::string name = relative;
  if (name.starts_with("Linux-x86_64/")) name.erase(0, 13);
  if (name.starts_with("shader-cache/")) return name == "shader-cache/545408B2.xsh" || name == "shader-cache/545408B2.fbo.vk.xpso";
  if (name.find('/') != std::string::npos) return false;
  if (name == "wwe2k14" || name == "wwe2k14.exe" || name == "wwe2k14-launcher" ||
      name == "wwe2k14-launcher.exe" || name == "WWE 2K14 Launcher.exe" ||
      name == "README.md" || name == "DISCLAIMER.md" || name == "LICENSE" || name == "SHA256SUMS") return true;
  if ((name.starts_with("avcodec-") || name.starts_with("avutil-")) && name.ends_with(".dll")) return true;
  if (name.starts_with("libavcodec.so.") || name.starts_with("libavutil.so.")) return true;
  return name.starts_with("ffmpeg-source-") && name.ends_with(".zip");
}
fs::path InstallRoot(const fs::path& launcher) {
  fs::path dir = fs::absolute(launcher).parent_path();
#ifndef _WIN32
  if (dir.filename() == "Linux-x86_64") dir = dir.parent_path();
#endif
  return dir;
}
void StageArchive(const fs::path& archive, const fs::path& stage) {
  if (!fs::is_directory(stage) || !fs::is_empty(stage) || fs::is_symlink(fs::symlink_status(stage)))
    throw std::runtime_error("The update staging folder is not empty or is unsafe.");
  Archive reader(archive);
  if (reader.zip.m_total_files > 2048) throw std::runtime_error("The update has too many files.");
  std::set<std::string> seen;
  uint64_t expanded = 0;
  for (mz_uint i = 0; i < reader.zip.m_total_files; ++i) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&reader.zip, i, &stat)) throw std::runtime_error("The update ZIP directory is invalid.");
    const mz_uint name_size = mz_zip_reader_get_filename(&reader.zip, i, nullptr, 0);
    if (name_size == 0 || name_size > 300) throw std::runtime_error("The update has an invalid filename.");
    std::string name(name_size, '\0');
    mz_zip_reader_get_filename(&reader.zip, i, name.data(), name_size);
    name.resize(name_size - 1);
    const bool directory = mz_zip_reader_is_file_a_directory(&reader.zip, i) != 0;
    if (directory && name.ends_with('/')) name.pop_back();
    if (!SafePath(name) || !name.starts_with("WWE2K14-Recomp-x64/")) {
      if (directory && name == "WWE2K14-Recomp-x64") continue;
      throw std::runtime_error("The update contains an unsafe archive path.");
    }
    name.erase(0, std::string("WWE2K14-Recomp-x64/").size());
    std::string folded = name;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!seen.insert(folded).second) throw std::runtime_error("The update contains duplicate paths.");
    const auto type = (stat.m_external_attr >> 16) & 0170000;
    if (type && type != 0100000 && type != 0040000) throw std::runtime_error("The update contains a link or special file.");
    if (directory || !AllowedFile(name)) continue;
    if (stat.m_uncomp_size > 256ull * 1024 * 1024 || stat.m_uncomp_size > kMaxExpanded - expanded)
      throw std::runtime_error("The expanded update is too large.");
    expanded += stat.m_uncomp_size;
    std::vector<char> bytes(static_cast<size_t>(stat.m_uncomp_size));
    char empty = 0;
    if (!mz_zip_reader_extract_to_mem(&reader.zip, i, bytes.empty() ? &empty : bytes.data(), bytes.size(), 0))
      throw std::runtime_error("The update archive is damaged.");
    const fs::path path = stage / FromUtf8(name);
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("The update could not be extracted. Check free disk space.");
#ifndef _WIN32
    if (path.filename() == "wwe2k14" || path.filename() == "wwe2k14-launcher")
      fs::permissions(path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);
#endif
  }
  if (!fs::is_regular_file(stage / "wwe2k14.exe") || !fs::is_regular_file(stage / "WWE 2K14 Launcher.exe") ||
      !fs::is_regular_file(stage / "Linux-x86_64/wwe2k14") || !fs::is_regular_file(stage / "Linux-x86_64/wwe2k14-launcher"))
    throw std::runtime_error("The update is missing a game or launcher executable.");
}
fs::path Prepare(const Release& release, const fs::path& launcher, std::atomic<bool>& cancel, const Progress& progress) {
  if (!release.available || !Digest(release.sha256) || !release.size || release.size > kMaxDownload ||
      !release.url.starts_with("https://github.com/" + std::string(kRepository) + "/releases/download/"))
    throw std::runtime_error("The update metadata is invalid.");
  const fs::path root = InstallRoot(launcher);
  if (GameRunning(root)) throw std::runtime_error("Close WWE 2K14 before updating.");
  const fs::path updates = root / ".updates";
  if (fs::is_symlink(fs::symlink_status(updates))) throw std::runtime_error("The update folder is unsafe.");
  const fs::path stage = NewDirectory(updates);
  try {
    const fs::path archive = stage / "download.zip";
    if (detail::Get(release.url, archive, release.size, cancel, progress) != 200)
      throw std::runtime_error("The update archive could not be downloaded.");
    if (cancel.load()) throw std::runtime_error("Update cancelled.");
    if (fs::file_size(archive) != release.size || HashFile(archive) != release.sha256)
      throw std::runtime_error("The downloaded update failed its SHA-256 check.");
    fs::create_directory(stage / "files");
    StageArchive(archive, stage / "files");
    return stage;
  } catch (...) {
    std::error_code e;
    fs::remove_all(stage, e);
    throw;
  }
}
}  // namespace wwe2k14::updates
