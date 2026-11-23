#include "update.h"
#include <nlohmann/json.hpp>
#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
#undef MINIZ_HEADER_FILE_ONLY
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace wwe2k14::updates;
using Json = nlohmann::json;
void Require(bool condition, const std::string& what) { if (!condition) throw std::runtime_error(what); }
template<class F> void Reject(F action, const std::string& what) {
  try { action(); } catch (...) { return; }
  throw std::runtime_error("Accepted " + what);
}
void Put(const fs::path& file, const std::string& value) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << value;
}
std::string Read(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}
void Zip(const fs::path& path, std::vector<std::pair<std::string, std::string>> entries) {
  mz_zip_archive zip{};
  Require(mz_zip_writer_init_heap(&zip, 0, 0), "ZIP initialization");
  for (const auto& [name, value] : entries) {
    std::string safe = name;
    std::replace(safe.begin(), safe.end(), ':', '_');
    Require(mz_zip_writer_add_mem(&zip, safe.c_str(), value.data(), value.size(), MZ_DEFAULT_COMPRESSION), "ZIP entry: " + name);
  }
  void* data = nullptr;
  size_t size = 0;
  Require(mz_zip_writer_finalize_heap_archive(&zip, &data, &size), "ZIP finalization");
  std::string archive(static_cast<char*>(data), size);
  for (const auto& [name, value] : entries) {
    if (name.find(':') == std::string::npos) continue;
    std::string safe = name;
    std::replace(safe.begin(), safe.end(), ':', '_');
    for (size_t pos = 0; (pos = archive.find(safe, pos)) != std::string::npos; pos += name.size()) archive.replace(pos, name.size(), name);
  }
  std::ofstream out(path, std::ios::binary);
  out.write(archive.data(), static_cast<std::streamsize>(archive.size()));
  mz_free(data);
  mz_zip_writer_end(&zip);
}
int main(int argc, char** argv) {
  const int helper = HelperMain(argc, argv);
  if (helper >= 0) return helper;
  if (argc == 2 && std::string(argv[1]) == "--updated") {
    Put(InstallRoot(FromUtf8(argv[0])) / "restarted.txt", "restarted");
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "--start-update") {
    StartInstaller(FromUtf8(argv[2]), FromUtf8(argv[3]));
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "--stage") {
    StageArchive(FromUtf8(argv[2]), FromUtf8(argv[3]));
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--game-running") return GameRunning(FromUtf8(argv[2])) ? 10 : 0;
  if (argc == 3 && std::string(argv[1]) == "--inspect") {
    const auto stage = fs::temp_directory_path() / ("updater-inspect-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(stage);
    try { StageArchive(FromUtf8(argv[2]), stage); fs::remove_all(stage); std::cout << "Archive staging passed\n"; return 0; }
    catch (const std::exception& error) { fs::remove_all(stage); std::cerr << error.what() << '\n'; return 1; }
  }
  const fs::path work = fs::temp_directory_path() / ("updater-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directory(work);
  try {
    Require(Newer("v1.10.0", "1.9.9"), "numeric version comparison");
    Require(!Newer("v1.0.0", "1.0.0") && !Newer("0.9.9", "1.0.0"), "no downgrades");
    for (const char* version : {"1.2", "1.2.3.4", "1.2.3-beta", "01.2.3", "1.-2.3", "1.2.99999999999999"})
      Reject([&] { Newer(version, "1.0.0"); }, version);
    Json release = {{"draft", false}, {"prerelease", false}, {"tag_name", "v1.1.0"}, {"body", "New fixes"},
      {"assets", Json::array({{{"name", "WWE2K14-Recomp-v1.1.0-x64.zip"}, {"state", "uploaded"},
      {"size", 100}, {"digest", "sha256:" + std::string(64, 'a')},
      {"browser_download_url", "https://github.com/PromethenDev/wwe2k14-recomp/releases/download/v1.1.0/WWE2K14-Recomp-v1.1.0-x64.zip"}}})}};
    Require(ParseRelease(release.dump(), "1.0.0").available, "matching stable release");
    auto modified = release; modified["prerelease"] = true;
    Require(!ParseRelease(modified.dump(), "1.0.0").available, "skip prereleases");
    modified = release; modified["draft"] = true;
    Require(!ParseRelease(modified.dump(), "1.0.0").available, "skip drafts");
    Require(!ParseRelease(release.dump(), "2.0.0").available, "no older release offer");
    modified = release; modified["assets"][0]["digest"] = nullptr;
    Reject([&] { ParseRelease(modified.dump(), "1.0.0"); }, "missing digest");
    modified = release; modified["assets"][0]["browser_download_url"] = "https://example.com/update.zip";
    Reject([&] { ParseRelease(modified.dump(), "1.0.0"); }, "untrusted asset URL");
    modified = release; modified["assets"][0]["size"] = 1024ull * 1024 * 1024;
    Reject([&] { ParseRelease(modified.dump(), "1.0.0"); }, "oversized asset");
    for (const char* path : {"../wwe2k14.exe", "userdata/save.dat", "WWE 2K14/default.xex", "music/song.mp3",
                             "wwe2k14.toml", "wwe2k14-enhanced.ini", "Linux-x86_64/wwe2k14.toml",
                             "licenses/../userdata/save.dat", "licenses/a:b", "licenses/a\\b", "/wwe2k14.exe",
                             "licenses/a//b", "licenses/a.", "licenses/a ", "licenses/CON.txt", "licenses/LPT1", "licenses/a*b"}) Require(!AllowedFile(path), path);
    Require(AllowedFile("Linux-x86_64/wwe2k14") && AllowedFile("WWE 2K14 Launcher.exe"), "managed executables");
    Put(work / "abc", "abc");
    Require(HashFile(work / "abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known vector");
    const std::string prefix = "WWE2K14-Recomp-x64/";
    std::vector<std::pair<std::string, std::string>> entries{
      {prefix + "wwe2k14.exe", "new game"}, {prefix + "WWE 2K14 Launcher.exe", "new launcher"},
      {prefix + "Linux-x86_64/wwe2k14", "new linux"}, {prefix + "Linux-x86_64/wwe2k14-launcher", "new linux launcher"},
      {prefix + "wwe2k14.toml", "default keys"}, {prefix + "userdata/save.dat", "must not install"},
      {prefix + "WWE 2K14/default.xex", "must not install"}, {prefix + "README.md", "new readme"}};
    Zip(work / "valid.zip", entries);
    const fs::path stage = work / "transaction", root = work / "install";
    fs::create_directories(stage / "files");
    StageArchive(work / "valid.zip", stage / "files");
    Require(!fs::exists(stage / "files/wwe2k14.toml") && !fs::exists(stage / "files/userdata"), "preserve user paths");
    Put(root / "wwe2k14.exe", "old game");
    Put(root / "userdata/save.dat", "original save");
    Put(root / "wwe2k14.toml", "custom keys");
    Put(root / "wwe2k14-enhanced.ini", "custom settings");
    Apply(stage, root);
    Require(Read(root / "wwe2k14.exe") == "new game" && Read(stage / "backup/wwe2k14.exe") == "old game", "install and backup");
    Require(Read(root / "userdata/save.dat") == "original save" && Read(root / "wwe2k14.toml") == "custom keys" &&
            Read(root / "wwe2k14-enhanced.ini") == "custom settings", "save/settings/keymap preservation");
    Rollback(stage, root);
    Require(Read(root / "wwe2k14.exe") == "new game", "committed update survives recovery");
    Reject([&] { Apply(stage, root); }, "reused transaction");
    // Simulate termination between old-file backup, new-file install, and the commit marker.
    const fs::path interrupted = work / "interrupted";
    Put(interrupted / "backup/wwe2k14.exe", "old game");
    Put(interrupted / "journal.json", Json::array({{{"path", "wwe2k14.exe"}, {"old", true}},
        {{"path", "README.md"}, {"old", false}}}).dump());
    Rollback(interrupted, root);
    Require(Read(root / "wwe2k14.exe") == "old game" && !fs::exists(root / "README.md"), "interrupted update recovery");
    Rollback(interrupted, root);
    Require(Read(root / "wwe2k14.exe") == "old game", "idempotent recovery");
    // Any malformed path must reject the entire archive, including paths to files we would normally skip.
    for (const char* path : {"../outside", "WWE2K14-Recomp-x64/../escape", "WWE2K14-Recomp-x64/licenses/a:b"}) {
      auto bad = entries; bad.emplace_back(path, "bad"); Zip(work / "bad.zip", bad);
      const fs::path folder = work / "bad-stage"; fs::create_directory(folder);
      Reject([&] { StageArchive(work / "bad.zip", folder); }, path); fs::remove_all(folder);
    }
    auto bad = entries; bad.emplace_back(prefix + "WWE2K14.EXE", "duplicate"); Zip(work / "duplicate.zip", bad);
    fs::create_directory(work / "duplicate");
    Reject([&] { StageArchive(work / "duplicate.zip", work / "duplicate"); }, "case-folded duplicate");
#ifndef _WIN32
    const fs::path symlink_stage = work / "symlink-stage";
    Put(symlink_stage / "files/README.md", "must not write");
    fs::create_symlink(work / "abc", root / "README.md");
    Reject([&] { Apply(symlink_stage, root); }, "symlink destination");
    Require(Read(work / "abc") == "abc", "symlink target unchanged");
#endif
    std::atomic<bool> cancel{false};
    Reject([&] { detail::Get("http://example.com/", work / "http", 100, cancel); }, "plaintext network URL");
    fs::remove_all(work);
    std::cout << "PASS: versions, stable release selection, digest/URL/size validation, SHA-256, archive paths, protected files, install backups, interruption recovery, symlinks, HTTPS policy\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << " (fixtures: " << work << ")\n";
    return 1;
  }
}
