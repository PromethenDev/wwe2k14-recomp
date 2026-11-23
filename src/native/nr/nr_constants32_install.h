// Install completeness checks for a packaged constants32 shader set (G56 b).
//
// A packaged install is the pair (legacy, candidate): `legacy` is the folder that names the
// shader contracts the game shipped with, and `candidate` is the folder a constants32 run
// would load. The candidate is complete only when it carries the regular `constants32`
// marker (contents may be empty), a manifest.tsv byte-identical to the legacy one, every
// SPIR-V module the manifest references, a candidate counterpart for every HLSL contract the
// legacy folder still has, and a candidate counterpart for every top-level legacy .spv
// (helpers and optional variants included). Extra files in the candidate are ignored.
//
// Standard library only: this header is compiled by the CPU test too, and it reads the
// filesystem through the std::error_code overloads so a missing or unreadable path becomes a
// message instead of an exception.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "nr_constants32.h"

namespace nr_constants32 {

// Manifest name shared by both folders. Four tab-separated columns: key stage spv source.
inline constexpr const char* kManifestName = "manifest.tsv";

namespace install_detail {

inline bool EndsWith(const std::string& text, const char* suffix) {
  const std::size_t length = std::char_traits<char>::length(suffix);
  return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

// A name that can only land in the folder itself: no separators, not "." or "..".
inline bool SafeBasename(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  for (const char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '\0') return false;
  }
  return true;
}

inline bool StatNonemptyRegular(const std::filesystem::path& path, const std::string& label,
                                std::uintmax_t* size, std::string* error) {
  std::error_code ec;
  const std::filesystem::file_status st = std::filesystem::status(path, ec);
  if (ec == std::errc::no_such_file_or_directory ||
      st.type() == std::filesystem::file_type::not_found) {
    *error = label + " is missing: " + path.string();
    return false;
  }
  if (ec) {
    *error = "cannot stat " + label + " " + path.string() + ": " + ec.message();
    return false;
  }
  if (!std::filesystem::is_regular_file(st)) {
    *error = label + " is not a regular file: " + path.string();
    return false;
  }
  std::error_code size_ec;
  const std::uintmax_t bytes = std::filesystem::file_size(path, size_ec);
  if (size_ec) {
    *error = "cannot size " + label + " " + path.string() + ": " + size_ec.message();
    return false;
  }
  if (bytes == 0) {
    *error = label + " is empty: " + path.string();
    return false;
  }
  if (size != nullptr) *size = bytes;
  return true;
}

inline bool RequireNonemptyRegular(const std::filesystem::path& path,
                                   const std::string& label, std::string* error) {
  return StatNonemptyRegular(path, label, nullptr, error);
}

inline bool ReadNonemptyRegular(const std::filesystem::path& path, const std::string& label,
                                std::string* bytes, std::string* error) {
  if (!StatNonemptyRegular(path, label, nullptr, error)) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "cannot open " + label + " " + path.string() + " for reading";
    return false;
  }
  bytes->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (in.bad()) {
    *error = "cannot read " + label + " " + path.string();
    return false;
  }
  if (bytes->empty()) {
    *error = label + " is empty: " + path.string();
    return false;
  }
  return true;
}

struct Row {
  std::string key;
  std::string stage;
  std::string spv;
  std::string source;
  std::size_t line = 0;
};

// Tab-separated four-column rows. Blank lines (including the trailing newline) carry no row;
// every other line must have exactly four nonempty columns.
inline bool ParseManifest(const std::string& bytes, const std::string& manifest_path,
                          std::vector<Row>* rows, std::string* error) {
  static const char* const kColumnNames[4] = {"key", "stage", "spv", "source"};
  rows->clear();
  std::size_t line_no = 0;
  std::size_t begin = 0;
  while (begin < bytes.size()) {
    std::size_t end = bytes.find('\n', begin);
    if (end == std::string::npos) end = bytes.size();
    std::string line = bytes.substr(begin, end - begin);
    begin = end + 1;
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::vector<std::string> fields;
    std::size_t pos = 0;
    while (true) {
      const std::size_t tab = line.find('\t', pos);
      if (tab == std::string::npos) {
        fields.push_back(line.substr(pos));
        break;
      }
      fields.push_back(line.substr(pos, tab - pos));
      pos = tab + 1;
    }
    const std::string where = manifest_path + ":" + std::to_string(line_no);
    if (fields.size() != 4) {
      *error = where + ": expected 4 tab-separated columns (key stage spv source), found " +
               std::to_string(fields.size());
      return false;
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (fields[i].empty()) {
        *error = where + ": empty " + kColumnNames[i] + " column";
        return false;
      }
    }
    Row row;
    row.key = fields[0];
    row.stage = fields[1];
    row.spv = fields[2];
    row.source = fields[3];
    row.line = line_no;
    rows->push_back(row);
  }
  return true;
}

}  // namespace install_detail

