#include "update.h"

#include <array>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace wwe2k14::updates::detail {
namespace {
struct Sink {
  std::ofstream output;
  uint64_t done = 0, limit;
  std::atomic<bool>& cancel;
  const Progress& progress;
  bool Write(const char* data, size_t size) {
    if (cancel.load() || size > limit - done) return false;
    output.write(data, static_cast<std::streamsize>(size));
    done += size;
    return output.good();
  }
};
#ifdef _WIN32
struct InternetHandle {
  HINTERNET value;
  explicit InternetHandle(HINTERNET v) : value(v) { if (!v) throw std::runtime_error("The update server could not be reached."); }
  ~InternetHandle() { WinHttpCloseHandle(value); }
  operator HINTERNET() const { return value; }
};
std::wstring Wide(const std::string& text) { return FromUtf8(text).wstring(); }
#endif
}

int Get(const std::string& url, const fs::path& file, uint64_t limit,
        std::atomic<bool>& cancel, const Progress& progress) {
  if (!url.starts_with("https://")) throw std::runtime_error("Updates require HTTPS.");
  Sink sink{std::ofstream(file, std::ios::binary | std::ios::trunc), 0, limit, cancel, progress};
  if (!sink.output) throw std::runtime_error("The update download could not be saved.");
  int status = 0;
#ifdef _WIN32
  std::wstring address = Wide(url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
    throw std::runtime_error("The update URL is invalid.");
  InternetHandle session(WinHttpOpen(L"WWE2K14-Updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  WinHttpSetTimeouts(session, 5000, 5000, 10000, 15000);
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  InternetHandle connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
  InternetHandle request(WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  // Default policy refuses HTTPS -> HTTP redirects; certificates remain fully validated.
  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
  if (!WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1),
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr))
    throw std::runtime_error("The update server could not be reached.");
  DWORD code = 0, length = sizeof(code);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &code, &length, WINHTTP_NO_HEADER_INDEX))
    throw std::runtime_error("The update response could not be read.");
  status = static_cast<int>(code);
  if (status == 200) {
    std::array<char, 65536> buffer{};
    const auto started = GetTickCount64();
    for (;;) {
      if (cancel.load() || GetTickCount64() - started > (limit <= 4 * 1024 * 1024 ? 15000 : 300000))
        throw std::runtime_error("The update download was cancelled or timed out.");
      DWORD read = 0;
      if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
        throw std::runtime_error("The update download was interrupted.");
      if (!read) break;
      if (!sink.Write(buffer.data(), read)) throw std::runtime_error("The update download was cancelled, too large, or could not be saved.");
      if (progress) progress(sink.done, limit);
    }
  }
#else
  static const bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  if (!initialized) throw std::runtime_error("HTTPS could not be initialized.");
  CURL* handle = curl_easy_init();
  if (!handle) throw std::runtime_error("HTTPS could not be initialized.");
  struct Guard { CURL* h; ~Guard() { curl_easy_cleanup(h); } } guard{handle};
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "WWE2K14-Updater/1");
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT, limit <= 4 * 1024 * 1024 ? 15L : 300L);
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 20L);
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t count, void* user) -> size_t {
    auto& target = *static_cast<Sink*>(user);
    if (count && size > SIZE_MAX / count) return 0;
    return target.Write(data, size * count) ? size * count : 0;
  });
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION,
    +[](void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) -> int {
      auto& target = *static_cast<Sink*>(user);
      if (target.cancel.load()) return 1;
      try { if (target.progress) target.progress(static_cast<uint64_t>(now), static_cast<uint64_t>(total)); }
      catch (...) { return 1; }
      return 0;
    });
  curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &sink);
  const CURLcode result = curl_easy_perform(handle);
  long code = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
  status = static_cast<int>(code);
  if (result != CURLE_OK) throw std::runtime_error("The update download was interrupted, cancelled, or could not be saved.");
#endif
  sink.output.close();
  if (!sink.output) throw std::runtime_error("The update download could not be saved.");
  return status;
}
}  // namespace wwe2k14::updates::detail
