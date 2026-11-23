#include "../wwe13_pipeline.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace wwe2k14::launcher;
using Bytes = WWE13ConvertBytes;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> Split(const std::string& line, char delimiter) {
  std::vector<std::string> result;
  size_t begin = 0;
  for (;;) {
    const size_t end = line.find(delimiter, begin);
    result.push_back(line.substr(begin, end == std::string::npos ? end : end - begin));
    if (end == std::string::npos) return result;
    begin = end + 1;
  }
}

bool Number(const std::string& text, uint32_t* result) {
  if (text.empty()) return false;
  uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<uint32_t>(c - '0');
    if (value > 0xffffffffu) return false;
  }
  *result = static_cast<uint32_t>(value);
  return true;
}

bool ReadPorts(const fs::path& path, std::vector<WWE13ImportPort>* ports,
               std::string* error) {
  std::ifstream input(path);
  if (!input) { *error = "port table could not be opened"; return false; }
  std::map<std::string, size_t> columns;
  std::string line;
  bool has_header = false;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto fields = Split(line, '\t');
    if (!has_header) {
      for (size_t i = 0; i < fields.size(); ++i) columns[fields[i]] = i;
      has_header = true;
      continue;
    }
    const auto get = [&](const char* key, const std::string& fallback = {}) {
      const auto found = columns.find(key);
      return found == columns.end() || found->second >= fields.size() || fields[found->second].empty()
          ? fallback : fields[found->second];
    };
    WWE13ImportPort port;
    if (!Number(get("id13"), &port.source_id) || !Number(get("target", get("id13")), &port.target_id) ||
        !Number(get("donor"), &port.donor_id)) {
      *error = "port row has an invalid source, target, or donor ID";
      return false;
    }
    port.name = get("name");
    port.handle = get("handle");
    port.pack = get("pack", "745B");
    port.tron = get("tron");
    port.own_pro = get("own_pro", "1") == "1";
    port.attrs = get("attrs", "13");
    port.index = get("index", "free");
    port.win = get("win", "13");
    port.mv = get("mv", "13");
    port.pce = get("pce", "donor");
    port.rank0 = get("rank0", "auto");
    port.rank2 = get("rank2", "auto");
    port.ent = get("ent", "13");
    port.ent_keep = get("ent_keep", "all");
    port.ent_host = get("ent_host");
    port.ent_video = get("ent_video", "keep");
    port.leaf_fix = get("leaf_fix");
    port.ra_name = get("ra_name");
    uint32_t optional = 0;
    if (!get("leaf10000").empty()) {
      if (!Number(get("leaf10000"), &optional)) { *error = "invalid leaf10000 ID"; return false; }
      port.leaf10000 = optional;
    }
    ports->push_back(std::move(port));
  }
  if (!has_header || ports->empty()) { *error = "port table has no records"; return false; }
  return true;
}

bool CollectFiles(const fs::path& root, std::map<std::string, fs::path>* files,
                  std::string* error) {
  std::error_code ec;
  if (!fs::is_directory(root, ec) || ec) { *error = "comparison root is missing: " + root.string(); return false; }
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || ec) continue;
    const std::string relative = it->path().lexically_relative(root).generic_string();
    (*files)[relative] = it->path();
  }
  if (ec) { *error = "could not enumerate comparison files: " + ec.message(); return false; }
  return true;
}

bool ReadBytes(const fs::path& path, Bytes* bytes, std::string* error) {
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size > 512ull * 1024 * 1024) { *error = "file missing or exceeds probe limit: " + path.string(); return false; }
  std::ifstream input(path, std::ios::binary);
  if (!input) { *error = "file could not be opened: " + path.string(); return false; }
  bytes->resize(static_cast<size_t>(size));
  if (size) input.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(size));
  if (!input && size) { *error = "file could not be read: " + path.string(); return false; }
  return true;
}

size_t FirstDiff(const Bytes& a, const Bytes& b) {
  size_t at = 0;
  while (at < std::min(a.size(), b.size()) && a[at] == b[at]) ++at;
  return at;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 9) {
    std::cerr << "usage: wwe13_pipeline_probe PORTS.tsv WWE13_ROOT PACK2_ROOT PACK3_ROOT WWE14_ROOT USERDATA_ROOT FRESH_STAGING_DIR REFERENCE_FILES\n";
    return 2;
  }
  std::vector<WWE13ImportPort> ports;
  std::string error;
  if (!ReadPorts(argv[1], &ports, &error)) { std::cerr << "ports: " << error << '\n'; return 1; }
  WWE13ImportPaths paths;
  paths.wwe13_root = argv[2];
  paths.wwe13_pack_roots = {argv[3], argv[4]};
  paths.wwe14_root = argv[5];
  paths.userdata_root = argv[6];
  WWE13ImportOptions options;
  options.own_attire_codes = false;
  WWE13ImportRunResult result;
  if (!RunWWE13ImportStages2To9(paths, ports, argv[7], options, &result, &error)) {
    std::cerr << "native stages 2-9 failed: " << error << '\n';
    return 1;
  }

  std::map<std::string, fs::path> actual, expected;
  if (!CollectFiles(fs::path(argv[7]) / "files", &actual, &error) ||
      !CollectFiles(argv[8], &expected, &error)) {
    std::cerr << "compare setup failed: " << error << '\n';
    return 1;
  }
  size_t missing = 0, extra = 0, different = 0;
  uint64_t identical_bytes = 0;
  for (const auto& [relative, path] : expected) {
    // porttool's files tree carries this private Stage 10 opt-in marker alongside game files.
    // The native entry point intentionally emits only Stage 2-9 game/userdata payloads.
    if (relative == "OWN-SLOTS-TEST.txt") continue;
    const auto found = actual.find(relative);
    if (found == actual.end()) { ++missing; std::cout << "MISSING " << relative << '\n'; continue; }
    Bytes got, want;
    if (!ReadBytes(found->second, &got, &error) || !ReadBytes(path, &want, &error)) {
      std::cerr << "compare read failed: " << error << '\n'; return 1;
    }
    if (got == want) { identical_bytes += got.size(); continue; }
    ++different;
    std::cout << "DIFF " << relative << " native_size=" << got.size()
              << " reference_size=" << want.size() << " first_byte=" << FirstDiff(got, want) << '\n';
  }
  for (const auto& [relative, path] : actual) {
    (void)path;
    if (!expected.count(relative)) { ++extra; std::cout << "EXTRA " << relative << '\n'; }
  }
  const size_t compared = expected.size() - (expected.count("OWN-SLOTS-TEST.txt") ? 1 : 0);
  if (expected.count("OWN-SLOTS-TEST.txt"))
    std::cout << "ignored reference-only Stage 10 marker OWN-SLOTS-TEST.txt\n";
  std::cout << "stages2to9_files " << (compared - missing - different)
            << " of " << compared << " byte-identical; missing=" << missing
            << " extra=" << extra << " different=" << different
            << " identical_bytes=" << identical_bytes << '\n';
  std::cout << "native staging folder " << result.staging_directory << " (" << result.files.size() << " files)\n";
  return missing || extra || different ? 1 : 0;
}
