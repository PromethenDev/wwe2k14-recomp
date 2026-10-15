#include "launcher_core.h"
#include "launcher_version.h"

#include "internal/archive.h"
#include "internal/util.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

namespace wwe2k14::launcher {
namespace {

std::string SystemSummary() {
  std::ostringstream summary;
  summary << "Launcher version: " << WWE2K14_LAUNCHER_VERSION << " (build " << WWE2K14_LAUNCHER_BUILD_COMMIT << ")\n";
#ifdef _WIN32
  SYSTEM_INFO system_info{};
  GetNativeSystemInfo(&system_info);
  summary << "Operating system: Windows\n"
          << "Architecture: " << (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x86-64" : "other") << "\n"
          << "Logical processors: " << system_info.dwNumberOfProcessors << "\n";
#else
  utsname system{};
  if (uname(&system) == 0) {
    summary << "Operating system: " << system.sysname << " " << system.release << "\n"
            << "Architecture: " << system.machine << "\n";
  } else {
    summary << "Operating system: Linux\n";
  }
  summary << "Logical processors: " << std::thread::hardware_concurrency() << "\n";
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    if (line.starts_with("model name\t: ")) {
      summary << "CPU: " << line.substr(13) << "\n";
      break;
    }
  }
#endif
  return summary.str();
}

}  // namespace

Result SaveBugReport(const Paths& paths, fs::path* created_zip) {
  try {
    std::vector<fs::directory_entry> logs;
    std::error_code error;
    fs::directory_iterator iterator(paths.logs_dir, fs::directory_options::skip_permission_denied, error);
    if (!error) {
      for (const auto& entry : iterator) {
        error.clear();
        if (entry.is_regular_file(error) && !error) logs.push_back(entry);
      }
    }
    std::sort(logs.begin(), logs.end(), [](const fs::directory_entry& left, const fs::directory_entry& right) {
      std::error_code left_error, right_error;
      const auto left_time = left.last_write_time(left_error);
      const auto right_time = right.last_write_time(right_error);
      if (!left_error && !right_error && left_time != right_time) return left_time > right_time;
      return internal::PathToUtf8(left.path().filename()) > internal::PathToUtf8(right.path().filename());
    });

    // Newest three logs, plus every always-on freeze report regardless of age
    // (a freeze report is written once at freeze time and may later be pushed
    // out of the newest-three window by continued log writes).
    std::vector<fs::directory_entry> selected_entries;
    const size_t selected = std::min<size_t>(3, logs.size());
    for (size_t i = 0; i < selected; ++i) selected_entries.push_back(logs[i]);
    for (const auto& candidate : logs) {
      const std::string filename = internal::PathToUtf8(candidate.path().filename());
      const bool freeze_report = filename.starts_with("wwe2k14-freeze-") &&
                                 filename.ends_with(".txt");
      if (!freeze_report) continue;
      const bool already = std::any_of(selected_entries.begin(), selected_entries.end(),
                                       [&](const fs::directory_entry& entry) {
                                         return entry.path() == candidate.path();
                                       });
      if (!already) selected_entries.push_back(candidate);
    }
    std::vector<internal::ZipEntry> entries;
    for (const auto& selected_entry : selected_entries) {
      internal::ZipEntry entry;
      entry.name = "logs/" + internal::PathToUtf8(selected_entry.path().filename());
      if (!internal::ReadBinaryFile(selected_entry.path(), entry.bytes)) {
        return Result::Fail("The launcher could not read the latest game logs.");
      }
      entries.push_back(std::move(entry));
    }
    if (fs::is_regular_file(paths.settings_file, error) && !error) {
      internal::ZipEntry ini;
      ini.name = "wwe2k14-enhanced.ini";
      if (!internal::ReadBinaryFile(paths.settings_file, ini.bytes)) {
        return Result::Fail("The launcher could not read its settings file.");
      }
      entries.push_back(std::move(ini));
    }

    std::ostringstream gpu_text;
    for (const auto& gpu : ListGpus()) {
      gpu_text << gpu.name << " | " << gpu.id << " | "
               << (gpu.integrated ? "integrated" : "discrete/other")
               << " | Vulkan index " << gpu.vulkan_index << "\n";
    }
    if (gpu_text.str().empty()) gpu_text << "No Vulkan graphics devices were listed.\n";
    internal::ZipEntry gpus;
    gpus.name = "gpu-list.txt";
    const std::string gpu_contents = gpu_text.str();
    gpus.bytes.assign(gpu_contents.begin(), gpu_contents.end());
    entries.push_back(std::move(gpus));

    internal::ZipEntry system;
    system.name = "system.txt";
    const std::string summary = SystemSummary();
    system.bytes.assign(summary.begin(), summary.end());
    entries.push_back(std::move(system));

    std::string stamp = internal::LocalTimestamp(std::chrono::system_clock::now(), "%Y%m%d-%H%M%S");
    fs::path destination = paths.exe_dir / ("wwe2k14-bug-report-" + stamp + ".zip");
    unsigned suffix = 2;
    error.clear();
    while (fs::exists(destination, error) && !error) {
      destination = paths.exe_dir / ("wwe2k14-bug-report-" + stamp + "-" + std::to_string(suffix++) + ".zip");
      error.clear();
    }
    if (!internal::CreateZip(destination, entries)) {
      return Result::Fail("The launcher could not create the bug report.");
    }
    if (created_zip) *created_zip = destination;
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The launcher could not create the bug report.");
  }
}

}  // namespace wwe2k14::launcher
