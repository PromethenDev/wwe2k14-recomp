// G1: save in-game menu changes into the launcher's settings file (wwe2k14-enhanced.ini), so the next start (and the
// launcher's own pages) use them. Host-tested in native/tests/launcher_ini_update_test.cpp.
//
// The launcher (out/launcher-native, core/settings.cpp) writes the file as UTF-16LE with a BOM and CRLF lines through a
// temp file + rename, reads UTF-16 (BOM) or UTF-8, keys are case-insensitive inside [Settings], the last occurrence
// wins, and it keeps unknown lines. This writer changes only the lines of the given keys inside [Settings] (all
// occurrences), appends missing keys at the end of that section, keeps every other code unit, the encoding, BOM and
// line endings exactly, and replaces the file atomically (temp file in the same folder, then rename). No [Settings]
// section, or an unreadable file: nothing is written.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace wwe2k14::launcher_ini {

using KeyValues = std::vector<std::pair<std::string, std::string>>;  // ASCII keys and values

namespace detail {
template <class C>
bool IsSpace(C c) { return c == C(' ') || c == C('\t') || c == C('\r'); }
template <class C>
char LowerAscii(C c) {
  const uint32_t u = sizeof(C) == 1 ? uint32_t(static_cast<unsigned char>(c)) : uint32_t(c);
  if (u >= 'A' && u <= 'Z') return char(u - 'A' + 'a');
  return u < 0x80 ? char(u) : '\x01';  // non-ASCII never matches an ASCII key
}
template <class S>
size_t TrimBegin(const S& s, size_t b, size_t e) { while (b < e && IsSpace(s[b])) ++b; return b; }
template <class S>
size_t TrimEnd(const S& s, size_t b, size_t e) { while (e > b && IsSpace(s[e - 1])) --e; return e; }
template <class S>
bool EqualsKey(const S& s, size_t b, size_t e, const std::string& key) {
  if (e - b != key.size()) return false;
  for (size_t i = 0; i < key.size(); ++i)
    if (LowerAscii(s[b + i]) != key[i]) return false;
  return true;
}
template <class S>
S Widen(const std::string& a) { S s; for (char c : a) s.push_back(typename S::value_type(static_cast<unsigned char>(c))); return s; }
}  // namespace detail

// Text form: S = std::string (UTF-8 bytes) or std::u16string (UTF-16 code units). Keys must be lower-case ASCII.
template <class S>
S Update(const S& text, const KeyValues& kv, bool* has_settings, int* replaced_lines, int* appended) {
  using namespace detail;
  using C = typename S::value_type;
  *has_settings = false;
  *replaced_lines = 0;
  *appended = 0;
  // Line ending of the file: CRLF if its first line ends so (the launcher always writes CRLF).
  const size_t first_nl = text.find(C('\n'));
  const bool crlf = first_nl != S::npos && first_nl > 0 && text[first_nl - 1] == C('\r');
  const S eol = crlf ? Widen<S>("\r\n") : Widen<S>("\n");
  std::vector<bool> seen(kv.size(), false);
  S out;
  out.reserve(text.size() + 64);
  bool in_settings = false;
  size_t insert_at = S::npos;  // in `out`: just after the last line of [Settings]
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find(C('\n'), pos);
    const size_t line_end = nl == S::npos ? text.size() : nl;          // excludes '\n'
    const size_t next = nl == S::npos ? text.size() : nl + 1;
    const size_t b = TrimBegin(text, pos, line_end), e = TrimEnd(text, pos, line_end);
    bool replaced = false;
    if (b < e && text[b] == C('[') && text[e - 1] == C(']')) {
      const size_t sb = TrimBegin(text, b + 1, e - 1), se = TrimEnd(text, b + 1, e - 1);
      in_settings = EqualsKey(text, sb, se, "settings");
      if (in_settings) *has_settings = true;
    } else if (in_settings && b < e && text[b] != C(';') && text[b] != C('#')) {
      const size_t eq = text.find(C('='), b);
      if (eq != S::npos && eq < e) {
        const size_t kb = b, ke = TrimEnd(text, b, eq);
        for (size_t i = 0; i < kv.size(); ++i) {
          if (!EqualsKey(text, kb, ke, kv[i].first)) continue;
          out += text.substr(pos, b - pos);  // keep indentation
          out += Widen<S>(kv[i].first + "=" + kv[i].second);
          // keep the original line ending: "\r" (if any) before '\n'
          if (line_end > pos && text[line_end - 1] == C('\r')) out.push_back(C('\r'));
          if (nl != S::npos) out.push_back(C('\n'));
          seen[i] = true;
          ++*replaced_lines;
          replaced = true;
          break;
        }
      }
    }
    if (!replaced) out += text.substr(pos, next - pos);
    if (in_settings) insert_at = out.size();
    pos = next;
  }
  if (!*has_settings) return text;
  S add;
  for (size_t i = 0; i < kv.size(); ++i) {
    if (seen[i]) continue;
    add += Widen<S>(kv[i].first + "=" + kv[i].second);
    add += eol;
    ++*appended;
  }
  if (!add.empty()) {
    // the section's last line may lack a newline (end of file)
    if (insert_at > 0 && out[insert_at - 1] != C('\n')) {
      out.insert(insert_at, eol);
      insert_at += eol.size();
    }
    out.insert(insert_at, add);
  }
  return out;
}

