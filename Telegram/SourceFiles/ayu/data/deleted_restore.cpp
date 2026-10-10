#include "ayu/data/deleted_restore.h"

#include "api/api_text_entities.h"
#include "ayu/ayu_settings.h"
#include "ayu/data/known_users.h"
#include "ayu/data/messages_storage.h"
#include "ayu/utils/ayu_mapper.h"
#include "ayu/utils/telegram_helpers.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"

namespace AyuRestore {
namespace {

[[nodiscard]] UserData *unknownSender(Data::Session &owner, ID id) {
	const auto user = owner.user(UserId(uint64(id)));
	if (!user->isLoaded()) {
		user->setName(
			QString("unknown user (id: %1)").arg(id),
			QString(),
			QString(),
			QString());
		user->setFlags(user->flags() | UserDataFlag::Deleted);
		user->setLoadedStatus(PeerData::LoadedStatus::Normal);
	}
	return user;
}

constexpr auto kCreateBatch = 60;
constexpr auto kOutgoingFlag = 0x00000002;

struct DialogsEntry {
	bool loading = false;
	bool loaded = false;
	base::flat_set<ID> ids;
	std::vector<Fn<void(const base::flat_set<ID> &)>> waiting;
};

base::flat_map<ID, DialogsEntry> DialogsWithDeleted;
base::flat_map<ID, base::flat_set<ID>> NotedDialogs;

[[nodiscard]] bool Supported(not_null<PeerData*> peer) {
	return !peer->isMonoforum();
}

void WithDeletedDialogs(
		ID userId,
		Fn<void(const base::flat_set<ID> &)> callback) {
	auto &entry = DialogsWithDeleted[userId];
	if (entry.loaded) {
		callback(entry.ids);
		return;
	}
	AyuMessages::flushPending();
	entry.waiting.push_back(std::move(callback));
	if (entry.loading) {
		return;
	}
	entry.loading = true;
	crl::async([=] {
		auto ids = std::vector<ID>();
		try {
			ids = AyuMessages::loadDeletedDialogIds(userId);
		} catch (...) {
			ids.clear();
		}
		crl::on_main([=, ids = std::move(ids)]() mutable {
			auto &entry = DialogsWithDeleted[userId];
			entry.loaded = true;
			entry.loading = false;
			entry.ids = base::flat_set<ID>(ids.begin(), ids.end());
			for (const auto id : NotedDialogs[userId]) {
				entry.ids.emplace(id);
			}
			auto waiting = std::move(entry.waiting);
			entry.waiting.clear();
			for (const auto &callback : waiting) {
				callback(entry.ids);
			}
		});
	});
}

} // namespace

void withDeletedDialogs(
		ID userId,
		Fn<void(const base::flat_set<ID> &)> callback) {
	WithDeletedDialogs(userId, std::move(callback));
}

void noteDeleted(not_null<History*> history) {
	const auto peer = history->peer;
	if (!Supported(peer)) {
		return;
	}
	const auto userId = AyuMessages::storageUserId(peer);
	const auto dialogId = getDialogIdFromPeer(peer);
	NotedDialogs[userId].emplace(dialogId);
	DialogsWithDeleted[userId].ids.emplace(dialogId);
	history->ayuRestoreMarkStale();
}

State::State(not_null<History*> history)
: _history(history) {
}

void State::markStale() {
	_stale = true;
}

void State::checkLoaded() {
	if (_requested && !_stale) {
		return;
	}
	_requested = true;
	_stale = false;

	const auto peer = _history->peer;
	if (!AyuSettings::getInstance().saveDeletedMessages()
		|| !AyuSettings::getInstance().restoreDeletedInChats()
		|| !Supported(peer)) {
		return;
	}

	const auto userId = AyuMessages::storageUserId(peer);
	const auto dialogId = getDialogIdFromPeer(peer);
	_pending = !_loaded;
	const auto weak = base::make_weak(this);
	WithDeletedDialogs(userId, [=](const base::flat_set<ID> &ids) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		} else if (ids.contains(dialogId)) {
			strong->load(userId, dialogId);
		} else {
			strong->_pending = false;
			strong->_waiting.clear();
		}
	});
}

