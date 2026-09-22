#pragma once

#include "event_schedule.h"

#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

namespace events {
enum class SyncState { Loading, Online, Offline };

struct View {
	Timers timers;
	SyncState sync = SyncState::Loading;
	bool cacheWriteFailed = false;
};

// Owned by the UI thread. Disk and HTTP operations are confined to its worker.
class Service {
public:
	Service() = default;
	~Service();
	Service(const Service&) = delete;
	Service& operator=(const Service&) = delete;
	void Start();
	void Stop();
	View GetView() const;

private:
	void Run(std::stop_token stop);
	mutable std::mutex stateMutex_;
	Schedule schedule_;
	SyncState sync_ = SyncState::Loading;
	bool cacheWriteFailed_ = false;
	bool started_ = false;
	std::mutex waitMutex_;
	std::condition_variable_any wake_;
	std::jthread worker_;
};
}  // namespace events