enum class Encoding { kUtf8, kUtf8Bom, kUtf16LeBom };

// Bytes -> code units; false when the file is UTF-16 big endian (the launcher never writes it) or odd-sized UTF-16.
inline bool Decode(const std::vector<uint8_t>& bytes, Encoding* enc, std::string* u8, std::u16string* u16) {
  if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
    if (bytes.size() % 2) return false;
    *enc = Encoding::kUtf16LeBom;
    u16->clear();
    for (size_t i = 2; i + 1 < bytes.size(); i += 2) u16->push_back(char16_t(bytes[i] | (bytes[i + 1] << 8)));
    return true;
  }
  if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) return false;
  const bool bom = bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
  *enc = bom ? Encoding::kUtf8Bom : Encoding::kUtf8;
  u8->assign(reinterpret_cast<const char*>(bytes.data()) + (bom ? 3 : 0), bytes.size() - (bom ? 3 : 0));
  return true;
}

struct FileResult {
  bool written = false;
  std::string message;  // for the log
};

// Reads `path`, updates the keys, writes `path`.tmp.<pid> and renames it over `path`.
inline FileResult UpdateFile(const std::filesystem::path& path, const KeyValues& kv) {
  FileResult r;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    r.message = "no settings file";
    return r;
  }
  std::vector<uint8_t> bytes;
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) { r.message = "cannot read"; return r; }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  Encoding enc;
  std::string u8;
  std::u16string u16;
  if (!Decode(bytes, &enc, &u8, &u16)) { r.message = "unsupported encoding"; return r; }
  bool has = false;
  int rep = 0, app = 0;
  std::vector<uint8_t> outb;
  if (enc == Encoding::kUtf16LeBom) {
    const std::u16string t = Update(u16, kv, &has, &rep, &app);
    outb = {0xFF, 0xFE};
    for (char16_t c : t) { outb.push_back(uint8_t(c & 0xFF)); outb.push_back(uint8_t(c >> 8)); }
  } else {
    const std::string t = Update(u8, kv, &has, &rep, &app);
    if (enc == Encoding::kUtf8Bom) outb = {0xEF, 0xBB, 0xBF};
    outb.insert(outb.end(), t.begin(), t.end());
  }
  if (!has) { r.message = "no [Settings] section"; return r; }
  if (outb == bytes) { r.message = "unchanged"; return r; }
#ifdef _WIN32
  const unsigned long pid = static_cast<unsigned long>(_getpid());
#else
  const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
  std::filesystem::path tmp = path;
  tmp += ".tmp.game." + std::to_string(pid);
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) { r.message = "cannot write temp file"; return r; }
    o.write(reinterpret_cast<const char*>(outb.data()), std::streamsize(outb.size()));
    o.flush();
    if (!o) { o.close(); std::filesystem::remove(tmp, ec); r.message = "temp write failed"; return r; }
  }
  std::filesystem::rename(tmp, path, ec);  // replaces the file (MoveFileExW REPLACE_EXISTING on Windows)
  if (ec) {
    std::filesystem::remove(tmp, ec);
    r.message = "rename failed";
    return r;
  }
  r.written = true;
  r.message = std::to_string(rep) + " line(s) replaced, " + std::to_string(app) + " appended";
  return r;
}

}  // namespace wwe2k14::launcher_ini
