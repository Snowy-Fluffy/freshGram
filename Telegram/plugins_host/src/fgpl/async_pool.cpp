#include "fgpl/async_pool.h"

#include <algorithm>

namespace fgpl {

AsyncPool::AsyncPool(int threads)
: _size(std::max(threads, 1)) {
}

AsyncPool::~AsyncPool() {
	stop();
}

void AsyncPool::ensureStarted() {
	if (_threads.empty()) {
		_stopping = false;
		for (auto i = 0; i != _size; ++i) {
			_threads.emplace_back([this] { loop(); });
		}
	}
}

void AsyncPool::post(std::function<void()> work) {
	{
		const auto lock = std::scoped_lock(_mutex);
		if (_stopping) {
			return;
		}
		ensureStarted();
		_queue.push_back(std::move(work));
	}
	_wake.notify_one();
}

void AsyncPool::postAfter(int64_t delayMs, std::function<void()> work) {
	{
		const auto lock = std::scoped_lock(_mutex);
		if (_stopping) {
			return;
		}
		ensureStarted();
		_timers.emplace(
			Clock::now() + std::chrono::milliseconds(std::max<int64_t>(delayMs, 0)),
			std::move(work));
	}
	_wake.notify_all();
}

void AsyncPool::stop() {
	auto threads = std::vector<std::thread>();
	auto dropped = std::deque<std::function<void()>>();
	auto timers = std::multimap<Clock::time_point, std::function<void()>>();
	{
		const auto lock = std::scoped_lock(_mutex);
		_stopping = true;
		threads.swap(_threads);
		dropped.swap(_queue);
		timers.swap(_timers);
	}
	_wake.notify_all();
	for (auto &thread : threads) {
		thread.join();
	}
	{
		const auto lock = std::scoped_lock(_mutex);
		_stopping = false;
	}
}

void AsyncPool::loop() {
	auto lock = std::unique_lock(_mutex);
	while (!_stopping) {
		const auto now = Clock::now();
		if (!_timers.empty() && _timers.begin()->first <= now) {
			auto work = std::move(_timers.begin()->second);
			_timers.erase(_timers.begin());
			lock.unlock();
			work();
			work = nullptr;
			lock.lock();
		} else if (!_queue.empty()) {
			auto work = std::move(_queue.front());
			_queue.pop_front();
			lock.unlock();
			work();
			work = nullptr;
			lock.lock();
		} else if (!_timers.empty()) {
			const auto until = _timers.begin()->first;
			_wake.wait_until(lock, until);
		} else {
			_wake.wait(lock);
		}
	}
}

} // namespace fgpl
