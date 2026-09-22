#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace events {
using UnixSeconds = std::int64_t;
inline constexpr std::size_t kMaxPayloadBytes = 128 * 1024;
inline constexpr UnixSeconds kWorldBossPeriod = 210 * 60;
inline constexpr UnixSeconds kLegionPeriod = 25 * 60;
inline constexpr UnixSeconds kHelltidePeriod = 60 * 60;
inline constexpr UnixSeconds kHelltideDuration = 55 * 60;
inline constexpr UnixSeconds kMaxCacheAge = 7 * 24 * 60 * 60;
inline constexpr int kRefreshSeconds = 5 * 60;

struct Schedule {
	UnixSeconds fetchedAt = 0;
	std::vector<UnixSeconds> worldBoss;
	std::vector<UnixSeconds> legion;
	std::vector<UnixSeconds> helltide;

	bool Complete() const {
		return fetchedAt > 0 && !worldBoss.empty() && !legion.empty() && !helltide.empty();
	}
};

enum class Phase { Unknown, StartsIn, EndsIn, Break };

struct Countdown {
	Phase phase = Phase::Unknown;
	UnixSeconds seconds = 0;
	bool estimated = false;
};

struct Timers {
	Countdown worldBoss;
	Countdown legion;
	Countdown helltide;
	bool expired = false;

	bool Estimated() const {
		return worldBoss.estimated || legion.estimated || helltide.estimated;
	}
};

UnixSeconds Now();
// All arithmetic uses Unix seconds, never local time or a timezone offset.
std::optional<Schedule> ParseSchedule(const std::string& json, UnixSeconds fetchedAt);
Timers CalculateTimers(const Schedule& schedule, UnixSeconds now);
std::string FormatDuration(UnixSeconds seconds, const std::string& hoursUnit,
	const std::string& minutesUnit);

std::optional<Schedule> LoadCache(const std::filesystem::path& path);
// Writes a temporary file, then atomically replaces the old cache.
bool SaveCache(const std::filesystem::path& path, const Schedule& schedule);
}  // namespace events
