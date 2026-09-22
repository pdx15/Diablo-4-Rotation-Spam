#ifdef NDEBUG
#error "Event service tests require assertions enabled"
#endif

#include <winhttp.h> // isolated Win32/HTTP test doubles
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

	std::filesystem::remove(path);
	fake_winhttp::Reset(""); fake_winhttp::networkFailure = true;
	{
		events::Service service;
		service.Start();
		auto view = WaitForResult(service);
		assert(view.sync == events::SyncState::Offline);
		assert(view.timers.worldBoss.phase == events::Phase::Unknown);
	}
	assert(fake_winhttp::handles == 0);
	std::cout << "PASS: no cache plus no network gives no invented timers\n";

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
	std::filesystem::remove_all(root);
}
