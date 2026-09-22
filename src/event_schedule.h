#pragma once

#include <cstdint>
#include <string>

namespace events {
using UnixSeconds = std::int64_t;

// Fixed game clock: Helltide starts on the top of every hour and stays active
// for 55 minutes, World Boss spawns every 3.5 hours, Legion events start
// every 25 minutes.
inline constexpr UnixSeconds kWorldBossPeriod = 210 * 60;
inline constexpr UnixSeconds kLegionPeriod = 25 * 60;
inline constexpr UnixSeconds kHelltidePeriod = 60 * 60;
inline constexpr UnixSeconds kHelltideDuration = 55 * 60;

// Phase references of the fixed game clock: event starts lie on
// anchor + n*period. Verified 2026-09-16 against the published schedule: the
// world boss grid reproduces the 2026-09-22/23 spawns (19:30, 23:00, 02:30,
// 06:00, 09:30 UTC) and the 25-minute legion grid lands exactly on the hour.
// If a patch shifts an event's phase, update the anchor here.
inline constexpr UnixSeconds kWorldBossAnchor = 1789588800;  // 2026-09-16T20:00:00Z
inline constexpr UnixSeconds kLegionAnchor = 1789588200;  // 2026-09-16T19:50:00Z

enum class Phase { StartsIn, EndsIn, Break };

struct Countdown {
	Phase phase = Phase::StartsIn;
	UnixSeconds seconds = 0;
};

struct Timers {
	Countdown worldBoss;
	Countdown legion;
	Countdown helltide;
};

UnixSeconds Now();
// All arithmetic uses Unix seconds on the fixed game clock, never local time
// or a timezone offset.
Timers CalculateTimers(UnixSeconds now);
std::string FormatDuration(UnixSeconds seconds, const std::string& hoursUnit,
	const std::string& minutesUnit);
}  // namespace events
