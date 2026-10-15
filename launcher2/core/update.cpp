// See update.h. The launcher's auto-updater: check, download, verify, replace program files, keep the
// player's files. This file has no UI code and never throws; every failure comes back as a Result or an
// "unavailable" UpdateInfo so the front end can stay quiet or show a plain message.

#include "update.h"

#include "internal/archive.h"
#include "internal/util.h"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <winhttp.h>
#else
#include <dirent.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace wwe2k14::launcher {
namespace {

using internal::LowerAscii;
using internal::PathToUtf8;

constexpr char kApiUrl[] =
    "https://api.github.com/repos/PromethenDev/wwe2k14-recomp/releases/latest";
constexpr char kUserAgent[] = "WWE2K14-Recomp-Updater/1.0 (+https://github.com/PromethenDev/wwe2k14-recomp)";
constexpr char kDownloadDir[] = "update-download";
constexpr char kStagingDir[] = "update-staging";

bool HasEnv(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0';
}

std::string GetEnv(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

// --------------------------------------------------------------------------------------------- JSON
// A compact recursive-descent JSON parser: enough for the GitHub release document, no dependency. Strings
// decode \uXXXX (including surrogate pairs) to UTF-8; numbers keep their raw token so a large asset size
// does not lose precision through a double.

struct JsonField;

struct Json {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  std::string text;  // string value or raw number token
  std::vector<Json> items;
  // A named struct rather than std::pair: libstdc++ rejects a pair holding the still-incomplete Json here.
  std::vector<JsonField> fields;

  const Json* Find(std::string_view key) const;
};

struct JsonField {
  std::string first;
  Json second;
};

const Json* Json::Find(std::string_view key) const {
  if (type != Type::kObject) return nullptr;
  for (const auto& field : fields) {
    if (field.first == key) return &field.second;
  }
  return nullptr;
}

void AppendUtf8(std::string& output, uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  bool Parse(Json& out) {
    Skip();
    if (!Value(out)) return false;
    Skip();
    return position_ == text_.size();
  }

 private:
  void Skip() {
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++position_;
      } else {
        break;
      }
    }
  }

  bool Value(Json& out) {
    if (depth_ >= 64) return false;
    ++depth_;
    Skip();
    if (position_ >= text_.size()) {
      --depth_;
      return false;
    }
    bool okay = false;
    switch (text_[position_]) {
      case '{': okay = Object(out); break;
      case '[': okay = Array(out); break;
      case '"':
        out.type = Json::Type::kString;
        okay = String(out.text);
        break;
      case 't':
      case 'f': okay = Bool(out); break;
      case 'n': okay = Null(out); break;
      default: okay = Number(out); break;
    }
    --depth_;
    return okay;
  }

  bool Null(Json& out) {
    if (text_.compare(position_, 4, "null") != 0) return false;
    position_ += 4;
    out.type = Json::Type::kNull;
    return true;
  }

  bool Bool(Json& out) {
    if (text_.compare(position_, 4, "true") == 0) {
      position_ += 4;
      out.type = Json::Type::kBool;
      out.boolean = true;
      return true;
    }
    if (text_.compare(position_, 5, "false") == 0) {
      position_ += 5;
      out.type = Json::Type::kBool;
      out.boolean = false;
      return true;
    }
    return false;
  }

