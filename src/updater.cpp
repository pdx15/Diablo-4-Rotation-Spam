#include "updater.h"

#include <windows.h>

#include <shellapi.h>
#include <shldisp.h>
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
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app_state.h"

extern LocStrings lang;

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")

namespace {

	constexpr char kRepoApiUrl[] =
		"https://api.github.com/repos/pdx15/Diablo-4-Rotation-Spam/releases/latest";
	constexpr wchar_t kLatestReleaseUrl[] =
		L"https://github.com/pdx15/Diablo-4-Rotation-Spam/releases/latest";
	constexpr wchar_t kOverlayWindowClass[] = L"OverlayClass";

	constexpr LONG kCopyHereNoUi = 1024 | 512 | 16 | 4;

	// Minimal sane size for the shipped executable. Anything smaller means the
	// archive was not fully extracted / the download was truncated.
	constexpr std::uint64_t kMinPayloadSize = 64ull * 1024ull;

	struct ReleaseInfo {
		std::string tagName;
		std::string htmlUrl;
		std::string downloadUrl;
		std::string assetName;
	};

	struct UpdaterState {
		UpdatePhase phase = UpdatePhase::Idle;

		std::string message;
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

	std::string FormatTr(const char* format, ...) {
		if (!format) return {};
		va_list args;
		va_start(args, format);
		char buffer[512];
		int written = vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		if (written < 0) return format;
		const size_t length = static_cast<size_t>(written) < sizeof(buffer) - 1
			? static_cast<size_t>(written)
			: sizeof(buffer) - 1;
		return std::string(buffer, length);
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

	std::wstring GetCurrentExePath() {
		wchar_t buffer[MAX_PATH] = {};
		DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		if (length == 0 || length >= MAX_PATH) return {};
		return std::wstring(buffer, length);
	}

	std::wstring GetDirOf(const std::wstring& path) {
		size_t slash = path.find_last_of(L"\\/");
		if (slash == std::wstring::npos) return {};
		return path.substr(0, slash);
	}

	std::wstring GetFileNameOf(const std::wstring& path) {
		size_t slash = path.find_last_of(L"\\/");
		if (slash == std::wstring::npos) return path;
		return path.substr(slash + 1);
	}

	bool EqualsI(const std::wstring& left, const std::wstring& right) {
		return _wcsicmp(left.c_str(), right.c_str()) == 0;
	}

	// Appends one line to update_log.txt next to the executable. The update flow
	// is impossible to debug without it: everything happens right before the
	// process exits, so nothing stays on screen.
	void UpdateLog(const wchar_t* format, ...) {
		wchar_t message[1024] = {};
		va_list args;
		va_start(args, format);
		int written = vswprintf(message, 1024, format, args);
		va_end(args);
		if (written < 0) message[1023] = L'\0';

		OutputDebugStringW(message);
		OutputDebugStringW(L"\n");

		std::wstring dir = GetDirOf(GetCurrentExePath());
		if (dir.empty()) return;

		std::wstring logPath = dir + L"\\update_log.txt";
		HANDLE file = CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return;

		SYSTEMTIME now = {};
		GetLocalTime(&now);
		wchar_t line[1200] = {};
		int lineLength = swprintf(line, 1200,
			L"[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
			now.wYear, now.wMonth, now.wDay, now.wHour,
			now.wMinute, now.wSecond, message);
		if (lineLength > 0) {
			std::string utf8 = WideToUtf8(std::wstring(line, static_cast<size_t>(lineLength)));
			DWORD writtenBytes = 0;
			SetFilePointer(file, 0, nullptr, FILE_END);
			WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()),
				&writtenBytes, nullptr);
		}
		CloseHandle(file);
	}

	std::wstring GetLastErrorText(DWORD err) {
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
		return text + L" (code " + std::to_wstring(err) + L")";
	}

	std::wstring GetLastErrorText() { return GetLastErrorText(GetLastError()); }