void State::disable() {
	_disabled = true;
	_requested = true;
	_loaded = false;
	_pending = false;
	_rows.clear();
	_index.clear();
	_waiting.clear();
	_threads.clear();
}

void State::load(ID userId, ID dialogId) {
	const auto limit = AyuSettings::getInstance().deletedRestoreLimit();
	const auto weak = base::make_weak(this);
	AyuMessages::flushPending();
	crl::async([=] {
		auto messages = std::vector<AyuMessageBase>();
		auto extras = std::vector<DeletedExtra>();
		auto markups = std::vector<DeletedMarkup>();
		try {
			extras = AyuMessages::loadDeletedExtras(userId, dialogId);
			markups = AyuMessages::loadDeletedMarkups(userId, dialogId);
			messages = AyuMessages::loadDeletedMessages(
				userId,
				dialogId,
				0,
				0,
				0,
				limit ? limit : std::numeric_limits<int>::max(),
				std::string());
		} catch (...) {
			messages.clear();
		}

		crl::on_main([=, messages = std::move(messages), extras = std::move(extras), markups = std::move(markups)]() mutable {
			const auto strong = weak.get();
			if (!strong || strong->_disabled) {
				return;
			}
			auto known = base::flat_set<std::pair<ID, TimeId>>();
			for (const auto &row : strong->_rows) {
				known.emplace(row.message.messageId, row.message.date);
			}
			strong->_rows.reserve(strong->_rows.size() + messages.size());
			for (auto &message : messages) {
				if (!known.emplace(message.messageId, message.date).second) {
					continue;
				}
				strong->_index.emplace(
					message.messageId,
					int(strong->_rows.size()));
				strong->_rows.push_back({ std::move(message), MsgId() });
				for (const auto &extra : extras) {
					if (extra.messageId == strong->_rows.back().message.messageId) {
						strong->_rows.back().extra = extra;
						break;
					}
				}
				for (const auto &markup : markups) {
					if (markup.messageId == strong->_rows.back().message.messageId) {
						strong->_rows.back().markup = markup.markup;
						break;
					}
				}
			}
			strong->_loaded = true;
			strong->_pending = false;
			strong->resolveWaiting();
			for (const auto rootId : base::take(strong->_threads)) {
				strong->materializeThread(rootId);
			}
			strong->_history->checkLocalMessages();
		});
	});
}

void State::resolveWaiting() {
	auto waiting = std::move(_waiting);
	_waiting.clear();
	auto &owner = _history->owner();
	for (const auto &fullId : waiting) {
		const auto holder = owner.message(fullId);
		const auto reply = holder ? holder->Get<HistoryMessageReply>() : nullptr;
		if (reply
			&& !reply->resolvedMessage
			&& _index.contains(reply->messageId().bare)) {
			holder->updateDependencyItem();
		}
	}
}

void State::restoreThread(MsgId rootId) {
	if (_disabled || !rootId) {
		return;
	}
	checkLoaded();
	if (_loaded) {
		materializeThread(rootId);
	} else if (_pending) {
		_threads.emplace(rootId);
	}
}

HistoryItem *State::itemFor(const AyuMessageBase &message) {
	auto &owner = _history->owner();
	const auto peer = _history->peer;
	if (const auto existing = owner.message(peer, MsgId(message.messageId))) {
		return existing;
	} else if (_disabled) {
		return nullptr;
	}
	auto i = _index.find(message.messageId);
	if (i == _index.end()) {
		i = _index.emplace(
			message.messageId,
			int(_rows.size())).first;
		_rows.push_back({ message, MsgId() });
	}
	auto &row = _rows[i->second];
	if (row.dead) {
		return nullptr;
	} else if (row.localId) {
		return owner.message(peer, row.localId);
	}
	try {
		return create(row);
	} catch (...) {
		row.dead = true;
		return nullptr;
	}
}

void State::materializeThread(MsgId rootId) {
	for (auto &row : _rows) {
		if (row.dead || row.message.replyTopId != rootId.bare) {
			continue;
		}
		try {
			create(row);
		} catch (...) {
			row.dead = true;
		}
	}
}

