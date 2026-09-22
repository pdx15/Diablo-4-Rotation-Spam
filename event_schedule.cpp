#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "event_schedule.h"
#include "third_party/picojson/picojson.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>

namespace events {
namespace {
	constexpr UnixSeconds kEarliestTimestamp = 946684800;   // 2000-01-01 UTC
	constexpr UnixSeconds kLatestTimestamp = 4102444800;  // 2100-01-01 UTC
	constexpr std::size_t kMaxEventsPerType = 512;

	// Bound recursion before handing untrusted network/cache JSON to the parser.
	bool BoundedJson(const std::string& text) {
		if (text.empty() || text.size() > kMaxPayloadBytes) return false;
		int depth = 0;
		bool inString = false, escaped = false;
		for (char c : text) {
			if (inString) {
				if (escaped) escaped = false;
				else if (c == '\\') escaped = true;
				else if (c == '"') inString = false;
			}
			else if (c == '"') inString = true;
			else if (c == '{' || c == '[') {
				if (++depth > 32) return false;
			}
			else if (c == '}' || c == ']') {
				if (--depth < 0) return false;
			}
		}
		return !inString && depth == 0;
	}

	bool ParseJson(const std::string& text, picojson::value& root) {
		if (!BoundedJson(text)) return false;
		try {
			std::string error;
			auto end = picojson::parse(root, text.begin(), text.end(), &error);
			return error.empty() && root.is<picojson::object>() &&
				std::all_of(end, text.end(), [](char c) {
					return c == ' ' || c == '\n' || c == '\r' || c == '\t';
				});
		}
		catch (const std::exception&) {
			return false; // e.g. a JSON number outside the parser's numeric range
		}
	}

	std::optional<UnixSeconds> Timestamp(const picojson::value& value) {
		if (!value.is<double>()) return std::nullopt;
		double number = value.get<double>();
		if (!std::isfinite(number) || number < kEarliestTimestamp ||
			number > kLatestTimestamp || std::floor(number) != number) return std::nullopt;
		return static_cast<UnixSeconds>(number);
	}

	bool ReadStarts(const picojson::value& root, const char* key, UnixSeconds fetchedAt,
		std::vector<UnixSeconds>& result) {
		const auto& list = root.get(key);
		if (!list.is<picojson::array>()) return false;
		const auto& array = list.get<picojson::array>();
		if (array.empty() || array.size() > kMaxEventsPerType) return false;
		for (const auto& entry : array) {
			if (!entry.is<picojson::object>()) continue;
			auto timestamp = Timestamp(entry.get("timestamp"));
			if (timestamp && *timestamp >= fetchedAt - kMaxCacheAge &&
				*timestamp <= fetchedAt + kMaxCacheAge) result.push_back(*timestamp);
		}
		std::sort(result.begin(), result.end());
		result.erase(std::unique(result.begin(), result.end()), result.end());
		return !result.empty();
	}

	std::optional<Schedule> ReadSchedule(const picojson::value& root, UnixSeconds fetchedAt) {
		if (!root.is<picojson::object>() || fetchedAt < kEarliestTimestamp ||
			fetchedAt > kLatestTimestamp) return std::nullopt;
		Schedule result;
		result.fetchedAt = fetchedAt;
		if (!ReadStarts(root, "world_boss", fetchedAt, result.worldBoss) ||
			!ReadStarts(root, "legion", fetchedAt, result.legion) ||
			!ReadStarts(root, "helltide", fetchedAt, result.helltide)) return std::nullopt;
		// helltides.com publishes a daily list, so every listed event may
		// already be in the past by the time it is fetched. That must still
		// count as valid data: the cycle predictions below keep the timers
		// running until the next publication instead of showing "No data".
		return result;
	}

	Countdown NextStart(const std::vector<UnixSeconds>& starts, UnixSeconds now,
		UnixSeconds period) {
		auto next = std::lower_bound(starts.begin(), starts.end(), now);
		if (next != starts.end()) return { Phase::StartsIn, *next - now, false };
		// The cached list has ended. Keep counting, but explicitly mark the prediction.
		const auto elapsed = now - starts.back();
		const auto intervals = (elapsed + period - 1) / period;
		return { Phase::StartsIn, starts.back() + intervals * period - now, true };
	}

