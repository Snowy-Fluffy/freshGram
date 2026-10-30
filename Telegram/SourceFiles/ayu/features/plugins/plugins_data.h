// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"

#include <QtCore/QString>

#include <string>
#include <vector>

class HistoryItem;
class PeerData;
class UserData;
struct TextWithTags;

namespace Main {
class Session;
} // namespace Main

namespace fgpl {
class DataProvider;
struct TextEntity;
struct IncomingInfo;
} // namespace fgpl

namespace FreshPlugins {

[[nodiscard]] fgpl::DataProvider *DataFor(const std::string &account);
void OpenData();
void CloseData();
void ReleaseData();

[[nodiscard]] bool IsHiddenByPlugin(not_null<const HistoryItem*> item);
void SetHiddenByPlugin(not_null<HistoryItem*> item, bool hidden, bool pending);
void ForgetHiddenByPlugin(FullMsgId id);
void ReleasePendingHidden();

#ifdef FRESHGRAM_PLUGINS
[[nodiscard]] fgpl::IncomingInfo IncomingInfoOf(
	not_null<HistoryItem*> item,
	bool edited);
[[nodiscard]] TextWithEntities DisplayTextOf(
	Main::Session *session,
	const QString &text,
	const std::vector<fgpl::TextEntity> &entities);
[[nodiscard]] std::vector<fgpl::TextEntity> EntitiesOf(const TextWithTags &text);
[[nodiscard]] TextWithTags TextWithEntitiesOf(
	Main::Session *session,
	const QString &text,
	const std::vector<fgpl::TextEntity> &entities);
#endif // FRESHGRAM_PLUGINS

[[nodiscard]] std::string PeerKind(not_null<PeerData*> peer);
[[nodiscard]] std::string MessageMediaKind(not_null<const HistoryItem*> item);
[[nodiscard]] bool ValidScheduleTime(TimeId when);
[[nodiscard]] MsgId ThreadRootOf(
	not_null<PeerData*> peer,
	not_null<HistoryItem*> item);

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
