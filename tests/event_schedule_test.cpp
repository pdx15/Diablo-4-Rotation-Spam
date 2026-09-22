#ifdef NDEBUG
#error "Event schedule tests require assertions enabled"
#endif

#include "../event_schedule.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>

using namespace events;
namespace {
// 2026-09-22 00:00:00 UTC. Tests never depend on the machine's current date/timezone.
constexpr UnixSeconds base = 1790035200;

std::string Payload(const std::string& boss = "1790042400", const std::string& legion = "1790036700",
	const std::string& helltide = "1790035200") {
	return "{\"world_boss\":[{\"boss\":\"Azmodan\",\"timestamp\":" + boss +
		",\"zone\":[{\"name\":\"Fractured Peaks\",\"timestamp\":1}]}],"
		"\"legion\":[{\"timestamp\":" + legion + "}],"
		"\"helltide\":[{\"timestamp\":" + helltide + "}]}";
}

Schedule Example() {
	auto schedule = ParseSchedule(Payload(), base);
	assert(schedule);
	return *schedule;
}

void TestParsing() {
	auto schedule = Example();
	assert(schedule.worldBoss == std::vector<UnixSeconds>{base + 7200});
	assert(schedule.legion == std::vector<UnixSeconds>{base + 1500});
	assert(schedule.helltide == std::vector<UnixSeconds>{base});

	auto unordered = ParseSchedule(R"({
		"meta":{"timestamp":1},
		"world_boss":[{"timestamp":1790055000},{"timestamp":1790042400},{"timestamp":1790042400}],
		"legion":[{"timestamp":null},{"timestamp":1790036700}],
		"helltide":[{"timestamp":1790035200},{"startTime":"ignored","timestamp":1790038800}]
	})", base);
	assert(unordered && unordered->worldBoss.size() == 2);
	assert(unordered->worldBoss.front() == base + 7200);
	assert(unordered->legion.size() == 1);
	assert(unordered->helltide.size() == 2);

	for (const auto& invalid : {"null", "true", "-1", "1790042400000", "1790042400.5",
		"\"1790042400\"", "1e400", "{}", "[]"}) {
		assert(!ParseSchedule(Payload(invalid), base));
	}
	assert(!ParseSchedule("<html>Unavailable</html>", base));
	assert(!ParseSchedule("[]", base));
	assert(!ParseSchedule("{}", base));
	assert(!ParseSchedule("{\"world_boss\":[]}", base));
	assert(!ParseSchedule(Payload() + "false", base));
	assert(ParseSchedule(Payload() + " \n\t", base));
	assert(!ParseSchedule(std::string(kMaxPayloadBytes + 1, ' '), base));
	assert(!ParseSchedule(std::string(40, '[') + "0" + std::string(40, ']'), base));
	assert(ParseSchedule(Payload(), base + 86400 - 1)); // in-range list stays valid
	auto stale = ParseSchedule(Payload(), base + 86400);
	assert(stale);  // a daily list fully in the past still feeds predictions
	auto staleTimers = CalculateTimers(*stale, base + 86400);
	assert(staleTimers.worldBoss.phase == Phase::StartsIn && staleTimers.worldBoss.estimated);
	assert(staleTimers.legion.phase == Phase::StartsIn && staleTimers.legion.estimated);
	assert(staleTimers.helltide.phase == Phase::EndsIn && staleTimers.helltide.estimated);
	assert(!ParseSchedule(Payload(), base + 8 * 86400)); // beyond the 7-day window
	assert(!ParseSchedule(Payload(), 0));

