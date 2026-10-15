#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace wwe2k14::launcher {
namespace {

using internal::LowerAscii;
using internal::PathFromUtf8;
using internal::PathToUtf8;
using internal::Trim;

struct IniEntry {
  std::string section;
  std::string key;
  std::string value;
};

std::vector<IniEntry> ParseIni(std::string_view text) {
  std::vector<IniEntry> entries;
  std::string section;
  size_t offset = 0;
  while (offset <= text.size()) {
    const size_t end = text.find('\n', offset);
    std::string line = Trim(text.substr(offset, end == std::string_view::npos ? text.size() - offset
                                                                              : end - offset));
    if (!line.empty() && line.front() == '[' && line.back() == ']') {
      section = LowerAscii(Trim(std::string_view(line).substr(1, line.size() - 2)));
    } else if (!line.empty() && line.front() != ';' && line.front() != '#') {
      const size_t equals = line.find('=');
      if (equals != std::string::npos) {
        entries.push_back({section, LowerAscii(Trim(std::string_view(line).substr(0, equals))),
                           Trim(std::string_view(line).substr(equals + 1))});
      }
    }
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return entries;
}

std::string IniValue(const std::vector<IniEntry>& entries, std::string_view key) {
  const std::string wanted = LowerAscii(key);
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    if (it->section == "settings" && it->key == wanted) return it->value;
  }
  return {};
}

bool EqualsAsciiInsensitive(std::string_view left, std::string_view right) {
  return LowerAscii(left) == LowerAscii(right);
}

bool IsValidUtf8(std::string_view text) {
  size_t offset = 0;
  while (offset < text.size()) {
    const uint8_t first = static_cast<uint8_t>(text[offset++]);
    if (first <= 0x7F) continue;

    unsigned continuation_count = 0;
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      continuation_count = 1;
      codepoint = first & 0x1F;
      minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
      continuation_count = 2;
      codepoint = first & 0x0F;
      minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
      continuation_count = 3;
      codepoint = first & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (text.size() - offset < continuation_count) return false;
    for (unsigned index = 0; index < continuation_count; ++index) {
      const uint8_t next = static_cast<uint8_t>(text[offset++]);
      if ((next & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3F);
    }
    if (codepoint < minimum || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return false;
    }
  }
  return true;
}

std::optional<Resolution> ParseResolution(std::string_view value) {
  if (EqualsAsciiInsensitive(value, "720p")) return Resolution::k720p;
  if (EqualsAsciiInsensitive(value, "1080p")) return Resolution::k1080p;
  if (EqualsAsciiInsensitive(value, "1440p")) return Resolution::k1440p;
  // 480p/576p and pre-schema-7 internal-resolution values depended on a WWE '13-only renderer
  // override that WWE 2K14 does not have; old settings fall back to the original 720p.
  if (EqualsAsciiInsensitive(value, "480p") || EqualsAsciiInsensitive(value, "576p")) {
    return Resolution::k720p;
  }
  return std::nullopt;
}

std::optional<DisplayMode> ParseDisplay(std::string_view value) {
  if (EqualsAsciiInsensitive(value, "fullscreen")) return DisplayMode::kFullscreen;
  if (EqualsAsciiInsensitive(value, "windowed")) return DisplayMode::kWindowed;
  return std::nullopt;
}

std::string ResolutionName(Resolution value) {
  switch (value) {
    case Resolution::k720p: return "720p";
    case Resolution::k1080p: return "1080p";
    case Resolution::k1440p: return "1440p";
  }
  return "720p";
}

std::string DisplayName(DisplayMode value) {
  return value == DisplayMode::kWindowed ? "windowed" : "fullscreen";
}

bool ValidGpuId(std::string_view id) {
  size_t separators[2]{};
  size_t count = 0;
  for (size_t i = 0; i < id.size(); ++i) {
    if (id[i] == ':') {
      if (count >= 2) return false;
      separators[count++] = i;
    }
  }
  if (count == 0 || separators[0] < 4 || separators[0] > 8) return false;
  const size_t device_begin = separators[0] + 1;
  const size_t device_end = count > 1 ? separators[1] : id.size();
  if (device_end - device_begin < 4 || device_end - device_begin > 8) return false;
  if (count > 1 && (separators[1] + 1 == id.size())) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    if (i == separators[0] || (count > 1 && i == separators[1])) continue;
    const char ch = id[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
      return false;
    }
  }
  return true;
}

bool IsLegacyNumericGpu(std::string_view value) {
  if (value.empty() || value.size() > 5) return false;
  size_t offset = value.front() == '-' ? 1 : 0;
  if (offset == value.size()) return false;
  long number = 0;
  for (; offset < value.size(); ++offset) {
    if (value[offset] < '0' || value[offset] > '9') return false;
    number = number * 10 + value[offset] - '0';
  }
  if (value.front() == '-') number = -number;
  return number >= -1 && number <= 4096;
}

std::string NormalizeGpuId(std::string_view value) {
  std::string id(value);
  const size_t first = id.find(':');
  const size_t second = id.find(':', first + 1);
  for (size_t index = 0; index < id.size(); ++index) {
    if (id[index] == ':' || (second != std::string::npos && index > second)) continue;
    id[index] = static_cast<char>(std::toupper(static_cast<unsigned char>(id[index])));
  }
  return id;
}

std::vector<std::string> SplitLines(std::string_view text) {
  std::vector<std::string> lines;
  size_t offset = 0;
  while (offset < text.size()) {
    const size_t end = text.find('\n', offset);
    std::string line(text.substr(offset, end == std::string_view::npos ? text.size() - offset
                                                                      : end - offset));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return lines;
}

std::string BuildIni(const Paths& paths, const Settings& settings) {
  std::string old_text;
  (void)internal::ReadTextFile(paths.settings_file, old_text);
  const std::vector<std::string> old_lines = SplitLines(old_text);
  const std::set<std::string> replaced = {"schema", "anti_aliasing", "resolution", "frame_rate",
                                          "display", "gpu", "game_folder", "auto_start",
                                          "auto_backup", "check_updates", "skipped_update_version"};
  std::vector<std::string> unknown_settings;
  std::vector<std::string> outside_settings;
  std::string active_section;
  bool had_settings = false;
  for (const std::string& raw_line : old_lines) {
    const std::string line = Trim(raw_line);
    if (!line.empty() && line.front() == '[' && line.back() == ']') {
      active_section = LowerAscii(Trim(std::string_view(line).substr(1, line.size() - 2)));
      if (active_section == "settings") {
        had_settings = true;
        continue;
      }
      outside_settings.push_back(raw_line);
      continue;
    }
    if (active_section == "settings") {
      const size_t equals = line.find('=');
      if (equals != std::string::npos && replaced.contains(LowerAscii(Trim(std::string_view(line).substr(0, equals))))) {
        continue;
      }
      unknown_settings.push_back(raw_line);
    } else {
      outside_settings.push_back(raw_line);
    }
  }

  std::ostringstream output;
  output << "[Settings]\r\nschema=7\r\n"
         << "resolution=" << (settings.explicit_choice ? ResolutionName(settings.resolution) : "auto") << "\r\n"
         << "display=" << DisplayName(settings.display) << "\r\n"
         << "gpu=" << (settings.gpu_id.empty() ? "auto" : settings.gpu_id) << "\r\n"
         << "game_folder=" << PathToUtf8(settings.game_folder.empty() ? paths.default_game_folder : settings.game_folder)
         << "\r\nauto_start=" << (settings.auto_start ? "1" : "0")
         << "\r\nauto_backup=" << (settings.auto_backup ? "1" : "0")
         << "\r\ncheck_updates=" << (settings.check_updates ? "1" : "0")
         << "\r\nskipped_update_version=" << settings.skipped_update_version << "\r\n";
  for (const auto& line : unknown_settings) output << line << "\r\n";
  if (!outside_settings.empty()) {
    output << "\r\n";
    for (const auto& line : outside_settings) output << line << "\r\n";
  } else if (!had_settings) {
    // The canonical settings block above is the complete new document.
  }
  return output.str();
}

}  // namespace

namespace {

Settings LoadSettingsImpl(const Paths& paths) {
  Settings settings;
  settings.game_folder = paths.default_game_folder;
  std::string text;
  if (!internal::ReadTextFile(paths.settings_file, text)) return settings;
  const auto entries = ParseIni(text);
  const std::string schema = IniValue(entries, "schema");
  const bool schema4to7 = schema == "4" || schema == "5" || schema == "6" || schema == "7";

  bool resolution_explicit = false;
  if (schema4to7) {
    const std::string resolution = IniValue(entries, "resolution");
    if (auto parsed = ParseResolution(resolution)) {
      settings.resolution = *parsed;
      resolution_explicit = true;
    }
  } else {
    const std::string old_internal_resolution = IniValue(entries, "internal_resolution");
    const std::string old_resolution = IniValue(entries, "resolution");
    if (EqualsAsciiInsensitive(old_internal_resolution, "480p") ||
        EqualsAsciiInsensitive(old_internal_resolution, "576p")) {
      // The internal-resolution override does not exist in WWE 2K14; use the original 720p.
      settings.resolution = Resolution::k720p;
      resolution_explicit = true;
    } else if (auto parsed = ParseResolution(old_resolution)) {
      settings.resolution = *parsed;
      resolution_explicit = *parsed == Resolution::k1080p || *parsed == Resolution::k1440p ||
                            EqualsAsciiInsensitive(old_internal_resolution, "720p");
    }
  }

  if (auto parsed = ParseDisplay(IniValue(entries, "display"))) settings.display = *parsed;

  const std::string gpu = IniValue(entries, "gpu");
  if (ValidGpuId(gpu)) settings.gpu_id = NormalizeGpuId(gpu);
  else if (IsLegacyNumericGpu(gpu)) settings.gpu_id.clear();

  const std::string game_folder = Trim(IniValue(entries, "game_folder"));
  if (!game_folder.empty() && IsValidUtf8(game_folder)) {
    try {
      settings.game_folder = PathFromUtf8(game_folder);
    } catch (...) {
      // Keep the default game folder when this setting cannot be converted.
    }
  }

  if (schema == "5" || schema == "6" || schema == "7") {
    const std::string auto_start = IniValue(entries, "auto_start");
    settings.auto_start = auto_start == "1" || EqualsAsciiInsensitive(auto_start, "true");
  }
  const std::string auto_backup = IniValue(entries, "auto_backup");
  if (!auto_backup.empty()) {
    settings.auto_backup = auto_backup == "1" || EqualsAsciiInsensitive(auto_backup, "true");
  }
  const std::string check_updates = IniValue(entries, "check_updates");
  if (!check_updates.empty()) {
    settings.check_updates = check_updates == "1" || EqualsAsciiInsensitive(check_updates, "true");
  }
  settings.skipped_update_version = Trim(IniValue(entries, "skipped_update_version"));
  settings.explicit_choice = resolution_explicit;
  return settings;
}

}  // namespace

Settings LoadSettings(const Paths& paths) {
  Settings defaults;
  defaults.game_folder = paths.default_game_folder;
  try {
    return LoadSettingsImpl(paths);
  } catch (...) {
    return defaults;
  }
}

Result SaveSettings(const Paths& paths, const Settings& settings) {
  try {
    const std::string text = BuildIni(paths, settings);
    if (!internal::WriteTextFileAtomic(paths.settings_file, text)) {
      return Result::Fail("The launcher could not save your settings.");
    }
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The launcher could not save your settings.");
  }
}

}  // namespace wwe2k14::launcher
