// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace Main {
class Session;
} // namespace Main

namespace fgpl {
class PluginManager;
struct ManagerSnapshot;
} // namespace fgpl

namespace Api {
struct MessageToSend;
struct SendAction;
} // namespace Api

namespace Ui {
struct PreparedList;
} // namespace Ui

class History;
struct SendingAlbum;
enum class SendMediaType;

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
// Plugins may change the caption and the options of a send of files, or stop it.
// True when they took the send and it goes on later: the list and the album are moved away.
[[nodiscard]] bool ApplyOutgoingFiles(
	Ui::PreparedList &list,
	SendMediaType type,
	std::shared_ptr<SendingAlbum> &album,
	const Api::SendAction &action);
[[nodiscard]] bool HasPendingOutgoing(not_null<History*> history);

// A file of the send that plugins are looking at: bytes if they are in memory, else a path to read.
struct OutgoingFileSource {
	std::string kind;
	std::string name;
	std::string mime;
	QString path;
	QByteArray bytes;
};
// Main thread only: the file number index of the send, while it is at the front of the history queue.
[[nodiscard]] std::optional<OutgoingFileSource> PendingOutgoingFile(
	not_null<History*> history,
	uint64 send,
	size_t index,
	bool thumbnail);
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
