#include "launcher_core.h"
#include "launcher_version.h"

#include "internal/archive.h"
#include "internal/util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#include <sys/utsname.h>
#endif

namespace wwe2k14::launcher {
namespace {

std::string Trim(std::string text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
  const size_t first = text.find_first_not_of(" \t");
  return first == std::string::npos ? std::string() : text.substr(first);
}

#ifdef _WIN32
std::string RegistryString(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buffer[256]{};
  DWORD size = sizeof(buffer);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
  return internal::PathToUtf8(fs::path(buffer));
}

DWORD RegistryDword(HKEY root, const wchar_t* key, const wchar_t* value) {
  DWORD data = 0;
  DWORD size = sizeof(data);
  if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) return 0;
  return data;
}

// "SSD", "hard disk" or "unknown" for the drive holding `folder` (seek penalty = spinning disk).
std::string DriveKind(const fs::path& folder) {
  std::error_code error;
  const fs::path root = fs::absolute(folder, error).root_name();
  if (error || root.empty()) return "unknown";
  const std::wstring device = L"\\\\.\\" + root.wstring();
  HANDLE volume =
      CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (volume == INVALID_HANDLE_VALUE) return "unknown";
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceSeekPenaltyProperty;
  query.QueryType = PropertyStandardQuery;
  DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
  DWORD returned = 0;
  const bool ok = DeviceIoControl(volume, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &seek, sizeof(seek),
                                  &returned, nullptr) && returned >= sizeof(seek);
  CloseHandle(volume);
  if (!ok) return "unknown";
  return root.string() + (seek.IncursSeekPenalty ? " hard disk" : " SSD");
}
#endif

// Hardware summary for bug reports (owner 2026-10-31). No computer name, account name or serial numbers.
std::string SystemSummary(const Paths& paths) {
  std::ostringstream summary;
  summary << "Launcher version: " << WWE2K14_LAUNCHER_VERSION << " (build " << WWE2K14_LAUNCHER_BUILD_COMMIT << ")\n";
#ifdef _WIN32
  SYSTEM_INFO system_info{};
  GetNativeSystemInfo(&system_info);
  const wchar_t* kVersionKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  const DWORD build = static_cast<DWORD>(std::strtoul(RegistryString(HKEY_LOCAL_MACHINE, kVersionKey,
                                                                     L"CurrentBuildNumber").c_str(), nullptr, 10));
  std::string product = RegistryString(HKEY_LOCAL_MACHINE, kVersionKey, L"ProductName");
  // Windows 11 still reports "Windows 10 ..." as ProductName; the build number tells them apart.
  if (build >= 22000 && product.starts_with("Windows 10")) product.replace(8, 2, "11");
  const std::string display_version = RegistryString(HKEY_LOCAL_MACHINE, kVersionKey, L"DisplayVersion");
  summary << "Operating system: " << (product.empty() ? "Windows" : product)
          << (display_version.empty() ? "" : " " + display_version) << " (build " << build << "."
          << RegistryDword(HKEY_LOCAL_MACHINE, kVersionKey, L"UBR") << ")\n"
          << "Architecture: "
          << (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x86-64" : "other") << "\n"
          << "CPU: " << Trim(RegistryString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                             L"ProcessorNameString")) << "\n"
          << "Logical processors: " << system_info.dwNumberOfProcessors << "\n";
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) {
    summary << "Memory: " << (memory.ullTotalPhys >> 20) << " MB total, " << (memory.ullAvailPhys >> 20)
            << " MB free\n";
  }
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
    summary << "Main display: " << mode.dmPelsWidth << "x" << mode.dmPelsHeight << " @ " << mode.dmDisplayFrequency
            << " Hz\n";
  }
  SYSTEM_POWER_STATUS power{};
  if (GetSystemPowerStatus(&power)) {
    summary << "Power: "
            << (power.ACLineStatus == 1 ? "plugged in" : power.ACLineStatus == 0 ? "on battery" : "unknown");
    if (power.BatteryFlag != 128 && power.BatteryLifePercent <= 100) {
      summary << ", battery " << int(power.BatteryLifePercent) << " %";
    }
    if (power.SystemStatusFlag == 1) summary << ", battery saver on";
    summary << "\n";
  }
  summary << "Drives: launcher " << DriveKind(paths.exe_dir) << ", game files "
          << DriveKind(LoadSettings(paths).game_folder) << "\n";
