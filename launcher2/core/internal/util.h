#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wwe2k14::launcher::internal {

std::string PathToUtf8(const std::filesystem::path& path);
std::filesystem::path PathFromUtf8(std::string_view text);
std::string Utf16LeBytesToUtf8(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> Utf8ToUtf16LeBytes(std::string_view text, bool bom = true);
bool ReadTextFile(const std::filesystem::path& path, std::string& text);
bool ReadBinaryFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes);
bool WriteBinaryFile(const std::filesystem::path& path, const void* data, size_t size);
bool WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes);
bool WriteTextFileAtomic(const std::filesystem::path& path, std::string_view text);
bool EnsureDirectory(const std::filesystem::path& path);
std::string Trim(std::string_view value);
std::string LowerAscii(std::string_view value);
std::string LocalTimestamp(const std::chrono::system_clock::time_point& when,
                           const char* format);
std::string IsoLocalTimestamp(const std::filesystem::file_time_type& when);
bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& root);

}  // namespace wwe2k14::launcher::internal
