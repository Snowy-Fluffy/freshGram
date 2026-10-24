// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

namespace Main {
class Session;
} // namespace Main

namespace fgpl {
class PluginManager;
struct ManagerSnapshot;
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
[[nodiscard]] bool ApplyOutgoing(
	Api::MessageToSend &message,
	std::optional<MsgId> localMessageId);
[[nodiscard]] State CurrentState();
[[nodiscard]] rpl::producer<State> StateValue();

[[nodiscard]] rpl::producer<> ListChanges();
void NotifyListChanged();

[[nodiscard]] bool Running();
[[nodiscard]] std::shared_ptr<const fgpl::ManagerSnapshot> Snapshot();
void Post(
	Fn<void(not_null<fgpl::PluginManager*>)> work,
	bool changesState = true);

[[nodiscard]] QString StorePath();
[[nodiscard]] QString RuntimeRootPath();
[[nodiscard]] QString RuntimePath();
void NotifyRuntimeChanged();
[[nodiscard]] QString AccountId(not_null<Main::Session*> session);

} // namespace FreshPlugins
