#include "event_schedule.h"

#include <chrono>

namespace events {
namespace {
	// Smallest t on the grid anchor + n*period with t >= now.
	UnixSeconds NextStart(UnixSeconds anchor, UnixSeconds period, UnixSeconds now) {
		const auto offset = now - anchor;
		auto steps = offset / period;  // truncates toward zero
		if (offset % period != 0 && offset > 0) ++steps;
		return anchor + steps * period;
	}
}  // namespace

UnixSeconds Now() {
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

Timers CalculateTimers(UnixSeconds now) {
	Timers result;
	result.worldBoss = { Phase::StartsIn,
		NextStart(kWorldBossAnchor, kWorldBossPeriod, now) - now };
	result.legion = { Phase::StartsIn,
		NextStart(kLegionAnchor, kLegionPeriod, now) - now };
	const auto elapsed = now % kHelltidePeriod;  // seconds since the top of the hour
	result.helltide = elapsed < kHelltideDuration
		? Countdown{ Phase::EndsIn, kHelltideDuration - elapsed }
		: Countdown{ Phase::Break, kHelltidePeriod - elapsed };
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
}  // namespace events
