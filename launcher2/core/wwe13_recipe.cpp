#include "wwe13_pipeline.h"
#include "internal/util.h"

#include <charconv>
#include <fstream>
#include <set>
#include <sstream>

namespace wwe2k14::launcher {
namespace {

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

std::vector<std::string> SplitTsv(const std::string& line) {
  std::vector<std::string> fields;
  size_t start = 0;
  for (;;) {
    const size_t end = line.find('\t', start);
    fields.push_back(line.substr(start, end == std::string::npos ? end : end - start));
    if (end == std::string::npos) return fields;
    start = end + 1;
  }
}

bool ParseId(const std::string& field, uint32_t* value) {
  if (field.empty()) return false;
  uint32_t parsed = 0;
  const auto result = std::from_chars(field.data(), field.data() + field.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != field.data() + field.size()) return false;
  *value = parsed;
  return true;
}

bool ParseOptionalId(const std::string& field, std::optional<uint32_t>* value) {
  if (field.empty()) {
    value->reset();
    return true;
  }
  uint32_t parsed = 0;
  if (!ParseId(field, &parsed)) return false;
  *value = parsed;
  return true;
}

}  // namespace

bool ReadWWE13ImportRecipes(const std::filesystem::path& file,
                            std::vector<WWE13ImportPort>* ports, std::string* error) {
  if (error) error->clear();
  if (!ports) return Fail(error, "The WWE '13 recipe output is missing.");
  std::ifstream input(file, std::ios::binary);
  if (!input) return Fail(error, "The WWE '13 recipe data could not be opened: " + internal::PathToUtf8(file));

  const std::vector<std::string> expected{
      "id13", "target", "name", "handle", "donor", "tron", "own_pro", "attrs", "index", "win", "mv",
      "pce", "rank0", "rank2", "ent", "ent_keep", "ent_host", "ent_video", "leaf_fix", "leaf10000",
      "ra_name", "gender"};
  std::string line;
  if (!std::getline(input, line)) return Fail(error, "The WWE '13 recipe data is empty.");
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (SplitTsv(line) != expected)
    return Fail(error, "The WWE '13 recipe data has an unsupported column layout.");

  std::vector<WWE13ImportPort> parsed;
  std::set<uint32_t> source_ids;
  std::set<uint32_t> target_ids;
  size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const std::vector<std::string> fields = SplitTsv(line);
    if (fields.size() != expected.size())
      return Fail(error, "Recipe row " + std::to_string(line_number) + " has the wrong number of fields.");
    WWE13ImportPort port;
    if (!ParseId(fields[0], &port.source_id) || !ParseId(fields[1], &port.target_id) ||
        !ParseId(fields[4], &port.donor_id) || port.source_id > 499 ||
        port.target_id < 100 || port.target_id > 499 || port.donor_id > 499)
      return Fail(error, "Recipe row " + std::to_string(line_number) + " has an unsupported source, target, or donor ID.");
    if (!source_ids.insert(port.source_id).second || !target_ids.insert(port.target_id).second)
      return Fail(error, "Recipe row " + std::to_string(line_number) + " duplicates a source or target ID.");
    if (fields[2].empty() || fields[2].find_first_of("\r\n\t") != std::string::npos ||
        fields[3].find_first_of("\r\n\t") != std::string::npos)
      return Fail(error, "Recipe row " + std::to_string(line_number) + " has an invalid display name or handle.");
    if (fields[6] != "0" && fields[6] != "1")
      return Fail(error, "Recipe row " + std::to_string(line_number) + " has an invalid own_pro value.");
    if (fields[21] != "M" && fields[21] != "F")
      return Fail(error, "Recipe row " + std::to_string(line_number) + " has an invalid roster gender.");
    if (!ParseOptionalId(fields[19], &port.leaf10000))
      return Fail(error, "Recipe row " + std::to_string(line_number) + " has an invalid leaf10000 value.");

    port.name = fields[2];
    port.handle = fields[3];
    port.tron = fields[5];
    port.own_pro = fields[6] == "1";
    port.attrs = fields[7];
    port.index = fields[8];
    port.win = fields[9];
    port.mv = fields[10];
    port.pce = fields[11];
    port.rank0 = fields[12];
    port.rank2 = fields[13];
    port.ent = fields[14];
    port.ent_keep = fields[15];
    port.ent_host = fields[16];
    port.ent_video = fields[17];
    port.leaf_fix = fields[18];
    port.ra_name = fields[20];
    port.own_announcer_name = port.ra_name.empty();  // no 2K14 line to reuse: bring the WWE '13 one
    port.gender = fields[21];
    parsed.push_back(std::move(port));
  }
  if (input.bad()) return Fail(error, "The WWE '13 recipe data could not be read completely.");
  if (parsed.size() != 106)
    return Fail(error, "The WWE '13 recipe data must contain exactly 106 supported wrestler rows.");
  *ports = std::move(parsed);
  return true;
}

}  // namespace wwe2k14::launcher
