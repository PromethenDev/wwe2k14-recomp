#include "../wwe13_convert.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace wwe2k14::launcher;
using Bytes = WWE13ConvertBytes;
namespace fs = std::filesystem;

namespace {

struct Port {
  uint32_t source_id = 0;
  uint32_t target_id = 0;
  uint32_t donor_id = 0;
  std::string name;
  std::string handle;
  std::string attrs = "13";
  std::string index = "free";
  bool own_pro = true;
  std::optional<uint32_t> leaf10000_source_id;
  bool add_leaf50 = false;
  bool waive20002 = false;
};

bool LocateWwe13(const fs::path& base, const fs::path& pack_cache, uint32_t id,
                 fs::path* output);

bool ReadFile(const fs::path& path, Bytes* output, std::string* error) {
  error->clear();
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size > 512ull * 1024 * 1024) {
    *error = "file missing or larger than the probe limit: " + path.filename().string();
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error = "file could not be opened: " + path.filename().string();
    return false;
  }
  output->resize(static_cast<size_t>(size));
  if (size) input.read(reinterpret_cast<char*>(output->data()), static_cast<std::streamsize>(size));
  if (!input && size) {
    *error = "file could not be read: " + path.filename().string();
    output->clear();
    return false;
  }
  return true;
}

std::vector<std::string> Split(const std::string& line, char delimiter) {
  std::vector<std::string> fields;
  size_t begin = 0;
  for (;;) {
    const size_t end = line.find(delimiter, begin);
    fields.push_back(line.substr(begin, end == std::string::npos ? end : end - begin));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return fields;
}

bool Number(const std::string& value, uint32_t* output) {
  if (value.empty()) return false;
  uint64_t parsed = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return false;
    parsed = parsed * 10 + static_cast<uint64_t>(c - '0');
    if (parsed > 999999) return false;
  }
  *output = static_cast<uint32_t>(parsed);
  return true;
}

uint16_t ReadLe16(const Bytes& bytes, size_t at) {
  return static_cast<uint16_t>(bytes[at] | (uint16_t(bytes[at + 1]) << 8));
}

bool ReadPorts(const fs::path& path, std::vector<Port>* ports, std::string* error) {
  std::ifstream input(path);
  if (!input) {
    *error = "the port table could not be opened";
    return false;
  }
  std::string line;
  std::map<std::string, size_t> columns;
  bool header = false;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    const std::vector<std::string> fields = Split(line, '\t');
    if (!header) {
      for (size_t i = 0; i < fields.size(); ++i) columns[fields[i]] = i;
      header = true;
      continue;
    }
    const auto field = [&](const char* name) -> std::string {
      const auto column = columns.find(name);
      return column == columns.end() || column->second >= fields.size() ? std::string() : fields[column->second];
    };
    Port port;
    if (!Number(field("id13"), &port.source_id) || !Number(field("target"), &port.target_id) ||
        !Number(field("donor"), &port.donor_id)) {
      *error = "a port row has an invalid source, target, or donor ID";
      return false;
    }
    port.attrs = field("attrs").empty() ? "13" : field("attrs");
    port.name = field("name");
    port.handle = field("handle");
    port.index = field("index").empty() ? "free" : field("index");
    const std::string own_pro = field("own_pro");
    port.own_pro = own_pro.empty() || own_pro == "1";
    const std::string leaf_fix = field("leaf_fix");
    port.add_leaf50 = leaf_fix == "add50";
    port.waive20002 = leaf_fix == "waive20002";
    const std::string leaf_source = field("leaf10000");
    if (!leaf_source.empty() && !Number(leaf_source, &port.leaf10000_source_id.emplace())) {
      *error = "a port row has an invalid leaf10000 source ID";
      return false;
    }
    ports->push_back(port);
  }
  if (!header || ports->empty()) {
    *error = "the port table has no rows";
    return false;
  }
  return true;
}

