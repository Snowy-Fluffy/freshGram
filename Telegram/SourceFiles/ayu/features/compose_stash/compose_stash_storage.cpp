/*
This file is part of freshGram. The compose stash is ported from Forkgram
(https://github.com/forkgram/tdesktop), based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/features/compose_stash/compose_stash_storage.h"

#include "ayu/data/ayu_database.h"
#include "ayu/data/messages_storage.h"
#include "ayu/features/compose_stash/compose_stash.h"
#include "ayu/utils/telegram_helpers.h"
#include "base/unixtime.h"
#include "data/data_peer.h"
#include "history/history.h"

#include <QtCore/QBuffer>
#include <QtCore/QDataStream>

namespace FreshComposeStash {
namespace {

constexpr auto kVersion = qint32(1);

struct Ids {
	ID userId = 0;
	ID dialogId = 0;
	ID topicRootId = 0;
	ID monoforumPeerId = 0;
};

[[nodiscard]] Ids IdsFor(not_null<History*> history, Data::DraftKey key) {
	return {
		.userId = AyuMessages::storageUserId(history->peer),
		.dialogId = getDialogIdFromPeer(history->peer),
		.topicRootId = ID(key.topicRootId().bare),
		.monoforumPeerId = ID(key.monoforumPeerId().value),
	};
}

[[nodiscard]] bool Storable(const Data::ComposeStash &stash) {
	const auto &draft = stash.draft;
	return !draft.hasRichMessage()
		&& (!draft.textWithTags.text.isEmpty()
			|| draft.reply.replying()
			|| draft.suggest.exists
			|| !stash.forward.ids.empty());
}

[[nodiscard]] std::vector<char> Pack(const Data::ComposeStash &stash) {
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	auto stream = QDataStream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);

	const auto &draft = stash.draft;
	stream << kVersion << draft.textWithTags.text;
	stream << qint32(draft.textWithTags.tags.size());
	for (const auto &tag : draft.textWithTags.tags) {
		stream << qint32(tag.offset) << qint32(tag.length) << tag.id;
	}
	const auto &reply = draft.reply;
	stream << quint64(reply.messageId.peer.value)
		<< qint64(reply.messageId.msg.bare)
		<< reply.quote.text
		<< qint32(reply.quoteOffset)
		<< qint64(reply.topicRootId.bare)
		<< quint64(reply.monoforumPeerId.value);
	const auto &suggest = draft.suggest;
	stream << quint32(suggest.exists)
		<< quint32(suggest.priceWhole)
		<< quint32(suggest.priceNano)
		<< quint32(suggest.ton)
		<< qint32(suggest.date)
		<< qint32(suggest.offerDuration);
	stream << qint32(draft.cursor.position)
		<< qint32(draft.cursor.anchor)
		<< qint32(draft.cursor.scroll);
	stream << qint32(stash.forward.ids.size());
	for (const auto &id : stash.forward.ids) {
		stream << quint64(id.peer.value) << qint64(id.msg.bare);
	}
	stream << qint32(stash.forward.options);
	return std::vector<char>(bytes.constData(), bytes.constData() + bytes.size());
}

[[nodiscard]] std::unique_ptr<Data::ComposeStash> Unpack(
		const std::vector<char> &payload) {
	auto bytes = QByteArray(payload.data(), qsizetype(payload.size()));
	auto stream = QDataStream(&bytes, QIODevice::ReadOnly);
	stream.setVersion(QDataStream::Qt_5_1);

	auto version = qint32();
	auto text = QString();
	auto count = qint32();
	stream >> version >> text >> count;
	if (stream.status() != QDataStream::Ok
		|| version != kVersion
		|| count < 0
		|| count > text.size()) {
		return nullptr;
	}
	auto with = TextWithTags();
	with.text = text;
	for (auto i = 0; i != count; ++i) {
		auto offset = qint32();
		auto length = qint32();
		auto id = QString();
		stream >> offset >> length >> id;
		with.tags.push_back({ offset, length, id });
	}
	auto reply = FullReplyTo();
	auto peer = quint64();
	auto msg = qint64();
	auto quote = QString();
	auto quoteOffset = qint32();
	auto topicRootId = qint64();
	auto monoforum = quint64();
	stream >> peer >> msg >> quote >> quoteOffset >> topicRootId >> monoforum;
	reply.messageId = FullMsgId(PeerId(peer), MsgId(msg));
	reply.quote.text = quote;
	reply.quoteOffset = quoteOffset;
	reply.topicRootId = MsgId(topicRootId);
	reply.monoforumPeerId = PeerId(monoforum);

	auto suggest = SuggestOptions();
	auto exists = quint32();
	auto whole = quint32();
	auto nano = quint32();
	auto ton = quint32();
	auto date = qint32();
	auto duration = qint32();
	stream >> exists >> whole >> nano >> ton >> date >> duration;
	suggest.exists = exists;
	suggest.priceWhole = whole;
	suggest.priceNano = nano;
	suggest.ton = ton;
	suggest.date = date;
	suggest.offerDuration = duration;

	auto position = qint32();
	auto anchor = qint32();
	auto scroll = qint32();
	stream >> position >> anchor >> scroll;

	auto forward = Data::ForwardDraft();
	auto ids = qint32();
	stream >> ids;
	if (stream.status() != QDataStream::Ok || ids < 0 || ids > 100000) {
		return nullptr;
	}
	for (auto i = 0; i != ids; ++i) {
		auto idPeer = quint64();
		auto idMsg = qint64();
		stream >> idPeer >> idMsg;
		forward.ids.push_back(FullMsgId(PeerId(idPeer), MsgId(idMsg)));
	}
	auto options = qint32();
	stream >> options;
	if (stream.status() != QDataStream::Ok) {
		return nullptr;
	}
	forward.options = Data::ForwardOptions(options);

	auto result = std::make_unique<Data::ComposeStash>();
	result->draft = Data::Draft(
		with,
		reply,
		suggest,
		MessageCursor(position, anchor, scroll),
		Data::WebPageDraft());
	result->forward = std::move(forward);
	return result;
}

} // namespace

void Persist(
		not_null<History*> history,
		Data::DraftKey key,
		const Data::ComposeStash *stash) {
	const auto ids = IdsFor(history, key);
	if (!stash || !Storable(*stash)) {
		AyuDatabase::removeComposeStash(
			ids.userId,
			ids.dialogId,
			ids.topicRootId,
			ids.monoforumPeerId);
		return;
	}
	AyuDatabase::saveComposeStash({
		0,
		ids.userId,
		ids.dialogId,
		ids.topicRootId,
		ids.monoforumPeerId,
		Pack(*stash),
		base::unixtime::now(),
	});
}

std::unique_ptr<Data::ComposeStash> Restore(
		not_null<History*> history,
		Data::DraftKey key) {
	const auto ids = IdsFor(history, key);
	const auto row = AyuDatabase::getComposeStash(
		ids.userId,
		ids.dialogId,
		ids.topicRootId,
		ids.monoforumPeerId);
	return row ? Unpack(row->payload) : nullptr;
}

} // namespace FreshComposeStash
