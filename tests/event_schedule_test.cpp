#ifdef NDEBUG
#error "Event schedule tests require assertions enabled"
#endif

#include "../src/event_schedule.h"

#include <cassert>
#include <iostream>
#include <limits>

using namespace events;
namespace {
// 2026-09-22 00:00:00 UTC: exactly on the hour and on the 25-minute legion
// grid. Tests never depend on the machine's current date or timezone.
constexpr UnixSeconds base = 1790035200;

void TestConstants() {
	// Pin the verified phase references: a silent edit must fail the suite.
	assert(kWorldBossAnchor == 1789588800);  // 2026-09-16T20:00:00Z
	assert(kLegionAnchor == 1789588200);  // 2026-09-16T19:50:00Z
	assert(kWorldBossPeriod == 210 * 60 && kLegionPeriod == 25 * 60);
	assert(kHelltidePeriod == 60 * 60 && kHelltideDuration == 55 * 60);
}

void TestGrids() {
	// The 210-minute boss grid from the 2026-09-16 20:00Z anchor reproduces
	// the published 2026-09-22/23 spawns: 02:00, 05:30, 09:00, 12:30, 16:00,
	// 19:30, 23:00Z and 02:30Z the next day.
	for (UnixSeconds offset : {7200, 19800, 32400, 45000, 57600, 70200, 82800, 95400})
		assert(CalculateTimers(base + offset).worldBoss.seconds == 0);
	// The 25-minute legion grid from the 19:50Z anchor lands on the hour at
	// base. 25 minutes does not divide 24 hours, so the grid slips relative to
	// the midnight: the day after base the spawns run 00:10, 00:35, 01:00Z...
	assert(CalculateTimers(base).legion.seconds == 0);
	assert(CalculateTimers(base + 86400).legion.seconds == 600);
	// A second before a spawn the raw countdown is one second.
	assert(CalculateTimers(base + 7200 - 1).worldBoss.seconds == 1);
}

void TestCountdowns() {
	auto timers = CalculateTimers(base + 300);
	assert(timers.worldBoss.phase == Phase::StartsIn && timers.worldBoss.seconds == 6900);
	assert(timers.legion.phase == Phase::StartsIn && timers.legion.seconds == 1200);
	assert(timers.helltide.phase == Phase::EndsIn && timers.helltide.seconds == 3000);

	// The grids continue unchanged across midnight (2026-09-22 23:59:59Z):
	// boss at 02:30Z (+9001 s), legion at 00:10Z (+601 s), break to the hour.
	auto late = CalculateTimers(base + 86399);
	assert(late.worldBoss.phase == Phase::StartsIn && late.worldBoss.seconds == 9001);
	assert(late.legion.phase == Phase::StartsIn && late.legion.seconds == 601);
	assert(late.helltide.phase == Phase::Break && late.helltide.seconds == 1);
}

void TestHelltideBoundaries() {
	for (UnixSeconds offset : {0, 1, 3299}) {
		auto timer = CalculateTimers(base + offset).helltide;
		assert(timer.phase == Phase::EndsIn && timer.seconds == 3300 - offset);
	}
	for (UnixSeconds offset : {3300, 3301, 3599}) {
		auto timer = CalculateTimers(base + offset).helltide;
		assert(timer.phase == Phase::Break && timer.seconds == 3600 - offset);
	}
	// Rollover into the next hour starts a fresh 55-minute phase.
	auto nextHour = CalculateTimers(base + 3600).helltide;
	assert(nextHour.phase == Phase::EndsIn && nextHour.seconds == 3300);
}

void TestFormatting() {
	assert(FormatDuration(0, "h", "min") == "0 h 00 min");
	assert(FormatDuration(-1, "h", "min") == "0 h 00 min");
	assert(FormatDuration(1, "h", "min") == "0 h 01 min");
	assert(FormatDuration(60, "h", "min") == "0 h 01 min");
	assert(FormatDuration(61, "h", "min") == "0 h 02 min");
	assert(FormatDuration(12600, "h", "min") == "3 h 30 min");
	assert(FormatDuration(3599, "ч", "мин") == "1 ч 00 мин");
	assert(!FormatDuration(std::numeric_limits<UnixSeconds>::max(), "h", "min").empty());
	// The wall clock stays in the Unix-seconds domain.
	assert(Now() > 1700000000);
}
}  // namespace

int main() {
	TestConstants();
	TestGrids();
	TestCountdowns();
	TestHelltideBoundaries();
	TestFormatting();
	std::cout << "PASS: verified 210/25/60-minute grids, exact starts and Helltide boundaries\n";
	std::cout << "PASS: midnight rollover, localized durations and wall clock\n";
}