bool ReadRowsFromFile(const fs::path& path, const std::string& table,
                      WWE13RawRowMap* rows, std::string* error) {
  Bytes bytes;
  if (!ReadFile(path, &bytes, error)) return false;
  if (!ReadWWE13PacRows(bytes, "CHAR", table, rows, error)) {
    *error = path.filename().string() + ": " + *error;
    return false;
  }
  return true;
}

bool AddDatIndexes(const WWE13RawRowMap& rows, std::set<uint16_t>* indexes, std::string* error) {
  for (const auto& [id, row] : rows) {
    (void)id;
    if (row.size() < 8) {
      *error = "a DAT row is too short to contain its row index";
      return false;
    }
    indexes->insert(static_cast<uint16_t>(row[6] | (uint16_t(row[7]) << 8)));
  }
  return true;
}

bool LoadWWE13Rows(const fs::path& wwe13, const fs::path& pack_cache,
                   const std::vector<Port>& ports, WWE13RowsInput* input,
                   std::string* error) {
  const fs::path base_etc = wwe13 / "pac" / "ch" / "chEtc.pac";
  WWE13RawRowMap base_dat, base_pro;
  if (!ReadRowsFromFile(base_etc, "DAT", &base_dat, error) ||
      !ReadRowsFromFile(base_etc, "PRO", &base_pro, error)) return false;
  input->training13_pro = base_pro;
  input->source13_dat = base_dat;
  input->source13_pro = base_pro;

  std::set<fs::path> pack_tables;
  for (const Port& port : ports) {
    fs::path source_file;
    if (!LocateWwe13(wwe13, pack_cache, port.source_id, &source_file)) {
      *error = "a source character PAC is missing for the port table";
      return false;
    }
    const std::string name = source_file.filename().string();
    const size_t dlc = name.find("_dlc");
    if (dlc == std::string::npos || dlc + 6 > name.size()) continue;
    const fs::path pack_root = source_file.parent_path().parent_path().parent_path();
    pack_tables.insert(pack_root / "pac" / "ch" / ("chEtc" + name.substr(dlc, 6) + ".pac"));
  }
  for (const fs::path& path : pack_tables) {
    WWE13RawRowMap pack_dat, pack_pro;
    if (!ReadRowsFromFile(path, "DAT", &pack_dat, error) ||
        !ReadRowsFromFile(path, "PRO", &pack_pro, error)) return false;
    for (const Port& port : ports) {
      const auto dat = pack_dat.find(port.source_id);
      const auto pro = pack_pro.find(port.source_id);
      if (dat != pack_dat.end()) input->source13_dat[port.source_id] = dat->second;
      if (pro != pack_pro.end()) input->source13_pro[port.source_id] = pro->second;
    }
  }
  return true;
}

bool LoadWWE14Rows(const fs::path& wwe14, const fs::path& userdata,
                   WWE13RowsInput* input, std::string* error) {
  const fs::path base_etc = wwe14 / "pac" / "ch" / "chEtc.pac";
  if (!ReadRowsFromFile(base_etc, "DAT", &input->game14_dat, error) ||
      !ReadRowsFromFile(base_etc, "DSBD", &input->game14_dsbd, error) ||
      !ReadRowsFromFile(base_etc, "PRO", &input->game14_pro, error)) return false;
  if (!AddDatIndexes(input->game14_dat, &input->occupied_dat_indices, error)) return false;

  const fs::path title_root = userdata / "0000000000000000" / "545408B2" / "00000002";
  std::error_code ec;
  if (!fs::is_directory(title_root, ec) || ec) {
    *error = "the pristine 2K14 userdata fixture is missing";
    return false;
  }
  for (fs::directory_iterator pack(title_root, fs::directory_options::skip_permission_denied, ec), end;
       !ec && pack != end; pack.increment(ec)) {
    const fs::path pac_dir = pack->path() / "pac";
    std::error_code pac_error;
    if (!fs::is_directory(pac_dir, pac_error) || pac_error) continue;
    bool string_overlay = false;
    for (fs::directory_iterator file(pac_dir, fs::directory_options::skip_permission_denied, pac_error), file_end;
         !pac_error && file != file_end; file.increment(pac_error)) {
      const std::string filename = file->path().filename().string();
      if (filename.rfind("string_dlc", 0) == 0 && file->path().extension() == ".pac") {
        string_overlay = true;
        break;
      }
    }
    if (!string_overlay) continue;
    const fs::path chetc_dir = pac_dir / "ch";
    std::error_code table_error;
    if (!fs::is_directory(chetc_dir, table_error) || table_error) continue;
    for (fs::directory_iterator file(chetc_dir, fs::directory_options::skip_permission_denied, table_error), file_end;
         !table_error && file != file_end; file.increment(table_error)) {
      const std::string filename = file->path().filename().string();
      if (filename.rfind("chEtc_dlc", 0) != 0 || file->path().extension() != ".pac") continue;
      WWE13RawRowMap overlay_dat;
      if (!ReadRowsFromFile(file->path(), "DAT", &overlay_dat, error) ||
          !AddDatIndexes(overlay_dat, &input->occupied_dat_indices, error)) return false;
    }
  }
  return true;
}

