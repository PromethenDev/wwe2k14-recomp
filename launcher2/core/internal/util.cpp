#include "util.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <ctime>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace wwe2k14::launcher::internal {
namespace {

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

uint32_t ReadUtf8Codepoint(std::string_view text, size_t& offset) {
  const auto byte = [&](size_t index) { return static_cast<uint8_t>(text[index]); };
  const uint8_t first = byte(offset++);
  if (first < 0x80) return first;
  unsigned count = first >= 0xF0 ? 3 : first >= 0xE0 ? 2 : first >= 0xC2 ? 1 : 0;
  uint32_t value = count == 3 ? (first & 0x07) : count == 2 ? (first & 0x0F)
                                   : count == 1 ? (first & 0x1F) : 0xFFFD;
  if (count == 0 || offset + count > text.size()) return 0xFFFD;
  for (unsigned i = 0; i < count; ++i) {
    const uint8_t next = byte(offset);
    if ((next & 0xC0) != 0x80) return 0xFFFD;
    ++offset;
    value = (value << 6) | (next & 0x3F);
  }
  if ((count == 1 && value < 0x80) || (count == 2 && value < 0x800) ||
      (count == 3 && value < 0x10000) || value > 0x10FFFF ||
      (value >= 0xD800 && value <= 0xDFFF)) {
    return 0xFFFD;
  }
  return value;
}

std::string Utf16BytesToUtf8(const std::vector<uint8_t>& bytes, size_t offset, bool big_endian) {
  std::string result;
  result.reserve(bytes.size());
  while (offset + 1 < bytes.size()) {
    uint16_t first = big_endian ? static_cast<uint16_t>((bytes[offset] << 8) | bytes[offset + 1])
                                : static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
    offset += 2;
    uint32_t codepoint = first;
    if (first >= 0xD800 && first <= 0xDBFF && offset + 1 < bytes.size()) {
      const uint16_t second = big_endian
                                  ? static_cast<uint16_t>((bytes[offset] << 8) | bytes[offset + 1])
                                  : static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
      if (second >= 0xDC00 && second <= 0xDFFF) {
        offset += 2;
        codepoint = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
      } else {
        codepoint = 0xFFFD;
      }
    } else if (first >= 0xD800 && first <= 0xDFFF) {
      codepoint = 0xFFFD;
    }
    AppendUtf8(result, codepoint);
  }
  return result;
}

}  // namespace

std::string PathToUtf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path PathFromUtf8(std::string_view text) {
  return std::filesystem::u8path(text.begin(), text.end());
}

std::string Utf16LeBytesToUtf8(const std::vector<uint8_t>& bytes) {
  size_t offset = 0;
  bool big_endian = false;
  if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
    offset = 2;
  } else if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
    offset = 2;
    big_endian = true;
  }
  return Utf16BytesToUtf8(bytes, offset, big_endian);
}

std::vector<uint8_t> Utf8ToUtf16LeBytes(std::string_view text, bool bom) {
  std::vector<uint8_t> result;
  result.reserve(2 + text.size() * 2);
  if (bom) {
    result.push_back(0xFF);
    result.push_back(0xFE);
  }
  size_t offset = 0;
  while (offset < text.size()) {
    uint32_t codepoint = ReadUtf8Codepoint(text, offset);
    auto append = [&](uint16_t unit) {
      result.push_back(static_cast<uint8_t>(unit & 0xFF));
      result.push_back(static_cast<uint8_t>(unit >> 8));
    };
    if (codepoint <= 0xFFFF) {
      append(static_cast<uint16_t>(codepoint));
    } else {
      codepoint -= 0x10000;
      append(static_cast<uint16_t>(0xD800 + (codepoint >> 10)));
      append(static_cast<uint16_t>(0xDC00 + (codepoint & 0x3FF)));
    }
  }
  return result;
}

bool ReadTextFile(const std::filesystem::path& path, std::string& text) {
  std::vector<uint8_t> bytes;
  if (!ReadBinaryFile(path, bytes)) return false;
  if (bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) ||
                            (bytes[0] == 0xFE && bytes[1] == 0xFF))) {
    text = Utf16LeBytesToUtf8(bytes);
  } else {
    size_t offset = bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF
                        ? 3
                        : 0;
    text.assign(reinterpret_cast<const char*>(bytes.data() + offset), bytes.size() - offset);
  }
  return true;
}

bool ReadBinaryFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
  std::error_code error;
  const uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size > static_cast<uintmax_t>((std::numeric_limits<size_t>::max)())) return false;
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  bytes.resize(static_cast<size_t>(size));
  if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return input.good() || (input.eof() && static_cast<size_t>(input.gcount()) == bytes.size());
}

bool WriteBinaryFile(const std::filesystem::path& path, const void* data, size_t size) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  if (size) output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  output.flush();
  return output.good();
}

bool WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  return WriteBinaryFile(path, bytes.data(), bytes.size());
}

bool WriteTextFileAtomic(const std::filesystem::path& path, std::string_view text) {
  std::error_code error;
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
  if (error) return false;
#ifdef _WIN32
  const auto pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
  const auto pid = static_cast<unsigned long>(getpid());
#endif
  std::filesystem::path temporary = path;
  temporary += ".tmp." + std::to_string(pid);
  const auto bytes = Utf8ToUtf16LeBytes(text);
  if (!WriteBinaryFile(temporary, bytes)) {
    std::filesystem::remove(temporary, error);
    return false;
  }
#ifdef _WIN32
  // No MOVEFILE_WRITE_THROUGH: the replace is still atomic, and forcing a disk flush on every
  // settings click made the launcher feel slow on laptops.
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
  }
#else
  std::filesystem::rename(temporary, path, error);
#endif
  if (error) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

bool EnsureDirectory(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return !error && std::filesystem::is_directory(path, error) && !error;
}

std::string Trim(std::string_view value) {
  size_t first = 0;
  while (first < value.size() && (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' ||
                                  value[first] == '\n')) ++first;
  size_t last = value.size();
  while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '\r' ||
                          value[last - 1] == '\n')) --last;
  return std::string(value.substr(first, last - first));
}

std::string LowerAscii(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch);
  });
  return result;
}

std::string LocalTimestamp(const std::chrono::system_clock::time_point& when, const char* format) {
  const std::time_t time = std::chrono::system_clock::to_time_t(when);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  std::array<char, 96> buffer{};
  if (std::strftime(buffer.data(), buffer.size(), format, &local) == 0) return "unknown-time";
  return buffer.data();
}

std::string IsoLocalTimestamp(const std::filesystem::file_time_type& when) {
  const auto system_when = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      when - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
  return LocalTimestamp(system_when, "%Y-%m-%dT%H:%M:%S");
}

bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& root) {
  std::error_code error;
  const auto canonical_child = std::filesystem::weakly_canonical(child, error);
  if (error) return false;
  const auto canonical_root = std::filesystem::weakly_canonical(root, error);
  if (error) return false;
  auto child_it = canonical_child.begin();
  for (auto root_it = canonical_root.begin(); root_it != canonical_root.end(); ++root_it, ++child_it) {
    if (child_it == canonical_child.end() || *child_it != *root_it) return false;
  }
  return true;
}

}  // namespace wwe2k14::launcher::internal
