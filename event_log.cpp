#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include "event_log.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>

#pragma comment(lib, "shell32.lib")

namespace events {
namespace {
std::mutex& LogMutex() {
	static std::mutex mutex;
	return mutex;
}

std::string UtcTimestamp() {
	const auto now = std::chrono::system_clock::now();
	const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
	std::tm brokenDown = {};
#ifdef _WIN32
	gmtime_s(&brokenDown, &seconds);
#else
	gmtime_r(&seconds, &brokenDown);
#endif
	char text[32] = {};
	if (std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &brokenDown) == 0)
		return "0000-00-00T00:00:00Z";
	return text;
}

// Discards roughly the oldest half of an overgrown log, keeping whole lines.
void TrimLogFile(const std::filesystem::path& path) {
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error || size <= kMaxLogBytes) return;
	std::ifstream input(path, std::ios::binary);
	if (!input) return;
	std::string text(static_cast<std::size_t>(size), '\0');
	input.read(text.data(), static_cast<std::streamsize>(text.size()));
	text.resize(static_cast<std::size_t>(input.gcount()));
	const auto cut = text.find('\n', text.size() / 2);
	const std::string tail = cut == std::string::npos ? std::string() : text.substr(cut + 1);
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) return;
	output.write(tail.data(), static_cast<std::streamsize>(tail.size()));
}
}  // namespace

std::filesystem::path DefaultLogPath() {
	try {
		wchar_t appData[MAX_PATH] = {};
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
			SHGFP_TYPE_CURRENT, appData))) return {};
		return std::filesystem::path(appData) / L"d4rt" / L"event_log.txt";
	}
	catch (const std::exception&) {
		return {};
	}
}

void LogEvent(const std::filesystem::path& logPath, const std::string& line) {
	try {
		if (logPath.empty()) return;
		const std::string entry = "[" + UtcTimestamp() + "] " + line + "\n";
		std::lock_guard lock(LogMutex());
		std::error_code error;
		if (!logPath.parent_path().empty())
			std::filesystem::create_directories(logPath.parent_path(), error);
		TrimLogFile(logPath);
		std::ofstream output(logPath, std::ios::binary | std::ios::app);
		if (!output) return;
		output.write(entry.data(), static_cast<std::streamsize>(entry.size()));
	}
	catch (const std::exception&) {
		// Diagnostics must never break the worker or the UI.
	}
}

void LogEvent(const std::string& line) { LogEvent(DefaultLogPath(), line); }
}  // namespace events