#else
  utsname system{};
  std::string os_name;
  std::ifstream os_release("/etc/os-release");
  std::string line;
  while (std::getline(os_release, line)) {
    if (line.starts_with("PRETTY_NAME=")) {
      os_name = line.substr(12);
      std::erase(os_name, '"');
    }
  }
  if (uname(&system) == 0) {
    summary << "Operating system: " << (os_name.empty() ? std::string(system.sysname) : os_name) << " (kernel "
            << system.release << ")\n"
            << "Architecture: " << system.machine << "\n";
  } else {
    summary << "Operating system: Linux\n";
  }
  std::ifstream cpuinfo("/proc/cpuinfo");
  while (std::getline(cpuinfo, line)) {
    if (line.starts_with("model name\t: ")) {
      summary << "CPU: " << line.substr(13) << "\n";
      break;
    }
  }
  summary << "Logical processors: " << std::thread::hardware_concurrency() << "\n";
  std::ifstream meminfo("/proc/meminfo");
  uint64_t total_kb = 0, available_kb = 0;
  while (std::getline(meminfo, line)) {
    if (line.starts_with("MemTotal:")) total_kb = std::strtoull(line.c_str() + 9, nullptr, 10);
    if (line.starts_with("MemAvailable:")) available_kb = std::strtoull(line.c_str() + 13, nullptr, 10);
  }
  if (total_kb) summary << "Memory: " << (total_kb >> 10) << " MB total, " << (available_kb >> 10) << " MB free\n";
  std::error_code error;
  for (const auto& supply : fs::directory_iterator("/sys/class/power_supply", error)) {
    std::ifstream type(supply.path() / "type");
    std::string kind;
    std::getline(type, kind);
    if (kind == "Mains") {
      std::ifstream online(supply.path() / "online");
      std::string value;
      std::getline(online, value);
      summary << "Power: " << (value == "1" ? "plugged in" : "on battery") << "\n";
      break;
    }
  }
#endif
  summary << "Shader translator: "
          << (ShaderToolsPresent(paths) ? "present"
                                        : "MISSING (shadertools/" + ShaderToolsPath(paths).filename().string() + ")")
          << "\n";
  {
    std::error_code own_error;
    const fs::path own_slots = paths.exe_dir / "own-slots.txt";
    if (fs::is_regular_file(own_slots, own_error) && !own_error) {
      summary << "Imported wrestler list (own-slots.txt): " << ReadOwnSlotIds(own_slots).size() << " ids\n";
    } else {
      summary << "Imported wrestler list (own-slots.txt): MISSING\n";
    }
    own_error.clear();
    std::vector<std::string> games;
    fs::directory_iterator mods(paths.exe_dir / "mods", fs::directory_options::skip_permission_denied, own_error);
    if (!own_error) {
      for (const auto& game : mods) {
        std::error_code item_error;
        if (!game.is_directory(item_error) || item_error) continue;
        const fs::path roster = game.path() / "roster.tsv";
        if (!fs::is_regular_file(roster, item_error) || item_error) continue;
        size_t rows = 0;
        std::ifstream input(roster, std::ios::binary);
        std::string row;
        while (std::getline(input, row)) {
          if (!row.empty() && row.back() == '\r') row.pop_back();
          if (!row.empty() && row[0] != '#') ++rows;
        }
        games.push_back("Import rosters: mods/" + internal::PathToUtf8(game.path().filename()) + "/roster.tsv " +
                        std::to_string(rows) + " rows\n");
      }
    }
    std::sort(games.begin(), games.end());
    for (const std::string& line : games) summary << line;
    if (games.empty()) summary << "Import rosters: none\n";
  }
  return summary.str();
}