	picojson::value StartsJson(const std::vector<UnixSeconds>& starts) {
		picojson::array result;
		for (auto start : starts) {
			result.emplace_back(picojson::object{
				{ "timestamp", picojson::value(static_cast<double>(start)) }
			});
		}
		return picojson::value(result);
	}
}  // namespace

UnixSeconds Now() {
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

std::optional<Schedule> ParseSchedule(const std::string& json, UnixSeconds fetchedAt) {
	picojson::value root;
	if (!ParseJson(json, root)) return std::nullopt;
	return ReadSchedule(root, fetchedAt);
}

Timers CalculateTimers(const Schedule& schedule, UnixSeconds now) {
	Timers result;
	if (!schedule.Complete()) return result;
	if (now < kEarliestTimestamp || now > kLatestTimestamp ||
		now - schedule.fetchedAt > kMaxCacheAge || schedule.fetchedAt > now + 300) {
		result.expired = true;
		return result;
	}
	result.worldBoss = NextStart(schedule.worldBoss, now, kWorldBossPeriod);
	result.legion = NextStart(schedule.legion, now, kLegionPeriod);

	auto next = std::upper_bound(schedule.helltide.begin(), schedule.helltide.end(), now);
	UnixSeconds start;
	bool estimated = false;
	if (next != schedule.helltide.begin()) {
		start = *std::prev(next);
		if (now - start >= kHelltidePeriod) {
			start += ((now - start) / kHelltidePeriod) * kHelltidePeriod;
			estimated = true;
		}
	}
	else {
		// Some responses only contain the next hour; recover the current cycle.
		start = schedule.helltide.front();
		start -= ((start - now + kHelltidePeriod - 1) / kHelltidePeriod) * kHelltidePeriod;
		estimated = true;
	}
	const auto elapsed = now - start;
	result.helltide = elapsed < kHelltideDuration
		? Countdown{ Phase::EndsIn, kHelltideDuration - elapsed, estimated }
		: Countdown{ Phase::Break, kHelltidePeriod - elapsed, estimated };
	return result;
}

std::string FormatDuration(UnixSeconds seconds, const std::string& hoursUnit,
	const std::string& minutesUnit) {
	// Round up so a running countdown never misleadingly reads 0 min for 59 seconds.
	const auto minutes = seconds <= 0 ? 0 : seconds / 60 + (seconds % 60 != 0);
	const auto remainder = minutes % 60;
	return std::to_string(minutes / 60) + " " + hoursUnit + " " +
		(remainder < 10 ? "0" : "") + std::to_string(remainder) + " " + minutesUnit;
}

std::optional<Schedule> LoadCache(const std::filesystem::path& path) {
	if (path.empty()) return std::nullopt;
	std::ifstream input(path, std::ios::binary);
	if (!input) return std::nullopt;
	std::string text(kMaxPayloadBytes + 1, '\0');
	input.read(text.data(), static_cast<std::streamsize>(text.size()));
	text.resize(static_cast<std::size_t>(input.gcount()));
	if (input.bad()) return std::nullopt;
	picojson::value root;
	if (!ParseJson(text, root) || !root.get("schema").is<double>() ||
		root.get("schema").get<double>() != 1) return std::nullopt;
	auto fetchedAt = Timestamp(root.get("fetched_at"));
	if (!fetchedAt) return std::nullopt;
	return ReadSchedule(root.get("schedule"), *fetchedAt);
}

bool SaveCache(const std::filesystem::path& path, const Schedule& schedule) {
	if (path.empty() || !schedule.Complete()) return false;
	picojson::value data(picojson::object{
		{ "world_boss", StartsJson(schedule.worldBoss) },
		{ "legion", StartsJson(schedule.legion) },
		{ "helltide", StartsJson(schedule.helltide) }
	});
	if (!ReadSchedule(data, schedule.fetchedAt)) return false;
	picojson::value root(picojson::object{
		{ "schema", picojson::value(1.0) },
		{ "fetched_at", picojson::value(static_cast<double>(schedule.fetchedAt)) },
		{ "schedule", data }
	});
	const auto text = root.serialize();
	if (text.size() > kMaxPayloadBytes) return false;
	std::error_code error;
	if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
	if (error) return false;
#ifdef _WIN32
	const auto processId = GetCurrentProcessId();
#else
	const auto processId = getpid();
#endif
	auto temporary = path;
	temporary += "." + std::to_string(processId) + ".tmp";
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		output.write(text.data(), static_cast<std::streamsize>(text.size()));
		output.flush();
		output.close();
		if (!output) {
			std::filesystem::remove(temporary, error);
			return false;
		}
	}
#ifdef _WIN32
	bool replaced = MoveFileExW(temporary.c_str(), path.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	std::filesystem::rename(temporary, path, error);
	bool replaced = !error;
#endif
	if (!replaced) std::filesystem::remove(temporary, error);
	return replaced;
}
}  // namespace events
