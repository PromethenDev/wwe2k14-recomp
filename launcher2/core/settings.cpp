#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <iomanip>
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

std::string HexEncode(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (unsigned char byte : value) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 15]);
  }
  return result;
}

std::optional<std::string> HexDecode(std::string_view value) {
  if (value.size() % 2 != 0) return std::nullopt;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string result;
  result.reserve(value.size() / 2);
  for (size_t i = 0; i < value.size(); i += 2) {
    const int high = nibble(value[i]);
    const int low = nibble(value[i + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    result.push_back(static_cast<char>((high << 4) | low));
  }
  return result;
}

float ParseFiniteFloat(std::string_view value, float fallback) {
  const std::string text(value);
  if (text.empty()) return fallback;  // missing key (settings from an older launcher): strtof("") parses as 0
  char* end = nullptr;
  const float parsed = std::strtof(text.c_str(), &end);
  return end == text.c_str() + text.size() && std::isfinite(parsed) ? parsed : fallback;
}

std::vector<std::string_view> SplitComma(std::string_view value) {
  std::vector<std::string_view> parts;
  size_t offset = 0;
  for (;;) {
    const size_t comma = value.find(',', offset);
    parts.push_back(value.substr(offset, comma == std::string_view::npos ? value.size() - offset
                                                                          : comma - offset));
    if (comma == std::string_view::npos) break;
    offset = comma + 1;
  }
  return parts;
}

MusicTrackSettings ParseMusicTrackSettings(const std::vector<std::string_view>& fields, size_t first) {
  MusicTrackSettings value;
  if (fields.size() < first + 11) return value;
  value.volume_db = std::clamp(ParseFiniteFloat(fields[first], 0.0f), -24.0f, 12.0f);
  value.normalize = fields[first + 1] == "1";
  value.target_rms_dbfs = std::clamp(ParseFiniteFloat(fields[first + 2], -18.0f), -36.0f, -6.0f);
  value.trim_start_seconds = std::clamp(ParseFiniteFloat(fields[first + 3], 0.0f), 0.0f, 86400.0f);
  value.trim_end_seconds = std::clamp(ParseFiniteFloat(fields[first + 4], 0.0f), 0.0f, 86400.0f);
  value.fade_in_seconds = std::clamp(ParseFiniteFloat(fields[first + 5], 0.25f), 0.0f, 10.0f);
  value.fade_out_seconds = std::clamp(ParseFiniteFloat(fields[first + 6], 1.0f), 0.0f, 10.0f);
  value.bass_gain_db = std::clamp(ParseFiniteFloat(fields[first + 7], 0.0f), -12.0f, 12.0f);
  value.bass_frequency_hz = std::clamp(ParseFiniteFloat(fields[first + 8], 120.0f), 20.0f, 1000.0f);
  value.treble_gain_db = std::clamp(ParseFiniteFloat(fields[first + 9], 0.0f), -12.0f, 12.0f);
  value.treble_frequency_hz = std::clamp(ParseFiniteFloat(fields[first + 10], 8000.0f), 1000.0f, 18000.0f);
  return value;
}

bool EqualsAsciiInsensitive(std::string_view left, std::string_view right) {
  return LowerAscii(left) == LowerAscii(right);
}

NativeApi ParseNativeApi(std::string_view value) {
  if (EqualsAsciiInsensitive(value, "d3d12")) return NativeApi::kD3D12;
  if (EqualsAsciiInsensitive(value, "d3d11")) return NativeApi::kD3D11;
  return NativeApi::kVulkan;
}

Renderer ParseRenderer(std::string_view value) {
  return EqualsAsciiInsensitive(value, "xbox") ? Renderer::kXbox : Renderer::kNative;
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
  if (EqualsAsciiInsensitive(value, "borderless")) return DisplayMode::kBorderless;
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
  if (value == DisplayMode::kBorderless) return "borderless";
  return value == DisplayMode::kWindowed ? "windowed" : "fullscreen";
}

std::string RendererName(Renderer value) {
  return value == Renderer::kXbox ? "xbox" : "native";
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
                                          "auto_backup", "check_updates", "skipped_update_version",
                                          "sync_frames_to_monitor", "stretch_to_fill_wide_screens",
                                          "sharper_picture", "picture_scale", "native_picture_size",
                                          "native_frame_rate", "native_anti_aliasing", "xbox_frame_rate",
                                          "texture_filtering", "performance_mode", "multi_shadows", "hiac_weapon_wheel",
                                          "weapon_wheel_more", "head_edit", "unlock_all", "imports_in_grid", "mods_enabled",
                                          "xbox_anti_aliasing", "renderer", "native_api", "language", "low_power", "lower_cpu", "fast_loading", "controller_vibration", "keybinds_while_typing", "intel_lake_shaders", "crowd", "shadows", "textures",
                                          "glitch_capture_mode", "recommendation_fields_version", "manual_resolution",
                                          "manual_picture_size", "manual_frame_rate", "manual_xbox_frame_rate",
                                          "music_target_rms_dbfs", "music_fade_in_seconds", "music_fade_out_seconds",
                                          "music_bass_gain_db", "music_bass_frequency_hz", "music_treble_gain_db",
                                          "music_treble_frequency_hz"};
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
      if (equals != std::string::npos) {
        const std::string key = LowerAscii(Trim(std::string_view(line).substr(0, equals)));
        if (replaced.contains(key) || key.starts_with("music_track_")) continue;
      }
      unknown_settings.push_back(raw_line);
    } else {
      outside_settings.push_back(raw_line);
    }
  }

  const bool manual_resolution = settings.recommendation_fields_tracked ? settings.manual_resolution
                                                                         : settings.explicit_choice;
  const bool manual_picture_size = settings.recommendation_fields_tracked ? settings.manual_picture_size
                                                                           : settings.explicit_choice;
  const bool manual_frame_rate = settings.recommendation_fields_tracked ? settings.manual_frame_rate
                                                                         : settings.explicit_choice;
  const bool manual_xbox_frame_rate = settings.recommendation_fields_tracked ? settings.manual_xbox_frame_rate
                                                                              : settings.explicit_choice;
  std::ostringstream output;
  output << "[Settings]\r\nschema=7\r\n"
         << "renderer=" << RendererName(settings.renderer) << "\r\n"
         << "native_api=" << NativeApiName(settings.native_api) << "\r\n"
         << "resolution=" << (manual_resolution ? ResolutionName(settings.resolution) : "auto") << "\r\n"
         << "display=" << DisplayName(settings.display) << "\r\n"
         << "sync_frames_to_monitor=" << (settings.sync_frames_to_monitor ? "1" : "0") << "\r\n"
         << "stretch_to_fill_wide_screens=" << (settings.stretch_to_fill_wide_screens ? "1" : "0") << "\r\n"
         << "picture_scale=" << std::clamp(settings.picture_scale, 1, 3) << "\r\n"
         << "native_picture_size=" << static_cast<int>(settings.picture_size) << "\r\n"
         << "native_frame_rate=" << static_cast<int>(settings.frame_rate) << "\r\n"
         << "native_anti_aliasing=" << (settings.anti_aliasing ? "1" : "0") << "\r\n"
         << "xbox_frame_rate=" << static_cast<int>(settings.xbox_frame_rate) << "\r\n"
         << "xbox_anti_aliasing=" << (settings.xbox_anti_aliasing ? "1" : "0") << "\r\n"
         << "lower_cpu=" << (settings.low_power ? "1" : "0") << "\r\n"
         << "fast_loading=" << (settings.fast_loading ? "1" : "0") << "\r\n"
         << "controller_vibration=" << (settings.controller_vibration ? "1" : "0") << "\r\n"
         << "keybinds_while_typing=" << (settings.keybinds_while_typing ? "1" : "0") << "\r\n"
         << "intel_lake_shaders=" << (settings.intel_lake_shaders ? "1" : "0") << "\r\n"
         << "texture_filtering=" << (settings.texture_filtering == 0 ? "0" : "1") << "\r\n"
         << "performance_mode=" << (settings.performance_mode ? "1" : "0") << "\r\n"
         << "multi_shadows=" << settings.multi_shadows << "\r\n"
         << "hiac_weapon_wheel=" << (settings.hiac_weapon_wheel ? "1" : "0") << "\r\n"
         << "weapon_wheel_more=" << (settings.weapon_wheel_more ? "1" : "0") << "\r\n"
         << "head_edit=" << (settings.head_edit ? "1" : "0") << "\r\n"
         << "unlock_all=" << (settings.unlock_all ? "1" : "0") << "\r\n"
         << "imports_in_grid=" << (settings.imports_in_grid ? "1" : "0") << "\r\n"
         << "mods_enabled=" << (settings.mods_enabled ? "1" : "0") << "\r\n"
         << "crowd=" << ((settings.crowd >= 0 && settings.crowd <= 2) ? settings.crowd : 1) << "\r\n"
         << "shadows=" << (settings.shadows == 0 ? 0 : 1) << "\r\n"
         << "textures=" << (settings.textures == 0 ? 0 : 1) << "\r\n"
         << "glitch_capture_mode=" << (settings.glitch_capture_mode ? "1" : "0") << "\r\n"
         << "language=" << (IsOfferedLanguage(settings.language) ? settings.language : 1) << "\r\n"
         << "gpu=" << (settings.gpu_id.empty() ? "auto" : settings.gpu_id) << "\r\n"
         << "recommendation_fields_version=1\r\n"
         << "manual_resolution=" << (manual_resolution ? "1" : "0") << "\r\n"
         << "manual_picture_size=" << (manual_picture_size ? "1" : "0") << "\r\n"
         << "manual_frame_rate=" << (manual_frame_rate ? "1" : "0") << "\r\n"
         << "manual_xbox_frame_rate=" << (manual_xbox_frame_rate ? "1" : "0") << "\r\n"
         << "game_folder=" << PathToUtf8(settings.game_folder.empty() ? paths.default_game_folder : settings.game_folder)
         << "\r\nauto_start=" << (settings.auto_start ? "1" : "0")
         << "\r\nauto_backup=" << (settings.auto_backup ? "1" : "0")
         << "\r\ncheck_updates=" << (settings.check_updates ? "1" : "0")
         << "\r\nskipped_update_version=" << settings.skipped_update_version << "\r\n"
         << std::setprecision(9)
         << "music_target_rms_dbfs=" << std::clamp(settings.music_preset.target_rms_dbfs, -36.0f, -6.0f) << "\r\n"
         << "music_fade_in_seconds=" << std::clamp(settings.music_preset.fade_in_seconds, 0.0f, 10.0f) << "\r\n"
         << "music_fade_out_seconds=" << std::clamp(settings.music_preset.fade_out_seconds, 0.0f, 10.0f) << "\r\n"
         << "music_bass_gain_db=" << std::clamp(settings.music_preset.bass_gain_db, -12.0f, 12.0f) << "\r\n"
         << "music_bass_frequency_hz=" << std::clamp(settings.music_preset.bass_frequency_hz, 20.0f, 1000.0f) << "\r\n"
         << "music_treble_gain_db=" << std::clamp(settings.music_preset.treble_gain_db, -12.0f, 12.0f) << "\r\n"
         << "music_treble_frequency_hz=" << std::clamp(settings.music_preset.treble_frequency_hz, 1000.0f, 18000.0f) << "\r\n";
  for (const auto& [filename, record] : settings.music_tracks) {
    const std::string filename_hex = HexEncode(filename);
    const std::string source_hex = HexEncode(record.source_relative_path);
    const MusicTrackSettings& edit = record.settings;
    output << "music_track_" << filename_hex << '=' << filename_hex << ',' << source_hex << ','
           << std::clamp(edit.volume_db, -24.0f, 12.0f) << ',' << (edit.normalize ? 1 : 0) << ','
           << std::clamp(edit.target_rms_dbfs, -36.0f, -6.0f) << ','
           << std::clamp(edit.trim_start_seconds, 0.0f, 86400.0f) << ','
           << std::clamp(edit.trim_end_seconds, 0.0f, 86400.0f) << ','
           << std::clamp(edit.fade_in_seconds, 0.0f, 10.0f) << ','
           << std::clamp(edit.fade_out_seconds, 0.0f, 10.0f) << ','
           << std::clamp(edit.bass_gain_db, -12.0f, 12.0f) << ','
           << std::clamp(edit.bass_frequency_hz, 20.0f, 1000.0f) << ','
           << std::clamp(edit.treble_gain_db, -12.0f, 12.0f) << ','
           << std::clamp(edit.treble_frequency_hz, 1000.0f, 18000.0f) << "\r\n";
  }
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
  settings.renderer = ParseRenderer(IniValue(entries, "renderer"));
  settings.native_api = ParseNativeApi(IniValue(entries, "native_api"));

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

  const std::string sync_frames = IniValue(entries, "sync_frames_to_monitor");
  if (!sync_frames.empty()) {
    settings.sync_frames_to_monitor = sync_frames == "1" || EqualsAsciiInsensitive(sync_frames, "true");
  }
  const std::string stretch = IniValue(entries, "stretch_to_fill_wide_screens");
  if (!stretch.empty()) {
    settings.stretch_to_fill_wide_screens = stretch == "1" || EqualsAsciiInsensitive(stretch, "true");
  }
  // picture_scale replaced the 1.0.1 test builds' sharper_picture (on = 2x).
  const std::string scale = IniValue(entries, "picture_scale");
  const std::string sharper = IniValue(entries, "sharper_picture");
  if (scale == "1" || scale == "2" || scale == "3") {
    settings.picture_scale = scale[0] - '0';
  } else if (sharper == "1" || EqualsAsciiInsensitive(sharper, "true")) {
    settings.picture_scale = 2;
  }

  const std::string native_size = IniValue(entries, "native_picture_size");
  if (native_size.size() == 1 && native_size[0] >= '0' && native_size[0] <= '6') {
    settings.picture_size = static_cast<PictureSize>(native_size[0] - '0');
  }
  const std::string native_rate = IniValue(entries, "native_frame_rate");
  if (native_rate.size() == 1 && native_rate[0] >= '0' && native_rate[0] <= '2') {
    settings.frame_rate = static_cast<FrameRate>(native_rate[0] - '0');
  }
  const std::string native_aa = IniValue(entries, "native_anti_aliasing");
  if (!native_aa.empty()) settings.anti_aliasing = native_aa == "1" || EqualsAsciiInsensitive(native_aa, "true");
  const std::string xbox_rate = IniValue(entries, "xbox_frame_rate");
  if (xbox_rate.size() == 1 && xbox_rate[0] >= '0' && xbox_rate[0] <= '2') {
    settings.xbox_frame_rate = static_cast<FrameRate>(xbox_rate[0] - '0');
  }
  const std::string xbox_aa = IniValue(entries, "xbox_anti_aliasing");
  if (!xbox_aa.empty()) settings.xbox_anti_aliasing = xbox_aa == "1" || EqualsAsciiInsensitive(xbox_aa, "true");
  // Only the v1.4 key: an old low_power=1 was the earlier default, not a player's choice (see launcher_core.h).
  const std::string low_power = IniValue(entries, "lower_cpu");
  if (!low_power.empty()) settings.low_power = low_power == "1" || EqualsAsciiInsensitive(low_power, "true");
  const std::string fast_loading = IniValue(entries, "fast_loading");
  if (!fast_loading.empty()) settings.fast_loading = fast_loading == "1" || EqualsAsciiInsensitive(fast_loading, "true");
  const std::string vibration = Trim(IniValue(entries, "controller_vibration"));
  if (vibration == "0" || EqualsAsciiInsensitive(vibration, "false")) settings.controller_vibration = false;
  else if (vibration == "1" || EqualsAsciiInsensitive(vibration, "true")) settings.controller_vibration = true;
  const std::string typing_binds = Trim(IniValue(entries, "keybinds_while_typing"));
  settings.keybinds_while_typing = typing_binds == "1" || EqualsAsciiInsensitive(typing_binds, "true");
  const std::string intel_lake_shaders = IniValue(entries, "intel_lake_shaders");
  if (!intel_lake_shaders.empty()) settings.intel_lake_shaders = intel_lake_shaders == "1" || EqualsAsciiInsensitive(intel_lake_shaders, "true");
  const std::string texture_filtering = Trim(IniValue(entries, "texture_filtering"));
  if (texture_filtering == "0" || texture_filtering == "1") settings.texture_filtering = texture_filtering[0] - '0';
  const std::string multi_shadows = Trim(IniValue(entries, "multi_shadows"));
  if (multi_shadows == "0" || multi_shadows == "1") settings.multi_shadows = multi_shadows[0] - '0';
  settings.hiac_weapon_wheel = Trim(IniValue(entries, "hiac_weapon_wheel")) == "1";
  settings.weapon_wheel_more = Trim(IniValue(entries, "weapon_wheel_more")) == "1";
  settings.head_edit = Trim(IniValue(entries, "head_edit")) == "1";
  settings.unlock_all = Trim(IniValue(entries, "unlock_all")) == "1";
  settings.imports_in_grid = Trim(IniValue(entries, "imports_in_grid")) != "0";  // on unless the player turned it off
  settings.mods_enabled = Trim(IniValue(entries, "mods_enabled")) != "0";  // owner 11-18: on unless switched off
  const std::string performance_mode = Trim(IniValue(entries, "performance_mode"));
  if (!performance_mode.empty()) settings.performance_mode = performance_mode == "1" || EqualsAsciiInsensitive(performance_mode, "true");
  const std::string crowd = Trim(IniValue(entries, "crowd"));
  if (crowd == "0" || crowd == "1") settings.crowd = crowd[0] - '0';  // Full or None (Half not offered)
  const std::string shadows = Trim(IniValue(entries, "shadows"));
  if (shadows == "0" || shadows == "1") settings.shadows = shadows[0] - '0';
  const std::string textures = Trim(IniValue(entries, "textures"));
  if (textures == "0" || textures == "1") settings.textures = textures[0] - '0';
  const std::string glitch_capture_mode = Trim(IniValue(entries, "glitch_capture_mode"));
  if (!glitch_capture_mode.empty()) {
    settings.glitch_capture_mode = glitch_capture_mode == "1" || EqualsAsciiInsensitive(glitch_capture_mode, "true");
  }
  const std::string language = Trim(IniValue(entries, "language"));
  if (language.size() == 1 && language[0] >= '1' && language[0] <= '9' && IsOfferedLanguage(language[0] - '0')) {
    settings.language = language[0] - '0';
  }

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
  if (IniValue(entries, "recommendation_fields_version") == "1") {
    auto enabled = [&](std::string_view key) {
      const std::string value = Trim(IniValue(entries, key));
      return value == "1" || EqualsAsciiInsensitive(value, "true");
    };
    settings.recommendation_fields_tracked = true;
    settings.manual_resolution = enabled("manual_resolution");
    settings.manual_picture_size = enabled("manual_picture_size");
    settings.manual_frame_rate = enabled("manual_frame_rate");
    settings.manual_xbox_frame_rate = enabled("manual_xbox_frame_rate");
    settings.explicit_choice = settings.manual_resolution || settings.manual_picture_size || settings.manual_frame_rate ||
                               settings.manual_xbox_frame_rate;
  } else {
    // Before F3 the launcher stored one global explicit bit (as a numeric resolution). Preserve all recommendation-
    // controlled values as the player's manual choices, then upgrade the INI on its next save.
    settings.explicit_choice = resolution_explicit;
    if (resolution_explicit) {
      settings.recommendation_fields_tracked = true;
      settings.manual_resolution = true;
      settings.manual_picture_size = true;
      settings.manual_frame_rate = true;
      settings.manual_xbox_frame_rate = true;
    }
  }

  settings.music_preset.target_rms_dbfs = std::clamp(
      ParseFiniteFloat(IniValue(entries, "music_target_rms_dbfs"), -18.0f), -36.0f, -6.0f);
  settings.music_preset.fade_in_seconds = std::clamp(
      ParseFiniteFloat(IniValue(entries, "music_fade_in_seconds"), 0.25f), 0.0f, 10.0f);
  settings.music_preset.fade_out_seconds = std::clamp(
      ParseFiniteFloat(IniValue(entries, "music_fade_out_seconds"), 1.0f), 0.0f, 10.0f);
  settings.music_preset.bass_gain_db = std::clamp(
      ParseFiniteFloat(IniValue(entries, "music_bass_gain_db"), 0.0f), -12.0f, 12.0f);
  settings.music_preset.bass_frequency_hz = std::clamp(
      ParseFiniteFloat(IniValue(entries, "music_bass_frequency_hz"), 120.0f), 20.0f, 1000.0f);
  settings.music_preset.treble_gain_db = std::clamp(
      ParseFiniteFloat(IniValue(entries, "music_treble_gain_db"), 0.0f), -12.0f, 12.0f);
  settings.music_preset.treble_frequency_hz = std::clamp(
      ParseFiniteFloat(IniValue(entries, "music_treble_frequency_hz"), 8000.0f), 1000.0f, 18000.0f);
  // 1.4.9 test launchers read missing music keys as 0 and saved this clamped all-zero preset (owner laptop 11-02).
  // The two frequencies are not editable in the UI, so this exact set can only come from that bug.
  if (settings.music_preset.target_rms_dbfs == -6.0f && settings.music_preset.fade_in_seconds == 0.0f &&
      settings.music_preset.fade_out_seconds == 0.0f && settings.music_preset.bass_gain_db == 0.0f &&
      settings.music_preset.bass_frequency_hz == 20.0f && settings.music_preset.treble_gain_db == 0.0f &&
      settings.music_preset.treble_frequency_hz == 1000.0f) {
    settings.music_preset = MusicPreset{};
  }
  for (const IniEntry& entry : entries) {
    if (entry.section != "settings" || !entry.key.starts_with("music_track_")) continue;
    const auto fields = SplitComma(entry.value);
    if (fields.size() < 13) continue;
    const auto filename = HexDecode(fields[0]);
    const auto source = HexDecode(fields[1]);
    if (!filename || !source || filename->empty() || source->empty() ||
        !IsValidUtf8(*filename) || !IsValidUtf8(*source)) continue;
    const fs::path filename_path = PathFromUtf8(*filename);
    if (filename_path.filename() != filename_path || filename_path.empty()) continue;
    MusicTrackRecord record;
    record.source_relative_path = *source;
    record.settings = ParseMusicTrackSettings(fields, 2);
    settings.music_tracks.insert_or_assign(*filename, std::move(record));
  }
  return settings;
}

}  // namespace

bool IsOfferedLanguage(int id) {
  for (const GameLanguage& language : kGameLanguages) {
    if (language.id == id) return true;
  }
  return false;
}

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
