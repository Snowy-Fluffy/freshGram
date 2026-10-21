// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

namespace Main {
class Session;
} // namespace Main

namespace fgpl {
class PluginManager;
} // namespace fgpl

namespace AyuPlugins {

enum class State {
	Unavailable,
	Disabled,
	NoRuntime,
	NoKey,
	Ready,
};

void Start();
void Stop();

[[nodiscard]] State CurrentState();
[[nodiscard]] rpl::producer<State> StateValue();

[[nodiscard]] rpl::producer<> ListChanges();
void NotifyListChanged();

[[nodiscard]] fgpl::PluginManager *Manager();

[[nodiscard]] QString StorePath();
[[nodiscard]] QString RuntimePath();
[[nodiscard]] QString AccountId(not_null<Main::Session*> session);

} // namespace AyuPlugins
