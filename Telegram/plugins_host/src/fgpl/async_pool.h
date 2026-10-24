#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace fgpl {

class AsyncPool final {
public:
	explicit AsyncPool(int threads);
	~AsyncPool();

	void post(std::function<void()> work);
	void postAfter(int64_t delayMs, std::function<void()> work);
	void stop();

private:
	using Clock = std::chrono::steady_clock;

	void ensureStarted();
	void loop();

	const int _size = 1;
	std::mutex _mutex;
	std::condition_variable _wake;
	std::deque<std::function<void()>> _queue;
	std::multimap<Clock::time_point, std::function<void()>> _timers;
	std::vector<std::thread> _threads;
	bool _stopping = false;

};

} // namespace fgpl
