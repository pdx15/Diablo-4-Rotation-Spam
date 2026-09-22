#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>

#include "event_log.h"
#include "event_service.h"
#include "version.h"

#include <chrono>
#include <memory>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace events {
namespace {
	struct HttpCloser {
		void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); }
	};
	using HttpHandle = std::unique_ptr<void, HttpCloser>;

	std::filesystem::path CachePath() {
		wchar_t appData[MAX_PATH] = {};
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
			SHGFP_TYPE_CURRENT, appData))) return {};
		return std::filesystem::path(appData) / L"d4rt" / L"events_cache.json";
	}

	std::string Win32Error(DWORD code) { return "(win32=" + std::to_string(code) + ")"; }

	// UTF-8 text for the log even when %APPDATA% contains non-ASCII characters.
	std::string PathUtf8(const std::filesystem::path& path) {
		const auto utf8 = path.u8string();
		return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
	}

	std::optional<std::string> FetchSchedule(std::stop_token stop, std::string* detail) {
		auto fail = [&](std::string reason) -> std::optional<std::string> {
			if (detail) *detail = std::move(reason);
			return std::nullopt;
		};
		// A browser-like agent string: some CDN/WAF setups reject unknown
		// clients before any JSON is served, while the site works in browsers.
		HttpHandle session(WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
			L"d4rt/" APP_VERSION_STR_W,
			WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0));
		if (!session) return fail("WinHttpOpen failed " + Win32Error(GetLastError()));
		if (!WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 5000))
			return fail("WinHttpSetTimeouts failed " + Win32Error(GetLastError()));
		HttpHandle connection(WinHttpConnect(session.get(), L"helltides.com",
			INTERNET_DEFAULT_HTTPS_PORT, 0));
		if (!connection)
			return fail("WinHttpConnect to helltides.com:443 failed " + Win32Error(GetLastError()));
		HttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", L"/api/schedule",
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
		if (!request)
			return fail("WinHttpOpenRequest for /api/schedule failed " + Win32Error(GetLastError()));
		if (stop.stop_requested()) return fail("cancelled before send");
		const wchar_t* headers = L"Accept: application/json\r\n";
		if (!WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1),
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
			return fail("WinHttpSendRequest failed " + Win32Error(GetLastError()));
		if (!WinHttpReceiveResponse(request.get(), nullptr))
			return fail("WinHttpReceiveResponse failed " + Win32Error(GetLastError()));
		DWORD status = 0, statusSize = sizeof(status);
		if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
			return fail("HTTP status query failed " + Win32Error(GetLastError()));
		if (status != 200) return fail("HTTP status " + std::to_string(status));

		std::string body;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
			char buffer[4096];
			DWORD read = 0;
			if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &read))
				return fail("response read failed after " + std::to_string(body.size()) +
					" bytes " + Win32Error(GetLastError()));
			if (read == 0) {
				if (detail) *detail = "HTTP 200, " + std::to_string(body.size()) + " bytes";
				return body;
			}
			if (body.size() + read > kMaxPayloadBytes)
				return fail("response exceeded " + std::to_string(kMaxPayloadBytes) + "-byte cap");
			body.append(buffer, read);
		}
		if (stop.stop_requested()) return fail("cancelled while reading");
		return fail("response read exceeded the 20-second deadline");
	}

	std::string ScheduleSummary(const Schedule& schedule) {
		return "boss=" + std::to_string(schedule.worldBoss.size()) +
			" legion=" + std::to_string(schedule.legion.size()) +
			" helltide=" + std::to_string(schedule.helltide.size()) +
			" fetched_at=" + std::to_string(schedule.fetchedAt);
	}
}  // namespace

Service::~Service() { Stop(); }

void Service::Start() {
	if (started_) return;
	started_ = true;
	try {
		worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
	}
	catch (const std::system_error&) {
		std::lock_guard lock(stateMutex_);
		sync_ = SyncState::Offline;
	}
}

void Service::Stop() {
	worker_.request_stop();
	wake_.notify_all();
	if (worker_.joinable()) worker_.join();
}

View Service::GetView() const {
	std::lock_guard lock(stateMutex_);
	return { CalculateTimers(schedule_, Now()), sync_, cacheWriteFailed_ };
}

void Service::Run(std::stop_token stop) {
	LogEvent(std::string("worker started (d4rt ") + APP_VERSION_STR + ")");
	std::filesystem::path path;
	try {
		path = CachePath();
		if (auto cached = LoadCache(path)) {
			LogEvent("cache loaded: " + ScheduleSummary(*cached));
			std::lock_guard lock(stateMutex_);
			schedule_ = std::move(*cached);
		}
		else if (path.empty()) LogEvent("no usable cache: application-data folder unavailable");
		else LogEvent("no usable cache at " + PathUtf8(path));
	}
	catch (const std::exception&) {
		LogEvent("cache load failed: exception");
		// A bad/inaccessible cache must not prevent a network refresh.
	}

	while (!stop.stop_requested()) {
		{
			std::lock_guard lock(stateMutex_);
			sync_ = SyncState::Loading;
		}
		std::optional<Schedule> fresh;
		bool fetched = false, saved = false;
		std::string fetchDetail = "aborted", parseDetail;
		try {
			if (auto body = FetchSchedule(stop, &fetchDetail)) {
				fetched = true;
				fresh = ParseSchedule(*body, Now(), &parseDetail);
				if (fresh) {
					parseDetail = ScheduleSummary(*fresh);
					if (!stop.stop_requested()) saved = SaveCache(path, *fresh);
				}
				else if (parseDetail.empty()) parseDetail = "unknown reason";
			}
		}
		catch (const std::exception&) {
			// Retain the last validated schedule on network, parse or disk failures.
		}
		if (stop.stop_requested()) break;
		bool refreshed = false;
		{
			// Logged before the new state becomes visible, so a reader that
			// just observed it never misses the matching refresh line.
			std::string refresh = "refresh: fetch " + fetchDetail;
			if (fetched) {
				refresh += fresh ? "; parse ok (" + parseDetail + ")"
					: "; parse failed (" + parseDetail + ")";
				refresh += !fresh ? "; cache unchanged"
					: saved ? "; cache saved" : "; cache write failed";
			}
			refresh += fresh ? "; state=online" : "; state=offline";
			LogEvent(refresh);
			std::lock_guard lock(stateMutex_);
			if (fresh) {
				schedule_ = std::move(*fresh);
				sync_ = SyncState::Online;
				cacheWriteFailed_ = !saved;
				refreshed = true;
			}
			else sync_ = SyncState::Offline;
		}
		std::unique_lock wait(waitMutex_);
		wake_.wait_for(wait, stop, std::chrono::seconds(
			refreshed ? kRefreshSeconds : kRetrySeconds), [] { return false; });
	}
	LogEvent("worker stopped");
}
}  // namespace events
