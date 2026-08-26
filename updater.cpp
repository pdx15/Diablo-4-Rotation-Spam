#include "updater.h"

#include <shldisp.h>
#include <shellapi.h>
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
#include <cwctype>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")

namespace {

constexpr char kRepoApiUrl[] =
    "https://api.github.com/repos/pdx15/Diablo-4-Rotation-Spam/releases/latest";
constexpr wchar_t kLatestReleaseUrl[] =
    L"https://github.com/pdx15/Diablo-4-Rotation-Spam/releases/latest";

constexpr LONG kCopyHereNoUi = 1024 | 512 | 16 | 4;

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
  float downloadProgress = 0.0f;
  std::uint64_t downloadedBytes = 0;
  std::uint64_t totalBytes = 0;
};

std::mutex g_updateMutex;
UpdaterState g_updateState;

void DeleteOldUpdateBackups() {
  wchar_t curExe[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, curExe, MAX_PATH) == 0) return;
  std::wstring bakPath = std::wstring(curExe) + L".bak";
  DeleteFileW(bakPath.c_str());

  wchar_t tempDir[MAX_PATH] = {};
  if (GetTempPathW(MAX_PATH, tempDir) > 0) {
    WIN32_FIND_DATAW fd;
    std::wstring pattern = std::wstring(tempDir) + L"d4rt_*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
          std::wstring tempPath = std::wstring(tempDir) + fd.cFileName;
          DeleteFileW(tempPath.c_str());
        }
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }

    std::wstring zipPattern = std::wstring(tempDir) + L"d4rt_unzip_*";
    h = FindFirstFileW(zipPattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          std::wstring dirPath = std::wstring(tempDir) + fd.cFileName;
          SHFILEOPSTRUCTW op = {};
          op.wFunc = FO_DELETE;
          op.pFrom = dirPath.c_str();
          op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
          SHFileOperationW(&op);
        }
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }
}

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
  if (phase != UpdatePhase::Downloading) {
    g_updateState.downloadProgress = 0.0f;
    g_updateState.downloadedBytes = 0;
    g_updateState.totalBytes = 0;
  }
}

void SetDownloadProgress(std::uint64_t downloaded, std::uint64_t total) {
  std::lock_guard<std::mutex> lock(g_updateMutex);
  g_updateState.downloadedBytes = downloaded;
  g_updateState.totalBytes = total;
  if (total > 0) {
    g_updateState.downloadProgress = static_cast<float>(
        static_cast<double>(downloaded) / static_cast<double>(total));
  } else {
    g_updateState.downloadProgress = -1.0f;
  }
  if (total > 0) {
    g_updateState.message = "Downloading update... " +
        std::to_string(downloaded / 1024) + " KB / " +
        std::to_string(total / 1024) + " KB";
  } else {
    g_updateState.message = "Downloading update... " +
        std::to_string(downloaded / 1024) + " KB";
  }
}

bool IsBusyPhase(UpdatePhase phase) {
  return phase == UpdatePhase::Checking || phase == UpdatePhase::Downloading ||
         phase == UpdatePhase::Installing || phase == UpdatePhase::Restarting;
}

std::optional<std::string> HttpGetString(const std::string& url,
                                         std::string& error);