	std::string detail;
	assert(!ParseSchedule("", base, &detail) && detail == "empty response body");
	assert(!ParseSchedule(std::string(kMaxPayloadBytes + 1, ' '), base, &detail) &&
		detail == "response too large (131073 bytes)");
	assert(!ParseSchedule("<html>Unavailable</html>", base, &detail) &&
		detail == "invalid JSON (24 bytes)");
	assert(!ParseSchedule("{}", base, &detail) &&
		detail == "schedule key 'world_boss': missing or not an array");
	assert(!ParseSchedule("{\"world_boss\":[]}", base, &detail) &&
		detail == "schedule key 'world_boss': empty list");
	assert(!ParseSchedule(Payload("null"), base, &detail) &&
		detail == "schedule key 'world_boss': no timestamps within range (1 entry)");
	assert(!ParseSchedule(Payload(), 0, &detail) && detail == "fetch time out of range");
	detail = "untouched";
	assert(ParseSchedule(Payload(), base, &detail) && detail == "untouched");
	std::cout << "PASS: structured JSON, sorting/deduplication, invalid/stale/bounded payloads\n";
	std::cout << "PASS: parse-failure reasons for empty/oversized/invalid payloads and bad keys\n";
}

void TestLocalSchedule() {
	auto local = BuildLocalSchedule(base);
	assert(local.Complete());
	assert(local.fetchedAt == base);
	assert(local.worldBossAnchor == kWorldBossAnchor);
	assert(local.legionAnchor == kLegionAnchor);
	// The grid starts two periods before now so an already-running phase is
	// recoverable: base (2026-09-22 00:00Z) sits exactly on the hour and on
	// the 25-minute legion grid.
	assert(local.helltide.front() == base - 2 * kHelltidePeriod);
	assert(local.helltide[2] == base && local.helltide[3] == base + kHelltidePeriod);
	assert(local.legion.front() == base - 2 * kLegionPeriod);
	assert(local.legion[2] == base && local.legion[3] == base + kLegionPeriod);
	// base is 446400 s after the boss anchor (35.43 periods), so the first
	// list point is the 34th grid point (base - 18000) and the next is +7200.
	assert(local.worldBoss.front() == base - 18000);
	assert(local.worldBoss[1] == base - 5400 && local.worldBoss[2] == base + 7200);
	const auto last = base + 48 * 60 * 60;
	auto checkGrid = [&](const std::vector<UnixSeconds>& list, UnixSeconds anchor,
		UnixSeconds period) {
		assert(!list.empty() && *std::prev(list.end()) <= last);
		assert(std::is_sorted(list.begin(), list.end()) &&
			std::adjacent_find(list.begin(), list.end(),
			[](auto a, auto b) { return b <= a; }) == list.end());
		for (auto t : list)
			assert((t - anchor) % period == 0 && t - anchor >= -2 * period);
	};
	checkGrid(local.worldBoss, kWorldBossAnchor, kWorldBossPeriod);
	checkGrid(local.legion, kLegionAnchor, kLegionPeriod);
	checkGrid(local.helltide, 0, kHelltidePeriod);

	// Countdowns over a full local grid are exact, not flagged as predictions.
	auto timers = CalculateTimers(local, base + 300);
	assert(!timers.Estimated() && !timers.expired);
	assert(timers.worldBoss.phase == Phase::StartsIn && timers.worldBoss.seconds == 6900);
	assert(timers.legion.phase == Phase::StartsIn && timers.legion.seconds == 1200);
	assert(timers.helltide.phase == Phase::EndsIn && timers.helltide.seconds == 3000);

	// A clock before the anchor still lands on the anchor's own grid: now is
	// 4000 s before the anchor, so the window now-2*period starts just past
	// the grid point two periods before the anchor.
	auto early = BuildLocalSchedule(kWorldBossAnchor - 4000);
	assert(early.Complete() &&
		early.worldBoss.front() == kWorldBossAnchor - 2 * kWorldBossPeriod);
	// A clock exactly on the grid counts that point as the current start.
	auto onGrid = BuildLocalSchedule(kWorldBossAnchor + 3 * kWorldBossPeriod);
	auto atStart = CalculateTimers(onGrid, onGrid.fetchedAt);
	assert(atStart.worldBoss.seconds == 0 && !atStart.worldBoss.estimated);
	// A shifted anchor moves the whole grid by the same offset.
	auto shifted = BuildLocalSchedule(base, kWorldBossAnchor + 600, kLegionAnchor);
	assert(shifted.worldBoss.front() == base - 18000 + 600);
	// Unusable clocks or anchors yield an incomplete schedule, never a crash.
	assert(!BuildLocalSchedule(0).Complete());
	assert(!BuildLocalSchedule(std::numeric_limits<UnixSeconds>::max()).Complete());
	assert(!BuildLocalSchedule(base, 0, kLegionAnchor).Complete());
	assert(!BuildLocalSchedule(base, kWorldBossAnchor, 1).Complete());
	std::cout << "PASS: local schedule grids, 48-hour horizon, exact countdowns and anchors\n";
}

