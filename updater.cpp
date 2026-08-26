#include "updater.h"

#include <windows.h>
#include <winhttp.h>

#include "version.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

constexpr char kRepoApiUrl[] =
    "https://api.github.com/repos/pdx15/Diablo-4-Rotation-Spam/releases/latest";
constexpr wchar_t kLatestReleaseUrl[] =
    L"https://github.com/pdx15/Diablo-4-Rotation-Spam/releases/latest";

struct ReleaseInfo {
  std::string tagName;
  std::string htmlUrl;
  std::string downloadUrl;
  std::string assetName;
};

struct UpdaterState {
  UpdatePhase phase = UpdatePhase::Idle;
  std::string message = "Ready";
  std::string latestVersion;
  std::string releaseUrl;
  std::string downloadUrl;
  std::wstring downloadedPath;
  bool canInstall = false;
};

std::mutex g_updateMutex;
UpdaterState g_updateState;

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return {};
  int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                 static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                      out.data(), size);
  return out;
}

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                static_cast<int>(text.size()), nullptr, 0,
                                nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring GetLastErrorText() {
  DWORD err = GetLastError();
  if (err == 0) return L"unknown error";

  wchar_t* buffer = nullptr;
  DWORD size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

  std::wstring text =
      (size > 0 && buffer) ? std::wstring(buffer, size) : L"unknown error";
  if (buffer) LocalFree(buffer);
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                           text.back() == L' ')) {
    text.pop_back();
  }
  return text;
}

void SetState(UpdatePhase phase, const std::string& message) {
  std::lock_guard<std::mutex> lock(g_updateMutex);
  g_updateState.phase = phase;
  g_updateState.message = message;
}

bool IsBusyPhase(UpdatePhase phase) {
  return phase == UpdatePhase::Checking || phase == UpdatePhase::Downloading ||
         phase == UpdatePhase::Installing;
}

std::optional<std::string> HttpGetString(const std::string& url,
                                         std::string& error);
bool DownloadUrlToFile(const std::string& url, const std::wstring& path,
                       std::string& error);

bool CrackUrl(const std::string& url, std::wstring& host, std::wstring& path,
              INTERNET_PORT& port, bool& secure, std::string& error) {
  std::wstring wideUrl = Utf8ToWide(url);
  URL_COMPONENTSW parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);

  if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
    error = "Invalid update URL: " + WideToUtf8(GetLastErrorText());
    return false;
  }

  secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  port = parts.nPort;
  host.assign(parts.lpszHostName, parts.dwHostNameLength);
  path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength > 0) {
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }

  return !host.empty() && !path.empty();
}

HINTERNET OpenRequest(const std::string& url, HINTERNET& session,
                      HINTERNET& connect, std::string& error) {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = 0;
  bool secure = false;
  if (!CrackUrl(url, host, path, port, secure, error)) return nullptr;

  session = WinHttpOpen(L"d4rt/" APP_VERSION_STR,
                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    error = "WinHTTP session failed: " + WideToUtf8(GetLastErrorText());
    return nullptr;
  }

  connect = WinHttpConnect(session, host.c_str(), port, 0);
  if (!connect) {
    error = "WinHTTP connect failed: " + WideToUtf8(GetLastErrorText());
    return nullptr;
  }

  DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request =
      WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                         flags);
  if (!request) {
    error = "WinHTTP request failed: " + WideToUtf8(GetLastErrorText());
    return nullptr;
  }

  const wchar_t* headers =
      L"Accept: application/vnd.github+json\r\n"
      L"X-GitHub-Api-Version: 2022-11-28\r\n";
  WinHttpAddRequestHeaders(request, headers, static_cast<DWORD>(-1),
                           WINHTTP_ADDREQ_FLAG_ADD);
  return request;
}

bool SendAndCheckStatus(HINTERNET request, std::string& error) {
  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    error = "HTTP request failed: " + WideToUtf8(GetLastErrorText());
    return false;
  }

  DWORD statusCode = 0;
  DWORD statusSize = sizeof(statusCode);
  if (!WinHttpQueryHeaders(request,
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &statusCode,
                           &statusSize, WINHTTP_NO_HEADER_INDEX)) {
    error = "HTTP status read failed: " + WideToUtf8(GetLastErrorText());
    return false;
  }

  if (statusCode < 200 || statusCode >= 300) {
    error = "GitHub returned HTTP " + std::to_string(statusCode);
    return false;
  }

  return true;
}