bool DownloadUrlToFile(const std::string& url, const std::wstring& path,
                       std::string& error,
                       void (*progressCb)(std::uint64_t, std::uint64_t));

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

  session =
      WinHttpOpen(L"d4rt/" APP_VERSION_STR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
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
  HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
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
  if (!WinHttpQueryHeaders(
          request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
          WINHTTP_NO_HEADER_INDEX)) {
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
                       std::string& error,
                       void (*progressCb)(std::uint64_t, std::uint64_t)) {
  HINTERNET session = nullptr;
  HINTERNET connect = nullptr;
  HINTERNET request = OpenRequest(url, session, connect, error);

  bool ok = false;
  HANDLE file = INVALID_HANDLE_VALUE;
  std::uint64_t totalBytes = 0;
  std::uint64_t downloaded = 0;

  if (request && SendAndCheckStatus(request, error)) {
    wchar_t contentLength[64] = {};
    DWORD clSize = sizeof(contentLength);
    if (WinHttpQueryHeadersW(request, WINHTTP_QUERY_CONTENT_LENGTH,
                             WINHTTP_HEADER_NAME_BY_INDEX, contentLength,
                             &clSize, WINHTTP_NO_HEADER_INDEX)) {
      totalBytes = static_cast<std::uint64_t>(_wtoi64(contentLength));
    }

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

        downloaded += static_cast<std::uint64_t>(read);
        if (progressCb) progressCb(downloaded, totalBytes);
      }

      if (ok && progressCb && totalBytes > 0 && downloaded >= totalBytes) {
        progressCb(totalBytes, totalBytes);
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

bool EndsWithI(const std::wstring& text, const std::wstring& suffix) {
  if (text.size() < suffix.size()) return false;
  std::wstring lowText = text, lowSuffix = suffix;
  std::transform(
      lowText.begin(), lowText.end(), lowText.begin(),
      [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  std::transform(
      lowSuffix.begin(), lowSuffix.end(), lowSuffix.begin(),
      [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return std::equal(lowSuffix.rbegin(), lowSuffix.rend(), lowText.rbegin());
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
    info.assetName = slash == std::string::npos
                         ? info.downloadUrl
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

bool FindFirstExeRecursive(const std::wstring& root, std::wstring& outExe) {
  std::wstring pattern = root + L"\\*";
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool found = false;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
        continue;
      if (FindFirstExeRecursive(root + L"\\" + fd.cFileName, outExe)) {
        found = true;
        break;
      }
    } else if (EndsWithI(fd.cFileName, L".exe")) {
      outExe = root + L"\\" + fd.cFileName;
      found = true;
      break;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return found;
}

bool ExtractZipFirstExe(const std::wstring& zipPath,
                        const std::wstring& destDir, std::wstring& outExe) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool comInit = SUCCEEDED(hr);
  bool ok = false;

  IShellDispatch* pShell = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IShellDispatch,
                                 reinterpret_cast<void**>(&pShell))) &&
      pShell) {
    Folder* pDest = nullptr;
    VARIANT vDest;
    VariantInit(&vDest);
    vDest.vt = VT_BSTR;
    vDest.bstrVal = SysAllocString(destDir.c_str());
    if (vDest.bstrVal && SUCCEEDED(pShell->NameSpace(vDest, &pDest)) && pDest) {
      Folder* pZip = nullptr;
      VARIANT vZip;
      VariantInit(&vZip);
      vZip.vt = VT_BSTR;
      vZip.bstrVal = SysAllocString(zipPath.c_str());
      if (vZip.bstrVal && SUCCEEDED(pShell->NameSpace(vZip, &pZip)) && pZip) {
        FolderItems* pItems = nullptr;
        if (SUCCEEDED(pZip->Items(&pItems)) && pItems) {
          VARIANT vItem;
          VariantInit(&vItem);
          vItem.vt = VT_DISPATCH;
          vItem.pdispVal = pItems;

          VARIANT vOpt;
          VariantInit(&vOpt);
          vOpt.vt = VT_I4;
          vOpt.lVal = kCopyHereNoUi;

          pDest->CopyHere(vItem, vOpt);
          VariantClear(&vItem);
          VariantClear(&vOpt);

          for (int i = 0; i < 120; ++i) {
            if (FindFirstExeRecursive(destDir, outExe)) {
              ok = true;
              break;
            }
            Sleep(250);
          }
          pItems->Release();
        }
        pZip->Release();
      }
      if (vZip.bstrVal) SysFreeString(vZip.bstrVal);
      pDest->Release();
    }
    if (vDest.bstrVal) SysFreeString(vDest.bstrVal);
    pShell->Release();
  }

  if (comInit) CoUninitialize();
  return ok;
}

void ReplaceAndRestart(const std::wstring& srcExe) {
  wchar_t curExe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, curExe, MAX_PATH);
  std::wstring target = curExe;

  std::wstring backup = target + L".bak";
  DeleteFileW(backup.c_str());
  if (!MoveFileExW(target.c_str(), backup.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    SetState(UpdatePhase::Failed,
             "Cannot prepare for update: " + WideToUtf8(GetLastErrorText()));
    return;
  }

  bool copied = false;
  for (int attempt = 0; attempt < 10 && !copied; ++attempt) {
    if (CopyFileW(srcExe.c_str(), target.c_str(), FALSE)) {
      copied = true;
    } else {
      Sleep(200);
    }
  }

  if (!copied) {
    MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
    SetState(UpdatePhase::Failed,
             "Cannot write update file: " + WideToUtf8(GetLastErrorText()));
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    g_updateState.phase = UpdatePhase::Restarting;
    g_updateState.message = "Update applied, restarting...";
  }

  std::wstring dir = target.substr(0, target.find_last_of(L"\\/"));
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (CreateProcessW(target.c_str(), nullptr, nullptr, nullptr, FALSE, 0,
                     nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }

  ExitProcess(0);
}

void RunFullUpdate(const ReleaseInfo& info) {
  if (info.downloadUrl.empty()) {
    SetState(UpdatePhase::Available,
             "Update available, but the release has no downloadable asset");
    OpenLatestReleasePage();
    return;
  }

  SetState(UpdatePhase::Downloading, "Downloading update...");
  std::wstring path = GetTempUpdatePath(info.assetName);
  std::string error;
  if (!DownloadUrlToFile(info.downloadUrl, path, error, SetDownloadProgress)) {
    SetState(UpdatePhase::Failed, "Update download failed: " + error);
    return;
  }

  SetState(UpdatePhase::Installing, "Installing update...");

  std::wstring srcExe = path;
  if (EndsWithI(path, L".zip")) {
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring extractDir = std::wstring(tempDir) + L"d4rt_unzip_" +
                              std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(extractDir.c_str(), nullptr);

    std::wstring innerExe;
    if (!ExtractZipFirstExe(path, extractDir, innerExe)) {
      SetState(UpdatePhase::Failed,
               "Cannot extract update archive. Opening release page.");
      ShellExecuteW(nullptr, L"open", extractDir.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
      OpenLatestReleasePage();
      return;
    }
    srcExe = innerExe;
  }

  ReplaceAndRestart(srcExe);
}

void CheckWorker() {
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
    g_updateState.downloadedPath.clear();
    g_updateState.downloadProgress = 0.0f;
    g_updateState.downloadedBytes = 0;
    g_updateState.totalBytes = 0;
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
        "Update available: " + info->tagName + " (current " APP_VERSION_STR "). Click Update to install.";
  }
}

}  // namespace

void CleanupUpdateArtifacts() { DeleteOldUpdateBackups(); }

void StartUpdateCheck() {
  DeleteOldUpdateBackups();
  {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    if (IsBusyPhase(g_updateState.phase)) return;
    g_updateState.phase = UpdatePhase::Checking;
    g_updateState.message = "Checking GitHub releases...";
  }

  std::thread(CheckWorker).detach();
}

void StartUpdateProcess() {
  ReleaseInfo info;
  {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    if (IsBusyPhase(g_updateState.phase)) return;
    if (g_updateState.phase != UpdatePhase::Available &&
        g_updateState.phase != UpdatePhase::Failed &&
        g_updateState.phase != UpdatePhase::UpToDate) {
      return;
    }
    info.tagName = g_updateState.latestVersion;
    info.htmlUrl = g_updateState.releaseUrl;
    info.downloadUrl = g_updateState.downloadUrl;
    size_t slash = info.downloadUrl.find_last_of('/');
    info.assetName = slash == std::string::npos
                         ? info.downloadUrl
                         : info.downloadUrl.substr(slash + 1);
    if (info.tagName.empty() || info.downloadUrl.empty()) return;
  }

  std::thread(RunFullUpdate, info).detach();
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
  status.downloadProgress = g_updateState.downloadProgress;
  status.downloadedBytes = g_updateState.downloadedBytes;
  status.totalBytes = g_updateState.totalBytes;
  return status;
}

bool IsUpdateBusy() {
  std::lock_guard<std::mutex> lock(g_updateMutex);
  return IsBusyPhase(g_updateState.phase);
}