void TestCountdowns() {
	auto schedule = Example();
	assert(schedule.worldBossAnchor == base + 7200);  // re-derived from the list
	assert(schedule.legionAnchor == base + 1500);
	auto view = CalculateTimers(schedule, base + 300);
	assert(view.worldBoss.phase == Phase::StartsIn && view.worldBoss.seconds == 6900);
	assert(view.legion.phase == Phase::StartsIn && view.legion.seconds == 1200);
	assert(view.helltide.phase == Phase::EndsIn && view.helltide.seconds == 3000);
	assert(!view.Estimated() && !view.expired);
	assert(CalculateTimers(schedule, base + 1500).legion.seconds == 0);
	assert(CalculateTimers(schedule, base + 1501).legion.seconds == 1499);
	assert(CalculateTimers(schedule, base + 1501).legion.estimated);
	assert(CalculateTimers(schedule, base + 7200).worldBoss.seconds == 0);
	assert(CalculateTimers(schedule, base + 7201).worldBoss.seconds == kWorldBossPeriod - 1);
	assert(CalculateTimers(schedule, base + 7201).worldBoss.estimated);
	assert(CalculateTimers(schedule, base + 86400).legion.seconds == 600);
	assert(CalculateTimers(schedule, base + 86400).worldBoss.seconds == 9000);

	// A refreshed schedule wins over a previous projection, even if its phase changes.
	auto refreshed = ParseSchedule(Payload("1790042460", "1790036760"), base + 600);
	assert(refreshed);
	assert(CalculateTimers(*refreshed, base + 600).worldBoss.seconds == 6660);
	assert(CalculateTimers(*refreshed, base + 600).legion.seconds == 960);
	std::cout << "PASS: boss/legion countdowns, exact starts, cached predictions, resynchronization\n";
}

void TestHelltideBoundaries() {
	auto schedule = Example();
	for (UnixSeconds offset : {0, 1, 3299}) {
		auto timer = CalculateTimers(schedule, base + offset).helltide;
		assert(timer.phase == Phase::EndsIn && timer.seconds == 3300 - offset);
	}
	for (UnixSeconds offset : {3300, 3301, 3599}) {
		auto timer = CalculateTimers(schedule, base + offset).helltide;
		assert(timer.phase == Phase::Break && timer.seconds == 3600 - offset);
	}
	auto nextHour = CalculateTimers(schedule, base + 3600).helltide;
	assert(nextHour.phase == Phase::EndsIn && nextHour.seconds == 3300 && nextHour.estimated);
	schedule.helltide.push_back(base + 3600);
	assert(!CalculateTimers(schedule, base + 3600).helltide.estimated);
	schedule.helltide.erase(schedule.helltide.begin()); // API returned only the next hour
	assert(CalculateTimers(schedule, base + 3299).helltide.seconds == 1);
	assert(CalculateTimers(schedule, base + 3300).helltide.phase == Phase::Break);
	std::cout << "PASS: Helltide start, 55-minute end, five-minute break and next-hour rollover\n";
}