// Complete install => {ok=true, enabled=true, error=""}; anything else => {false, false, error}.
inline Selection InspectCompleteSet(const std::string& legacy, const std::string& candidate) {
  using namespace install_detail;

  Selection out;

  // Candidate must carry the regular constants32 marker; the file may be empty.
  const MarkerStatus marker = InspectMarker(candidate);
  if (marker.error) {
    out.error = marker.error_text;
    return out;
  }
  if (!marker.present) {
    out.error = "candidate shader folder " + candidate + " has no regular " + kMarkerName +
                " marker";
    return out;
  }

  const std::filesystem::path legacy_dir(legacy);
  const std::filesystem::path candidate_dir(candidate);
  const std::filesystem::path legacy_manifest = legacy_dir / kManifestName;
  const std::filesystem::path candidate_manifest = candidate_dir / kManifestName;

  // Both manifests must be nonempty regular files with byte-identical contents.
  std::string legacy_bytes;
  std::string candidate_bytes;
  std::string error;
  if (!ReadNonemptyRegular(legacy_manifest, "legacy " + std::string(kManifestName),
                           &legacy_bytes, &error)) {
    out.error = error;
    return out;
  }
  if (!ReadNonemptyRegular(candidate_manifest, "candidate " + std::string(kManifestName),
                           &candidate_bytes, &error)) {
    out.error = error;
    return out;
  }
  if (legacy_bytes != candidate_bytes) {
    out.error = "legacy and candidate " + std::string(kManifestName) + " differ: " +
                std::to_string(legacy_bytes.size()) + " vs " +
                std::to_string(candidate_bytes.size()) + " bytes";
    return out;
  }

  std::vector<Row> rows;
  if (!ParseManifest(legacy_bytes, legacy_manifest.string(), &rows, &error)) {
    out.error = error;
    return out;
  }
  if (rows.empty()) {
    out.error = legacy_manifest.string() + ": no shader contract rows";
    return out;
  }

  // Rows: stage, safe basenames, and every referenced SPIR-V module present in both folders.
  for (const Row& row : rows) {
    const std::string where = legacy_manifest.string() + ":" + std::to_string(row.line);
    if (row.stage != "vs" && row.stage != "ps") {
      out.error = where + ": stage \"" + row.stage + "\" is not vs or ps";
      return out;
    }
    if (!SafeBasename(row.key)) {
      out.error = where + ": key \"" + row.key + "\" is not a safe local basename";
      return out;
    }
    if (!SafeBasename(row.spv)) {
      out.error = where + ": spv \"" + row.spv + "\" is not a safe local basename";
      return out;
    }
    if (!EndsWith(row.spv, ".spv")) {
      out.error = where + ": spv \"" + row.spv + "\" does not end with .spv";
      return out;
    }
    if (!RequireNonemptyRegular(legacy_dir / row.spv, "legacy SPIR-V " + row.spv, &error)) {
      out.error = error;
      return out;
    }
    if (!RequireNonemptyRegular(candidate_dir / row.spv, "candidate SPIR-V " + row.spv,
                                &error)) {
      out.error = error;
      return out;
    }
  }

  // HLSL contracts: a legacy <key>.<stage>.hlsl requires a nonempty regular candidate
  // counterpart; rows without a legacy contract need none, because packaged shader sets omit
  // HLSL files that carry no metadata.
  for (const Row& row : rows) {
    const std::string contract = row.key + "." + row.stage + ".hlsl";
    const std::filesystem::path legacy_contract = legacy_dir / contract;
    std::error_code exists_ec;
    const bool legacy_has_contract = std::filesystem::exists(legacy_contract, exists_ec);
    if (exists_ec) {
      out.error = "cannot stat legacy HLSL contract " + legacy_contract.string() + ": " +
                  exists_ec.message();
      return out;
    }
    if (!legacy_has_contract) continue;
    if (!RequireNonemptyRegular(candidate_dir / contract, "candidate HLSL contract " + contract,
                                &error)) {
      out.error = error;
      return out;
    }
  }

  // Every top-level legacy .spv, whether or not the manifest names it, needs a candidate
  // counterpart with the same name. Candidate-only extras are allowed.
  std::vector<std::string> legacy_spv;
  std::error_code ec;
  std::filesystem::directory_iterator it(legacy_dir, ec);
  if (ec) {
    out.error = "cannot enumerate legacy shader folder " + legacy_dir.string() + ": " +
                ec.message();
    return out;
  }
  const std::filesystem::directory_iterator end;
  while (it != end) {
    const std::filesystem::directory_entry& entry = *it;
    std::error_code entry_ec;
    const std::filesystem::file_status st = entry.status(entry_ec);
    if (entry_ec && entry_ec != std::errc::no_such_file_or_directory) {
      out.error = "cannot stat " + entry.path().string() + ": " + entry_ec.message();
      return out;
    }
    const std::string name = entry.path().filename().string();
    if (!entry_ec && std::filesystem::is_regular_file(st) && EndsWith(name, ".spv")) {
      legacy_spv.push_back(name);
    }
    it.increment(ec);
  }
  if (ec) {
    out.error = "cannot enumerate legacy shader folder " + legacy_dir.string() + ": " +
                ec.message();
    return out;
  }
  if (legacy_spv.empty()) {
    out.error = "legacy shader folder " + legacy_dir.string() +
                " has no top-level .spv modules";
    return out;
  }
  std::sort(legacy_spv.begin(), legacy_spv.end());
  for (const std::string& name : legacy_spv) {
    if (!RequireNonemptyRegular(legacy_dir / name, "legacy SPIR-V " + name, &error)) {
      out.error = error;
      return out;
    }
    if (!RequireNonemptyRegular(candidate_dir / name, "candidate SPIR-V " + name, &error)) {
      out.error = error;
      return out;
    }
  }

  out.ok = true;
  out.enabled = true;
  return out;
}

}  // namespace nr_constants32
