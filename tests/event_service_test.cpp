#ifdef NDEBUG
#error "Event service tests require assertions enabled"
#endif

#include <winhttp.h> // isolated Win32/HTTP test doubles
#include "../event_log.h"
#include "../event_service.h"

#include <fstream>
#include <iostream>

namespace {
std::string Payload() {
	auto now = events::Now();
	return "{\"world_boss\":[{\"timestamp\":" + std::to_string(now + 7200) +
		"}],\"legion\":[{\"timestamp\":" + std::to_string(now + 1500) +
		"}],\"helltide\":[{\"timestamp\":" + std::to_string(now - now % 3600) + "}]}";
}

events::View WaitForResult(events::Service& service) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < deadline) {
		auto view = service.GetView();
		if (view.sync != events::SyncState::Loading) return view;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	assert(false && "Event worker did not finish");
	return {};
}

std::string ReadLog(const std::filesystem::path& logPath) {
	std::ifstream input(logPath, std::ios::binary);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void CheckOfflineFallback(const std::filesystem::path& path) {
	auto before = events::LoadCache(path);
	assert(before);
	events::Service service;
	service.Start();
	auto view = WaitForResult(service);
	assert(view.sync == events::SyncState::Offline);
	assert(view.timers.worldBoss.phase == events::Phase::StartsIn);
	assert(view.timers.worldBoss.seconds > 0);
	service.Stop();
	assert(fake_winhttp::requests == 1 && fake_winhttp::handles == 0);
	auto after = events::LoadCache(path);
	assert(after && after->fetchedAt == before->fetchedAt && after->worldBoss == before->worldBoss);
}
}