void TestFormattingAndExpiry() {
	assert(FormatDuration(0, "h", "min") == "0 h 00 min");
	assert(FormatDuration(-1, "h", "min") == "0 h 00 min");
	assert(FormatDuration(1, "h", "min") == "0 h 01 min");
	assert(FormatDuration(60, "h", "min") == "0 h 01 min");
	assert(FormatDuration(61, "h", "min") == "0 h 02 min");
	assert(FormatDuration(12600, "h", "min") == "3 h 30 min");
	assert(FormatDuration(3599, "ч", "мин") == "1 ч 00 мин");
	assert(!FormatDuration(std::numeric_limits<UnixSeconds>::max(), "h", "min").empty());
	assert(CalculateTimers({}, base).worldBoss.phase == Phase::Unknown);
	auto schedule = Example();
	assert(!CalculateTimers(schedule, base + kMaxCacheAge).expired);
	auto expired = CalculateTimers(schedule, base + kMaxCacheAge + 1);
	assert(expired.expired && expired.worldBoss.phase == Phase::Unknown);
	assert(expired.legion.phase == Phase::Unknown && expired.helltide.phase == Phase::Unknown);
	assert(CalculateTimers(schedule, base - 301).expired); // bad future-dated cache / clock rollback
	assert(CalculateTimers(schedule, std::numeric_limits<UnixSeconds>::min()).expired);
	std::cout << "PASS: localized hour/minute formatting, rounding and cache expiry\n";
}

void TestCache() {
	auto root = std::filesystem::temp_directory_path() /
		("d4rt-events-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	auto path = root / "nested" / "events_cache.json";
	assert(!LoadCache(path));
	auto schedule = Example();
	assert(SaveCache(path, schedule));
	auto loaded = LoadCache(path);
	assert(loaded && loaded->fetchedAt == base);
	assert(loaded->worldBoss == schedule.worldBoss && loaded->legion == schedule.legion);
	assert(loaded->helltide == schedule.helltide);
	assert(CalculateTimers(*loaded, base + 300).helltide.seconds == 3000);
	schedule.worldBoss[0] += 60;
	assert(SaveCache(path, schedule)); // replaces an existing file, not just first-save
	assert(LoadCache(path)->worldBoss == schedule.worldBoss);
	assert(!SaveCache(path, {})); // invalid data must not destroy the last good cache
	assert(LoadCache(path)->worldBoss == schedule.worldBoss);
	assert(!SaveCache({}, schedule));
	assert(!SaveCache(path / "not-a-directory.json", schedule));
	assert(LoadCache(path));

	// Schema 2 persists the local-clock anchors; a schema 1 cache still loads
	// and falls back to the built-in phase references.
	auto anchored = Example();
	anchored.worldBossAnchor = base + 7200;
	anchored.legionAnchor = base + 1500;
	assert(SaveCache(path, anchored));
	loaded = LoadCache(path);
	assert(loaded && loaded->worldBossAnchor == base + 7200 &&
		loaded->legionAnchor == base + 1500);
	{
		std::ofstream v1(path, std::ios::binary | std::ios::trunc);
		v1 << std::string("{\"schema\":1,\"fetched_at\":") + std::to_string(base) +
			",\"schedule\":" + Payload() + "}";
	}
	loaded = LoadCache(path);
	assert(loaded && loaded->worldBossAnchor == kWorldBossAnchor &&
		loaded->legionAnchor == kLegionAnchor);

	for (const auto& text : {std::string("{"), std::string("{\"schema\":2}"),
		std::string("{\"schema\":3}"),
		std::string("{\"schema\":1,\"fetched_at\":0}"), std::string(kMaxPayloadBytes + 1, ' ')}) {
		std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
		assert(!LoadCache(path));
	}
	std::filesystem::remove_all(root);
	std::cout << "PASS: disk cache round-trip/replacement, missing/corrupt/oversized cache, write failure\n";
}
}

int main() {
	TestParsing();
	TestLocalSchedule();
	TestCountdowns();
	TestHelltideBoundaries();
	TestFormattingAndExpiry();
	TestCache();
}
