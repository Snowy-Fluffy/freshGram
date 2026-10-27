// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"

class HistoryItem;
class PeerData;
class UserData;

namespace Main {
class Session;
} // namespace Main

namespace fgpl {
class DataProvider;
} // namespace fgpl

namespace FreshPlugins {

[[nodiscard]] fgpl::DataProvider *DataFor(const std::string &account);
void OpenData();
void CloseData();
void ReleaseData();

[[nodiscard]] std::string PeerKind(not_null<PeerData*> peer);
[[nodiscard]] std::string MessageMediaKind(not_null<const HistoryItem*> item);
[[nodiscard]] bool ValidScheduleTime(TimeId when);

[[nodiscard]] std::string UserStatus(
	not_null<UserData*> user,
	int64_t &lastSeen);

[[nodiscard]] bool WantsEvent(const char *type);
[[nodiscard]] bool WantsEvents();
[[nodiscard]] bool PluginSending();
[[nodiscard]] bool IsPluginMessage(
	not_null<Main::Session*> session,
	FullMsgId id);
void NoteMessageIdChange(
	not_null<Main::Session*> session,
	FullMsgId old,
	MsgId now);
void SendMessageEvent(
	const char *type,
	not_null<const HistoryItem*> item,
	const std::string &account,
	MsgId localId = 0,
	bool byPlugin = false);

} // namespace FreshPlugins
