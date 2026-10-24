#include "ayu/secret/secret_peer.h"
#include "ayu/data/kept_dialogs.h"

#include "ayu/ayu_settings.h"
#include "ayu/data/ayu_database.h"
#include "ayu/data/messages_storage.h"
#include "ayu/utils/telegram_helpers.h"
#include "base/flat_map.h"
#include "core/application.h"
#include "window/notifications_manager.h"
#include "base/flat_set.h"
#include "base/unixtime.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_folder.h"
#include "dialogs/dialogs_key.h"
#include "data/data_forum_topic.h"
#include "data/data_forum.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "logs.h"
#include "main/main_session.h"

#include <map>
#include <set>
#include <tuple>

namespace FreshKept {
namespace {

enum class Kind : int {
	User = 0,
	Chat = 1,
	Broadcast = 2,
	Megagroup = 3,
	Forum = 4,
};

struct SessionState {
	bool requested = false;
	bool loaded = false;
	std::vector<KeptDialog> rows;
	std::map<ID, std::vector<KeptTopic>> topics;
	base::flat_set<ID> done;
};

std::set<std::pair<ID, ID>> Noted;
std::set<PeerData*> Leaving;

[[nodiscard]] bool Supported(not_null<PeerData*> peer) {
	if (FreshSecret::IsSecretPeer(peer)) {
		return false;
	}
	if (peer->isMonoforum() || peer->migrateTo()) {
		return false;
	}
	if (const auto channel = peer->asChannel()) {
		return !channel->isCommunity();
	}
	return peer->isUser() || peer->isChat();
}

[[nodiscard]] bool KindEnabled(not_null<PeerData*> peer) {
	const auto &settings = AyuSettings::getInstance();
	if (peer->isUser()) {
		return settings.keepRemovedUserChats();
	} else if (const auto channel = peer->asChannel()) {
		return channel->isForum()
			? settings.keepRemovedForums()
			: channel->isMegagroup()
			? settings.keepRemovedGroups()
			: settings.keepRemovedChannels();
	}
	return settings.keepRemovedGroups();
}

[[nodiscard]] bool Allowed(not_null<PeerData*> peer) {
	return Supported(peer) && KindEnabled(peer);
}

[[nodiscard]] int KindOf(not_null<PeerData*> peer) {
	if (peer->isUser()) {
		return int(Kind::User);
	} else if (peer->isChat()) {
		return int(Kind::Chat);
	} else if (const auto channel = peer->asChannel()) {
		return int(channel->isForum()
			? Kind::Forum
			: channel->isMegagroup()
			? Kind::Megagroup
			: Kind::Broadcast);
	}
	return -1;
}

[[nodiscard]] KeptDialog MakeRow(not_null<History*> history, bool lost) {
	const auto peer = history->peer;
	auto row = KeptDialog();
	row.fakeId = 0;
	row.userId = AyuMessages::storageUserId(peer);
	row.dialogId = getDialogIdFromPeer(peer);
	row.kind = KindOf(peer);
	row.title = peer->name().toStdString();
	row.username = peer->username().toStdString();
	row.accessHash = 0;
	if (const auto user = peer->asUser()) {
		row.accessHash = ID(user->accessHash());
	} else if (const auto channel = peer->asChannel()) {
		row.accessHash = ID(channel->accessHash());
	}
	row.folderId = history->folder() ? history->folder()->id() : 0;
	row.lastMessageDate = history->freshKeptTime()
		? history->freshKeptTime()
		: history->chatListTimeId();
	row.lost = lost ? 1 : 0;
	return row;
}

using TopicKey = std::tuple<ID, PeerId, MsgId>;

[[nodiscard]] TopicKey KeyFor(not_null<PeerData*> peer, MsgId rootId) {
	return { AyuMessages::storageUserId(peer), peer->id, rootId };
}

std::map<TopicKey, QString> KeptDeletedTopics;
std::map<TopicKey, base::weak_ptr<Data::ForumTopic>> KeptTopicObjects;
std::set<TopicKey> PurgeOnce;

[[nodiscard]] QString MarkedTitle(const QString &title) {
	const auto &mark = AyuSettings::getInstance().deletedMark();
	return mark.isEmpty() ? title : (mark + ' ' + title);
}

[[nodiscard]] KeptTopic MakeTopicRow(
		not_null<PeerData*> peer,
		not_null<Data::ForumTopic*> topic) {
	auto row = KeptTopic();
	row.fakeId = 0;
	row.userId = AyuMessages::storageUserId(peer);
	row.dialogId = getDialogIdFromPeer(peer);
	row.rootId = ID(topic->rootId().bare);
	row.title = topic->title().toStdString();
	row.colorId = topic->colorId();
	row.iconId = ID(topic->iconId());
	row.creatorId = ID(topic->creatorId().value);
	row.date = topic->creationDate();
	row.flags = (topic->closed() ? 1 : 0)
		| (topic->isPinnedDialog(FilterId()) ? 2 : 0);
	const auto i = KeptDeletedTopics.find(KeyFor(peer, topic->rootId()));
	if (i != KeptDeletedTopics.end()) {
		row.title = i->second.toStdString();
		row.flags |= 4;
	}
	return row;
}

void SaveTopics(not_null<History*> history) {
	const auto forum = history->peer->forum();
	if (!forum) {
		return;
	}
	auto rows = std::vector<KeptTopic>();
	forum->enumerateTopics([&](not_null<Data::ForumTopic*> topic) {
		if (topic->creating() || !topic->rootId()) {
			return;
		}
		rows.push_back(MakeTopicRow(history->peer, topic));
	});
	AyuDatabase::saveKeptTopics(rows);
}

void RestoreTopics(
		not_null<PeerData*> peer,
		const std::vector<KeptTopic> &topics) {
	const auto forum = peer->forum();
	if (!forum) {
		return;
	}
	for (const auto &row : topics) {
		const auto rootId = MsgId(row.rootId);
		if (!rootId || forum->topicFor(rootId)) {
			continue;
		}
		auto title = QString::fromStdString(row.title);
		if (row.flags & 4) {
			KeptDeletedTopics[KeyFor(peer, rootId)] = title;
			title = MarkedTitle(title);
		}
		const auto topic = forum->applyTopicAdded(
			rootId,
			title,
			row.colorId,
			DocumentId(row.iconId),
			PeerId(uint64(row.creatorId)),
			row.date,
			false);
		topic->setClosed(row.flags & 1);
		if (row.flags & 4) {
			KeptTopicObjects[KeyFor(peer, rootId)]
				= base::make_weak(topic);
		}
		if ((row.flags & 2) && topic->folderKnown()) {
			peer->owner().setChatPinned(topic, FilterId(), true);
		}
	}
	if (const auto channel = peer->asChannel(); channel && channel->isForbidden()) {
		forum->topicsList()->setLoaded();
	}
}

std::set<PeerData*> TopicsWatched;

void WatchTopics(not_null<History*> history) {
	const auto peer = history->peer;
	const auto forum = peer->forum();
	if (!forum || !TopicsWatched.emplace(peer.get()).second) {
		return;
	}
	forum->lifetime().add([=] {
		TopicsWatched.erase(peer.get());
	});
	const auto weak = base::make_weak(history.get());
	forum->chatsListLoadedEvents(
	) | rpl::on_next([=] {
		if (const auto strong = weak.get()) {
			SaveTopics(strong);
		}
	}, forum->lifetime());
}

std::set<PeerData*> DeletedTopicsRestored;

void RestoreDeletedTopics(not_null<History*> history) {
	const auto peer = history->peer;
	if (!peer->forum()
		|| !AyuSettings::getInstance().keepDeletedTopics()
		|| !DeletedTopicsRestored.emplace(peer.get()).second) {
		return;
	}
	peer->forum()->lifetime().add([=] {
		DeletedTopicsRestored.erase(peer.get());
	});
	const auto userId = AyuMessages::storageUserId(peer);
	const auto dialogId = getDialogIdFromPeer(peer);
	const auto weak = base::make_weak(history.get());
	const auto session = &history->session();
	crl::async([=] {
		auto rows = AyuDatabase::getKeptTopics(userId, dialogId);
		crl::on_main(session, [=, rows = std::move(rows)] {
			const auto strong = weak.get();
			const auto forum = strong ? strong->peer->forum() : nullptr;
			if (!forum) {
				return;
			}
			auto restored = false;
			for (const auto &row : rows) {
				const auto rootId = MsgId(row.rootId);
				if (!(row.flags & 4) || !rootId || forum->topicFor(rootId)) {
					continue;
				}
				auto title = QString::fromStdString(row.title);
				KeptDeletedTopics[KeyFor(strong->peer, rootId)] = title;
				const auto topic = forum->applyTopicAdded(
					rootId,
					MarkedTitle(title),
					row.colorId,
					DocumentId(row.iconId),
					PeerId(uint64(row.creatorId)),
					row.date,
					false);
				topic->setClosed(row.flags & 1);
				KeptTopicObjects[KeyFor(strong->peer, rootId)]
					= base::make_weak(topic);
				restored = true;
			}
			if (restored) {
				strong->checkLocalMessages();
			}
		});
	});
}

void SaveSnapshot(not_null<History*> history) {
	if (history->peer->isForum()) {
		SaveTopics(history);
	}
	const auto user = history->peer->isUser();
	const auto mark = user
		&& AyuSettings::getInstance().markOldMessagesDeleted();
	const auto limit = AyuSettings::getInstance().keptSnapshotLimit();
	const auto now = base::unixtime::now();
	auto saved = 0;
	if (history->peer->isForum()) {
		auto items = history->owner().messagesOf(history->peer->id);
		ranges::sort(items, std::greater<>(), [](not_null<HistoryItem*> item) {
			return item->id.bare;
		});
		auto perTopic = base::flat_map<MsgId, int>();
		for (const auto &item : items) {
			if (item->isDeleted() || item->isLocal()) {
				continue;
			}
			auto &count = perTopic[item->topicRootId()];
			if (limit && count >= limit) {
				continue;
			}
			AyuMessages::addDeletedMessage(item);
			++count;
		}
		return;
	}
	for (const auto &block : ranges::views::reverse(history->blocks)) {
		for (const auto &view : ranges::views::reverse(block->messages)) {
			if (!user && limit && saved >= limit) {
				return;
			}
			const auto item = view->data();
			if (item->isDeleted() || item->isLocal()) {
				continue;
			}
			if (mark) {
				item->setDeleted();
				item->freshSetDeletedAt(now);
			}
			AyuMessages::addDeletedMessage(item);
			++saved;
		}
	}
}

[[nodiscard]] PeerId PeerIdOf(const KeptDialog &row) {
	const auto bare = uint64(row.dialogId < 0 ? -row.dialogId : row.dialogId);
	if (!bare) {
		return PeerId();
	}
	switch (Kind(row.kind)) {
	case Kind::User: return peerFromUser(UserId(bare));
	case Kind::Chat: return peerFromChat(ChatId(bare));
	case Kind::Broadcast:
	case Kind::Forum:
	case Kind::Megagroup: return peerFromChannel(ChannelId(bare));
	}
	return PeerId();
}

PeerData *EnsurePeer(not_null<Data::Session*> owner, const KeptDialog &row) {
	const auto peerId = PeerIdOf(row);
	if (!peerId) {
		return nullptr;
	}
	const auto title = QString::fromStdString(row.title);
	switch (Kind(row.kind)) {
	case Kind::User: {
		const auto user = owner->peer(peerId)->asUser();
		if (user && !user->isLoaded()) {
			user->setAccessHash(uint64(row.accessHash));
			user->setName(
				title,
				QString(),
				QString(),
				QString::fromStdString(row.username));
			user->setLoadedStatus(PeerData::LoadedStatus::Normal);
		}
		return user;
	}
	case Kind::Chat: {
		if (!owner->chatLoaded(peerToChat(peerId))) {
			owner->processChat(MTP_chatForbidden(
				MTP_long(peerToChat(peerId).bare),
				MTP_string(title)));
		}
		return owner->chatLoaded(peerToChat(peerId));
	}
	case Kind::Broadcast:
	case Kind::Forum:
	case Kind::Megagroup: {
		if (!owner->channelLoaded(peerToChannel(peerId))) {
			const auto megagroup = (Kind(row.kind) == Kind::Megagroup)
				|| (Kind(row.kind) == Kind::Forum);
			owner->processChat(MTP_channelForbidden(
				MTP_flags(megagroup
					? MTPDchannelForbidden::Flag::f_megagroup
					: MTPDchannelForbidden::Flag::f_broadcast),
				MTP_long(peerToChannel(peerId).bare),
				MTP_long(row.accessHash),
				MTP_string(title),
				MTPint()));
		}
		const auto channel = owner->channelLoaded(peerToChannel(peerId));
		if (channel
			&& Kind(row.kind) == Kind::Forum
			&& !channel->isForum()) {
			channel->addFlags(ChannelDataFlag::Forum);
		}
		return channel;
	}
	}
	return nullptr;
}

void Restore(
		not_null<Main::Session*> session,
		const std::shared_ptr<SessionState> &state,
		const KeptDialog &row) {
	auto &owner = session->data();
	const auto peerId = PeerIdOf(row);
	if (!peerId) {
		return;
	}
	if (const auto existing = owner.historyLoaded(peerId)) {
		if (existing->freshKept()) {
			return;
		} else if (existing->inChatList()) {
			if (row.lost) {
				AyuDatabase::removeKeptDialog(row.userId, row.dialogId);
			}
			return;
		}
	}
	const auto peer = EnsurePeer(&owner, row);
	if (!peer || !Allowed(peer)) {
		return;
	}
	if (const auto channel = peer->asChannel(); channel && channel->amIn()) {
		return;
	} else if (const auto chat = peer->asChat(); chat && chat->amIn()) {
		return;
	}
	LOG(("FreshKept: restoring dialog %1 (lost %2)").arg(row.dialogId).arg(row.lost));
	const auto history = owner.history(peer);
	if (peer->isForum()) {
		const auto i = state->topics.find(row.dialogId);
		if (i != state->topics.end()) {
			RestoreTopics(peer, i->second);
		}
	}
	history->setFreshKeptTime(row.lastMessageDate);
	history->setFreshKept(true);
	if (!history->folderKnown()) {
		history->clearFolder();
	}
	history->restoreFreshKept();
}

[[nodiscard]] bool Ready(
		not_null<Data::Session*> owner,
		const KeptDialog &row) {
	if (row.lost) {
		return true;
	}
	if (row.folderId == Data::Folder::kId) {
		const auto folder = owner->folderLoaded(Data::Folder::kId);
		return folder && owner->chatsListLoaded(folder);
	}
	return true;
}

void Process(
		not_null<Main::Session*> session,
		const std::shared_ptr<SessionState> &state) {
	auto &owner = session->data();
	if (!state->loaded || !owner.chatsListLoaded()) {
		return;
	}
	for (const auto &row : state->rows) {
		if (state->done.contains(row.dialogId) || !Ready(&owner, row)) {
			continue;
		}
		state->done.emplace(row.dialogId);
		Restore(session, state, row);
	}
}

void Check(
		not_null<Main::Session*> session,
		const std::shared_ptr<SessionState> &state) {
	if (!session->data().chatsListLoaded()) {
		return;
	}
	if (state->loaded) {
		Process(session, state);
		return;
	}
	if (state->requested) {
		return;
	}
	state->requested = true;
	const auto userId = ID(session->userId().bare & PeerId::kChatTypeMask);
	crl::async([=] {
		auto rows = AyuDatabase::getKeptDialogs(userId);
		auto topics = AyuDatabase::getKeptTopicsFor(userId);
		crl::on_main(session, [=, rows = std::move(rows), topics = std::move(topics)]() mutable {
			state->rows = std::move(rows);
			for (auto &topic : topics) {
				state->topics[topic.dialogId].push_back(std::move(topic));
			}
			state->loaded = true;
			Process(session, state);
		});
	});
}

void RecordList(
		not_null<Main::Session*> session,
		Data::Folder *folder) {
	auto &owner = session->data();
	if (!AyuSettings::getInstance().saveDeletedMessages()
		|| !owner.chatsListLoaded(folder)) {
		return;
	}
	auto rows = std::vector<KeptDialog>();
	for (const auto &row : owner.chatsList(folder)->indexed()->all()) {
		const auto history = row->history();
		if (history
			&& history->folderKnown()
			&& !history->freshKept()
			&& Allowed(history->peer)) {
			rows.push_back(MakeRow(history, false));
			WatchTopics(history);
		}
		if (history && history->peer->isForum()) {
			RestoreDeletedTopics(history);
		}
	}
	if (rows.empty()) {
		return;
	}
	crl::async([rows = std::move(rows)] {
		AyuDatabase::syncKeptDialogs(rows);
	});
}

} // namespace

void setup(not_null<Data::Session*> owner, rpl::lifetime &lifetime) {
	const auto state = std::make_shared<SessionState>();
	const auto session = &owner->session();
	owner->chatsListLoadedEvents(
	) | rpl::on_next([=](Data::Folder *folder) {
		Check(session, state);
		RecordList(session, folder);
	}, lifetime);
}

void markLost(not_null<History*> history) {
	const auto peer = history->peer;
	if (!AyuSettings::getInstance().saveDeletedMessages()
		|| !Allowed(peer)
		|| history->freshKept()
		|| !history->inChatList()
		|| (!history->lastMessage() && !hasKeptTopics(history))) {
		return;
	}
	// The chat stays in the list where it was when it was lost.
	history->setFreshKeptTime(base::unixtime::now());
	history->setFreshKept(true);
	SaveSnapshot(history);
	AyuDatabase::saveKeptDialog(MakeRow(history, true));
}

bool keepOnDelete(not_null<History*> history) {
	if (history->freshKept() || !history->inChatList()) {
		return false;
	}
	markLost(history);
	return history->freshKept();
}

void note(not_null<History*> history) {
	const auto peer = history->peer;
	if (!Allowed(peer)
		|| history->freshKept()
		|| !history->inChatList()
		|| !history->folderKnown()) {
		return;
	}
	const auto key = std::make_pair(
		AyuMessages::storageUserId(peer),
		getDialogIdFromPeer(peer));
	if (!Noted.emplace(key).second) {
		return;
	}
	AyuDatabase::saveKeptDialog(MakeRow(history, false));
}

bool keepDeletedTopic(not_null<Data::Forum*> forum, MsgId rootId) {
	const auto &settings = AyuSettings::getInstance();
	const auto channel = forum->channel();
	if (PurgeOnce.erase(KeyFor(forum->peer(), rootId))) {
		return false;
	}
	if (!settings.keepDeletedTopics()
		|| !settings.saveDeletedMessages()
		|| !channel
		|| !channel->amIn()
		|| rootId.bare == Data::ForumTopic::kGeneralId) {
		return false;
	}
	const auto key = KeyFor(channel, rootId);
	if (KeptDeletedTopics.contains(key)) {
		return true;
	}
	const auto topic = forum->topicFor(rootId);
	if (!topic || topic->creating()) {
		return false;
	}
	const auto now = base::unixtime::now();
	for (const auto &item : forum->owner().messagesOf(channel->id)) {
		if (item->topicRootId() != rootId
			|| item->isLocal()
			|| item->isDeleted()) {
			continue;
		}
		item->setDeleted();
		item->freshSetDeletedAt(now);
		AyuMessages::addDeletedMessage(item);
	}
	KeptDeletedTopics.emplace(key, topic->title());
	KeptTopicObjects[key] = base::make_weak(topic);
	AyuDatabase::saveKeptTopics({ MakeTopicRow(channel, topic) });
	topic->applyTitle(MarkedTitle(topic->title()));
	Core::App().notifications().clearFromTopic(topic);
	topic->readTillEnd();
	return true;
}

bool isKeptDeletedTopic(not_null<Data::ForumTopic*> topic) {
	return KeptDeletedTopics.contains(KeyFor(topic->peer(), topic->rootId()));
}

bool purgeDeletedTopic(not_null<Data::Forum*> forum, MsgId rootId) {
	const auto peer = forum->peer();
	const auto key = KeyFor(peer, rootId);
	if (!KeptDeletedTopics.contains(key)) {
		return false;
	}
	KeptDeletedTopics.erase(key);
	KeptTopicObjects.erase(key);
	AyuDatabase::removeKeptTopic(
		AyuMessages::storageUserId(peer),
		getDialogIdFromPeer(peer),
		ID(rootId.bare));
	AyuMessages::clearDeletedMessages(peer, ID(rootId.bare));
	PurgeOnce.emplace(key);
	forum->applyTopicDeleted(rootId);
	forum->history()->freshRestoreMarkStale();
	return true;
}

void releaseDeletedTopics() {
	auto topics = std::move(KeptTopicObjects);
	KeptTopicObjects.clear();
	KeptDeletedTopics.clear();
	for (const auto &[key, weak] : topics) {
		if (const auto topic = weak.get()) {
			PurgeOnce.emplace(key);
			topic->forum()->applyTopicDeleted(std::get<2>(key));
		}
	}
}

void forget(not_null<PeerData*> peer) {
	if (const auto history = peer->owner().historyLoaded(peer)) {
		history->setFreshKept(false);
	}
	const auto userId = AyuMessages::storageUserId(peer);
	const auto dialogId = getDialogIdFromPeer(peer);
	Noted.erase(std::make_pair(userId, dialogId));
	AyuDatabase::removeKeptDialog(userId, dialogId);
	AyuDatabase::removeKeptTopics(userId, dialogId);
}

bool showsDeleted(not_null<PeerData*> peer) {
	if (!AyuSettings::getInstance().showDeletedChatIcon()) {
		return false;
	}
	const auto history = peer->owner().historyLoaded(peer);
	return history && history->freshKept();
}

bool isKept(not_null<PeerData*> peer) {
	const auto history = peer->owner().historyLoaded(peer);
	return history && history->freshKept();
}

bool serverUnavailable(not_null<const History*> history) {
	if (!history->freshKept()) {
		return false;
	}
	const auto channel = history->peer->asChannel();
	return channel && channel->isForbidden();
}

bool hasKeptTopics(not_null<const History*> history) {
	const auto forum = history->peer->forum();
	return forum && !forum->topicsList()->indexed()->empty();
}

bool hasListableLastMessage(not_null<const History*> history) {
	const auto last = history->lastMessage();
	if (!last) {
		return false;
	}
	return history->freshKept()
		|| history->inChatList()
		|| !last->isLocal()
		|| !last->isDeleted();
}

void revive(not_null<History*> history, not_null<HistoryItem*> item) {
	if (!history->freshKept()
		|| !history->peer->isUser()
		|| item->isLocal()
		|| item->isDeleted()) {
		return;
	}
	forget(history->peer);
}

void checkWiped(not_null<History*> history) {
	if (!history->peer->isUser()
		|| history->freshKept()
		|| !Allowed(history->peer)
		|| !history->inChatList()
		|| !history->loadedAtTop()
		|| history->isEmpty()) {
		return;
	}
	for (const auto &block : history->blocks) {
		for (const auto &view : block->messages) {
			const auto item = view->data();
			if (!item->isLocal() && !item->isDeleted()) {
				return;
			}
		}
	}
	markLost(history);
}

void userLeaving(not_null<PeerData*> peer) {
	Leaving.emplace(peer.get());
}

bool takeUserLeaving(not_null<PeerData*> peer) {
	return Leaving.erase(peer.get()) > 0;
}

void chatAmInChanged(not_null<ChatData*> chat) {
	const auto history = chat->owner().historyLoaded(chat);
	if (chat->amIn()) {
		takeUserLeaving(chat);
		if (history && history->freshKept()) {
			forget(chat);
		}
		return;
	}
	const auto leaving = takeUserLeaving(chat);
	if (history && history->inChatList() && !leaving) {
		markLost(history);
	}
}

} // namespace FreshKept