	bool DirectoryExists(const std::wstring& path) {
		DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES &&
			(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool FileExists(const std::wstring& path) {
		DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES &&
			!(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool GetFileSize64(const std::wstring& path, std::uint64_t& size) {
		WIN32_FILE_ATTRIBUTE_DATA data = {};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
			return false;
		}
		size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) |
			static_cast<std::uint64_t>(data.nFileSizeLow);
		return true;
	}

	void RemoveDirectoryTree(const std::wstring& path) {
		if (path.empty() || !DirectoryExists(path)) return;
		std::wstring pattern = path + L"\\*";
		WIN32_FIND_DATAW fd;
		HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
		if (handle != INVALID_HANDLE_VALUE) {
			do {
				if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
					continue;
				}
				std::wstring child = path + L"\\" + fd.cFileName;
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					RemoveDirectoryTree(child);
				}
				else {
					SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
					DeleteFileW(child.c_str());
				}
			} while (FindNextFileW(handle, &fd));
			FindClose(handle);
		}
		RemoveDirectoryW(path.c_str());
	}

	void DeleteOldUpdateBackups() {
		std::wstring curExe = GetCurrentExePath();
		if (!curExe.empty()) {
			std::wstring bakPath = curExe + L".bak";
			if (FileExists(bakPath)) {
				SetFileAttributesW(bakPath.c_str(), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(bakPath.c_str());
			}
		}

		wchar_t tempDir[MAX_PATH] = {};
		if (GetTempPathW(MAX_PATH, tempDir) == 0) return;

		WIN32_FIND_DATAW fd;
		std::wstring pattern = std::wstring(tempDir) + L"d4rt_*";
		HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
		if (handle == INVALID_HANDLE_VALUE) return;
		do {
			if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
				continue;
			}
			std::wstring path = std::wstring(tempDir) + fd.cFileName;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				RemoveDirectoryTree(path);
			}
			else {
				SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(path.c_str());
			}
		} while (FindNextFileW(handle, &fd));
		FindClose(handle);
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

	void SetFailed(const std::string& message, const std::wstring& logDetails) {
		UpdateLog(L"FAILED: %s", logDetails.c_str());
		SetState(UpdatePhase::Failed, message);
	}

	void SetDownloadProgress(std::uint64_t downloaded, std::uint64_t total) {
		std::lock_guard<std::mutex> lock(g_updateMutex);
		g_updateState.downloadedBytes = downloaded;
		g_updateState.totalBytes = total;
		if (total > 0) {
			g_updateState.downloadProgress = static_cast<float>(
				static_cast<double>(downloaded) / static_cast<double>(total));
		}
		else {
			g_updateState.downloadProgress = -1.0f;
		}
		if (total > 0) {
			g_updateState.message = FormatTr(
				lang.updateMsgDownloadProgress.c_str(),
				static_cast<unsigned long long>(downloaded / 1024),
				static_cast<unsigned long long>(total / 1024));
		}
		else {
			g_updateState.message = FormatTr(
				lang.updateMsgDownloadProgressUnknown.c_str(),
				static_cast<unsigned long long>(downloaded / 1024));
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
			error = lang.updateErrInvalidUrl + WideToUtf8(GetLastErrorText());
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
			error = lang.updateErrWinHttpSession + WideToUtf8(GetLastErrorText());
			return nullptr;
		}

		DWORD timeoutConnect = 15000;
		DWORD timeoutReceive = 60000;
		WinHttpSetTimeouts(session, 15000, timeoutConnect, timeoutReceive,
			timeoutReceive);

		connect = WinHttpConnect(session, host.c_str(), port, 0);
		if (!connect) {
			error = lang.updateErrWinHttpConnect + WideToUtf8(GetLastErrorText());
			return nullptr;
		}

		DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
		HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
			WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
		if (!request) {
			error = lang.updateErrWinHttpRequest + WideToUtf8(GetLastErrorText());
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
			error = lang.updateErrHttpRequest + WideToUtf8(GetLastErrorText());
			return false;
		}

		DWORD statusCode = 0;
		DWORD statusSize = sizeof(statusCode);
		if (!WinHttpQueryHeaders(
			request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
			WINHTTP_NO_HEADER_INDEX)) {
			error = lang.updateErrHttpStatusRead + WideToUtf8(GetLastErrorText());
			return false;
		}

		if (statusCode < 200 || statusCode >= 300) {
			error = FormatTr(lang.updateErrHttpStatus.c_str(),

				static_cast<unsigned int>(statusCode));
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
					error = lang.updateErrHttpRead + WideToUtf8(GetLastErrorText());
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
			wchar_t lengthText[64] = {};
			DWORD lengthSize = sizeof(lengthText);
			if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
				WINHTTP_HEADER_NAME_BY_INDEX, lengthText,
				&lengthSize, WINHTTP_NO_HEADER_INDEX)) {
				totalBytes = _wcstoui64(lengthText, nullptr, 10);
			}