  bool Number(Json& out) {
    const size_t start = position_;
    if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) ++position_;
    bool digits = false;
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c >= '0' && c <= '9') {
        digits = true;
        ++position_;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        ++position_;
      } else {
        break;
      }
    }
    if (!digits) return false;
    out.type = Json::Type::kNumber;
    out.text = std::string(text_.substr(start, position_ - start));
    return true;
  }

  bool Hex4(uint32_t& value) {
    if (position_ + 4 > text_.size()) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[position_++];
      uint32_t digit = 0;
      if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
      else return false;
      value = (value << 4) | digit;
    }
    return true;
  }

  bool String(std::string& out) {
    if (position_ >= text_.size() || text_[position_] != '"') return false;
    ++position_;
    out.clear();
    while (position_ < text_.size()) {
      const char c = text_[position_++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (position_ >= text_.size()) return false;
      const char escape = text_[position_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t codepoint = 0;
          if (!Hex4(codepoint)) return false;
          if (codepoint >= 0xD800 && codepoint <= 0xDBFF && position_ + 1 < text_.size() &&
              text_[position_] == '\\' && text_[position_ + 1] == 'u') {
            position_ += 2;
            uint32_t low = 0;
            if (!Hex4(low)) return false;
            if (low >= 0xDC00 && low <= 0xDFFF) {
              codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
            } else {
              codepoint = 0xFFFD;
            }
          } else if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            codepoint = 0xFFFD;
          }
          AppendUtf8(out, codepoint);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool Array(Json& out) {
    out.type = Json::Type::kArray;
    ++position_;  // '['
    Skip();
    if (position_ < text_.size() && text_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      Json item;
      if (!Value(item)) return false;
      out.items.push_back(std::move(item));
      Skip();
      if (position_ >= text_.size()) return false;
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == ']') {
        ++position_;
        return true;
      }
      return false;
    }
  }

  bool Object(Json& out) {
    out.type = Json::Type::kObject;
    ++position_;  // '{'
    Skip();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      return true;
    }
    while (true) {
      Skip();
      std::string key;
      if (!String(key)) return false;
      Skip();
      if (position_ >= text_.size() || text_[position_] != ':') return false;
      ++position_;
      Json value;
      if (!Value(value)) return false;
      out.fields.push_back(JsonField{std::move(key), std::move(value)});
      Skip();
      if (position_ >= text_.size()) return false;
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == '}') {
        ++position_;
        return true;
      }
      return false;
    }
  }

  std::string_view text_;
  size_t position_ = 0;
  int depth_ = 0;
};

std::string JsonString(const Json& object, std::string_view key) {
  const Json* value = object.Find(key);
  if (value == nullptr || value->type != Json::Type::kString) return {};
  return value->text;
}

bool JsonBool(const Json& object, std::string_view key) {
  const Json* value = object.Find(key);
  return value != nullptr && value->type == Json::Type::kBool && value->boolean;
}

uint64_t JsonUint64(const Json& object, std::string_view key) {
  const Json* value = object.Find(key);
  if (value == nullptr || value->type != Json::Type::kNumber) return 0;
  uint64_t result = 0;
  for (char c : value->text) {
    if (c < '0' || c > '9') break;
    result = result * 10 + static_cast<uint64_t>(c - '0');
  }
  return result;
}

