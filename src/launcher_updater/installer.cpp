#include "update.h"

#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wwe2k14::updates {
namespace {
using Json = nlohmann::json;
void SafeDestination(const fs::path& root, const fs::path& relative) {
  if (!AllowedFile(relative.generic_string())) throw std::runtime_error("The update contains an unmanaged destination.");
  fs::path current = root;
  for (const auto& part : relative) {
    current /= part;
    std::error_code error;
    const auto status = fs::symlink_status(current, error);
    if (error && error != std::errc::no_such_file_or_directory) throw fs::filesystem_error("Cannot inspect update destination", current, error);
    if (fs::is_symlink(status)) throw std::runtime_error("An update destination is a symbolic link.");
#ifdef _WIN32
    const DWORD attr = GetFileAttributesW(current.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
      throw std::runtime_error("An update destination is a reparse point.");
#endif
  }
}
void WriteJournal(const fs::path& path, const Json& value) {
  const std::string text = value.dump(2);
#ifdef _WIN32
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("The update journal could not be created.");
  DWORD written = 0;
  const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                  written == text.size() && FlushFileBuffers(file);
  CloseHandle(file);
#else
  const int file = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
  if (file < 0) throw std::runtime_error("The update journal could not be created.");
  size_t done = 0;
  while (done < text.size()) {
    const ssize_t wrote = ::write(file, text.data() + done, text.size() - done);
    if (wrote < 0 && errno == EINTR) continue;
    if (wrote <= 0) break;
    done += static_cast<size_t>(wrote);
  }
  const bool ok = done == text.size() && fsync(file) == 0;
  ::close(file);
#endif
  if (!ok) throw std::runtime_error("The update journal could not be saved. No installed files were changed.");
}
Json ReadJournal(const fs::path& stage) {
  std::ifstream input(stage / "journal.json");
  Json json;
  input >> json;
  if (!json.is_array() || json.size() > 2048) throw std::runtime_error("The update journal is invalid.");
  return json;
}
std::string Utf8(const fs::path& path) {
  const auto value = path.u8string();
  return std::string(value.begin(), value.end());
}
bool Same(const fs::path& a, const fs::path& b) {
  std::error_code e;
  return fs::equivalent(a, b, e) && !e;
}
class InstallLock {
 public:
#ifdef _WIN32
  HANDLE file = INVALID_HANDLE_VALUE;
  explicit InstallLock(const fs::path& root) {
    file = CreateFileW((root / ".updates/install.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Another update is already running.");
  }
  ~InstallLock() { CloseHandle(file); }
#else
  int file = -1;
  explicit InstallLock(const fs::path& root) {
    file = ::open((root / ".updates/install.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (file < 0 || flock(file, LOCK_EX | LOCK_NB) != 0) {
      if (file >= 0) ::close(file);
      throw std::runtime_error("Another update is already running.");
    }
  }
  ~InstallLock() { ::close(file); }
#endif
};
void Start(const fs::path& program, const std::vector<std::string>& arguments) {
#ifdef _WIN32
  auto quote = [](const std::wstring& value) {
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : value) {
      if (c == L'\\') { ++slashes; continue; }
      result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
      slashes = 0;
      result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
  };
  std::wstring command = quote(program.wstring());
  for (const auto& arg : arguments) command += L" " + quote(FromUtf8(arg).wstring());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      program.parent_path().c_str(), &startup, &process))
    throw std::runtime_error("The update helper or launcher could not be started.");
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
#else
  // CLOEXEC pipe reports exec failure to the launcher before it exits. No shell interpolation.
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) != 0) throw std::runtime_error("The update helper could not be started.");
  std::vector<std::string> storage{program.string()};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& arg : storage) argv.push_back(arg.data());
  argv.push_back(nullptr);
  const pid_t pid = fork();
  if (pid == 0) {
    close(pipefd[0]);
    execv(program.c_str(), argv.data());
    const int error = errno;
    (void)write(pipefd[1], &error, sizeof(error));
    _exit(127);
  }
  close(pipefd[1]);
  int error = 0;
  ssize_t count;
  do { count = read(pipefd[0], &error, sizeof(error)); } while (count < 0 && errno == EINTR);
  close(pipefd[0]);
  if (pid < 0 || count != 0) throw std::runtime_error("The update helper or launcher could not be started.");
