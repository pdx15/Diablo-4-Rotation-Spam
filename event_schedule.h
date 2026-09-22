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
// A failed refresh is retried sooner so a transient outage or a list that the
// site has not published yet delays the timers by a minute, not five.
inline constexpr int kRetrySeconds = 60;
// Phase references of the deterministic local clock (see BuildLocalSchedule):
// event starts lie on anchor + n*period. Verified 2026-09-16 against the
// helltides.com schedule: the world boss grid below reproduces the published
// 2026-09-22/23 spawns (19:30, 23:00, 02:30, 06:00, 09:30 UTC) and the 25-min
// legion grid lands exactly on the hour. A successful fetch re-derives both
// anchors from the publisher's lists; these defaults only seed the very first
// local schedule. If Blizzard shifts an event's phase while no fetch ever
// succeeds, update these constants (as the anchor-verified trackers do).
inline constexpr UnixSeconds kWorldBossAnchor = 1789588800;  // 2026-09-16T20:00:00Z
inline constexpr UnixSeconds kLegionAnchor = 1789588200;  // 2026-09-16T19:50:00Z

struct Schedule {
	UnixSeconds fetchedAt = 0;
	std::vector<UnixSeconds> worldBoss;
	std::vector<UnixSeconds> legion;
	std::vector<UnixSeconds> helltide;
	// Current phase of each periodic event, used by the local fallback and
	// persisted in the cache so a restart stays on the same grid.
	UnixSeconds worldBossAnchor = kWorldBossAnchor;
	UnixSeconds legionAnchor = kLegionAnchor;

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
// When failureDetail is not null it receives a short English reason if parsing
// fails; it is left untouched on success.
std::optional<Schedule> ParseSchedule(const std::string& json, UnixSeconds fetchedAt,
	std::string* failureDetail = nullptr);
// Deterministic local schedule: 48 hours (plus a few past entries, so an
// already-running phase is recoverable) of helltide starts on the hour, world
// boss every kWorldBossPeriod and legion every kLegionPeriod on their anchor
// grids. No network involved; it backs the panel when the publisher is
// unreachable. Returns an incomplete Schedule if now or an anchor is out of
// range so callers can detect an unusable clock.
Schedule BuildLocalSchedule(UnixSeconds now, UnixSeconds worldBossAnchor = kWorldBossAnchor,
	UnixSeconds legionAnchor = kLegionAnchor);
Timers CalculateTimers(const Schedule& schedule, UnixSeconds now);
std::string FormatDuration(UnixSeconds seconds, const std::string& hoursUnit,
	const std::string& minutesUnit);

std::optional<Schedule> LoadCache(const std::filesystem::path& path);
// Writes a temporary file, then atomically replaces the old cache.
bool SaveCache(const std::filesystem::path& path, const Schedule& schedule);
}  // namespace events