bool IsHexDigest(std::string_view value) {
  if (value.size() != 64) return false;
  for (char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// "sha256:<hex>" -> "<hex>"; anything else yields an empty string.
std::string ParseDigest(std::string_view value) {
  constexpr std::string_view prefix = "sha256:";
  if (!value.starts_with(prefix)) return {};
  std::string hex = LowerAscii(value.substr(prefix.size()));
  return IsHexDigest(hex) ? hex : std::string();
}

// --------------------------------------------------------------------------------------------- versions
struct ParsedVersion {
  std::vector<long long> core;
  std::string prerelease;
};

ParsedVersion ParseVersion(std::string_view value) {
  ParsedVersion result;
  while (!value.empty() && (value.front() == 'v' || value.front() == 'V')) value.remove_prefix(1);
  const size_t build = value.find('+');
  if (build != std::string_view::npos) value = value.substr(0, build);
  const size_t dash = value.find('-');
  std::string_view core = value;
  if (dash != std::string_view::npos) {
    core = value.substr(0, dash);
    result.prerelease = std::string(value.substr(dash + 1));
  }
  size_t start = 0;
  while (start <= core.size()) {
    const size_t dot = core.find('.', start);
    const std::string_view part = core.substr(start, dot == std::string_view::npos ? core.size() - start
                                                                                   : dot - start);
    long long number = 0;
    bool any = false;
    for (char c : part) {
      if (c < '0' || c > '9') break;
      number = number * 10 + (c - '0');
      any = true;
    }
    result.core.push_back(any ? number : 0);
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return result;
}

// --------------------------------------------------------------------------------------------- never touch
std::string NormalizeRelative(std::string_view relative) {
  std::string path = LowerAscii(relative);
  std::replace(path.begin(), path.end(), '\\', '/');
  while (path.starts_with("./")) path.erase(0, 2);
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  while (!path.empty() && path.back() == '/') path.pop_back();
  return path;
}

// --------------------------------------------------------------------------------------------- http
#ifdef _WIN32
std::wstring ToWide(const std::string& text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

struct WinHttpConnection {
  HINTERNET session = nullptr;
  HINTERNET connect = nullptr;
  HINTERNET request = nullptr;

  ~WinHttpConnection() {
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
  }

  bool Open(const std::string& url, double timeout_seconds) {
    const std::wstring wide_url = ToWide(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) return false;

    session = WinHttpOpen(ToWide(kUserAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
      session = WinHttpOpen(ToWide(kUserAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) return false;
    const int milliseconds = static_cast<int>(timeout_seconds * 1000.0);
    WinHttpSetTimeouts(session, std::max(milliseconds, 1000), std::max(milliseconds, 1000),
                       std::max(milliseconds, 1000), std::max(milliseconds, 1000));
    connect = WinHttpConnect(session, host, components.nPort, 0);
    if (!connect) return false;
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) return false;
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
      return false;
    }
    if (!WinHttpReceiveResponse(request, nullptr)) return false;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
      return false;
    }
    return status >= 200 && status < 300;
  }
};

bool HttpGet(const std::string& url, double timeout_seconds, std::string& body) {
  WinHttpConnection connection;
  if (!connection.Open(url, timeout_seconds)) return false;
  body.clear();
  std::array<char, 16 * 1024> buffer{};
  while (true) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(connection.request, &available)) return false;
    if (available == 0) break;
    while (available > 0) {
      const DWORD chunk = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
      DWORD read = 0;
      if (!WinHttpReadData(connection.request, buffer.data(), chunk, &read)) return false;
      if (read == 0) break;
      body.append(buffer.data(), read);
      available -= read;
      if (body.size() > 8u * 1024u * 1024u) return false;  // the API document is small
    }
  }
  return true;
}

bool HttpDownloadToFile(const std::string& url, const fs::path& destination, uint64_t expected,
                        const ProgressFn& progress, const CancelFlag& cancel) {
  WinHttpConnection connection;
  if (!connection.Open(url, 300.0)) return false;
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  uint64_t written = 0;
  std::array<char, 64 * 1024> buffer{};
  while (true) {
    if (cancel.load()) return false;
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(connection.request, &available)) return false;
    if (available == 0) break;
    while (available > 0) {
      const DWORD chunk = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
      DWORD read = 0;
      if (!WinHttpReadData(connection.request, buffer.data(), chunk, &read)) return false;
      if (read == 0) break;
      output.write(buffer.data(), read);
      if (!output) return false;
      written += read;
      available -= read;
      if (progress) progress(written, expected, "Downloading update…");
    }
  }
  output.flush();
  return output.good();
}
#else
bool HttpGet(const std::string& url, double timeout_seconds, std::string& body) {
  if (!HasEnv("WWE2K14_UPDATE_API_URL") && url.rfind("https://", 0) != 0) return false;
  int pipe_fds[2] = {-1, -1};
  if (::pipe(pipe_fds) != 0) return false;
  const std::string timeout = std::to_string(static_cast<int>(timeout_seconds));
  std::vector<std::string> arguments = {"curl", "-fsSL", "--max-time", timeout, "--connect-timeout", "4",
                                        "-A", kUserAgent};
  // Real releases: HTTPS only, including every redirect hop (the test override may use a local http server).
  if (!HasEnv("WWE2K14_UPDATE_API_URL")) arguments.insert(arguments.end(), {"--proto", "=https", "--proto-redir", "=https"});
  arguments.push_back(url);
  std::vector<char*> argv;
  for (std::string& argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return false;
  }
  posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);

  pid_t child = 0;
  const int spawn_status = posix_spawnp(&child, "curl", &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(pipe_fds[1]);
  if (spawn_status != 0) {
    ::close(pipe_fds[0]);
    return false;
  }
  body.clear();
  std::array<char, 16 * 1024> buffer{};
  while (true) {
    const ssize_t read = ::read(pipe_fds[0], buffer.data(), buffer.size());
    if (read <= 0) break;
    body.append(buffer.data(), static_cast<size_t>(read));
    if (body.size() > 8u * 1024u * 1024u) {
      ::close(pipe_fds[0]);
      return false;
    }
  }
  ::close(pipe_fds[0]);
  int wait_status = 0;
  if (::waitpid(child, &wait_status, 0) != child) return false;
  return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
}

bool HttpDownloadToFile(const std::string& url, const fs::path& destination, uint64_t expected,
                        const ProgressFn& progress, const CancelFlag& cancel) {
  if (!HasEnv("WWE2K14_UPDATE_API_URL") && url.rfind("https://", 0) != 0) return false;
  std::vector<std::string> arguments = {"curl", "-fsSL", "--max-time", "900", "--connect-timeout", "10",
                                        "-A", kUserAgent, "-o", PathToUtf8(destination)};
  if (!HasEnv("WWE2K14_UPDATE_API_URL")) arguments.insert(arguments.end(), {"--proto", "=https", "--proto-redir", "=https"});
  arguments.push_back(url);
  std::vector<char*> argv;
  for (std::string& argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);

  pid_t child = 0;
  const int spawn_status = posix_spawnp(&child, "curl", nullptr, nullptr, argv.data(), environ);
  if (spawn_status != 0) return false;

  const auto started = std::chrono::steady_clock::now();
  while (true) {
    if (cancel.load()) {
      ::kill(child, SIGTERM);
      ::waitpid(child, nullptr, 0);
      return false;
    }
    int wait_status = 0;
    const pid_t done = ::waitpid(child, &wait_status, WNOHANG);
    if (done == child) return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
    if (done < 0) return false;
    if (std::chrono::steady_clock::now() - started > std::chrono::minutes(20)) {
      ::kill(child, SIGKILL);
      ::waitpid(child, nullptr, 0);
      return false;
    }
    std::error_code error;
    const uintmax_t size = fs::file_size(destination, error);
    if (progress && !error) progress(static_cast<uint64_t>(size), expected, "Downloading update…");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
#endif

// --------------------------------------------------------------------------------------------- hashing
std::string FileSha256(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return picosha2::hash256_hex_string(std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>());
}

// --------------------------------------------------------------------------------------------- applying
struct PendingFile {
  fs::path destination;
  std::string backup_relative;  // path under backups/update-<old>/
  std::string label;            // progress text
  bool seed_only = false;       // keep an existing file; only add it when missing
  bool self_replace = false;    // Windows: the running launcher, renamed to *.old first
  unsigned mode = 0;            // Unix mode from the zip entry (0 when not recorded)
};

bool IsSeedOnlyPath(const std::string& lowered) {
  return lowered.find("shader-cache/") != std::string::npos || lowered.ends_with("wwe2k14.toml");
}

bool IsLinuxPrefix(const std::string& lowered) {
  return lowered.starts_with("linux-x86_64/");
}

bool InstallFile(const std::vector<uint8_t>& bytes, const fs::path& destination, bool self_replace,
                 unsigned mode) {
  std::error_code error;
  if (!destination.parent_path().empty()) {
    fs::create_directories(destination.parent_path(), error);
    if (error) return false;
  }
  bool wrote = false;
  if (self_replace) {
    // Windows: a running executable can be renamed but not overwritten.
    fs::path old = destination;
    old += ".old";
    if (fs::exists(destination, error)) {
      fs::remove(old, error);
      error.clear();
      fs::rename(destination, old, error);
      if (error) return false;
    }
    wrote = internal::WriteBinaryFile(destination, bytes);
  } else {
    fs::path temporary = destination;
    temporary += ".update-new";
    if (!internal::WriteBinaryFile(temporary, bytes)) {
      fs::remove(temporary, error);
      return false;
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      fs::remove(temporary, error);
      return false;
    }
    wrote = true;
#else
    fs::rename(temporary, destination, error);
    if (error) {
      fs::remove(temporary, error);
      return false;
    }
    wrote = true;
#endif
  }
#ifndef _WIN32
  // Executables (the launcher, the game) must stay executable after the replacement.
  const std::string name = LowerAscii(PathToUtf8(destination.filename()));
  const bool known_executable = name == "wwe2k14" || name == "wwe2k14-launcher";
  if (wrote && ((mode & 0111) != 0 || known_executable)) ::chmod(destination.c_str(), 0755);
#else
  (void)mode;
#endif
  return wrote;
}

bool IsRunningLauncherName(const fs::path& destination) {
#ifdef _WIN32
  const std::string name = LowerAscii(PathToUtf8(destination.filename()));
  return name == "wwe 2k14 launcher.exe" || name == "wwe2k14-launcher.exe";
#else
  (void)destination;
  return false;  // Linux replaces a running binary by rename, atomically.
#endif
}

// Maps one archive entry (already relative to the package top folder) to a destination on this platform.
// Returns false when the entry belongs to the other platform or is a user file that must not be touched.
bool MapEntry(const Paths& paths, const fs::path& root_parent, const std::string& relative,
              const std::string& lowered, PendingFile& file) {
  file = {};
  file.label = relative;
  file.seed_only = IsSeedOnlyPath(lowered);
#ifdef _WIN32
  if (IsLinuxPrefix(lowered)) return false;  // a Windows install does not need the Linux files
  if (IsNeverTouchPath(relative)) {
    if (lowered != "wwe2k14.toml") return false;
  }
  file.destination = paths.exe_dir / relative;
  file.backup_relative = relative;
#else
  if (IsLinuxPrefix(lowered)) {
    const std::string rest = relative.substr(std::string("linux-x86_64/").size());
    const std::string rest_lower = LowerAscii(rest);
    if (IsNeverTouchPath(rest) && rest_lower != "wwe2k14.toml") return false;
    file.destination = paths.exe_dir / rest;
    file.backup_relative = rest;
  } else {
    // The shared top-level documents live one folder above the Linux launcher.
    if (IsNeverTouchPath(relative) && lowered != "wwe2k14.toml") return false;
    file.destination = root_parent / relative;
    file.backup_relative = "__root__/" + relative;
  }
#endif
  file.self_replace = IsRunningLauncherName(file.destination);
  return true;
}

}  // namespace

// --------------------------------------------------------------------------------------------- public

int CompareVersions(std::string_view left, std::string_view right) {
  const ParsedVersion a = ParseVersion(left);
  const ParsedVersion b = ParseVersion(right);
  const size_t count = std::max(a.core.size(), b.core.size());
  for (size_t i = 0; i < count; ++i) {
    const long long av = i < a.core.size() ? a.core[i] : 0;
    const long long bv = i < b.core.size() ? b.core[i] : 0;
    if (av != bv) return av < bv ? -1 : 1;
  }
  // Same numbers: a release is newer than its prerelease (1.4.0 > 1.4.0-rc1).
  if (a.prerelease.empty() != b.prerelease.empty()) return a.prerelease.empty() ? 1 : -1;
  if (a.prerelease != b.prerelease) return a.prerelease < b.prerelease ? -1 : 1;
  return 0;
}

bool IsNeverTouchPath(std::string_view relative_path) {
  const std::string path = NormalizeRelative(relative_path);
  if (path.empty()) return true;
  if (path == "wwe2k14-enhanced.ini" || path == "wwe2k14.toml") return true;
  static constexpr std::string_view prefixes[] = {"userdata/", "music/", "logs/", "backups/", "wwe 2k14/"};
  for (std::string_view prefix : prefixes) {
    if (path == prefix.substr(0, prefix.size() - 1) || path.starts_with(prefix)) return true;
  }
  return false;
}

std::optional<UpdateInfo> ParseLatestReleaseJson(std::string_view json, std::string_view current_version) {
  Json root;
  JsonParser parser(json);
  if (!parser.Parse(root) || root.type != Json::Type::kObject) return std::nullopt;
  const std::string tag = JsonString(root, "tag_name");
  if (tag.empty()) return std::nullopt;

  UpdateInfo info;
  info.tag = tag;
  info.version = tag;
  while (!info.version.empty() && (info.version.front() == 'v' || info.version.front() == 'V')) {
    info.version.erase(info.version.begin());
  }
  info.notes = JsonString(root, "body");

  if (const Json* assets = root.Find("assets"); assets != nullptr && assets->type == Json::Type::kArray) {
    for (const Json& asset : assets->items) {
      if (asset.type != Json::Type::kObject) continue;
      const std::string name = JsonString(asset, "name");
      const std::string lowered = LowerAscii(name);
      if (!lowered.ends_with(".zip") || !lowered.starts_with("wwe2k14-recomp") ||
          lowered.find("source") != std::string::npos) {
        continue;
      }
      info.name = name;
      info.url = JsonString(asset, "browser_download_url");
      info.size = JsonUint64(asset, "size");
      info.sha256 = ParseDigest(JsonString(asset, "digest"));
      break;
    }
  }

  const bool draft = JsonBool(root, "draft");
  const bool prerelease = JsonBool(root, "prerelease");
  info.available = !draft && !prerelease && !info.url.empty() && !info.sha256.empty() &&
                   CompareVersions(info.version, current_version) > 0;
  return info;
}

UpdateInfo CheckLatestUpdate(std::string_view current_version, double timeout_seconds) {
  UpdateInfo unavailable;
  std::string url = GetEnv("WWE2K14_UPDATE_API_URL");
  if (url.empty()) url = kApiUrl;
  std::string body;
  if (!HttpGet(url, timeout_seconds, body)) return unavailable;
  auto parsed = ParseLatestReleaseJson(body, current_version);
  if (!parsed) return unavailable;
  return *parsed;
}

namespace update_internal {

bool IsAllowedDownloadUrl(std::string_view url) {
  if (url.rfind("https://", 0) == 0) {
    if (HasEnv("WWE2K14_UPDATE_API_URL")) return true;
    const std::string lowered = LowerAscii(url);
    return lowered.rfind("https://github.com/", 0) == 0 ||
           lowered.rfind("https://objects.githubusercontent.com/", 0) == 0 ||
           lowered.rfind("https://release-assets.githubusercontent.com/", 0) == 0;
  }
  // Plain http is accepted only when a test override is active.
  return HasEnv("WWE2K14_UPDATE_API_URL") && url.rfind("http://", 0) == 0;
}

bool HttpGetForTest(std::string_view url, double timeout_seconds, std::string* body) {
  if (body == nullptr) return false;
  return HttpGet(std::string(url), timeout_seconds, *body);
}

}  // namespace update_internal

Result DownloadUpdate(const Paths& paths, const UpdateInfo& info, const ProgressFn& progress,
                      const CancelFlag& cancel, fs::path* downloaded) {
  if (info.url.empty() || !update_internal::IsAllowedDownloadUrl(info.url)) {
    return Result::Fail("The update download address was not one the launcher trusts.");
  }
  const std::string name = PathToUtf8(fs::path(info.name).filename());
  if (name.empty() || name == "." || name == "..") {
    return Result::Fail("The update download had no file name.");
  }
  const fs::path directory = paths.backups_dir / kDownloadDir;
  if (!internal::EnsureDirectory(directory)) {
    return Result::Fail("The launcher could not prepare the download folder.");
  }
  const fs::path part = directory / (name + ".part");
  const fs::path final_path = directory / name;
  std::error_code error;
  fs::remove(part, error);
  if (progress) progress(0, info.size, "Downloading update…");
  if (!HttpDownloadToFile(info.url, part, info.size, progress, cancel)) {
    fs::remove(part, error);
    if (cancel.load()) return Result::Fail("");  // the caller reports the cancellation
    return Result::Fail("The update could not be downloaded. Check your connection and try again.");
  }
  if (cancel.load()) {
    fs::remove(part, error);
    return Result::Fail("");
  }
  const uintmax_t actual_size = fs::file_size(part, error);
  if (error) {
    fs::remove(part, error);
    return Result::Fail("The update could not be downloaded. Check your connection and try again.");
  }
  if (info.size != 0 && static_cast<uint64_t>(actual_size) != info.size) {
    fs::remove(part, error);
    return Result::Fail("The download was damaged - please try again.");
  }
  if (FileSha256(part) != info.sha256) {
    fs::remove(part, error);
    return Result::Fail("The download was damaged - please try again.");
  }
  fs::remove(final_path, error);
  error.clear();
  fs::rename(part, final_path, error);
  if (error) {
    fs::remove(part, error);
    return Result::Fail("The downloaded update could not be saved.");
  }
  if (downloaded) *downloaded = final_path;
  return Result::Ok();
}

Result ApplyUpdate(const Paths& paths, const fs::path& zip_file, const UpdateInfo& info,
                   std::string_view old_version, const ProgressFn& progress, const CancelFlag& cancel) {
  (void)info;
  (void)cancel;  // once replacement starts it always runs to completion or rolls back
  try {
    internal::ZipReader reader;
    if (!reader.Open(zip_file)) {
      return Result::Fail("The update file could not be opened.");
    }
    const size_t count = reader.Count();
    if (count == 0) return Result::Fail("The update file was empty.");

    // The archive has one top-level folder (WWE2K14-Recomp-x64/); strip it so paths are relative to it.
    std::string prefix;
    for (size_t index = 0; index < count; ++index) {
      mz_zip_archive_file_stat stat{};
      if (!reader.Stat(index, stat)) continue;
      const std::string name = stat.m_filename;
      const size_t slash = name.find('/');
      if (slash != std::string::npos) {
        prefix = name.substr(0, slash + 1);
        break;
      }
    }

    const fs::path root_parent = paths.exe_dir.parent_path();
    struct Entry {
      size_t archive_index = 0;
      PendingFile file;
    };
    std::vector<Entry> entries;
    for (size_t index = 0; index < count; ++index) {
      if (reader.IsDirectory(index)) continue;
      mz_zip_archive_file_stat stat{};
      if (!reader.Stat(index, stat)) continue;
      std::string name = stat.m_filename;
      std::replace(name.begin(), name.end(), '\\', '/');
      std::string relative = (!prefix.empty() && name.starts_with(prefix)) ? name.substr(prefix.size()) : name;
      const std::string lowered = LowerAscii(relative);
      if (lowered.empty()) continue;
      PendingFile file;
      if (!MapEntry(paths, root_parent, relative, lowered, file)) continue;
      file.mode = static_cast<unsigned>((stat.m_external_attr >> 16) & 0777);
      entries.push_back({index, std::move(file)});
    }
    if (entries.empty()) return Result::Fail("The update file did not contain any program files.");

    // 1) Extract to a staging folder first.
    std::error_code error;
    const fs::path staging = paths.backups_dir / kStagingDir;
    fs::remove_all(staging, error);
    if (!internal::EnsureDirectory(staging)) {
      return Result::Fail("The launcher could not prepare the update.");
    }
    std::vector<fs::path> staged(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
      if (progress) progress(i, entries.size(), "Preparing update…");
      const fs::path stage_path = staging / entries[i].file.backup_relative;
      if (!internal::EnsureDirectory(stage_path.parent_path())) {
        fs::remove_all(staging, error);
        return Result::Fail("The launcher could not prepare the update.");
      }
      std::vector<uint8_t> bytes;
      if (!reader.Extract(entries[i].archive_index, bytes) ||
          !internal::WriteBinaryFile(stage_path, bytes)) {
        fs::remove_all(staging, error);
        return Result::Fail("The update file was damaged.");
      }
      staged[i] = stage_path;
    }

    // 2) Back up the current program files.
    const fs::path backup_root = paths.backups_dir / ("update-" + std::string(old_version));
    fs::remove_all(backup_root, error);
    if (!internal::EnsureDirectory(backup_root)) {
      fs::remove_all(staging, error);
      return Result::Fail("The launcher could not back up the current version.");
    }
    std::vector<bool> existed(entries.size(), false);
    for (size_t i = 0; i < entries.size(); ++i) {
      if (progress) progress(i, entries.size(), "Backing up the current version…");
      std::error_code exists_error;
      if (!fs::is_regular_file(entries[i].file.destination, exists_error)) continue;
      existed[i] = true;
      const fs::path backup_path = backup_root / entries[i].file.backup_relative;
      if (!internal::EnsureDirectory(backup_path.parent_path())) {
        fs::remove_all(staging, error);
        return Result::Fail("The launcher could not back up the current version.");
      }
      fs::copy_file(entries[i].file.destination, backup_path, fs::copy_options::overwrite_existing, error);
      if (error) {
        fs::remove_all(staging, error);
        return Result::Fail("The launcher could not back up the current version.");
      }
    }

    // 3) Replace program files.
    std::vector<bool> applied(entries.size(), false);
    for (size_t i = 0; i < entries.size(); ++i) {
      std::error_code exists_error;
      if (entries[i].file.seed_only && fs::exists(entries[i].file.destination, exists_error)) continue;
      if (progress) progress(i, entries.size(), "Installing update…");
      std::vector<uint8_t> bytes;
      if (!internal::ReadBinaryFile(staged[i], bytes) ||
          !InstallFile(bytes, entries[i].file.destination, entries[i].file.self_replace,
                       entries[i].file.mode)) {
        // Roll back everything written so far.
        std::error_code rollback_error;
        for (size_t j = 0; j < entries.size(); ++j) {
          if (!applied[j]) continue;
          if (existed[j]) {
            fs::copy_file(backup_root / entries[j].file.backup_relative, entries[j].file.destination,
                          fs::copy_options::overwrite_existing, rollback_error);
            rollback_error.clear();
          } else {
            fs::remove(entries[j].file.destination, rollback_error);
            rollback_error.clear();
          }
        }
        fs::remove_all(staging, rollback_error);
        return Result::Fail("The update could not be installed. Your previous version has been restored.");
      }
      applied[i] = true;
    }

    fs::remove_all(staging, error);
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The update could not be installed. Your previous version has been restored.");
  }
}

Result RelaunchLauncher(const fs::path& launcher_executable) {
  try {
    std::error_code error;
    if (!fs::is_regular_file(launcher_executable, error)) {
      return Result::Fail("The launcher could not restart itself. Start it again to use the new version.");
    }
#ifdef _WIN32
    std::wstring command = L"\"";
    command += launcher_executable.wstring();
    command += L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = launcher_executable.parent_path().wstring();
    if (!CreateProcessW(launcher_executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, working_directory.empty() ? nullptr : working_directory.c_str(), &startup,
                        &process)) {
      return Result::Fail("The launcher could not restart itself. Start it again to use the new version.");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return Result::Ok();
#else
    std::string path = PathToUtf8(launcher_executable);
    char* argv[2] = {path.data(), nullptr};
    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0) {
      return Result::Fail("The launcher could not restart itself. Start it again to use the new version.");
    }
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
    pid_t child = 0;
    const int status = posix_spawn(&child, launcher_executable.c_str(), nullptr, &attributes, argv, environ);
    posix_spawnattr_destroy(&attributes);
    if (status != 0) {
      return Result::Fail("The launcher could not restart itself. Start it again to use the new version.");
    }
    return Result::Ok();
#endif
  } catch (...) {
    return Result::Fail("The launcher could not restart itself. Start it again to use the new version.");
  }
}

bool GameProcessRunning(const Paths& paths) {
  try {
    if (paths.game_exe.empty()) return false;
    std::error_code error;
    const fs::path game = fs::weakly_canonical(paths.game_exe, error);
    const fs::path wanted = error ? paths.game_exe : game;
#ifdef _WIN32
    const std::wstring wanted_text = wanted.wstring();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    const DWORD self = GetCurrentProcessId();
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !running; more = Process32NextW(snapshot, &entry)) {
      if (entry.th32ProcessID == self || entry.th32ProcessID == 0) continue;
      // Cheap filter on the file name before opening the process.
      if (CompareStringOrdinal(entry.szExeFile, -1, wanted.filename().c_str(), -1, TRUE) != CSTR_EQUAL) continue;
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
      if (process == nullptr) continue;
      wchar_t image[4096];
      DWORD size = static_cast<DWORD>(std::size(image));
      if (QueryFullProcessImageNameW(process, 0, image, &size)) {
        running = CompareStringOrdinal(image, static_cast<int>(size), wanted_text.c_str(),
                                       static_cast<int>(wanted_text.size()), TRUE) == CSTR_EQUAL;
      }
      CloseHandle(process);
    }
    CloseHandle(snapshot);
    return running;
#else
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return false;
    const std::string self = std::to_string(::getpid());
    bool running = false;
    while (dirent* entry = ::readdir(proc)) {
      const std::string name = entry->d_name;
      if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
          name == self) {
        continue;
      }
      std::error_code link_error;
      fs::path target = fs::read_symlink(fs::path("/proc") / name / "exe", link_error);
      if (link_error) continue;
      // A binary replaced while running reads "<path> (deleted)"; it is still this install's game.
      std::string text = target.string();
      constexpr std::string_view deleted = " (deleted)";
      if (text.ends_with(deleted)) target = fs::path(text.substr(0, text.size() - deleted.size()));
      if (target == wanted) {
        running = true;
        break;
      }
    }
    ::closedir(proc);
    return running;
#endif
  } catch (...) {
    return false;
  }
}

