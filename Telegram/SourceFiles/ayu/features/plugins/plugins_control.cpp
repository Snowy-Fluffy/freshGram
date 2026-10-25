// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_control.h"

#include "crl/crl_on_main.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#endif

#include <chrono>

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

constexpr auto kIdleWaitMs = 50;
constexpr auto kStatsEach = std::chrono::milliseconds(500);

} // namespace

Control::Control(fgpl::ManagerConfig config, Fn<void()> published)
: _config(std::make_unique<fgpl::ManagerConfig>(std::move(config)))
, _published(std::move(published))
, _snapshot(std::make_shared<const fgpl::ManagerSnapshot>())
, _thread([this] { loop(); }) {
}

Control::~Control() {
	{
		const auto lock = std::scoped_lock(_mutex);
		_stopping = true;
	}
	if (const auto manager = _ready.load()) {
		manager->wake();
	}
	_thread.join();
	_manager = nullptr;
}

bool Control::post(Work work, bool changesState) {
	{
		const auto lock = std::scoped_lock(_mutex);
		if (_stopping) {
			return false;
		}
		_queue.emplace_back(std::move(work), changesState);
	}
	if (const auto manager = _ready.load()) {
		manager->wake();
	}
	return true;
}

std::shared_ptr<const fgpl::ManagerSnapshot> Control::snapshot() const {
	const auto lock = std::scoped_lock(_snapshotMutex);
	return _snapshot;
}

void Control::publish() {
	auto fresh = std::make_shared<const fgpl::ManagerSnapshot>(_manager->snapshot());
	{
		const auto lock = std::scoped_lock(_snapshotMutex);
		_snapshot = std::move(fresh);
	}
	crl::on_main([callback = _published] {
		callback();
	});
}

void Control::loop() {
	_manager = std::make_unique<fgpl::PluginManager>(std::move(*_config));
	_config = nullptr;
	const auto manager = not_null(_manager.get());
	_ready = manager.get();
	publish();
	auto published = std::chrono::steady_clock::now();
	auto statsChanged = false;
	while (true) {
		auto batch = std::deque<std::pair<Work, bool>>();
		{
			const auto lock = std::scoped_lock(_mutex);
			if (_stopping) {
				break;
			}
			batch.swap(_queue);
		}
		auto changed = false;
		for (auto &[work, changesState] : batch) {
			work(manager);
			changed = changed || changesState;
			statsChanged = statsChanged || !changesState;
		}
		statsChanged = (manager->pump() > 0) || statsChanged;
		const auto now = std::chrono::steady_clock::now();
		if (changed || (statsChanged && now - published >= kStatsEach)) {
			publish();
			published = now;
			statsChanged = false;
		}
		manager->waitForWork(kIdleWaitMs, [&] {
			const auto lock = std::scoped_lock(_mutex);
			return _stopping || !_queue.empty();
		});
	}
	manager->shutdown();
}

#endif

} // namespace FreshPlugins