std::optional<std::string> HttpGetString(const std::string& url,
                                         std::string& error) {
  HINTERNET session = nullptr;
  HINTERNET connect = nullptr;
  HINTERNET request = OpenRequest(url, session, connect, error);

  std::optional<std::string> result;
  if (request && SendAndCheckStatus(request, error)) {
    std::string data;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
      std::string chunk(available, '\0');
      DWORD read = 0;
      if (!WinHttpReadData(request, chunk.data(), available, &read)) {
        error = "HTTP read failed: " + WideToUtf8(GetLastErrorText());
        data.clear();
        break;
      }
      chunk.resize(read);
      data += chunk;
    }
    if (!data.empty()) result = data;
  }

  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  if (session) WinHttpCloseHandle(session);
  return result;
}

bool DownloadUrlToFile(const std::string& url, const std::wstring& path,
                       std::string& error) {
  HINTERNET session = nullptr;
  HINTERNET connect = nullptr;
  HINTERNET request = OpenRequest(url, session, connect, error);

  bool ok = false;
  HANDLE file = INVALID_HANDLE_VALUE;
  if (request && SendAndCheckStatus(request, error)) {
    file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      error = "Cannot create update file: " + WideToUtf8(GetLastErrorText());
    } else {
      ok = true;
      DWORD available = 0;
      while (ok && WinHttpQueryDataAvailable(request, &available) &&
             available > 0) {
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) {
          error = "Download read failed: " + WideToUtf8(GetLastErrorText());
          ok = false;
          break;
        }

        DWORD written = 0;
        if (!WriteFile(file, buffer.data(), read, &written, nullptr) ||
            written != read) {
          error = "Download write failed: " + WideToUtf8(GetLastErrorText());
          ok = false;
          break;
        }
      }
    }
  }

  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  if (!ok) DeleteFileW(path.c_str());
  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  if (session) WinHttpCloseHandle(session);
  return ok;
}

std::optional<std::string> ExtractJsonString(const std::string& json,
                                             const std::string& key,
                                             size_t startAt = 0) {
  std::string marker = "\"" + key + "\"";
  size_t keyPos = json.find(marker, startAt);
  if (keyPos == std::string::npos) return std::nullopt;
  size_t colon = json.find(':', keyPos + marker.size());
  if (colon == std::string::npos) return std::nullopt;
  size_t quote = json.find('"', colon + 1);
  if (quote == std::string::npos) return std::nullopt;

  std::string value;
  bool escaped = false;
  for (size_t i = quote + 1; i < json.size(); ++i) {
    char c = json[i];
    if (escaped) {
      switch (c) {
        case 'n':
          value.push_back('\n');
          break;
        case 'r':
          value.push_back('\r');
          break;
        case 't':
          value.push_back('\t');
          break;
        default:
          value.push_back(c);
          break;
      }
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      return value;
    } else {
      value.push_back(c);
    }
  }

  return std::nullopt;
}

std::string ToLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
  if (text.size() < suffix.size()) return false;
  return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin());
}

std::optional<ReleaseInfo> ParseReleaseInfo(const std::string& json) {
  ReleaseInfo info;
  auto tag = ExtractJsonString(json, "tag_name");
  if (!tag || tag->empty()) return std::nullopt;
  info.tagName = *tag;
  info.htmlUrl = ExtractJsonString(json, "html_url").value_or("");

  std::vector<std::string> urls;
  size_t pos = 0;
  while (true) {
    auto url = ExtractJsonString(json, "browser_download_url", pos);
    if (!url) break;
    urls.push_back(*url);
    size_t found = json.find(*url, pos);
    pos = (found == std::string::npos) ? pos + 1 : found + url->size();
    if (pos >= json.size()) break;
  }

  auto choose = [&](const std::string& ext) -> std::optional<std::string> {
    for (const std::string& url : urls) {
      std::string lower = ToLower(url);
      if (EndsWith(lower, ext)) return url;
    }
    return std::nullopt;
  };

  auto selected = choose(".exe");
  if (!selected) selected = choose(".zip");
  if (!selected && !urls.empty()) selected = urls.front();
  if (selected) {
    info.downloadUrl = *selected;
    size_t slash = info.downloadUrl.find_last_of('/');
    info.assetName = slash == std::string::npos ? info.downloadUrl
                                                : info.downloadUrl.substr(slash + 1);
  }

  return info;
}

std::vector<int> ParseVersionNumbers(const std::string& version) {
  std::vector<int> numbers;
  int current = 0;
  bool inNumber = false;
  for (char c : version) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      current = current * 10 + (c - '0');
      inNumber = true;
    } else if (inNumber) {
      numbers.push_back(current);
      current = 0;
      inNumber = false;
    }
  }
  if (inNumber) numbers.push_back(current);
  return numbers;
}

