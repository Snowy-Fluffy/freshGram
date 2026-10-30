// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

#include <filesystem>

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

class History;

namespace Window {
class SessionController;
} // namespace Window

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
[[nodiscard]] bool HasPendingOutgoing(not_null<History*> history);
[[nodiscard]] State CurrentState();
[[nodiscard]] rpl::producer<State> StateValue();

[[nodiscard]] rpl::producer<> ListChanges();
void NotifyListChanged();

[[nodiscard]] bool Running();
[[nodiscard]] std::shared_ptr<const fgpl::ManagerSnapshot> Snapshot();
bool Post(
	Fn<void(not_null<fgpl::PluginManager*>)> work,
	bool changesState = true);

[[nodiscard]] QString StorePath();
[[nodiscard]] QString RuntimeRootPath();
[[nodiscard]] QString RuntimePath();
// A file name as the standard library reads it, wide on Windows.
[[nodiscard]] std::filesystem::path PathOf(const QString &text);
void NotifyRuntimeChanged();
[[nodiscard]] QString AccountId(not_null<Main::Session*> session);
void WatchActiveChat(not_null<Window::SessionController*> controller);

} // namespace FreshPlugins