// Content list for bug reports (GitHub #144: duplicate add-on in two accounts). Names, file counts and sizes
// only, paths relative to userdata (no user name). Never throws; each package walk is capped.
std::string ContentList(const Paths& paths) {
  std::ostringstream text;
  text << "Launcher version: " << WWE2K14_LAUNCHER_VERSION << " (build " << WWE2K14_LAUNCHER_BUILD_COMMIT << ")\n";
  text << "account | kind | folder | files | bytes\n";
  constexpr uint64_t kWalkCap = 20000;
  const char* accounts[] = {"0000000000000000", "B13EBABEBABEBABE"};
  const std::pair<const char*, const char*> kinds[] = {
      {"545408B2/00000002", "add-on"}, {"545408B2/00000001", "saves"}, {"545408B2/000B0000", "title-update"},
      {"545408B2/Headers/00000002", "header"}};
  for (const char* account : accounts) {
    for (const auto& kind : kinds) {
      std::error_code error;
      const fs::path root = paths.userdata_dir / account / kind.first;
      std::vector<fs::path> children;
      fs::directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error);
      if (error) continue;
      for (const auto& entry : iterator) children.push_back(entry.path());
      std::sort(children.begin(), children.end());
      for (const fs::path& child : children) {
        uint64_t files = 0, bytes = 0;
        bool capped = false;
        error.clear();
        if (fs::is_directory(child, error) && !error) {
          fs::recursive_directory_iterator walk(child, fs::directory_options::skip_permission_denied, error);
          const fs::recursive_directory_iterator end;
          while (!error && walk != end) {
            std::error_code item_error;
            if (walk->is_regular_file(item_error) && !item_error) {
              if (++files > kWalkCap) { capped = true; --files; break; }
              const auto size = walk->file_size(item_error);
              if (!item_error) bytes += size;
            }
            walk.increment(error);
          }
        } else {
          error.clear();
          const auto size = fs::file_size(child, error);
          files = 1;
          if (!error) bytes = size;
        }
        text << account << " | " << kind.second << " | " << internal::PathToUtf8(child.filename()) << " | " << files
             << (capped ? "+" : "") << " | " << bytes << "\n";
      }
    }
  }
  return text.str();
}

// Cap for the missing-shader dumps attached to a session report.
constexpr size_t kShaderDumpFiles = 64;
constexpr uint64_t kShaderDumpBytes = 2u << 20;

}  // namespace

std::string DescribeExitNote(std::string_view note_text) {
  const size_t at = note_text.find("exit_code=0x");
  if (at == std::string_view::npos) return "Game exit: unknown (no exit code recorded)\n";
  const std::string digits(note_text.substr(at + 12, 8));
  const unsigned long code = std::strtoul(digits.c_str(), nullptr, 16);
  const char* words = "unknown code";
  switch (code) {
    case 0x00000000: words = "normal close"; break;
    case 0x00000001: words = "closed from Task Manager / forced"; break;
    case 0xC0000005: words = "crash (memory access)"; break;
    case 0xC00000FD: words = "crash (stack overflow)"; break;
    case 0xC0000409: words = "crash (fast fail / security check)"; break;
    case 0xC0000017: case 0xC000012D: words = "out of memory"; break;
    case 0xC0000374: words = "heap corruption"; break;
    case 0xE06D7363: words = "C++ exception"; break;
    case 0x40010004: words = "killed (debugger/console close)"; break;
    default: break;
  }
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "Game exit: 0x%08lX - %s\n", code, words);
  return buffer;
}