bool IsRemoteNewer(const std::string& remote, const std::string& current) {
  std::vector<int> left = ParseVersionNumbers(remote);
  std::vector<int> right = ParseVersionNumbers(current);
  if (left.empty() || right.empty()) return false;

  size_t count = std::max(left.size(), right.size());
  left.resize(count, 0);
  right.resize(count, 0);
  for (size_t i = 0; i < count; ++i) {
    if (left[i] != right[i]) return left[i] > right[i];
  }
  return false;
}

std::wstring GetTempUpdatePath(const std::string& assetName) {
  wchar_t tempDir[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tempDir);

  std::wstring wideName = Utf8ToWide(assetName);
  if (wideName.empty()) wideName = L"d4rt_update.exe";

  for (wchar_t& c : wideName) {
    if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
        c == L'"' || c == L'<' || c == L'>' || c == L'|') {
      c = L'_';
    }
  }

  return std::wstring(tempDir) + L"d4rt_" + wideName;
}

std::wstring QuoteCmdPath(const std::wstring& path) {
  std::wstring out = path;
  for (wchar_t& c : out) {
    if (c == L'"') c = L'\'';
  }
  return out;
}

std::wstring PowerShellLiteral(const std::wstring& text) {
  std::wstring out = L"'";
  for (wchar_t c : text) {
    if (c == L'\'') out += L"''";
    else out.push_back(c);
  }
  out += L"'";
  return out;
}

bool WriteInstallerScript(const std::wstring& assetPath,
                          std::wstring& scriptPath) {
  wchar_t tempDir[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tempDir);
  scriptPath = std::wstring(tempDir) + L"d4rt_apply_update.cmd";

  wchar_t exePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  std::wstring targetPath = exePath;
  std::wstring unpackDir = std::wstring(tempDir) + L"d4rt_update_unpack";
  DWORD pid = GetCurrentProcessId();

  std::wofstream out(scriptPath, std::ios::trunc);
  if (!out.is_open()) return false;

  std::wstring asset = QuoteCmdPath(assetPath);
  std::wstring target = QuoteCmdPath(targetPath);
  std::wstring unpack = QuoteCmdPath(unpackDir);
  std::wstring psCommand =
      L"Remove-Item -LiteralPath " + PowerShellLiteral(unpackDir) +
      L" -Recurse -Force -ErrorAction SilentlyContinue; "
      L"Expand-Archive -LiteralPath " +
      PowerShellLiteral(assetPath) + L" -DestinationPath " +
      PowerShellLiteral(unpackDir) +
      L" -Force; $exe = Get-ChildItem -LiteralPath " +
      PowerShellLiteral(unpackDir) +
      L" -Filter '*.exe' -Recurse | Select-Object -First 1; "
      L"if ($null -eq $exe) { exit 2 }; "
      L"Copy-Item -LiteralPath $exe.FullName -Destination " +
      PowerShellLiteral(targetPath) + L" -Force";

  out << L"@echo off\n";
  out << L"setlocal\n";
  out << L"set \"PID=" << pid << L"\"\n";
  out << L"set \"ASSET=" << asset << L"\"\n";
  out << L"set \"TARGET=" << target << L"\"\n";
  out << L"set \"WORK=" << unpack << L"\"\n";
  out << L":wait\n";
  out << L"tasklist /FI \"PID eq %PID%\" | find \"%PID%\" >nul\n";
  out << L"if not errorlevel 1 (\n";
  out << L"  timeout /t 1 /nobreak >nul\n";
  out << L"  goto wait\n";
  out << L")\n";
  out << L"if /I \"%ASSET:~-4%\"==\".zip\" (\n";
  out << L"  powershell -NoProfile -ExecutionPolicy Bypass -Command \""
      << psCommand << L"\"\n";
  out << L") else (\n";
  out << L"  copy /Y \"%ASSET%\" \"%TARGET%\" >nul\n";
  out << L")\n";
  out << L"start \"\" \"%TARGET%\"\n";
  out << L"del \"%ASSET%\" >nul 2>nul\n";
  out << L"rmdir /S /Q \"%WORK%\" >nul 2>nul\n";
  out << L"del \"%~f0\" >nul 2>nul\n";
  return true;
}

