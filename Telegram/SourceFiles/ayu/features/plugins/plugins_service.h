// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

namespace Main {
class Session;
} // namespace Main

namespace fgpl {
class PluginManager;
} // namespace fgpl

namespace Api {
struct MessageToSend;
} // namespace Api

namespace FreshPlugins {

enum class State {
	Unavailable,
	Disabled,
	NoSandbox,
	NoRuntime,
	NoKey,
	Ready,
};

void Start();
void Stop();

[[nodiscard]] bool SandboxSupported();
[[nodiscard]] bool ApplyOutgoing(Api::MessageToSend &message);
[[nodiscard]] State CurrentState();
[[nodiscard]] rpl::producer<State> StateValue();

[[nodiscard]] rpl::producer<> ListChanges();
void NotifyListChanged();

[[nodiscard]] fgpl::PluginManager *Manager();

[[nodiscard]] QString StorePath();
[[nodiscard]] QString RuntimeRootPath();
[[nodiscard]] QString RuntimePath();
void NotifyRuntimeChanged();
[[nodiscard]] QString AccountId(not_null<Main::Session*> session);

} // namespace FreshPlugins
