#include "wwe15_looks.h"

#include "wwe13_pipeline.h"
#include "wwe15_pipeline.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace wwe2k14::launcher {
namespace {

namespace fs = std::filesystem;

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

bool ReadBytes(const fs::path& path, WWE15Bytes* output, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Fail(error, "Could not read " + path.string() + ".");
  output->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (!in.eof() && in.fail()) return Fail(error, "Could not read " + path.string() + ".");
  return true;
}

bool WriteBytes(const fs::path& path, const WWE15Bytes& bytes, std::string* error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return Fail(error, "Could not create " + path.parent_path().string() + ".");
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return Fail(error, "Could not write " + path.string() + ".");
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) return Fail(error, "Could not write " + path.string() + ".");
  return true;
}

std::string JsonString(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out += kHex[c >> 4];
          out += kHex[c & 0x0f];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '\"';
  return out;
}

std::string ReportJson(const std::vector<WWE15LooksReport>& reports) {
  std::ostringstream out;
  out << "[\n";
  for (size_t i = 0; i < reports.size(); ++i) {
    const WWE15LooksReport& r = reports[i];
    out << " {\n  \"id\": " << r.id << ",\n  \"swapped\": [";
    for (size_t j = 0; j < r.swapped.size(); ++j)
      out << (j ? ",\n   " : "\n   ") << JsonString(r.swapped[j]);
    if (!r.swapped.empty()) out << '\n' << "  ";
    out << "],\n  \"kept14\": [";
    for (size_t j = 0; j < r.kept14.size(); ++j)
      out << (j ? ",\n   " : "\n   ") << JsonString(r.kept14[j]);
    if (!r.kept14.empty()) out << '\n' << "  ";
    out << "],\n  \"only15\": [";
    for (size_t j = 0; j < r.only15.size(); ++j)
      out << (j ? ",\n   " : "\n   ") << JsonString(r.only15[j]);
    if (!r.only15.empty()) out << '\n' << "  ";
    out << "],\n  \"dropped_leaves\": {";
    size_t k = 0;
    for (const auto& [name, ids] : r.dropped_leaves) {
      out << (k++ ? ",\n   " : "\n   ") << JsonString(name) << ": [";
      for (size_t n = 0; n < ids.size(); ++n) out << (n ? ", " : "") << ids[n];
      out << ']';
    }
    if (!r.dropped_leaves.empty()) out << '\n' << "  ";
    out << "}\n }" << (i + 1 == reports.size() ? "\n" : ",\n");
  }
  out << "]\n";
  return out.str();
}

}  // namespace

const std::vector<uint32_t>& WWE15ExistingLookIds() {
  static const std::vector<uint32_t> ids{
      100, 101, 102, 103, 104, 107, 109, 115, 123, 125, 130, 131, 139, 145, 160, 161,
      176, 179, 201, 205, 218, 254, 267, 272, 275, 279, 290, 298, 322, 324, 325, 327,
      335, 343, 347, 348, 353, 354, 355, 359, 360, 363, 367, 368, 369, 370, 371};
  return ids;
}

bool BuildWWE15LooksFiles(const fs::path& wwe14_root, const fs::path& wwe15_root,
                          const std::vector<uint32_t>& ids, std::vector<WWE15Bytes>* arcs,
                          WWE15LooksFiles* output, std::string* error) {
  if (error) error->clear();
  if (!arcs || arcs->size() != 2 || !output)
    return Fail(error, "The WWE 2K15 looks outputs are missing or the PAC-list pair is incomplete.");
  if (ids.empty()) return Fail(error, "Choose at least one wrestler ID for WWE 2K15 looks.");
  const fs::path g15 = FindWWE15PacFolder(wwe15_root);
  if (g15.empty()) return Fail(error, "The WWE 2K15 ch folder could not be found under " + wwe15_root.string() + ".");

  std::set<uint32_t> seen;
  std::vector<WWE15Bytes> next_arcs = *arcs;
  WWE15LooksFiles next_output;
  for (uint32_t id : ids) {
    if (id > 999999 || !seen.insert(id).second)
      return Fail(error, "The WWE 2K15 looks ID list contains an invalid or duplicate ID: " + std::to_string(id) + ".");
    const fs::path base_path = wwe14_root / "pac" / "ch" / ("ch" + std::to_string(id) + ".pac");
    const fs::path source_path = g15 / "ch" / ("ch" + std::to_string(id) + ".pac");
    WWE15Bytes base, source, rewritten;
    if (!ReadBytes(base_path, &base, error) || !ReadBytes(source_path, &source, error)) return false;
    WWE15LooksReport report;
    if (!ConvertWWE15LooksPac(base, source, id, &rewritten, &report, error)) return false;

    if (rewritten != base) {
      bool any_listed = false;
      for (WWE15Bytes& arc : next_arcs) {
        bool synced = false;
        std::string detail;
        if (!SyncWWE13PacArchiveIfListed(&arc, base, rewritten, &synced, &detail))
          return Fail(error, "WWE 2K14 wrestler " + std::to_string(id) + " pac-list update failed: " + detail);
        any_listed = any_listed || synced;
      }
      if (!any_listed)
        return Fail(error, "WWE 2K14 wrestler " + std::to_string(id) + " ch PAC is not listed in either plist360 arc.");
    }
    next_output.files["pac/ch/ch" + std::to_string(id) + ".pac"] = std::move(rewritten);
    next_output.reports.push_back(std::move(report));
  }
  *arcs = std::move(next_arcs);
  *output = std::move(next_output);
  return true;
}

bool WriteWWE15LooksProbe(const fs::path& wwe14_root, const fs::path& wwe15_root,
                          const fs::path& output_directory, const std::vector<uint32_t>& ids,
                          std::string* error) {
  if (error) error->clear();
  std::error_code ec;
  if (output_directory.empty() || fs::exists(output_directory, ec))
    return Fail(error, "The output directory must not already exist.");
  std::vector<WWE15Bytes> arcs(2);
  if (!ReadBytes(wwe14_root / "plist360.arc", &arcs[0], error) ||
      !ReadBytes(wwe14_root / "plist360_4x3.arc", &arcs[1], error)) return false;
  WWE15LooksFiles result;
  if (!BuildWWE15LooksFiles(wwe14_root, wwe15_root, ids, &arcs, &result, error)) return false;
  for (const auto& [relative, bytes] : result.files)
    if (!WriteBytes(output_directory / "files" / "game" / fs::u8path(relative), bytes, error)) return false;
  if (!WriteBytes(output_directory / "files" / "game" / "plist360.arc", arcs[0], error) ||
      !WriteBytes(output_directory / "files" / "game" / "plist360_4x3.arc", arcs[1], error)) return false;
  const std::string json = ReportJson(result.reports);
  return WriteBytes(output_directory / "report.json", WWE15Bytes(json.begin(), json.end()), error);
}

}  // namespace wwe2k14::launcher