void DownloadReleaseAsset(const ReleaseInfo& info) {
  if (info.downloadUrl.empty()) {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    g_updateState.phase = UpdatePhase::Available;
    g_updateState.message = "Update available, but the release has no asset";
    g_updateState.canInstall = false;
    return;
  }

  SetState(UpdatePhase::Downloading, "Downloading update...");
  std::wstring path = GetTempUpdatePath(info.assetName);
  std::string error;
  if (!DownloadUrlToFile(info.downloadUrl, path, error)) {
    SetState(UpdatePhase::Failed, "Update download failed: " + error);
    return;
  }

  std::lock_guard<std::mutex> lock(g_updateMutex);
  g_updateState.phase = UpdatePhase::Downloaded;
  g_updateState.downloadedPath = path;
  g_updateState.canInstall = true;
  g_updateState.message = "Update downloaded. Restart to install.";
}

void CheckWorker(bool autoDownload) {
  SetState(UpdatePhase::Checking, "Checking GitHub releases...");

  std::string error;
  auto json = HttpGetString(kRepoApiUrl, error);
  if (!json) {
    SetState(UpdatePhase::Failed, "Update check failed: " + error);
    return;
  }

  auto info = ParseReleaseInfo(*json);
  if (!info) {
    SetState(UpdatePhase::Failed, "Cannot parse GitHub release info");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    g_updateState.latestVersion = info->tagName;
    g_updateState.releaseUrl = info->htmlUrl;
    g_updateState.downloadUrl = info->downloadUrl;
    g_updateState.canInstall = false;
    g_updateState.downloadedPath.clear();
  }

  if (!IsRemoteNewer(info->tagName, APP_VERSION_STR)) {
    SetState(UpdatePhase::UpToDate,
             "Current version is up to date (" APP_VERSION_STR ")");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    g_updateState.phase = UpdatePhase::Available;
    g_updateState.message =
        "Update available: " + info->tagName + " (current " APP_VERSION_STR ")";
  }

  if (autoDownload) DownloadReleaseAsset(*info);
}

}  // namespace

void StartUpdateCheck(bool autoDownload) {
  {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    if (IsBusyPhase(g_updateState.phase)) return;
    g_updateState.phase = UpdatePhase::Checking;
    g_updateState.message = "Checking GitHub releases...";
  }

  std::thread(CheckWorker, autoDownload).detach();
}

void StartUpdateDownload() {
  ReleaseInfo info;
  {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    if (IsBusyPhase(g_updateState.phase) ||
        g_updateState.phase != UpdatePhase::Available) {
      return;
    }
    info.tagName = g_updateState.latestVersion;
    info.htmlUrl = g_updateState.releaseUrl;
    info.downloadUrl = g_updateState.downloadUrl;
    size_t slash = info.downloadUrl.find_last_of('/');
    info.assetName = slash == std::string::npos
                         ? info.downloadUrl
                         : info.downloadUrl.substr(slash + 1);
    g_updateState.phase = UpdatePhase::Downloading;
    g_updateState.message = "Downloading update...";
  }

  std::thread(DownloadReleaseAsset, info).detach();
}

void InstallDownloadedUpdate() {
  std::wstring assetPath;
  {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    if (!g_updateState.canInstall || g_updateState.downloadedPath.empty())
      return;
    g_updateState.phase = UpdatePhase::Installing;
    g_updateState.message = "Restarting to install update...";
    assetPath = g_updateState.downloadedPath;
  }

  std::wstring scriptPath;
  if (!WriteInstallerScript(assetPath, scriptPath)) {
    SetState(UpdatePhase::Failed, "Cannot create update installer script");
    return;
  }

  std::wstring params = L"/c \"" + scriptPath + L"\"";
  HINSTANCE result = ShellExecuteW(nullptr, L"open", L"cmd.exe",
                                  params.c_str(), nullptr, SW_HIDE);
  if (reinterpret_cast<intptr_t>(result) <= 32) {
    SetState(UpdatePhase::Failed, "Cannot start update installer script");
    return;
  }

  ExitProcess(0);
}

void OpenLatestReleasePage() {
  ShellExecuteW(nullptr, L"open", kLatestReleaseUrl, nullptr, nullptr,
                SW_SHOWNORMAL);
}

UpdateStatus GetUpdateStatus() {
  std::lock_guard<std::mutex> lock(g_updateMutex);
  UpdateStatus status;
  status.phase = g_updateState.phase;
  status.message = g_updateState.message;
  status.latestVersion = g_updateState.latestVersion;
  status.hasDownload = !g_updateState.downloadUrl.empty();
  status.canInstall = g_updateState.canInstall;
  return status;
}

bool IsUpdateBusy() {
  std::lock_guard<std::mutex> lock(g_updateMutex);
  return IsBusyPhase(g_updateState.phase);
}