Result SaveBugReport(const Paths& paths, fs::path* created_zip, const fs::path& session_log) {
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
    std::string session_stamp;
    if (!session_log.empty()) {
      // One game session (Bug Reports page / crash watcher): its log, its timeline and freeze reports, and both
      // launcher logs. The game log is the evidence; the newest-three window would miss it after later starts.
      const GameSession session = ReadGameSession(paths, session_log);
      const std::string name = internal::PathToUtf8(session_log.filename());
      if (name.size() >= 23) session_stamp = name.substr(8, 15);  // wwe2k14-YYYYMMDD-HHMMSS.log
      std::vector<fs::path> wanted{session_log};
      if (!session.timeline.empty()) wanted.push_back(session.timeline);
      wanted.insert(wanted.end(), session.freeze_reports.begin(), session.freeze_reports.end());
      wanted.push_back(paths.logs_dir / "launcher.log");
      wanted.push_back(paths.logs_dir / "launcher-previous.log");
      for (const fs::path& file : wanted) {
        error.clear();
        fs::directory_entry entry(file, error);
        if (!error && entry.is_regular_file(error) && !error) selected_entries.push_back(entry);
      }
      if (selected_entries.empty() || selected_entries.front().path() != session_log) {
        return Result::Fail("That game session's log is no longer in the logs folder.");
      }
    }
    const size_t selected = session_log.empty() ? std::min<size_t>(3, logs.size()) : 0;
    for (size_t i = 0; i < selected; ++i) selected_entries.push_back(logs[i]);
    // The two newest game timelines are always included too (card LOG1): the two launcher logs and the game log
    // otherwise fill the newest-three window (owner laptop test 2026-10-31).
    size_t timelines = 0;
    for (const auto& candidate : session_log.empty() ? logs : std::vector<fs::directory_entry>{}) {
      const std::string filename = internal::PathToUtf8(candidate.path().filename());
      const bool freeze_report = filename.starts_with("wwe2k14-freeze-") &&
                                 filename.ends_with(".txt");
      const bool timeline = filename.starts_with("wwe2k14-timeline-") && filename.ends_with(".txt");
      if (timeline && timelines >= 2) continue;
      if (timeline) ++timelines;
      const bool previous_launcher_log = filename == "launcher-previous.log";  // the session that started the game
      if (!freeze_report && !timeline && !previous_launcher_log) continue;
      const bool already = std::any_of(selected_entries.begin(), selected_entries.end(),
                                       [&](const fs::directory_entry& entry) {
                                         return entry.path() == candidate.path();
                                       });
      if (!already) selected_entries.push_back(candidate);
    }
    std::vector<internal::ZipEntry> entries;
    // Crash watcher's exit note for the session (newest note when no session was picked), decoded in session-end.txt.
    {
      fs::path note;
      if (!session_log.empty()) {
        note = paths.logs_dir / "crash-watch" / (internal::PathToUtf8(session_log.filename()) + ".exit");
      } else {
        std::error_code note_error;
        fs::file_time_type newest{};
        fs::directory_iterator watch(paths.logs_dir / "crash-watch", fs::directory_options::skip_permission_denied,
                                     note_error);
        for (; !note_error && watch != fs::directory_iterator(); watch.increment(note_error)) {
          std::error_code item_error;
          if (!watch->path().filename().string().ends_with(".exit")) continue;
          const auto time = watch->last_write_time(item_error);
          if (!item_error && (note.empty() || time > newest)) { newest = time; note = watch->path(); }
        }
      }
      std::error_code note_error;
      std::vector<uint8_t> bytes;
      if (!note.empty() && fs::is_regular_file(note, note_error) && !note_error &&
          internal::ReadBinaryFile(note, bytes)) {
        const std::string text(bytes.begin(), bytes.end());
        internal::ZipEntry raw;
        raw.name = "logs/crash-watch/" + internal::PathToUtf8(note.filename());
        raw.bytes = bytes;
        entries.push_back(std::move(raw));
        const std::string log_name = internal::PathToUtf8(note.filename()).substr(
            0, internal::PathToUtf8(note.filename()).size() - 5);
        std::string decoded = "Game log: " + log_name + "\n" + DescribeExitNote(text);
        // The exit code alone reads "crash" after a normal close too (the known exit crash, GitHub #193 report):
        // add how the launcher's session list classifies the run.
        std::error_code log_error;
        const fs::path game_log = paths.logs_dir / internal::PathFromUtf8(log_name);
        if (fs::is_regular_file(game_log, log_error) && !log_error) {
          const GameSession session = ReadGameSession(paths, game_log);
          decoded += session.end == SessionEnd::kClosed    ? "Session: closed by the player (a crash code after that is the known exit crash)\n"
                     : session.end == SessionEnd::kCrashed ? "Session: crashed while playing\n"
                     : session.end == SessionEnd::kFroze   ? "Session: froze (freeze report written)\n"
                                                           : "Session: no ending in the log (forced close, power loss or still running)\n";
        }
        internal::ZipEntry end;
        end.name = "session-end.txt";
        end.bytes.assign(decoded.begin(), decoded.end());
        entries.push_back(std::move(end));
      }
    }
    // The session's missing-shader dumps (logs/nr-out-<stamp>/missing-shaders), capped.
    if (!session_stamp.empty()) {
      std::error_code dump_error;
      const fs::path dumps = paths.logs_dir / ("nr-out-" + session_stamp) / "missing-shaders";
      std::vector<fs::path> files;
      fs::directory_iterator walk(dumps, fs::directory_options::skip_permission_denied, dump_error);
      for (; !dump_error && walk != fs::directory_iterator(); walk.increment(dump_error)) {
        std::error_code item_error;
        if (walk->is_regular_file(item_error) && !item_error) files.push_back(walk->path());
      }
      std::sort(files.begin(), files.end());
      size_t taken = 0, skipped = 0;
      uint64_t total = 0;
      for (const fs::path& file : files) {
        std::error_code size_error;
        const uint64_t size = fs::file_size(file, size_error);
        internal::ZipEntry dump;
        if (size_error || taken >= kShaderDumpFiles || total + size > kShaderDumpBytes ||
            !internal::ReadBinaryFile(file, dump.bytes)) {
          ++skipped;
          continue;
        }
        dump.name = "missing-shaders/" + internal::PathToUtf8(file.filename());
        total += size;
        ++taken;
        entries.push_back(std::move(dump));
      }
      if (skipped) {
        const std::string note = std::to_string(skipped) + " missing-shader dump(s) skipped (cap " +
                                 std::to_string(kShaderDumpFiles) + " files / 2 MB)\n";
        internal::ZipEntry skipped_entry;
        skipped_entry.name = "missing-shaders/SKIPPED.txt";
        skipped_entry.bytes.assign(note.begin(), note.end());
        entries.push_back(std::move(skipped_entry));
      }
    }
    for (const auto& selected_entry : selected_entries) {
      internal::ZipEntry entry;
      entry.name = "logs/" + internal::PathToUtf8(selected_entry.path().filename());
      if (!internal::ReadBinaryFile(selected_entry.path(), entry.bytes)) {
        return Result::Fail("The launcher could not read the latest game logs.");
      }
      entries.push_back(std::move(entry));
    }

    // Keep capture markers in bug reports even if later launches have pushed the source log out of the newest-three
    // window. Only marker text is copied; the captures/ directory (including .rdc files) is never traversed.
    std::string capture_markers;
    for (const auto& candidate : logs) {
      if (!internal::PathToUtf8(candidate.path().filename()).ends_with(".log")) continue;
      std::ifstream input(candidate.path(), std::ios::binary);
      std::string line;
      while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find("[glitch-capture]") != std::string::npos) capture_markers += line + "\n";
      }
    }
    if (!capture_markers.empty()) {
      internal::ZipEntry markers;
      markers.name = "logs/glitch-capture-markers.txt";
      markers.bytes.assign(capture_markers.begin(), capture_markers.end());
      entries.push_back(std::move(markers));
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
               << " | Vulkan index " << gpu.vulkan_index << " | driver " << gpu.driver << " | video memory "
               << gpu.vram_mb << " MB\n";
    }
    if (gpu_text.str().empty()) gpu_text << "No Vulkan graphics devices were listed.\n";
    internal::ZipEntry gpus;
    gpus.name = "gpu-list.txt";
    const std::string gpu_contents = gpu_text.str();
    gpus.bytes.assign(gpu_contents.begin(), gpu_contents.end());
    entries.push_back(std::move(gpus));

    internal::ZipEntry system;
    system.name = "system.txt";
    const std::string summary = SystemSummary(paths);
    system.bytes.assign(summary.begin(), summary.end());
    entries.push_back(std::move(system));

    internal::ZipEntry content;
    content.name = "content-list.txt";
    const std::string content_text = ContentList(paths);
    content.bytes.assign(content_text.begin(), content_text.end());
    entries.push_back(std::move(content));

    std::string stamp = internal::LocalTimestamp(std::chrono::system_clock::now(), "%Y%m%d-%H%M%S");
    fs::path destination = paths.exe_dir / ("wwe2k14-bug-report-" + stamp + ".zip");
    if (!session_stamp.empty()) destination = paths.exe_dir / ("wwe2k14-bug-report-game-" + session_stamp + ".zip");
    unsigned suffix = 2;
    error.clear();
    while (session_stamp.empty() && fs::exists(destination, error) && !error) {
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