std::vector<not_null<HistoryItem*>> State::orphans(
		TimeId from,
		TimeId till) const {
	auto result = std::vector<not_null<HistoryItem*>>();
	if (!_loaded || _history->peer->isForum()) {
		return result;
	}
	auto &owner = _history->owner();
	for (const auto &row : _rows) {
		if (row.dead) {
			continue;
		}
		const auto item = owner.message(
			_history->peer,
			MsgId(row.message.messageId));
		if (item
			&& item->isDeleted()
			&& !item->mainView()
			&& item->date() >= from
			&& item->date() < till) {
			result.push_back(item);
		}
	}
	return result;
}

HistoryItem *State::find(MsgId id, not_null<HistoryItem*> holder) {
	if (_disabled || !id) {
		return nullptr;
	} else if (!_loaded) {
		if (_pending) {
			_waiting.emplace(holder->fullId());
		}
		return nullptr;
	}
	const auto i = _index.find(id.bare);
	if (i == _index.end() || _rows[i->second].dead) {
		return nullptr;
	}
	auto &row = _rows[i->second];
	if (row.localId) {
		if (const auto item = _history->owner().message(
				_history->peer,
				row.localId)) {
			return item;
		}
	}
	try {
		return create(row);
	} catch (...) {
		row.dead = true;
		return nullptr;
	}
}

HistoryItem *State::duplicateOf(const Row &row) const {
	const auto real = _history->owner().message(
		_history->peer,
		MsgId(row.message.messageId));
	if (!real
		|| !real->isRegular()
		|| real->isDeleted()
		|| real->date() != row.message.date) {
		return nullptr;
	}
	const auto text = real->emptyText()
		? std::string()
		: real->originalText().text.toStdString();
	if (text == row.message.text
		|| (text.empty()
			&& real->notificationText().text.toStdString() == row.message.text)) {
		return real;
	}
	return nullptr;
}

void State::dropDuplicates() {
	if (!_loaded) {
		return;
	}
	auto &owner = _history->owner();
	const auto peer = _history->peer;
	for (auto &row : _rows) {
		if (row.dead) {
			continue;
		}
		const auto real = duplicateOf(row);
		if (!real) {
			continue;
		}
		if (row.localId) {
			if (const auto local = owner.message(peer, row.localId)) {
				local->destroy();
			}
			row.localId = MsgId();
		}
		row.dead = true;
		AyuMessages::removeDeletedMessage(real);
	}
}

HistoryItem *State::materialize(TimeId from, TimeId till) {
	if (!_loaded || _materializing || _rows.empty()) {
		return nullptr;
	}
	_materializing = true;
	auto last = (HistoryItem*)nullptr;
	auto created = 0;
	auto more = false;
	for (auto &row : _rows) {
		const auto date = row.message.date;
		if (row.dead || date < from || date >= till) {
			continue;
		}
		if (created >= kCreateBatch) {
			more = true;
			break;
		}
		try {
			if (const auto item = create(row)) {
				if (!last
					|| item->date() > last->date()
					|| (item->date() == last->date()
						&& item->id > last->id)) {
					last = item;
				}
				++created;
			}
		} catch (...) {
			row.dead = true;
			LOG(("AyuRestore: failed to restore a saved message"));
		}
	}
	_materializing = false;
	if (more) {
		const auto weak = base::make_weak(this);
		crl::on_main([=] {
			if (const auto strong = weak.get()) {
				strong->_history->checkLocalMessages();
			}
		});
	}
	return last;
}

