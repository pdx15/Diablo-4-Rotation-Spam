#pragma once

#include <windows.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>

using HINTERNET = void*;
constexpr DWORD WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY = 4;
constexpr unsigned short INTERNET_DEFAULT_HTTPS_PORT = 443;
constexpr DWORD WINHTTP_FLAG_SECURE = 0x800000;
constexpr DWORD WINHTTP_QUERY_STATUS_CODE = 19, WINHTTP_QUERY_FLAG_NUMBER = 0x20000000;
constexpr DWORD ERROR_WINHTTP_TIMEOUT = 12002;
constexpr DWORD ERROR_WINHTTP_CANNOT_CONNECT = 12029;
constexpr DWORD ERROR_WINHTTP_CONNECTION_ERROR = 12030;
#define WINHTTP_NO_PROXY_NAME nullptr
#define WINHTTP_NO_PROXY_BYPASS nullptr
#define WINHTTP_NO_REFERER nullptr
#define WINHTTP_DEFAULT_ACCEPT_TYPES nullptr
#define WINHTTP_NO_REQUEST_DATA nullptr
#define WINHTTP_HEADER_NAME_BY_INDEX nullptr
#define WINHTTP_NO_HEADER_INDEX nullptr

namespace fake_winhttp {
struct Handle { std::size_t offset = 0; };
inline std::atomic<int> handles = 0, requests = 0;
inline DWORD status = 200;
inline bool networkFailure = false, readFailure = false;
inline std::string body;
inline bool blockResponse = false;
inline bool responseEntered = false;
inline bool responseReleased = false;
inline std::mutex mutex;
inline std::condition_variable condition;

inline void Reset(const std::string& payload) {
	assert(handles == 0);
	requests = 0;
	status = 200;
	networkFailure = readFailure = false;
	body = payload;
	blockResponse = responseEntered = responseReleased = false;
	fake_win32::lastError = 0;
}
inline HINTERNET NewHandle() { ++handles; return new Handle; }
}
inline HINTERNET WinHttpOpen(const wchar_t* agent, DWORD, const wchar_t*, const wchar_t*, DWORD) {
	// The service must impersonate the captured Chrome 152 browser: Cloudflare
	// blocks unknown/outdated user agents.
	assert(agent && std::wstring(agent).find(L"Chrome/152.0.0.0") != std::wstring::npos);
	return fake_winhttp::NewHandle();
}
inline int WinHttpCloseHandle(HINTERNET handle) {
	delete static_cast<fake_winhttp::Handle*>(handle);
	--fake_winhttp::handles;
	return 1;
}
inline int WinHttpSetTimeouts(HINTERNET, int resolve, int connect, int send, int receive) {
	assert(resolve > 0 && connect > 0 && send > 0 && receive > 0);
	assert(receive <= 10000);
	return 1;
}
inline int WinHttpSetOption(HINTERNET, DWORD, void*, DWORD) {
	return 1;
}
inline HINTERNET WinHttpConnect(HINTERNET, const wchar_t* host, unsigned short port, DWORD) {
	assert(std::wstring(host) == L"helltides.com" && port == 443);
	return fake_winhttp::NewHandle();
}
inline HINTERNET WinHttpOpenRequest(HINTERNET, const wchar_t* method, const wchar_t* path,
	const wchar_t*, const wchar_t* referrer, const wchar_t**, DWORD flags) {
	assert(std::wstring(method) == L"GET" && std::wstring(path) == L"/api/schedule");
	assert(referrer == nullptr); // captured navigation has Sec-Fetch-Site: none
	assert(flags & WINHTTP_FLAG_SECURE);
	return fake_winhttp::NewHandle();
}
inline int WinHttpSendRequest(HINTERNET, const wchar_t* headers, DWORD, void*, DWORD, DWORD, std::uintptr_t) {
	++fake_winhttp::requests;
	// The captured Chrome 152 header set that Cloudflare expects.
	assert(headers);
	const std::wstring sent(headers ? headers : L"");
	assert(sent.find(L"Sec-Ch-Ua: \"Chromium\";v=\"152\"") != std::wstring::npos);
	assert(sent.find(L"Sec-Ch-Ua-Mobile: ?0") != std::wstring::npos);
	assert(sent.find(L"Sec-Ch-Ua-Platform: \"Windows\"") != std::wstring::npos);
	assert(sent.find(L"Sec-Fetch-Dest: document") != std::wstring::npos);
	assert(sent.find(L"Sec-Fetch-Mode: navigate") != std::wstring::npos);
	assert(sent.find(L"Sec-Fetch-Site: none") != std::wstring::npos);
	assert(sent.find(L"Sec-Fetch-User: ?1") != std::wstring::npos);
	assert(sent.find(L"Upgrade-Insecure-Requests: 1") != std::wstring::npos);
	assert(sent.find(L"Referer:") == std::wstring::npos); // none-site navigation sends no Referer
	if (fake_winhttp::networkFailure) {
		fake_win32::lastError = ERROR_WINHTTP_CANNOT_CONNECT;
		return 0;
	}
	return 1;
}
inline int WinHttpReceiveResponse(HINTERNET, void*) {
	std::unique_lock lock(fake_winhttp::mutex);
	fake_winhttp::responseEntered = true;
	fake_winhttp::condition.notify_all();
	if (fake_winhttp::blockResponse) {
		const bool released = fake_winhttp::condition.wait_for(lock, std::chrono::seconds(2), [] {
			return fake_winhttp::responseReleased;
		});
		if (!released) fake_win32::lastError = ERROR_WINHTTP_TIMEOUT;
		return released;
	}
	return 1;
}
inline int WinHttpQueryHeaders(HINTERNET, DWORD, const wchar_t*, void* status, DWORD*, DWORD*) {
	*static_cast<DWORD*>(status) = fake_winhttp::status;
	return 1;
}
inline int WinHttpReadData(HINTERNET handle, void* buffer, DWORD capacity, DWORD* read) {
	if (fake_winhttp::readFailure) {
		fake_win32::lastError = ERROR_WINHTTP_CONNECTION_ERROR;
		return 0;
	}
	auto& offset = static_cast<fake_winhttp::Handle*>(handle)->offset;
	*read = static_cast<DWORD>(std::min<std::size_t>(capacity, fake_winhttp::body.size() - offset));
	std::memcpy(buffer, fake_winhttp::body.data() + offset, *read);
	offset += *read;
	return 1;
}
