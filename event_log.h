#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace events {
// The log keeps at most this many bytes; older lines are discarded first.
inline constexpr std::size_t kMaxLogBytes = 256 * 1024;

// Appends one "[UTC timestamp] line" entry to the event log. The default file
// is %APPDATA%\d4rt\event_log.txt. Logging never throws and never blocks the
// caller for long; when the log cannot be written the entry is dropped.
void LogEvent(const std::string& line);
void LogEvent(const std::filesystem::path& logPath, const std::string& line);
// Empty when the per-user application-data folder cannot be resolved.
std::filesystem::path DefaultLogPath();
}  // namespace events
