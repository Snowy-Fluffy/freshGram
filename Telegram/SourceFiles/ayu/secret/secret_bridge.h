#pragma once

#include "ayu/secret/secret_manager.h"

#include "data/data_types.h"

#include <map>
#include <optional>
#include <set>

class DocumentData;
class History;
class HistoryItem;
class UserData;

namespace Window {
class SessionController;
} // namespace Window

namespace FreshSecret {

class Bridge final {
public:
	Bridge(not_null<Main::Session*> session, not_null<Manager*> manager);
	~Bridge();

	void refreshAll();
	void refreshChat(int chatId);
	void removeAll();
	void removeChat(int chatId);
	void promptRequest(int chatId);
	void openChat(int chatId);
	void promptClosed(int chatId);

	[[nodiscard]] std::optional<std::pair<int, int64_t>> lookup(
		not_null<const HistoryItem*> item) const;
	[[nodiscard]] HistoryItem *item(int chatId, int64_t randomId) const;
	[[nodiscard]] History *history(int chatId) const;
	[[nodiscard]] UserData *realUser(int chatId) const;
	void registerDocument(DocumentId id, int chatId, int64_t randomId);
	void forgetDocuments(int chatId);
	[[nodiscard]] int64_t randomIdOf(
		int chatId,
		FullMsgId id) const;

	[[nodiscard]] std::vector<not_null<HistoryItem*>> itemsDeletedByUser(
		const std::vector<not_null<HistoryItem*>> &items);
	void historyClearedByUser(not_null<History*> history);
	void messagesRead(not_null<History*> history);
	void mediaOpened(not_null<HistoryItem*> item);

private:
	struct Binding {
		int chatId = 0;
		UserData *user = nullptr;
		UserData *real = nullptr;
		History *history = nullptr;
		int revision = -2;
		std::map<int64_t, MsgId> items;
		std::set<int64_t> requested;
		bool typing = false;
		bool asked = false;
		int lastState = -1;
		rpl::lifetime lifetime;
	};

	[[nodiscard]] Binding &ensureBinding(const ChatInfo &info);
	void syncMessages(Binding &binding, const ChatInfo &info);
	[[nodiscard]] HistoryItem *createItem(
		Binding &binding,
		const ChatInfo &info,
		const MessageData &message,
		bool unread);

	const not_null<Main::Session*> _session;
	const not_null<Manager*> _manager;
	std::map<int, Binding> _bindings;
	std::map<MsgId, std::pair<int, int64_t>> _byMsg;
	bool _refreshing = false;
	bool _ready = false;
	rpl::lifetime _lifetime;

};

[[nodiscard]] Bridge &BridgeFor(not_null<Main::Session*> session);
[[nodiscard]] UserData *RealUser(const PeerData *peer);
[[nodiscard]] bool IsSecretDocument(const DocumentData *document);
[[nodiscard]] QString NotificationTitle();
[[nodiscard]] TextWithEntities NotificationText(not_null<HistoryItem*> item);
[[nodiscard]] QByteArray DocumentBytes(const DocumentData *document);
void Start(not_null<Main::Session*> session);
void WatchActiveChat(not_null<Window::SessionController*> controller);

} // namespace FreshSecret