#endif
}
}

bool GameRunning(const fs::path& root) {
#ifdef _WIN32
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Running games could not be checked.");
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool found = false;
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    if (_wcsicmp(entry.szExeFile, L"wwe2k14.exe") != 0) continue;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
    if (!process) { found = true; break; } // Refuse when a matching game cannot be inspected.
    std::array<wchar_t, 32768> name{};
    DWORD length = static_cast<DWORD>(name.size());
    const bool queried = QueryFullProcessImageNameW(process, 0, name.data(), &length) != 0;
    found = !queried || Same(fs::path(std::wstring(name.data(), length)), root / "wwe2k14.exe");
    CloseHandle(process);
    if (found) break;
  }
  CloseHandle(snapshot);
  return found;
#else
  std::error_code error;
  fs::directory_iterator processes("/proc", error);
  if (error) throw std::runtime_error("Running games could not be checked.");
  for (const auto& process : processes) {
    const std::string pid = process.path().filename().string();
    if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
    const fs::path executable = fs::read_symlink(process.path() / "exe", error);
    if (error) { error.clear(); continue; }
    if (Same(executable, root / "Linux-x86_64/wwe2k14") || Same(executable, root / "wwe2k14")) return true;
  }
  return false;
#endif
}
bool InstallInProgress(const fs::path& root) {
  if (!fs::exists(root / ".updates/install.lock")) return false;
  try { InstallLock lock(root); return false; } catch (...) { return true; }
}
void WithInstallLock(const fs::path& root, const std::function<void()>& action) {
  const fs::path dir = root / ".updates";
  if (fs::is_symlink(fs::symlink_status(dir))) throw std::runtime_error("The update directory is unsafe.");
  fs::create_directories(dir);
  InstallLock lock(root);
  action();
}
void Rollback(const fs::path& stage, const fs::path& root) {
  if (!fs::exists(stage / "journal.json") || fs::exists(stage / "committed") || fs::exists(stage / "rolled-back")) return;
  const Json journal = ReadJournal(stage);
  for (auto it = journal.rbegin(); it != journal.rend(); ++it) {
    const fs::path relative = FromUtf8(it->at("path").get<std::string>());
    SafeDestination(root, relative);
    SafeDestination(stage / "backup", relative);
    const fs::path backup = stage / "backup" / relative;
    const fs::path destination = root / relative;
    if (fs::is_regular_file(backup)) {
      if (fs::exists(destination)) {
        if (!fs::is_regular_file(destination)) throw std::runtime_error("Rollback found a non-file destination.");
        fs::remove(destination);
      }
      fs::rename(backup, destination);
    } else if (!it->at("old").get<bool>() && !fs::exists(stage / "files" / relative) && fs::is_regular_file(destination)) {
      fs::remove(destination);
    }
  }
  WriteJournal(stage / "rolled-back", Json{{"restored", true}});
}
void Apply(const fs::path& stage, const fs::path& root) {
  if (fs::exists(stage / "journal.json")) throw std::runtime_error("This update transaction has already started.");
  Json journal = Json::array();
  for (const auto& entry : fs::recursive_directory_iterator(stage / "files")) {
    if (entry.is_symlink()) throw std::runtime_error("The staged update contains a symbolic link.");
    if (entry.is_directory()) continue;
    if (!entry.is_regular_file()) throw std::runtime_error("The staged update contains a special file.");
    const fs::path relative = entry.path().lexically_relative(stage / "files");
    SafeDestination(root, relative);
    if (fs::exists(root / relative) && !fs::is_regular_file(root / relative))
      throw std::runtime_error("An installed program file is a directory or special file.");
    journal.push_back({{"path", relative.generic_string()}, {"old", fs::exists(root / relative)}});
  }
  if (journal.empty()) throw std::runtime_error("The staged update is empty.");
  fs::create_directory(stage / "backup");
  WriteJournal(stage / "journal.json", journal);
  try {
    for (const auto& item : journal) {
      const fs::path relative = FromUtf8(item.at("path").get<std::string>());
      SafeDestination(root, relative);
      fs::create_directories((root / relative).parent_path());
      if (item.at("old").get<bool>()) {
        fs::create_directories((stage / "backup" / relative).parent_path());
        fs::rename(root / relative, stage / "backup" / relative);
      }
      fs::rename(stage / "files" / relative, root / relative);
    }
    WriteJournal(stage / "committed", Json{{"installed", true}});
  } catch (...) {
    Rollback(stage, root);
    throw;
  }
}
void StartInstaller(const fs::path& stage, const fs::path& launcher) {
  const fs::path root = InstallRoot(launcher);
  if (GameRunning(root) || InstallInProgress(root)) throw std::runtime_error("Close the game and any other updater before installing.");
  if (!Same(stage.parent_path(), root / ".updates")) throw std::runtime_error("The update staging location is invalid.");
#ifdef _WIN32
  const auto helper = stage / "update-helper.exe";
  const auto pid = GetCurrentProcessId();
#else
  const auto helper = stage / "update-helper";
  const auto pid = getpid();
#endif
  fs::copy_file(launcher, helper, fs::copy_options::overwrite_existing);
  Start(helper, {"--apply-update", Utf8(stage), Utf8(root), Utf8(fs::absolute(launcher)), std::to_string(pid)});
}
int HelperMain(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) != "--apply-update") return -1;
  if (argc != 6) return 2;
  const fs::path stage = FromUtf8(argv[2]), root = FromUtf8(argv[3]), launcher = FromUtf8(argv[4]);
  try {
    if (!stage.is_absolute() || !root.is_absolute() || !launcher.is_absolute() ||
        !Same(stage.parent_path(), root / ".updates") || !Same(InstallRoot(launcher), root) ||
        fs::is_symlink(fs::symlink_status(stage)) || fs::is_symlink(fs::symlink_status(root / ".updates")))
      throw std::runtime_error("The update paths are invalid.");
    unsigned long parent = 0;
    size_t consumed = 0;
    parent = std::stoul(argv[5], &consumed);
    if (consumed != std::string(argv[5]).size() || parent < 2 || parent > 0x7fffffff)
      throw std::runtime_error("The update parent process is invalid.");
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parent));
    if (process) {
      const DWORD wait = WaitForSingleObject(process, 30000);
      CloseHandle(process);
      if (wait != WAIT_OBJECT_0) throw std::runtime_error("The launcher did not close. Please try again.");
    }
#else
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (kill(static_cast<pid_t>(parent), 0) == 0 || errno == EPERM) {
      if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("The launcher did not close. Please try again.");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
    {
      InstallLock lock(root);
      if (GameRunning(root)) throw std::runtime_error("The game is running. Close it before updating.");
      // Restore any interrupted transaction before beginning a new one.
      for (const auto& entry : fs::directory_iterator(root / ".updates")) {
        if (entry.is_directory() && !entry.is_symlink() && entry.path() != stage) Rollback(entry.path(), root);
      }
      Apply(stage, root);
    }
    Start(launcher, {"--updated"});
    return 0;
  } catch (const std::exception& error) {
    std::ofstream(stage / "update-error.txt") << error.what() << '\n';
#ifdef _WIN32
    MessageBoxW(nullptr, FromUtf8(error.what()).c_str(), L"WWE 2K14 update", MB_OK | MB_ICONERROR);
#endif
    // A failed replacement is rolled back by Apply; restarting remains a player choice.
    return 1;
  }
}
}  // namespace wwe2k14::updates
