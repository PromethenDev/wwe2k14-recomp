#pragma once

#include <filesystem>
#include <string>

namespace wwe2k14::launcher {

// Joins `relative` ('/'-separated, e.g. "pac/ch/chEtc_dlc01.pac") onto `base`, matching each component against
// what exists on disk ignoring ASCII letter case. An exact-case match wins; otherwise the first case-insensitive
// match in sorted name order. When a component is not found, it and the rest are appended unchanged.
// Never throws (uses std::error_code). Empty components (from "//") are skipped.
std::filesystem::path ResolvePathCase(const std::filesystem::path& base, const std::string& relative);

}  // namespace wwe2k14::launcher
