// Keyboard controls: the keybind_* values in wwe2k14.toml next to the game (the game loads that file at start,
// ReXGlue src/ui/rex_app.cpp + src/core/cvar.cpp). Ported from the previous launcher (launcher/main.cpp
// LoadMappingValues / SaveMappingValues): same 24 rows, same defaults, same file rules - only root-table keybind_*
// lines are read or changed, every other byte of the file is kept, and a key is written only when it was already in
// the file, was changed, or differs from the game's default.

#include "launcher_core.h"

#include "internal/util.h"

#include <array>
#include <string_view>

namespace wwe2k14::launcher {
namespace {

// Keep these 24 rows aligned with the controls shown in the launcher and README. The keys are the release's modern (WWE 2K-style PC) layout,
// 2026-10-02: J strike, L grapple, K whip/pin, Space signature/finisher, 1 reversal, Shift run, U pick up, Ctrl limb
// target. They differ from the game's built-in defaults (ReXGlue mnk_input_driver.cpp: Space/Shift/R/E...), so the
// release ships them in wwe2k14.toml (tools/package-release.sh) and EnsureKeyboardDefaults() adds any missing row.
constexpr std::array<KeyBinding, 24> kBindings{{
    {"A", "keybind_a", "L"},
    {"B", "keybind_b", "K"},
    {"X", "keybind_x", "J"},
    {"Y", "keybind_y", "Space"},
    {"Left trigger (LT)", "keybind_left_trigger", "Shift"},
    {"Right trigger (RT)", "keybind_right_trigger", "1"},
    {"Left bumper (LB)", "keybind_left_shoulder", "U"},
    {"Right bumper (RB)", "keybind_right_shoulder", "Control"},
    {"Left stick up", "keybind_lstick_up", "W"},
    {"Left stick down", "keybind_lstick_down", "S"},
    {"Left stick left", "keybind_lstick_left", "A"},
    {"Left stick right", "keybind_lstick_right", "D"},
    {"Left stick press (L3)", "keybind_lstick_press", "C"},
    {"Right stick up", "keybind_rstick_up", "T"},
    {"Right stick down", "keybind_rstick_down", "G"},
    {"Right stick left", "keybind_rstick_left", "F"},
    {"Right stick right", "keybind_rstick_right", "H"},
    {"Right stick press (R3)", "keybind_rstick_press", "V"},
    {"D-pad up", "keybind_dpad_up", "Up"},
    {"D-pad down", "keybind_dpad_down", "Down"},
    {"D-pad left", "keybind_dpad_left", "Left"},
    {"D-pad right", "keybind_dpad_right", "Right"},
    {"Back", "keybind_back", "Tab"},
    {"Start", "keybind_start", "Escape"},
}};

// Exact spellings accepted by ReXGlue's kKeyNames table (src/ui/keybinds.cpp). NumpadEnter is the same key as Return
// for the game, so both share a canonical name for the duplicate check.
constexpr std::string_view kKeyNames[] = {
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Backtick", "Minus", "Plus", "Comma", "Period", "Semicolon", "Slash", "Backslash", "LBracket", "RBracket",
    "Quote", "Escape", "Return", "Space", "Tab", "Backspace", "Delete", "Insert", "Home", "End", "PageUp",
    "PageDown", "Left", "Right", "Up", "Down", "Shift", "Control", "Alt",
    "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9",
    "NumpadEnter", "NumpadPlus", "NumpadMinus", "NumpadStar", "NumpadSlash", "PrintScreen", "Pause", "CapsLock",
    "NumLock", "ScrollLock", "LMB", "RMB", "MMB",
};

// wwe2k14.toml is UTF-8 and must keep every byte: read and write it as raw bytes (the ini helpers convert UTF-16).
bool ReadBytes(const fs::path& path, std::string& text) {
  std::vector<uint8_t> bytes;
  if (!internal::ReadBinaryFile(path, bytes)) return false;
  text.assign(bytes.begin(), bytes.end());
  return true;
}

bool WriteBytesAtomic(const fs::path& path, const std::string& text) {
  fs::path temporary = path;
  temporary += ".launcher-tmp";
  std::error_code error;
  if (!internal::WriteBinaryFile(temporary, text.data(), text.size())) {
    fs::remove(temporary, error);
    return false;
  }
  fs::rename(temporary, path, error);  // replaces the old file in one step on both systems
  if (error) {
    fs::remove(temporary, error);
    return false;
  }
  return true;
}

std::string_view CanonicalKey(std::string_view name) { return name == "NumpadEnter" ? "Return" : name; }

fs::path ControlsFile(const Paths& paths) {
  // The game reads wwe2k14.toml beside its own executable (Windows: next to the launcher; Linux: Linux-x86_64/).
  const fs::path game_dir = paths.game_exe.empty() ? paths.exe_dir : paths.game_exe.parent_path();
  return game_dir / "wwe2k14.toml";
}

size_t FindBinding(std::string_view cvar) {
  for (size_t i = 0; i < kBindings.size(); ++i) {
    if (cvar == kBindings[i].cvar) return i;
  }
  return kBindings.size();
}

bool IsTomlSpace(char c) { return c == ' ' || c == '\t'; }

size_t SkipBom(std::string_view line) {
  return line.size() >= 3 && uint8_t(line[0]) == 0xEF && uint8_t(line[1]) == 0xBB && uint8_t(line[2]) == 0xBF ? 3
                                                                                                                 : 0;
}

bool IsTomlTableHeader(std::string_view line) {
  size_t cursor = SkipBom(line);
  while (cursor < line.size() && IsTomlSpace(line[cursor])) ++cursor;
  return cursor < line.size() && line[cursor] == '[';
}

struct BindingLine {
  size_t index = kBindings.size();
  size_t value_begin = 0;
  size_t value_end = 0;
  std::string value;
  bool quoted = false;
};

BindingLine ParseBindingLine(std::string_view line) {
  BindingLine result;
  size_t cursor = SkipBom(line);
  while (cursor < line.size() && IsTomlSpace(line[cursor])) ++cursor;
  const size_t key_begin = cursor;
  while (cursor < line.size() &&
         ((line[cursor] >= 'a' && line[cursor] <= 'z') || (line[cursor] >= 'A' && line[cursor] <= 'Z') ||
          (line[cursor] >= '0' && line[cursor] <= '9') || line[cursor] == '_')) {
    ++cursor;
  }
  result.index = FindBinding(line.substr(key_begin, cursor - key_begin));
  if (result.index == kBindings.size()) return result;
  while (cursor < line.size() && IsTomlSpace(line[cursor])) ++cursor;
  if (cursor == line.size() || line[cursor++] != '=') {
    result.index = kBindings.size();
    return result;
  }
  while (cursor < line.size() && IsTomlSpace(line[cursor])) ++cursor;
  result.value_begin = cursor;
  if (cursor < line.size() && (line[cursor] == '"' || line[cursor] == '\'')) {
    const char quote = line[cursor++];
    bool escaped = false;
    while (cursor < line.size()) {
      const char character = line[cursor];
      if (escaped) {
        if (character != quote && character != '\\') {
          result.value.clear();
          return result;
        }
        result.value.push_back(character);
        escaped = false;
      } else if (character == '\\' && quote == '"') {
        escaped = true;
      } else if (character == quote) {
        result.value_end = cursor + 1;
        result.quoted = true;
        return result;
      } else {
        result.value.push_back(character);
      }
      ++cursor;
    }
    result.value.clear();
    return result;
  }
  size_t end = line.find('#', cursor);
  if (end == std::string_view::npos) end = line.size();
  while (end > cursor && IsTomlSpace(line[end - 1])) --end;
  result.value_end = end;
  result.value.assign(line.substr(cursor, end - cursor));
  return result;
}

// Calls visit(line_without_eol, eol) for every line of text; eol is "\n", "\r\n" or "" for the last line.
template <typename Visit>
void ForEachLine(const std::string& text, Visit&& visit) {
  size_t line_begin = 0;
  while (line_begin < text.size()) {
    size_t line_end = text.find('\n', line_begin);
    const bool has_newline = line_end != std::string::npos;
    if (!has_newline) line_end = text.size();
    size_t content_end = line_end;
    if (content_end > line_begin && text[content_end - 1] == '\r') --content_end;
    const std::string_view line(text.data() + line_begin, content_end - line_begin);
    const std::string_view eol(text.data() + content_end, (has_newline ? line_end + 1 : line_end) - content_end);
    visit(line, eol);
    line_begin = has_newline ? line_end + 1 : text.size();
  }
}

}  // namespace

const std::vector<KeyBinding>& KeyBindings() {
  static const std::vector<KeyBinding> bindings(kBindings.begin(), kBindings.end());
  return bindings;
}

bool IsSupportedKeyName(std::string_view name) {
  for (std::string_view key : kKeyNames) {
    if (key == name) return true;
  }
  return false;
}

KeyboardControls DefaultKeyboardControls() {
  KeyboardControls controls;
  for (const KeyBinding& binding : kBindings) {
    controls.keys.emplace_back(binding.default_key);
    controls.present.push_back(false);
    controls.modified.push_back(false);
  }
  return controls;
}

KeyboardControls LoadKeyboardControls(const Paths& paths) {
  KeyboardControls controls = DefaultKeyboardControls();
  try {
    std::string text;
    const fs::path file = ControlsFile(paths);
    std::error_code error;
    if (!fs::is_regular_file(file, error) || !ReadBytes(file, text)) return controls;
    bool root_table = true;
    ForEachLine(text, [&](std::string_view line, std::string_view) {
      if (IsTomlTableHeader(line)) root_table = false;
      if (!root_table) return;
      const BindingLine parsed = ParseBindingLine(line);
      if (parsed.index < kBindings.size() && parsed.quoted) {
        controls.keys[parsed.index] = parsed.value;
        controls.present[parsed.index] = true;
      }
    });
  } catch (...) {
    return DefaultKeyboardControls();
  }
  return controls;
}

std::vector<bool> DuplicateKeyBindings(const KeyboardControls& controls) {
  std::vector<bool> duplicate(controls.keys.size(), false);
  for (size_t i = 0; i < controls.keys.size(); ++i) {
    if (controls.keys[i].empty() || !IsSupportedKeyName(controls.keys[i])) continue;
    for (size_t j = i + 1; j < controls.keys.size(); ++j) {
      if (CanonicalKey(controls.keys[i]) == CanonicalKey(controls.keys[j])) duplicate[i] = duplicate[j] = true;
    }
  }
  return duplicate;
}

Result EnsureKeyboardDefaults(const Paths& paths) {
  KeyboardControls controls = LoadKeyboardControls(paths);
  bool missing = false;
  for (bool present : controls.present) missing = missing || !present;
  if (!missing) return Result::Ok();
  // Rows already in the file keep the player's keys (even if they now clash); only missing rows are added.
  for (bool duplicate : DuplicateKeyBindings(controls)) {
    if (duplicate) return Result::Ok();
  }
  return SaveKeyboardControls(paths, controls);
}

Result SaveKeyboardControls(const Paths& paths, const KeyboardControls& controls) {
  try {
    if (controls.keys.size() != kBindings.size() || controls.present.size() != kBindings.size() ||
        controls.modified.size() != kBindings.size()) {
      return Result::Fail("The keyboard controls could not be saved.");
    }
    for (size_t i = 0; i < kBindings.size(); ++i) {
      if (controls.modified[i] && !controls.keys[i].empty() && !IsSupportedKeyName(controls.keys[i])) {
        return Result::Fail("A key is not supported by the game, so nothing was saved.");
      }
    }
    for (bool duplicate : DuplicateKeyBindings(controls)) {
      if (duplicate) return Result::Fail("Two buttons use the same key. Change one of them first.");
    }
    const fs::path file = ControlsFile(paths);
    std::error_code error;
    std::string original;
    if (fs::exists(file, error) && !ReadBytes(file, original)) {
      return Result::Fail("The keyboard settings file could not be read, so nothing was saved.");
    }

    std::vector<bool> found(kBindings.size(), false);
    std::string merged;
    merged.reserve(original.size() + 512);
    size_t insertion_position = std::string::npos;
    bool root_table = true;
    ForEachLine(original, [&](std::string_view line, std::string_view eol) {
      if (root_table && IsTomlTableHeader(line)) {
        root_table = false;
        insertion_position = merged.size();
      }
      const BindingLine parsed = root_table ? ParseBindingLine(line) : BindingLine{};
      if (parsed.index < kBindings.size()) {
        found[parsed.index] = true;
        if (controls.modified[parsed.index] && parsed.value_end >= parsed.value_begin) {
          merged.append(line.substr(0, parsed.value_begin));
          merged.append("\"").append(controls.keys[parsed.index]).append("\"");
          merged.append(line.substr(parsed.value_end));
        } else {
          merged.append(line);
        }
      } else {
        merged.append(line);
      }
      merged.append(eol);
    });

    const size_t first_newline = original.find('\n');
    const std::string newline =
        first_newline != std::string::npos && first_newline > 0 && original[first_newline - 1] == '\r' ? "\r\n"
                                                                                                         : "\n";
    if (insertion_position == std::string::npos) insertion_position = merged.size();
    std::string additions;
    for (size_t i = 0; i < kBindings.size(); ++i) {
      // Every row the file lacks is written: the game's built-in keys are not the launcher's defaults.
      if (found[i]) continue;
      if (additions.empty() && insertion_position > 0 && merged[insertion_position - 1] != '\n') additions += newline;
      additions.append(kBindings[i].cvar).append(" = \"").append(controls.keys[i]).append("\"").append(newline);
    }
    merged.insert(insertion_position, additions);
    if (merged == original) return Result::Ok();
    if (!WriteBytesAtomic(file, merged)) {
      return Result::Fail("The keyboard controls could not be saved. Is the game folder read-only?");
    }
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The keyboard controls could not be saved.");
  }
}

}  // namespace wwe2k14::launcher