bool CompareRowMaps(const WWE13RawRowMap& actual, const WWE13RawRowMap& expected,
                    const std::vector<Port>& ports, const char* label,
                    size_t* matched, std::map<size_t, size_t>* sizes) {
  for (const Port& port : ports) {
    const auto got = actual.find(port.target_id);
    const auto want = expected.find(port.target_id);
    if (got == actual.end() || want == expected.end()) {
      std::cout << "DIFF row " << label << " id=" << port.target_id << " missing" << '\n';
      continue;
    }
    ++(*sizes)[got->second.size()];
    if (got->second == want->second) {
      ++*matched;
    } else {
      size_t at = 0;
      while (at < std::min(got->second.size(), want->second.size()) && got->second[at] == want->second[at]) ++at;
      std::cout << "DIFF row " << label << " id=" << port.target_id << " native_size=" << got->second.size()
                << " reference_size=" << want->second.size() << " first_byte=" << at << '\n';
    }
  }
  return true;
}

void PrintSizes(const char* label, const std::map<size_t, size_t>& sizes) {
  std::cout << label << " row sizes:";
  for (const auto& [size, count] : sizes) std::cout << ' ' << size << " bytes x " << count;
  std::cout << '\n';
}

std::string Id6(uint32_t id) {
  std::ostringstream value;
  value.width(6);
  value.fill('0');
  value << id;
  return value.str();
}

bool LocateWwe13(const fs::path& base, const fs::path& pack_cache, uint32_t id,
                 fs::path* output) {
  if (id < 100) {
    for (const uint32_t pack : {2u, 3u}) {
      const fs::path candidate = pack_cache / ("pack" + std::to_string(pack)) / "pac" / "ch" /
                                 ("ch" + std::to_string(id) + "_dlc" + (pack < 10 ? "0" : "") +
                                  std::to_string(pack) + ".pac");
      if (fs::is_regular_file(candidate)) {
        *output = candidate;
        return true;
      }
    }
  }
  const fs::path candidate = base / "pac" / "ch" / ("ch" + std::to_string(id) + ".pac");
  if (fs::is_regular_file(candidate)) {
    *output = candidate;
    return true;
  }
  return false;
}

size_t FirstDifference(const Bytes& actual, const Bytes& expected) {
  const size_t common = std::min(actual.size(), expected.size());
  for (size_t i = 0; i < common; ++i)
    if (actual[i] != expected[i]) return i;
  return common;
}