			file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				error = lang.updateErrCreateFile + WideToUtf8(GetLastErrorText());
			}
			else {
				ok = true;
				DWORD available = 0;
				while (ok && WinHttpQueryDataAvailable(request, &available) &&
					available > 0) {
					std::vector<char> buffer(available);
					DWORD read = 0;
					if (!WinHttpReadData(request, buffer.data(), available, &read)) {
						error = lang.updateErrDownloadRead + WideToUtf8(GetLastErrorText());
						ok = false;
						break;
					}

					DWORD written = 0;
					if (!WriteFile(file, buffer.data(), read, &written, nullptr) ||
						written != read) {
						error = lang.updateErrDownloadWrite + WideToUtf8(GetLastErrorText());
						ok = false;
						break;
					}

					downloaded += static_cast<std::uint64_t>(read);
					if (progressCb) progressCb(downloaded, totalBytes);
				}

				if (ok) FlushFileBuffers(file);

				// A silently truncated download used to be the worst failure mode:
				// the half-written file replaced a working exe and nothing started.
				if (ok && totalBytes > 0 && downloaded != totalBytes) {
					UpdateLog(L"download truncated: %llu of %llu bytes",
						static_cast<unsigned long long>(downloaded),
						static_cast<unsigned long long>(totalBytes));
					error = lang.updateErrDownloadRead + "incomplete download";
					ok = false;
				}

				if (ok && progressCb && totalBytes > 0) {
					progressCb(totalBytes, totalBytes);
				}
			}
		}

		if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
		if (!ok) DeleteFileW(path.c_str());
		if (request) WinHttpCloseHandle(request);
		if (connect) WinHttpCloseHandle(connect);
		if (session) WinHttpCloseHandle(session);

		if (ok) {
			UpdateLog(L"downloaded %llu bytes to %s",
				static_cast<unsigned long long>(downloaded), path.c_str());
		}
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
			}
			else if (c == '\\') {
				escaped = true;
			}
			else if (c == '"') {
				return value;
			}
			else {
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
			}
			else if (inNumber) {
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

	std::wstring MakeStagingDir() {
		wchar_t tempDir[MAX_PATH] = {};
		if (GetTempPathW(MAX_PATH, tempDir) == 0) return {};
		std::wstring dir = std::wstring(tempDir) + L"d4rt_stage_" +
			std::to_wstring(GetCurrentProcessId()) + L"_" +
			std::to_wstring(GetTickCount64());
		RemoveDirectoryTree(dir);
		if (!CreateDirectoryW(dir.c_str(), nullptr)) return {};
		return dir;
	}

	// ---------------------------------------------------------------------------
	// Payload verification
	// ---------------------------------------------------------------------------

	bool IsValidPeFile(const std::wstring& path) {
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;

		bool ok = false;
		unsigned char dosHeader[64] = {};
		DWORD read = 0;
		if (ReadFile(file, dosHeader, sizeof(dosHeader), &read, nullptr) &&
			read == sizeof(dosHeader) && dosHeader[0] == 'M' && dosHeader[1] == 'Z') {
			LONG peOffset = static_cast<LONG>(dosHeader[60]) |
				(static_cast<LONG>(dosHeader[61]) << 8) |
				(static_cast<LONG>(dosHeader[62]) << 16) |
				(static_cast<LONG>(dosHeader[63]) << 24);
			if (peOffset > 0 && peOffset < 0x10000000) {
				LARGE_INTEGER pos;
				pos.QuadPart = peOffset;
				if (SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) {
					unsigned char signature[4] = {};
					if (ReadFile(file, signature, sizeof(signature), &read, nullptr) &&
						read == sizeof(signature) && signature[0] == 'P' &&
						signature[1] == 'E' && signature[2] == 0 && signature[3] == 0) {
						ok = true;
					}
				}
			}
		}

		CloseHandle(file);
		return ok;
	}

	bool IsUsablePayload(const std::wstring& path) {
		std::uint64_t size = 0;
		if (!GetFileSize64(path, size)) return false;
		if (size < kMinPayloadSize) {
			UpdateLog(L"payload too small: %s (%llu bytes)", path.c_str(),
				static_cast<unsigned long long>(size));
			return false;
		}
		if (!IsValidPeFile(path)) {
			UpdateLog(L"payload is not a valid PE file: %s", path.c_str());
			return false;
		}
		return true;
	}

	// Waits until a file stops growing and can be opened without sharing, i.e.
	// whoever writes it (shell unzip) is really done with it.
	bool WaitForFileReady(const std::wstring& path, DWORD timeoutMs) {
		ULONGLONG start = GetTickCount64();
		std::uint64_t lastSize = 0;
		bool hasLast = false;
		int stableTicks = 0;

		while (GetTickCount64() - start < timeoutMs) {
			std::uint64_t size = 0;
			if (GetFileSize64(path, size) && size > 0) {
				if (hasLast && size == lastSize) {
					++stableTicks;
				}
				else {
					stableTicks = 0;
				}
				lastSize = size;
				hasLast = true;

				if (stableTicks >= 2) {
					HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
						OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
					if (file != INVALID_HANDLE_VALUE) {
						CloseHandle(file);
						return true;
					}
				}
			}
			Sleep(200);
		}
		return false;
	}

	// ---------------------------------------------------------------------------
	// Archive extraction
	// ---------------------------------------------------------------------------

	bool RunProcessAndWait(const std::wstring& application,
		const std::wstring& commandLine, DWORD timeoutMs,
		DWORD& exitCode) {
		STARTUPINFOW si = {};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi = {};

		std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
		mutableCmd.push_back(L'\0');

		if (!CreateProcessW(application.empty() ? nullptr : application.c_str(),
			mutableCmd.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
			UpdateLog(L"CreateProcess failed for %s: %s", application.c_str(),
				GetLastErrorText().c_str());
			return false;
		}

		bool ok = false;
		if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) {
			DWORD code = 1;
			if (GetExitCodeProcess(pi.hProcess, &code)) {
				exitCode = code;
				ok = true;
			}
		}
		else {
			UpdateLog(L"process timed out: %s", application.c_str());
			TerminateProcess(pi.hProcess, 1);
		}

		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return ok;
	}

	bool DirectoryHasEntries(const std::wstring& dir) {
		std::wstring pattern = dir + L"\\*";
		WIN32_FIND_DATAW fd;
		HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
		if (handle == INVALID_HANDLE_VALUE) return false;
		bool found = false;
		do {
			if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
				continue;
			}
			found = true;
			break;
		} while (FindNextFileW(handle, &fd));
		FindClose(handle);
		return found;
	}

	// bsdtar ships with Windows 10 1803+ and unzips synchronously.
	bool ExtractWithTar(const std::wstring& zipPath, const std::wstring& destDir) {
		wchar_t systemDir[MAX_PATH] = {};
		if (GetSystemDirectoryW(systemDir, MAX_PATH) == 0) return false;
		std::wstring tarPath = std::wstring(systemDir) + L"\\tar.exe";
		if (!FileExists(tarPath)) return false;

		std::wstring command = L"\"" + tarPath + L"\" -x -f \"" + zipPath +
			L"\" -C \"" + destDir + L"\"";
		DWORD exitCode = 1;
		if (!RunProcessAndWait(tarPath, command, 180000, exitCode)) return false;
		UpdateLog(L"tar.exe exit code %lu", exitCode);
		return exitCode == 0 && DirectoryHasEntries(destDir);
	}

	std::wstring EscapeForPowerShellSingleQuotes(const std::wstring& text) {
		std::wstring out;
		out.reserve(text.size() + 8);
		for (wchar_t c : text) {
			out.push_back(c);
			if (c == L'\'') out.push_back(L'\'');
		}
		return out;
	}

	bool ExtractWithPowerShell(const std::wstring& zipPath,
		const std::wstring& destDir) {
		wchar_t systemDir[MAX_PATH] = {};
		if (GetSystemDirectoryW(systemDir, MAX_PATH) == 0) return false;
		std::wstring psPath =
			std::wstring(systemDir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
		if (!FileExists(psPath)) return false;

		std::wstring command =
			L"\"" + psPath +
			L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
			L"\"$ErrorActionPreference='Stop'; Expand-Archive -LiteralPath '" +
			EscapeForPowerShellSingleQuotes(zipPath) + L"' -DestinationPath '" +
			EscapeForPowerShellSingleQuotes(destDir) + L"' -Force\"";

		DWORD exitCode = 1;
		if (!RunProcessAndWait(psPath, command, 180000, exitCode)) return false;
		UpdateLog(L"powershell Expand-Archive exit code %lu", exitCode);
		return exitCode == 0 && DirectoryHasEntries(destDir);
	}

	bool FindFirstExeRecursive(const std::wstring& root, std::wstring& outExe) {
		std::wstring pattern = root + L"\\*";
		WIN32_FIND_DATAW fd;
		HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
		if (handle == INVALID_HANDLE_VALUE) return false;
		bool found = false;
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
					continue;
				if (FindFirstExeRecursive(root + L"\\" + fd.cFileName, outExe)) {
					found = true;
					break;
				}
			}
			else if (EndsWithI(fd.cFileName, L".exe")) {
				outExe = root + L"\\" + fd.cFileName;
				found = true;
				break;
			}
		} while (FindNextFileW(handle, &fd));
		FindClose(handle);
		return found;
	}

	// Last-resort fallback. IShellDispatch::CopyHere is ASYNCHRONOUS: the old code
	// returned as soon as the .exe entry appeared and then killed the apartment
	// with CoUninitialize, which aborted the copy and left a truncated exe.
	// Here we wait for the extracted file to be complete before leaving the STA.
	bool ExtractWithShell(const std::wstring& zipPath, const std::wstring& destDir) {
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

						std::wstring extracted;
						for (int i = 0; i < 240 && !ok; ++i) {
							if (FindFirstExeRecursive(destDir, extracted)) {
								// The entry exists, but the shell may still be writing it.
								if (WaitForFileReady(extracted, 120000) &&
									IsUsablePayload(extracted)) {
									ok = true;
									break;
								}
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
		UpdateLog(L"shell extraction result: %d", ok ? 1 : 0);
		return ok;
	}

	bool ExtractArchive(const std::wstring& zipPath, const std::wstring& destDir) {
		if (ExtractWithTar(zipPath, destDir)) {
			UpdateLog(L"extracted with tar.exe");
			return true;
		}
		UpdateLog(L"tar.exe extraction unavailable/failed, trying PowerShell");

		if (ExtractWithPowerShell(zipPath, destDir)) {
			UpdateLog(L"extracted with PowerShell");
			return true;
		}
		UpdateLog(L"PowerShell extraction failed, trying Shell COM");

		return ExtractWithShell(zipPath, destDir);
	}

	// ---------------------------------------------------------------------------
	// Installation
	// ---------------------------------------------------------------------------

	void CollectFilesRecursive(const std::wstring& root,
		const std::wstring& relativePrefix,
		std::vector<std::wstring>& out) {
		std::wstring pattern = root + L"\\*";
		WIN32_FIND_DATAW fd;
		HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
		if (handle == INVALID_HANDLE_VALUE) return;
		do {
			if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
				continue;
			}
			std::wstring relative = relativePrefix.empty()
				? std::wstring(fd.cFileName)
				: relativePrefix + L"\\" + fd.cFileName;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				CollectFilesRecursive(root + L"\\" + fd.cFileName, relative, out);
			}
			else {
				out.push_back(relative);
			}
		} while (FindNextFileW(handle, &fd));
		FindClose(handle);
	}

	// Picks the executable that should replace the running one.
	std::optional<std::wstring> ChoosePayload(const std::wstring& stagingDir,
		const std::vector<std::wstring>& files) {
		const std::wstring currentName = GetFileNameOf(GetCurrentExePath());

		std::optional<std::wstring> byName;
		std::optional<std::wstring> byBrand;
		std::optional<std::wstring> biggest;
		std::uint64_t biggestSize = 0;

		for (const std::wstring& relative : files) {
			if (!EndsWithI(relative, L".exe")) continue;
			std::wstring full = stagingDir + L"\\" + relative;
			if (!IsUsablePayload(full)) continue;

			std::wstring name = GetFileNameOf(relative);
			if (!byName && !currentName.empty() && EqualsI(name, currentName)) {
				byName = relative;
			}
			if (!byBrand && EqualsI(name, L"d4rt.exe")) byBrand = relative;

			std::uint64_t size = 0;
			if (GetFileSize64(full, size) && size > biggestSize) {
				biggestSize = size;
				biggest = relative;
			}
		}

		if (byName) return byName;
		if (byBrand) return byBrand;
		return biggest;
	}

	bool CopyFileVerified(const std::wstring& source, const std::wstring& target) {
		std::uint64_t sourceSize = 0;
		if (!GetFileSize64(source, sourceSize)) return false;

		for (int attempt = 0; attempt < 10; ++attempt) {
			SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
			if (CopyFileW(source.c_str(), target.c_str(), FALSE)) {
				std::uint64_t targetSize = 0;
				if (GetFileSize64(target, targetSize) && targetSize == sourceSize) {
					return true;
				}
				UpdateLog(L"copy size mismatch for %s (%llu vs %llu), retrying",
					target.c_str(), static_cast<unsigned long long>(targetSize),
					static_cast<unsigned long long>(sourceSize));
			}
			else {
				UpdateLog(L"CopyFile %s -> %s failed: %s", source.c_str(),
					target.c_str(), GetLastErrorText().c_str());
			}
			Sleep(300);
		}
		return false;
	}

	void EnsureDirectoryChain(const std::wstring& path) {
		if (path.empty() || DirectoryExists(path)) return;
		EnsureDirectoryChain(GetDirOf(path));
		CreateDirectoryW(path.c_str(), nullptr);
	}

	bool LaunchUpdatedApp(const std::wstring& target) {
		std::wstring dir = GetDirOf(target);
		std::wstring commandLine = L"\"" + target + L"\" --updated-from " +
			std::to_wstring(GetCurrentProcessId());

		for (int attempt = 0; attempt < 5; ++attempt) {
			std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
			mutableCmd.push_back(L'\0');

			STARTUPINFOW si = {};
			si.cb = sizeof(si);
			PROCESS_INFORMATION pi = {};

			if (CreateProcessW(target.c_str(), mutableCmd.data(), nullptr, nullptr,
				FALSE, 0, nullptr, dir.empty() ? nullptr : dir.c_str(),
				&si, &pi)) {
				// If the new binary is broken it dies instantly; detect that instead
				// of exiting and leaving the user with nothing running.
				DWORD wait = WaitForSingleObject(pi.hProcess, 1500);
				bool alive = (wait == WAIT_TIMEOUT);
				DWORD exitCode = 0;
				if (!alive) GetExitCodeProcess(pi.hProcess, &exitCode);
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);

				if (alive) {
					UpdateLog(L"new version started: %s", commandLine.c_str());
					return true;
				}
				UpdateLog(L"new version exited immediately with code %lu", exitCode);
			}
			else {
				UpdateLog(L"CreateProcess for new version failed: %s",
					GetLastErrorText().c_str());
			}
			Sleep(700);
		}
		return false;
	}

	struct FindOwnWindowData {
		DWORD processId = 0;
		HWND hwnd = nullptr;
	};

	BOOL CALLBACK FindOwnWindowProc(HWND hwnd, LPARAM param) {
		auto* data = reinterpret_cast<FindOwnWindowData*>(param);
		DWORD owner = 0;
		GetWindowThreadProcessId(hwnd, &owner);
		if (owner != data->processId) return TRUE;

		wchar_t className[64] = {};
		if (GetClassNameW(hwnd, className, 64) == 0) return TRUE;
		if (wcscmp(className, kOverlayWindowClass) != 0) return TRUE;

		data->hwnd = hwnd;
		return FALSE;
	}

	void ShutdownForRestart() {
		// Must match by process id: the new instance uses the very same window
		// class, so a plain FindWindow could close the freshly started copy.
		FindOwnWindowData data;
		data.processId = GetCurrentProcessId();
		EnumWindows(FindOwnWindowProc, reinterpret_cast<LPARAM>(&data));

		if (data.hwnd) {
			// Graceful close so the tray icon disappears instead of leaving a ghost.
			PostMessageW(data.hwnd, WM_CLOSE, 0, 0);
			for (int i = 0; i < 30 && IsWindow(data.hwnd); ++i) Sleep(100);
		}
		UpdateLog(L"exiting old process %lu", GetCurrentProcessId());
		Sleep(200);
		ExitProcess(0);
	}

	// Replaces the running executable (plus any extra files shipped in the
	// archive) and starts the new version. Any failure is rolled back so the app
	// stays usable.
	void ApplyUpdateAndRestart(const std::wstring& stagingDir,
		const std::wstring& payloadRelative,
		const std::vector<std::wstring>& allFiles) {
		std::wstring target = GetCurrentExePath();
		if (target.empty()) {
			SetFailed(lang.updateErrPrepareFailed + WideToUtf8(GetLastErrorText()),
				L"GetModuleFileName failed");
			return;
		}

		std::wstring targetDir = GetDirOf(target);
		std::wstring payloadFull = stagingDir + L"\\" + payloadRelative;
		std::wstring backup = target + L".bak";

		SetFileAttributesW(backup.c_str(), FILE_ATTRIBUTE_NORMAL);
		DeleteFileW(backup.c_str());

		UpdateLog(L"installing %s over %s", payloadFull.c_str(), target.c_str());

		if (!MoveFileExW(target.c_str(), backup.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			SetFailed(lang.updateErrPrepareFailed + WideToUtf8(GetLastErrorText()),
				L"cannot rename current exe: " + GetLastErrorText());
			return;
		}

		auto rollback = [&](const std::wstring& reason) {
			UpdateLog(L"rolling back: %s", reason.c_str());
			SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
			DeleteFileW(target.c_str());
			MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
			};

		if (!CopyFileVerified(payloadFull, target)) {
			DWORD lastError = GetLastError();
			rollback(L"payload copy failed");
			SetFailed(lang.updateErrWriteFailed + WideToUtf8(GetLastErrorText(lastError)),
				L"copy of new exe failed");
			return;
		}

		if (!IsUsablePayload(target)) {
			rollback(L"installed file failed verification");
			SetFailed(lang.updateErrVerifyFailed, L"installed exe failed verification");
			return;
		}

		// Ship any extra files that come with the release (dlls, data, ...).
		// Paths are rebased on the payload's own folder, so an archive that wraps
		// everything in a "d4rt_v1045\" directory does not recreate that
		// directory next to the executable.
		const std::wstring payloadDir = GetDirOf(payloadRelative);
		const std::wstring payloadPrefix =
			payloadDir.empty() ? std::wstring() : payloadDir + L"\\";

		for (const std::wstring& relative : allFiles) {
			if (EqualsI(relative, payloadRelative)) continue;
			if (!payloadPrefix.empty() &&
				_wcsnicmp(relative.c_str(), payloadPrefix.c_str(),
					payloadPrefix.size()) != 0) {
				UpdateLog(L"skipping file outside payload folder: %s", relative.c_str());
				continue;
			}

			std::wstring rebased = relative.substr(payloadPrefix.size());
			std::wstring source = stagingDir + L"\\" + relative;
			std::wstring destination = targetDir + L"\\" + rebased;
			EnsureDirectoryChain(GetDirOf(destination));
			if (!CopyFileVerified(source, destination)) {
				UpdateLog(L"optional file not updated: %s", rebased.c_str());
			}
		}

		{
			std::lock_guard<std::mutex> lock(g_updateMutex);
			g_updateState.phase = UpdatePhase::Restarting;
			g_updateState.message = lang.updateMsgRestarting;
		}

		if (!LaunchUpdatedApp(target)) {
			DWORD lastError = GetLastError();
			rollback(L"new version could not be started");
			SetFailed(lang.updateErrRestartFailed + WideToUtf8(GetLastErrorText(lastError)),
				L"launching updated exe failed");
			return;
		}

		UpdateLog(L"update to a new build finished, handing over");
		ShutdownForRestart();
	}

	void RunFullUpdate(const ReleaseInfo& info) {
		if (info.downloadUrl.empty()) {
			SetState(UpdatePhase::Available, lang.updateErrNoAsset);
			OpenLatestReleasePage();
			return;
		}

		UpdateLog(L"starting update to %s from %s",
			Utf8ToWide(info.tagName).c_str(),
			Utf8ToWide(info.downloadUrl).c_str());

		SetState(UpdatePhase::Downloading, lang.updateMsgDownloading);
		std::wstring downloadPath = GetTempUpdatePath(info.assetName);
		std::string error;
		if (!DownloadUrlToFile(info.downloadUrl, downloadPath, error,
			SetDownloadProgress)) {
			SetFailed(lang.updateErrDownloadFailed + error,
				L"download failed: " + Utf8ToWide(error));
			return;
		}

		std::wstring stagingDir = MakeStagingDir();
		if (stagingDir.empty()) {
			SetFailed(lang.updateErrPrepareFailed + WideToUtf8(GetLastErrorText()),
				L"cannot create staging directory");
			return;
		}

		std::wstring payloadRelative;
		std::vector<std::wstring> files;

		if (EndsWithI(downloadPath, L".zip")) {
			SetState(UpdatePhase::Installing, lang.updateMsgExtracting);
			if (!ExtractArchive(downloadPath, stagingDir)) {
				SetFailed(lang.updateErrExtractFailed, L"all extraction methods failed");
				ShellExecuteW(nullptr, L"open", stagingDir.c_str(), nullptr, nullptr,
					SW_SHOWNORMAL);
				OpenLatestReleasePage();
				return;
			}

			CollectFilesRecursive(stagingDir, L"", files);
			UpdateLog(L"extracted %zu file(s)", files.size());

			auto payload = ChoosePayload(stagingDir, files);
			if (!payload) {
				SetFailed(lang.updateErrVerifyFailed,
					L"no valid executable inside the archive");
				OpenLatestReleasePage();
				return;
			}
			payloadRelative = *payload;
		}
		else {
			// Plain .exe asset: stage it so the install path is identical.
			std::wstring name = GetFileNameOf(downloadPath);
			if (name.empty()) name = L"d4rt.exe";
			std::wstring staged = stagingDir + L"\\" + name;
			if (!CopyFileVerified(downloadPath, staged)) {
				SetFailed(lang.updateErrPrepareFailed + WideToUtf8(GetLastErrorText()),
					L"cannot stage downloaded exe");
				return;
			}
			if (!IsUsablePayload(staged)) {
				SetFailed(lang.updateErrVerifyFailed, L"downloaded exe failed verification");
				return;
			}
			payloadRelative = name;
			files.push_back(name);
		}

		SetState(UpdatePhase::Installing, lang.updateMsgInstalling);
		ApplyUpdateAndRestart(stagingDir, payloadRelative, files);
	}

	void CheckWorker() {
		SetState(UpdatePhase::Checking, lang.updateMsgChecking);

		std::string error;
		auto json = HttpGetString(kRepoApiUrl, error);
		if (!json) {
			SetState(UpdatePhase::Failed, lang.updateErrCheckFailed + error);
			return;
		}

		auto info = ParseReleaseInfo(*json);
		if (!info) {
			SetState(UpdatePhase::Failed, lang.updateErrParseFailed);
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
				FormatTr(lang.updateMsgUpToDate.c_str(), APP_VERSION_STR));
			return;
		}

		{
			std::lock_guard<std::mutex> lock(g_updateMutex);
			g_updateState.phase = UpdatePhase::Available;
			g_updateState.message = FormatTr(lang.updateMsgAvailable.c_str(),
				info->tagName.c_str(), APP_VERSION_STR);
		}
	}

}

void CleanupUpdateArtifacts() { DeleteOldUpdateBackups(); }

void WaitForPreviousInstance(unsigned long processId) {
	if (processId == 0 || processId == GetCurrentProcessId()) return;
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
	if (!process) return;
	UpdateLog(L"waiting for previous instance %lu to exit", processId);
	WaitForSingleObject(process, 15000);
	CloseHandle(process);
	// Give Windows a moment to release the old image before deleting the backup.
	Sleep(300);
	UpdateLog(L"previous instance gone, running %s", APP_VERSION_STR_W);
}

void StartUpdateCheck() {
	DeleteOldUpdateBackups();
	{
		std::lock_guard<std::mutex> lock(g_updateMutex);
		if (IsBusyPhase(g_updateState.phase)) return;
		g_updateState.phase = UpdatePhase::Checking;
		g_updateState.message = lang.updateMsgChecking;
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
		g_updateState.phase = UpdatePhase::Downloading;
		g_updateState.message = lang.updateMsgDownloading;
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

	status.message = g_updateState.message.empty() ? lang.updateMsgReady
		: g_updateState.message;
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