void CleanupUpdateLeftovers(const Paths& paths) {
  std::error_code error;
  // New launcher: the old one renamed itself out of the way; remove it (retry while the old process exits).
  for (int attempt = 0; attempt < 20; ++attempt) {
    bool removed_any = false;
    for (const fs::path& directory : {paths.exe_dir, paths.exe_dir.parent_path()}) {
      if (directory.empty()) continue;
      std::error_code list_error;
      for (fs::directory_iterator it(directory, list_error), end; it != end; it.increment(list_error)) {
        const fs::path file = it->path();
        std::error_code file_error;
        if (!fs::is_regular_file(file, file_error)) continue;
        if (file.extension() != ".old") continue;
        std::error_code remove_error;
        if (fs::remove(file, remove_error)) removed_any = true;
      }
    }
    if (!removed_any) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  // A cancelled or damaged download leaves a .part behind; the staging folder is scratch.
  const fs::path download = paths.backups_dir / kDownloadDir;
  std::error_code download_error;
  if (fs::is_directory(download, download_error)) {
    for (fs::directory_iterator it(download, download_error), end; it != end; it.increment(download_error)) {
      if (it->path().extension() == ".part") {
        std::error_code remove_error;
        fs::remove(it->path(), remove_error);
      }
    }
  }
  error.clear();
  fs::remove_all(paths.backups_dir / kStagingDir, error);
}

}  // namespace wwe2k14::launcher