int main() {
	const auto root = std::filesystem::temp_directory_path() /
		("d4rt-service-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(root);
	fake_win32::appData = root.string();
	const auto path = root / "d4rt" / "events_cache.json";
	const auto logPath = root / "d4rt" / "event_log.txt";
	fake_winhttp::Reset(Payload());
	fake_winhttp::blockResponse = true;
	{
		events::Service service;
		assert(service.GetView().timers.worldBoss.phase == events::Phase::Unknown);
		service.Start();
		service.Start(); // opening the overlay again must not spawn another worker
		{
			std::unique_lock lock(fake_winhttp::mutex);
			assert(fake_winhttp::condition.wait_for(lock, std::chrono::seconds(3), [] {
				return fake_winhttp::responseEntered;
			}));
		}
		const auto start = std::chrono::steady_clock::now();
		assert(service.GetView().sync == events::SyncState::Loading);
		assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
		{
			std::lock_guard lock(fake_winhttp::mutex);
			fake_winhttp::responseReleased = true;
		}
		fake_winhttp::condition.notify_all();
		auto view = WaitForResult(service);
		assert(view.sync == events::SyncState::Online && !view.cacheWriteFailed);
		assert(view.timers.worldBoss.phase == events::Phase::StartsIn);
		assert(view.timers.legion.phase == events::Phase::StartsIn);
		assert(events::LoadCache(path));
		const auto stopping = std::chrono::steady_clock::now();
		service.Stop();
		assert(std::chrono::steady_clock::now() - stopping < std::chrono::seconds(1));
	}
	assert(fake_winhttp::handles == 0 && fake_winhttp::requests == 1);
	std::cout << "PASS: background HTTPS refresh, nonblocking view, lazy start, cache save, cancellable wait and handle cleanup\n";

	fake_winhttp::Reset(Payload()); fake_winhttp::networkFailure = true;
	CheckOfflineFallback(path);
	fake_winhttp::Reset(Payload()); fake_winhttp::status = 429;
	CheckOfflineFallback(path);
	fake_winhttp::Reset(Payload()); fake_winhttp::readFailure = true;
	CheckOfflineFallback(path);
	fake_winhttp::Reset("<html>Unavailable</html>");
	CheckOfflineFallback(path);
	fake_winhttp::Reset(std::string(events::kMaxPayloadBytes + 1, ' '));
	CheckOfflineFallback(path);
	std::cout << "PASS: cached restart on network, HTTP 429, read, malformed JSON and oversized-response failures\n";

	const std::string log = ReadLog(logPath);
	assert(log.find("worker started (d4rt ") != std::string::npos);
	assert(log.find("cache loaded: boss=") != std::string::npos);
	assert(log.find("HTTP 200") != std::string::npos && log.find("state=online") != std::string::npos);
	assert(log.find("WinHttpSendRequest failed (win32=12029)") != std::string::npos);
	assert(log.find("HTTP status 429") != std::string::npos);
	assert(log.find("response read failed after 0 bytes (win32=12030)") != std::string::npos);
	assert(log.find("parse failed (invalid JSON (24 bytes))") != std::string::npos);
	assert(log.find("response exceeded 131072-byte cap") != std::string::npos);
	assert(log.find("state=offline") != std::string::npos);
	assert(log.find("worker stopped") != std::string::npos);
	std::cout << "PASS: event log records startup, cache, fetch/parse failures and states\n";

	std::filesystem::remove(path);
	fake_winhttp::Reset(""); fake_winhttp::networkFailure = true;
	{
		events::Service service;
		service.Start();
		auto view = WaitForResult(service);
		assert(view.sync == events::SyncState::Offline);
		// No cache and an unreachable publisher (e.g. blocked by Cloudflare):
		// the deterministic local clock (hourly helltide, 210-minute boss,
		// 25-minute legion) keeps the panel alive instead of showing no data.
		assert(view.timers.worldBoss.phase == events::Phase::StartsIn &&
			0 <= view.timers.worldBoss.seconds &&
			view.timers.worldBoss.seconds <= events::kWorldBossPeriod);
		assert(view.timers.legion.phase == events::Phase::StartsIn &&
			0 <= view.timers.legion.seconds &&
			view.timers.legion.seconds <= events::kLegionPeriod);
		assert((view.timers.helltide.phase == events::Phase::EndsIn ||
			view.timers.helltide.phase == events::Phase::Break) &&
			0 < view.timers.helltide.seconds && view.timers.helltide.seconds <= 55 * 60);
		assert(!events::LoadCache(path)); // the local schedule never touches disk
	}
	assert(fake_winhttp::handles == 0);
	assert(ReadLog(logPath).find("local schedule from anchors") != std::string::npos);
	std::cout << "PASS: no cache plus no network falls back to the unpersisted local schedule\n";

	std::ofstream(root / "blocked") << "not a directory";
	fake_win32::appData = (root / "blocked").string();
	fake_winhttp::Reset(Payload());
	{
		events::Service service;
		service.Start();
		auto view = WaitForResult(service);
		assert(view.sync == events::SyncState::Online && view.cacheWriteFailed);
		assert(view.timers.worldBoss.phase == events::Phase::StartsIn);
	}
	assert(fake_winhttp::handles == 0);
	std::cout << "PASS: unwritable cache does not discard valid in-memory schedule\n";

	{
		const auto rotationPath = root / "rotation" / "event_log.txt";
		const std::string filler(400, 'x');
		for (int i = 0; i < 3000; ++i)
			events::LogEvent(rotationPath, "line " + std::to_string(i) + " " + filler);
		std::error_code error;
		const auto size = std::filesystem::file_size(rotationPath, error);
		assert(!error && size <= events::kMaxLogBytes + 512);
		const std::string rotated = ReadLog(rotationPath);
		assert(!rotated.empty() && rotated.front() == '[');
		assert(rotated.find("line 2999") != std::string::npos);
		assert(rotated.find("line 0 ") == std::string::npos);
		std::cout << "PASS: event log rotation caps the file and keeps the newest lines\n";
	}
	std::filesystem::remove_all(root);
}