HistoryItem *State::create(Row &row) {
	const auto peer = _history->peer;
	auto &owner = _history->owner();
	if (row.localId) {
		if (owner.message(peer, row.localId)) {
			return nullptr;
		}
		row.localId = MsgId();
	}
	const auto &message = row.message;
	if (owner.message(peer, MsgId(message.messageId))) {
		return nullptr;
	}

	if (message.documentType == AyuMessages::kServiceDocumentType) {
		const auto service = _history->makeMessage({
			.id = owner.nextLocalMessageId(),
			.flags = MessageFlags(MessageFlag::Local),
			.date = message.date,
		}, PreparedServiceText{
			.text = TextWithEntities{ QString::fromStdString(message.text) },
		});
		if (peer->isUser() || !_history->ayuKept()) {
			service->setDeleted();
			service->ayuSetDeletedAt(message.entityCreateDate);
			service->markDeletedAnimated();
		}
		row.localId = service->id;
		return service;
	}

	PeerData *from = owner.userLoaded(message.fromId);
	if (!from) {
		from = owner.channelLoaded(message.fromId);
	}
	if (!from) {
		from = owner.chatLoaded(message.fromId);
	}
	if (!from && message.fromId && !peer->isUser() && !peer->isBroadcast()) {
		from = AyuUsers::find(&peer->session(), UserId(uint64(message.fromId)));
		if (!from) {
			from = unknownSender(owner, message.fromId);
		}
	}

	auto flags = MessageFlags(MessageFlag::Local);
	auto replyTo = FullReplyTo();
	if (const auto forum = peer->forum()) {
		const auto root = MsgId(message.topicId);
		if (root.bare > Data::ForumTopic::kGeneralId) {
			if (!forum->topicFor(root)) {
				return nullptr;
			}
			flags |= MessageFlag::HasReplyInfo;
			replyTo.messageId = FullMsgId(peer->id, root);
			replyTo.topicRootId = root;
		}
	} else if (message.replyTopId || message.replyMessageId) {
		flags |= MessageFlag::HasReplyInfo;
		replyTo.messageId = FullMsgId(
			peer->id,
			MsgId(message.replyMessageId
				? message.replyMessageId
				: message.replyTopId));
		replyTo.topicRootId = MsgId(message.replyTopId);
	}
	const auto outgoing = (message.flags & kOutgoingFlag) != 0;
	if (outgoing) {
		flags |= MessageFlag::Outgoing;
	}
	if (peer->isChannel() && !peer->isMegagroup()) {
		flags |= MessageFlag::Post;
		from = nullptr;
	} else {
		if (!from && peer->isUser()) {
			from = outgoing ? peer->session().user().get() : peer.get();
		}
		if (from) {
			flags |= MessageFlag::HasFromId;
		}
	}
	if (!message.postAuthor.empty()) {
		flags |= MessageFlag::HasPostAuthor;
	}

	auto text = Ui::Text::WithEntities(QString::fromStdString(message.text));
	text.entities = Api::EntitiesFromMTP(
		&_history->session(),
		AyuMapper::deserializeTextWithEntities(message.textEntities).v);

	const auto markup = AyuMapper::deserializeReplyMarkup(row.markup);
	const auto build = [&](TextWithEntities text) {
		return _history->makeMessage({
			.id = owner.nextLocalMessageId(),
			.flags = flags,
			.from = from ? from->id : PeerId(),
			.replyTo = replyTo,
			.date = message.date,
			.postAuthor = QString::fromStdString(message.postAuthor),
			.markup = markup,
		}, std::move(text), AyuMessages::restoredMedia(message));
	};
	auto item = build(text);
	if (item->isEmpty() && !text.entities.empty()) {
		LOG(("AyuRestore: message %1 is empty, retrying without entities").arg(message.messageId));
		item->destroy();
		text.entities.clear();
		item = build(text);
	}

	if (item->isEmpty()) {
		LOG(("AyuRestore: message %1 stays empty, text %2 bytes").arg(message.messageId).arg(message.text.size()));
		row.dead = true;
		item->destroy();
		return nullptr;
	}
	AyuMessages::restoreSavedMedia(item, message);
	if (row.extra) {
		AyuMessages::restoreExtra(item, *row.extra);
	}
	if (peer->isUser() || !_history->ayuKept()) {
		item->setDeleted();
		item->ayuSetDeletedAt(message.entityCreateDate);
		item->markDeletedAnimated();
	}
	row.localId = item->id;
	if (peer->isForum() && message.topicId) {
		if (const auto topic = item->topic()) {
			if (topic->rootId() == item->topicRootId()) {
				topic->maybeSetLastMessage(item);
			}
		}
	}
	return item;
}

} // namespace AyuRestore
