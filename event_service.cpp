#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>

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

	std::optional<std::string> FetchSchedule(std::stop_token stop) {
		HttpHandle session(WinHttpOpen(L"d4rt/" APP_VERSION_STR_W,
			WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0));
		if (!session || !WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 5000))
			return std::nullopt;
		HttpHandle connection(WinHttpConnect(session.get(), L"helltides.com",
			INTERNET_DEFAULT_HTTPS_PORT, 0));
		if (!connection) return std::nullopt;
		HttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", L"/api/schedule",
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
		if (!request || stop.stop_requested()) return std::nullopt;
		const wchar_t* headers = L"Accept: application/json\r\n";
		if (!WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1),
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
			!WinHttpReceiveResponse(request.get(), nullptr)) return std::nullopt;
		DWORD status = 0, statusSize = sizeof(status);
		if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
			status != 200) return std::nullopt;

		std::string body;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
			char buffer[4096];
			DWORD read = 0;
			if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &read)) return std::nullopt;
			if (read == 0) return body;
			if (body.size() + read > kMaxPayloadBytes) return std::nullopt;
			body.append(buffer, read);
		}
		return std::nullopt;
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
	std::filesystem::path path;
	try {
		path = CachePath();
		if (auto cached = LoadCache(path)) {
			std::lock_guard lock(stateMutex_);
			schedule_ = std::move(*cached);
		}
	}
	catch (const std::exception&) {
		// A bad/inaccessible cache must not prevent a network refresh.
	}

	while (!stop.stop_requested()) {
		{
			std::lock_guard lock(stateMutex_);
			sync_ = SyncState::Loading;
		}
		std::optional<Schedule> fresh;
		bool saved = false;
		try {
			if (auto body = FetchSchedule(stop)) fresh = ParseSchedule(*body, Now());
			if (fresh && !stop.stop_requested()) saved = SaveCache(path, *fresh);
		}
		catch (const std::exception&) {
			// Retain the last validated schedule on network, parse or disk failures.
		}
		if (stop.stop_requested()) break;
		{
			std::lock_guard lock(stateMutex_);
			if (fresh) {
				schedule_ = std::move(*fresh);
				sync_ = SyncState::Online;
				cacheWriteFailed_ = !saved;
			}
			else sync_ = SyncState::Offline;
		}
		std::unique_lock wait(waitMutex_);
		wake_.wait_for(wait, stop, std::chrono::seconds(kRefreshSeconds), [] { return false; });
	}
}
}  // namespace events
