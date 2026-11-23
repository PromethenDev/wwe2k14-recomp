// G1: host test for saving menu changes into the launcher's settings file (launcher_ini_update.h).
// Build: g++ -std=c++20 -I src src/native/tests/launcher_ini_update_test.cpp -o /tmp/launcher_ini_update_test
// Optional argument: a real launcher-written wwe2k14-enhanced.ini (copied to /tmp first; the original is not touched).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "launcher_ini_update.h"

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
  std::printf("check=%s result=%s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) ++failures;
}
namespace li = wwe2k14::launcher_ini;
std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void WriteAll(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
// UTF-8 -> UTF-16LE with BOM, as the launcher's Utf8ToUtf16LeBytes (BMP + surrogate pairs).
std::vector<uint8_t> ToUtf16(const std::string& s) {
  std::vector<uint8_t> out = {0xFF, 0xFE};
  auto put = [&](uint32_t u) { out.push_back(uint8_t(u & 0xFF)); out.push_back(uint8_t(u >> 8)); };
  for (size_t i = 0; i < s.size();) {
    uint32_t c = uint8_t(s[i]);
    int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (n > 1) c &= (0xFF >> (n + 1));
    for (int k = 1; k < n; ++k) c = (c << 6) | (uint8_t(s[i + k]) & 0x3F);
    i += n;
    if (c > 0xFFFF) { c -= 0x10000; put(0xD800 + (c >> 10)); put(0xDC00 + (c & 0x3FF)); } else put(c);
  }
  return out;
}
// Launcher reader semantics (settings.cpp ParseIni + IniValue): last [Settings] occurrence wins, case-insensitive.
std::string LauncherValue(const std::string& text, const std::string& key) {
  std::string section, value;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    while (!line.empty() && line.front() == ' ') line.erase(0, 1);
    if (!line.empty() && line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      for (auto& c : section) c = char(std::tolower(uint8_t(c)));
    } else if (section == "settings") {
      size_t eq = line.find('=');
      if (eq != std::string::npos) {
        std::string k = line.substr(0, eq);
        for (auto& c : k) c = char(std::tolower(uint8_t(c)));
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (k == key) value = line.substr(eq + 1);
      }
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  return value;
}
}  // namespace

int main(int argc, char** argv) {
  const auto dir = std::filesystem::temp_directory_path() / "g1-ini-test";
  std::filesystem::create_directories(dir);
  const std::string base =
      "[Settings]\r\nschema=7\r\nrenderer=native\r\ngame_folder=C:\\Spiele\\WWE 2K14 \xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80\r\n"
      "sync_frames_to_monitor=1\r\nstretch_to_fill_wide_screens=0\r\nLower_CPU = 0\r\ncrowd=1\r\nshadows=1\r\n"
      "music_track_abc=1|2\r\n; comment crowd=5\r\n[Other]\r\ncrowd=9\r\n";
  const auto f = dir / "wwe2k14-enhanced.ini";
  WriteAll(f, ToUtf16(base));
  const li::KeyValues kv = {{"crowd", "0"}, {"lower_cpu", "1"}, {"shadows", "0"}};
  auto r = li::UpdateFile(f, kv);
  const auto expect = ToUtf16(
      "[Settings]\r\nschema=7\r\nrenderer=native\r\ngame_folder=C:\\Spiele\\WWE 2K14 \xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80\r\n"
      "sync_frames_to_monitor=1\r\nstretch_to_fill_wide_screens=0\r\nlower_cpu=1\r\ncrowd=0\r\nshadows=0\r\n"
      "music_track_abc=1|2\r\n; comment crowd=5\r\n[Other]\r\ncrowd=9\r\n");
  Check(r.written && ReadAll(f) == expect, "utf16_bom_crlf_only_keys_change_other_section_and_unicode_kept");
  Check(!std::filesystem::exists(dir / ("wwe2k14-enhanced.ini.tmp.game." + std::to_string(getpid()))), "no_temp_left");
  r = li::UpdateFile(f, kv);
  Check(!r.written && r.message == "unchanged", "second_save_is_a_no_op");

  // Missing keys are appended at the end of [Settings], before the next section; UTF-8 without BOM stays so.
  const std::string u8 = "[settings]\nschema=7\ncrowd=1\n[Music]\nx=1\n";
  WriteAll(f, std::vector<uint8_t>(u8.begin(), u8.end()));
  r = li::UpdateFile(f, {{"crowd", "0"}, {"lower_cpu", "1"}});
  const std::string want = "[settings]\nschema=7\ncrowd=0\nlower_cpu=1\n[Music]\nx=1\n";
  Check(r.written && ReadAll(f) == std::vector<uint8_t>(want.begin(), want.end()), "utf8_lf_append_inside_section");

  // Section at end of file without a final newline.
  const std::string tail = "[Settings]\r\ncrowd=1";
  WriteAll(f, std::vector<uint8_t>(tail.begin(), tail.end()));
  r = li::UpdateFile(f, {{"shadows", "0"}});
  const std::string want2 = "[Settings]\r\ncrowd=1\r\nshadows=0\r\n";
  Check(r.written && ReadAll(f) == std::vector<uint8_t>(want2.begin(), want2.end()), "append_after_last_line_without_newline");

  // Duplicates: every occurrence replaced (the launcher reads the last).
  const std::string dup = "[Settings]\r\ncrowd=1\r\ncrowd=1\r\n";
  WriteAll(f, std::vector<uint8_t>(dup.begin(), dup.end()));
  r = li::UpdateFile(f, {{"crowd", "0"}});
  Check(r.written && LauncherValue(std::string(ReadAll(f).begin(), ReadAll(f).end()), "crowd") == "0", "duplicates_replaced");

  // No [Settings] section / no file / big-endian: nothing written.
  const std::string none = "[Music]\r\ncrowd=1\r\n";
  WriteAll(f, std::vector<uint8_t>(none.begin(), none.end()));
  r = li::UpdateFile(f, {{"crowd", "0"}});
  Check(!r.written && ReadAll(f) == std::vector<uint8_t>(none.begin(), none.end()), "no_settings_section_untouched");
  r = li::UpdateFile(dir / "missing.ini", {{"crowd", "0"}});
  Check(!r.written, "missing_file_not_created");
  WriteAll(f, {0xFE, 0xFF, 0x00, 0x5B});
  r = li::UpdateFile(f, {{"crowd", "0"}});
  Check(!r.written, "utf16_be_refused");

  // A real launcher-written file: only the given key lines change, the launcher reads the new values.
  if (argc > 1) {
    const auto real = dir / "real.ini";
    std::filesystem::copy_file(argv[1], real, std::filesystem::copy_options::overwrite_existing);
    const auto before = ReadAll(real);
    li::Encoding enc;
    std::string a8;
    std::u16string a16;
    li::Decode(before, &enc, &a8, &a16);
    r = li::UpdateFile(real, {{"crowd", "0"}, {"lower_cpu", "1"}, {"stretch_to_fill_wide_screens", "1"}});
    const auto after = ReadAll(real);
    std::string b8;
    std::u16string b16;
    li::Decode(after, &enc, &b8, &b16);
    // compare line by line (as UTF-16 code units -> crude ASCII view for the key check)
    auto lines = [](const std::u16string& t) {
      std::vector<std::u16string> v;
      size_t p = 0;
      while (p <= t.size()) { size_t n = t.find(u'\n', p); v.push_back(t.substr(p, n == std::u16string::npos ? std::u16string::npos : n - p)); if (n == std::u16string::npos) break; p = n + 1; }
      return v;
    };
    const auto la = lines(a16), lb = lines(b16);
    int changed = 0;
    for (size_t i = 0; i < la.size() && i < lb.size(); ++i) changed += la[i] != lb[i];
    std::string ascii;
    for (char16_t c : b16) ascii.push_back(c < 0x80 ? char(c) : '?');
    std::printf("real file: %s; %d line(s) differ, %zu -> %zu lines\n", r.message.c_str(), changed, la.size(), lb.size());
    Check(r.written && after.size() >= 2 && after[0] == 0xFF && after[1] == 0xFE, "real_file_stays_utf16_bom");
    Check(LauncherValue(ascii, "crowd") == "0" && LauncherValue(ascii, "lower_cpu") == "1" &&
              LauncherValue(ascii, "stretch_to_fill_wide_screens") == "1",
          "real_file_launcher_reads_new_values");
    Check(changed <= 3 && lb.size() >= la.size() && lb.size() - la.size() <= 3, "real_file_only_key_lines_differ");
  }
  std::filesystem::remove_all(dir);
  std::printf("launcher_ini_update_test %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