fs::path FindFilesRoot(fs::path path) {
  path = fs::absolute(path);
  while (!path.empty()) {
    if (path.filename() == "files") return path;
    const fs::path parent = path.parent_path();
    if (parent == path) break;
    path = parent;
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 8 || (std::string(argv[7]) != "0" && std::string(argv[7]) != "1")) {
    std::cerr << "usage: wwe13_convert_probe PORTS.tsv WWE13_ROOT WWE13_PACK_CACHE WWE2K14_ROOT USERDATA_ROOT REFERENCE_CH_DIR OWN_CODES(0|1)\n";
    return 2;
  }
  const fs::path table = argv[1];
  const fs::path wwe13 = argv[2];
  const fs::path pack_cache = argv[3];
  const fs::path wwe14 = argv[4];
  const fs::path userdata = argv[5];
  const fs::path reference = argv[6];
  const bool own_codes = std::string(argv[7]) == "1";
  std::vector<Port> ports;
  std::string error;
  if (!ReadPorts(table, &ports, &error)) {
    std::cerr << "ports: " << error << '\n';
    return 1;
  }

  size_t matched = 0;
  size_t failed = 0;
  uint64_t compared_bytes = 0;
  for (const Port& port : ports) {
    const fs::path reference_file = reference / ("ch" + std::to_string(port.target_id) + "_dlc02.pac");
    const fs::path stock_file = wwe14 / "pac" / "ch" / ("ch" + std::to_string(port.donor_id) + ".pac");
    const fs::path source_file = [&] {
      fs::path found;
      LocateWwe13(wwe13, pack_cache, port.source_id, &found);
      return found;
    }();
    if (source_file.empty()) {
      ++failed;
      std::cout << "ERROR " << port.source_id << " -> " << port.target_id << " source PAC missing\n";
      continue;
    }

    Bytes source;
    if (!ReadFile(source_file, &source, &error)) {
      ++failed;
      std::cout << "ERROR " << port.source_id << " -> " << port.target_id << ' ' << error << '\n';
      continue;
    }
    Bytes expected;
    if (!ReadFile(reference_file, &expected, &error)) {
      ++failed;
      std::cout << "ERROR " << port.source_id << " -> " << port.target_id << ' ' << error << '\n';
      continue;
    }

    WWE13CharacterPacRecipe recipe;
    recipe.source_id = port.source_id;
    recipe.target_id = port.target_id;
    recipe.leaf_10000_source_id = port.leaf10000_source_id;
    recipe.add_shared_leaf50 = port.add_leaf50;
    recipe.waive_missing_20002 = port.waive20002;

    Bytes leaf10000_source;
    const Bytes* leaf10000_source_ptr = nullptr;
    if (port.leaf10000_source_id) {
      const fs::path leaf_source_file = wwe13 / "pac" / "ch" /
                                        ("ch" + std::to_string(*port.leaf10000_source_id) + ".pac");
      if (!ReadFile(leaf_source_file, &leaf10000_source, &error)) {
        ++failed;
        std::cout << "ERROR " << port.source_id << " -> " << port.target_id << ' ' << error << '\n';
        continue;
      }
      leaf10000_source_ptr = &leaf10000_source;
    }

    Bytes stock;
    Bytes shared_leaf50;
    const Bytes* shared_leaf50_ptr = nullptr;
    if (port.add_leaf50) {
      if (!ReadFile(stock_file, &stock, &error) ||
          !ReadWWE13CharacterLeaf(stock, Id6(port.donor_id) + "02", 50, &shared_leaf50, &error)) {
        ++failed;
        std::cout << "ERROR " << port.source_id << " -> " << port.target_id << ' ' << error << '\n';
        continue;
      }
      shared_leaf50_ptr = &shared_leaf50;
    }

    Bytes actual;
    if (!ConvertWWE13CharacterPac(recipe, source, leaf10000_source_ptr, nullptr, nullptr,
                                  shared_leaf50_ptr, &actual, &error)) {
      ++failed;
      std::cout << "ERROR " << port.source_id << " -> " << port.target_id << ' ' << error << '\n';
      continue;
    }
    compared_bytes += actual.size() + expected.size();
    if (actual == expected) {
      ++matched;
      continue;
    }
    ++failed;
    const size_t difference = FirstDifference(actual, expected);
    std::cout << "DIFF " << port.source_id << " -> " << port.target_id
              << " output_size=" << actual.size() << " reference_size=" << expected.size()
              << " first_byte=" << difference << '\n';
  }

  std::cout << "stage2_character_pac " << matched << " of " << ports.size()
            << " byte-identical; " << failed << " differences/errors; compared "
            << compared_bytes << " bytes\n";

  WWE13RowsInput row_input;
  if (!LoadWWE13Rows(wwe13, pack_cache, ports, &row_input, &error) ||
      !LoadWWE14Rows(wwe14, userdata, &row_input, &error)) {
    std::cerr << "stage3 inputs: " << error << '\n';
    return 1;
  }
  std::vector<WWE13RowsPort> row_ports;
  for (const Port& port : ports) {
    WWE13RowsPort spec;
    spec.source_id = port.source_id;
    spec.target_id = port.target_id;
    spec.donor_id = port.donor_id;
    spec.attrs = port.attrs;
    spec.index = port.index;
    spec.own_pro = port.own_pro;
    row_ports.push_back(std::move(spec));
  }
  std::vector<WWE13ConvertedRows> converted_rows;
  WWE13RowsOptions row_options;
  row_options.own_attire_codes = own_codes;
  if (!ConvertWWE13Rows(row_ports, row_input, row_options, &converted_rows, &error)) {
    std::cerr << "stage3 conversion: " << error << '\n';
    return 1;
  }
  WWE13RawRowMap actual_dat, actual_dsbd, actual_pro;
  for (WWE13ConvertedRows& row : converted_rows) {
    actual_dat.emplace(row.target_id, std::move(row.dat));
    actual_dsbd.emplace(row.target_id, std::move(row.dsbd));
    actual_pro.emplace(row.target_id, std::move(row.pro));
  }

  WWE13RawRowMap expected_dat, expected_dsbd, expected_pro;
  const fs::path expected_chetc = reference / "chEtc_dlc02.pac";
  if (!ReadRowsFromFile(expected_chetc, "DAT", &expected_dat, &error) ||
      !ReadRowsFromFile(expected_chetc, "DSBD", &expected_dsbd, &error) ||
      !ReadRowsFromFile(expected_chetc, "PRO", &expected_pro, &error)) {
    std::cerr << "stage3 reference: " << error << '\n';
    return 1;
  }
  size_t dat_match = 0, dsbd_match = 0, pro_match = 0;
  std::map<size_t, size_t> dat_sizes, dsbd_sizes, pro_sizes;
  CompareRowMaps(actual_dat, expected_dat, ports, "DAT", &dat_match, &dat_sizes);
  CompareRowMaps(actual_dsbd, expected_dsbd, ports, "DSBD", &dsbd_match, &dsbd_sizes);
  CompareRowMaps(actual_pro, expected_pro, ports, "PRO", &pro_match, &pro_sizes);
  PrintSizes("DAT", dat_sizes);
  PrintSizes("DSBD", dsbd_sizes);
  PrintSizes("PRO", pro_sizes);
  const size_t row_triplets = static_cast<size_t>(std::count_if(
      ports.begin(), ports.end(), [&](const Port& port) {
        return actual_dat[port.target_id] == expected_dat[port.target_id] &&
               actual_dsbd[port.target_id] == expected_dsbd[port.target_id] &&
               actual_pro[port.target_id] == expected_pro[port.target_id];
      }));
  std::cout << "stage3_chEtc_rows " << row_triplets << " of " << ports.size()
            << " DAT/DSBD/PRO triplets byte-identical; fields include donor body/entrance data, converted height/class/attire codes,"
               " assigned DAT index, PRO moves and per-slot IDs\n";

  const fs::path reference_files = FindFilesRoot(reference);
  if (reference_files.empty()) {
    std::cerr << "stage4 reference: could not find the files root above the reference ch directory\n";
    return 1;
  }
  Bytes source_misc, expected_misc;
  const fs::path source_misc_path = wwe14 / "pac" / "misc01_Start.pac";
  const fs::path expected_misc_path = reference_files / "game" / "pac" / "misc01_Start.pac";
  if (!ReadFile(source_misc_path, &source_misc, &error) ||
      !ReadFile(expected_misc_path, &expected_misc, &error)) {
    std::cerr << "stage4 input: " << error << '\n';
    return 1;
  }
  std::vector<WWE13AttireColorPort> attire_ports;
  attire_ports.reserve(ports.size());
  for (const Port& port : ports) attire_ports.push_back({port.target_id, port.donor_id});
  Bytes actual_misc;
  if (!ConvertWWE13AttireColors(source_misc, attire_ports, &actual_misc, &error)) {
    std::cerr << "stage4 conversion: " << error << '\n';
    return 1;
  }
  std::map<uint32_t, Bytes> actual_attire, expected_attire;
  if (!ReadWWE13AttireColorRecords(actual_misc, &actual_attire, &error) ||
      !ReadWWE13AttireColorRecords(expected_misc, &expected_attire, &error)) {
    std::cerr << "stage4 records: " << error << '\n';
    return 1;
  }
  size_t attire_matched = 0;
  for (const Port& port : ports) {
    const auto got = actual_attire.find(port.target_id);
    const auto want = expected_attire.find(port.target_id);
    if (got != actual_attire.end() && want != expected_attire.end() && got->second == want->second) {
      ++attire_matched;
    } else {
      std::cout << "DIFF attire-color record target=" << port.target_id << " donor=" << port.donor_id
                << " missing or byte-mismatched\n";
    }
  }
  size_t attire_codes_resolved = 0;
  size_t first_attire_nonblank = 0;
  for (const Port& port : ports) {
    const auto dat = actual_dat.find(port.target_id);
    if (dat == actual_dat.end() || dat->second.size() < 128) continue;
    for (size_t slot = 0; slot < 4; ++slot) {
      const uint16_t code = ReadLe16(dat->second, 120 + slot * 2);
      const uint32_t color_id = code / 100;
      const auto record = actual_attire.find(color_id);
      if (!code || record == actual_attire.end()) continue;
      ++attire_codes_resolved;
      if (slot == 0 && record->second.size() == 664 &&
          std::any_of(record->second.begin() + 4, record->second.end(),
                      [](uint8_t byte) { return byte != 0; }))
        ++first_attire_nonblank;
    }
  }
  std::cout << "stage4_attire_code_resolution " << attire_codes_resolved << " of "
            << ports.size() * 4 << " codes resolve; first attire record has nonzero payload for "
            << first_attire_nonblank << " of " << ports.size()
            << " targets (visual appearance still requires the prohibited game check)\n";
  const bool misc_identical = actual_misc == expected_misc;
  std::cout << "stage4_attire_color_leaf600 " << attire_matched << " of " << ports.size()
            << " target records byte-identical; misc01_Start.pac "
            << (misc_identical ? "byte-identical" : "DIFF") << "; compared "
            << actual_misc.size() + expected_misc.size() << " archive bytes\n";
  if (!misc_identical) {
    std::cout << "DIFF misc01_Start.pac native_size=" << actual_misc.size()
              << " reference_size=" << expected_misc.size()
              << " first_byte=" << FirstDifference(actual_misc, expected_misc) << '\n';
  }

  WWE13StringKeyMap keyed_strings;
  for (const Port& port : ports) {
    keyed_strings[60000 + port.target_id] = Bytes(port.name.begin(), port.name.end());
    keyed_strings[62000 + port.target_id] = Bytes(port.name.begin(), port.name.end());
    keyed_strings[64000 + port.target_id] = Bytes(port.name.begin(), port.name.end());
    keyed_strings[79000 + port.target_id] = Bytes(port.handle.begin(), port.handle.end());
  }
  std::vector<fs::path> source_string_files{wwe14 / "pac" / "string.pac"};
  const fs::path userdata_title = userdata / "0000000000000000" / "545408B2" / "00000002";
  std::error_code iter_error;
  for (fs::recursive_directory_iterator it(userdata_title, fs::directory_options::skip_permission_denied,
                                           iter_error), end;
       !iter_error && it != end; it.increment(iter_error)) {
    if (!it->is_regular_file(iter_error)) continue;
    const std::string filename = it->path().filename().string();
    if (filename.rfind("string_dlc", 0) == 0 && it->path().extension() == ".pac")
      source_string_files.push_back(it->path());
  }
  if (iter_error) {
    std::cerr << "stage5 input: could not enumerate string overlay PACs: " << iter_error.message() << '\n';
    return 1;
  }
  std::sort(source_string_files.begin(), source_string_files.end());
  WWE13StringKeyMap actual_string_values, expected_string_values;
  size_t string_files_identical = 0, string_files_compared = 0, string_keys_inserted = 0;
  uint64_t string_bytes_compared = 0;
  for (const fs::path& source_path : source_string_files) {
    fs::path expected_path;
    if (source_path == wwe14 / "pac" / "string.pac") {
      expected_path = reference_files / "game" / "pac" / "string.pac";
    } else {
      const fs::path relative = source_path.lexically_relative(userdata);
      expected_path = reference_files / "userdata" / relative;
    }
    Bytes source_string, expected_string;
    if (!ReadFile(source_path, &source_string, &error) ||
        !ReadFile(expected_path, &expected_string, &error)) {
      std::cerr << "stage5 input: " << error << '\n';
      return 1;
    }
    Bytes actual_string;
    size_t inserted = 0;
    if (!ConvertWWE13StringKeys(source_string, keyed_strings, &actual_string, &inserted, &error)) {
      std::cerr << "stage5 conversion " << source_path.filename().string() << ": " << error << '\n';
      return 1;
    }
    WWE13StringKeyMap actual_values, expected_values;
    if (!ReadWWE13StringKeyValues(actual_string, &actual_values, &error) ||
        !ReadWWE13StringKeyValues(expected_string, &expected_values, &error)) {
      std::cerr << "stage5 key values " << source_path.filename().string() << ": " << error << '\n';
      return 1;
    }
    actual_string_values.insert(actual_values.begin(), actual_values.end());
    expected_string_values.insert(expected_values.begin(), expected_values.end());
    string_keys_inserted += inserted;
    string_bytes_compared += actual_string.size() + expected_string.size();
    ++string_files_compared;
    if (actual_string == expected_string) {
      ++string_files_identical;
    } else {
      std::cout << "DIFF string PAC " << source_path.filename().string()
                << " native_size=" << actual_string.size() << " reference_size=" << expected_string.size()
                << " first_byte=" << FirstDifference(actual_string, expected_string) << '\n';
    }
  }
  size_t string_targets_matched = 0;
  for (const Port& port : ports) {
    bool target_matches = true;
    for (const uint32_t key : {60000u + port.target_id, 62000u + port.target_id,
                               64000u + port.target_id, 79000u + port.target_id}) {
      const auto actual = actual_string_values.find(key);
      const auto expected = expected_string_values.find(key);
      if (actual == actual_string_values.end() || expected == expected_string_values.end() ||
          actual->second != expected->second) {
        target_matches = false;
        break;
      }
    }
    if (target_matches) ++string_targets_matched;
    else std::cout << "DIFF stage5 string keys for target=" << port.target_id << '\n';
  }
  std::cout << "stage5_strings " << string_targets_matched << " of " << ports.size()
            << " four-key name/handle sets byte-identical; " << string_files_identical << " of "
            << string_files_compared << " string PAC archives byte-identical; inserted "
            << string_keys_inserted << " table key-values; compared " << string_bytes_compared << " bytes\n";
  return matched == ports.size() && row_triplets == ports.size() &&
                 attire_matched == ports.size() && misc_identical &&
                 attire_codes_resolved == ports.size() * 4 && first_attire_nonblank == ports.size() &&
                 string_targets_matched == ports.size() && string_files_identical == string_files_compared
             ? 0
             : 1;
}
