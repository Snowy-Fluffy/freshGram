// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace fgpl {
class PluginManager;
struct ManagerConfig;
struct ManagerSnapshot;
} // namespace fgpl

namespace FreshPlugins {

class Control final {
public:
	using Work = std::function<void(not_null<fgpl::PluginManager*>)>;

	Control(fgpl::ManagerConfig config, Fn<void()> published);
	~Control();

	bool post(Work work, bool changesState);
	[[nodiscard]] std::shared_ptr<const fgpl::ManagerSnapshot> snapshot() const;

private:
	void loop();
	void publish();

	std::unique_ptr<fgpl::ManagerConfig> _config;
	std::unique_ptr<fgpl::PluginManager> _manager;
	std::atomic<fgpl::PluginManager*> _ready = nullptr;
	const Fn<void()> _published;
	std::mutex _mutex;
	std::deque<std::pair<Work, bool>> _queue;
	bool _stopping = false;
	mutable std::mutex _snapshotMutex;
	std::shared_ptr<const fgpl::ManagerSnapshot> _snapshot;
	std::thread _thread;

};

} // namespace FreshPlugins
