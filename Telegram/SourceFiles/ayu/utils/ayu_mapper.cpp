// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/utils/ayu_mapper.h"

#include "apiwrap.h"
#include "api/api_text_entities.h"
#include "data/data_message_reaction_id.h"
#include "data/data_message_reactions.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_reply_markup.h"
#include "mtproto/connection_abstract.h"
#include "mtproto/details/mtproto_dump_to_text.h"

#include <QtCore/QDataStream>

namespace AyuMapper {

constexpr auto kMessageFlagUnread = 0x00000001;
constexpr auto kMessageFlagOut = 0x00000002;
constexpr auto kMessageFlagForwarded = 0x00000004;
constexpr auto kMessageFlagReply = 0x00000008;
constexpr auto kMessageFlagMention = 0x00000010;
constexpr auto kMessageFlagContentUnread = 0x00000020;
constexpr auto kMessageFlagHasMarkup = 0x00000040;
constexpr auto kMessageFlagHasEntities = 0x00000080;
constexpr auto kMessageFlagHasFromId = 0x00000100;
constexpr auto kMessageFlagHasMedia = 0x00000200;
constexpr auto kMessageFlagHasViews = 0x00000400;
constexpr auto kMessageFlagHasBotId = 0x00000800;
constexpr auto kMessageFlagIsSilent = 0x00001000;
constexpr auto kMessageFlagIsPost = 0x00004000;
constexpr auto kMessageFlagEdited = 0x00008000;
constexpr auto kMessageFlagHasPostAuthor = 0x00010000;
constexpr auto kMessageFlagIsGrouped = 0x00020000;
constexpr auto kMessageFlagFromScheduled = 0x00040000;
constexpr auto kMessageFlagHasReactions = 0x00100000;
constexpr auto kMessageFlagHideEdit = 0x00200000;
constexpr auto kMessageFlagRestricted = 0x00400000;
constexpr auto kMessageFlagHasReplies = 0x00800000;
constexpr auto kMessageFlagIsPinned = 0x01000000;
constexpr auto kMessageFlagHasTTL = 0x02000000;
constexpr auto kMessageFlagInvertMedia = 0x08000000;
constexpr auto kMessageFlagHasSavedPeer = 0x10000000;

template<typename MTPObject>
std::vector<char> serializeObject(MTPObject object) {
	mtpBuffer buffer;
	object.write(buffer);

	const auto from = reinterpret_cast<char*>(buffer.data());
	const auto end = from + buffer.size() * sizeof(mtpPrime);

	std::vector<char> entities(from, end);
	return entities;
}

template<typename MTPObject>
std::optional<MTPObject> tryDeserializeObject(const std::vector<char> &serialized) {
	if (serialized.empty() || serialized.size() % sizeof(mtpPrime) != 0) {
		return std::nullopt;
	}
	auto buffer = mtpBuffer(serialized.size() / sizeof(mtpPrime));
	memcpy(buffer.data(), serialized.data(), serialized.size());

	auto from = static_cast<const mtpPrime*>(buffer.data());
	const auto end = from + buffer.size();

	auto data = MTPObject();
	if (!data.read(from, end) || from != end) {
		return std::nullopt;
	}
	return data;
}

template<typename MTPObject>
MTPObject deserializeObject(std::vector<char> serialized) {
	auto result = tryDeserializeObject<MTPObject>(serialized);
	if (!result) {
		LOG(("AyuMapper: Failed to deserialize object"));
		return MTPObject();
	}
	return std::move(*result);
}

std::vector<char> serializeReactions(not_null<HistoryItem*> item) {
	const auto &list = item->reactions();
	if (list.empty()) {
		return {};
	}
	auto results = QVector<MTPReactionCount>();
	results.reserve(list.size());
	auto order = 0;
	for (const auto &reaction : list) {
		results.push_back(MTP_reactionCount(
			MTP_flags(reaction.my
				? MTPDreactionCount::Flag::f_chosen_order
				: MTPDreactionCount::Flag()),
			MTP_int(reaction.my ? ++order : 0),
			Data::ReactionToMTP(reaction.id),
			MTP_int(reaction.count)));
	}
	// Who reacted, as far as the client knows it (the recent reactions).
	auto recent = QVector<MTPMessagePeerReaction>();
	for (const auto &[id, list] : item->recentReactions()) {
		for (const auto &reaction : list) {
			using Flag = MTPDmessagePeerReaction::Flag;
			recent.push_back(MTP_messagePeerReaction(
				MTP_flags((reaction.big ? Flag::f_big : Flag())
					| (reaction.my ? Flag::f_my : Flag())),
				peerToMTP(reaction.peer->id),
				MTP_int(item->date()),
				Data::ReactionToMTP(id)));
		}
	}
	const auto withRecent = !recent.isEmpty();
	return serializeObject(MTP_messageReactions(
		MTP_flags(withRecent
			? MTPDmessageReactions::Flag::f_recent_reactions
				| MTPDmessageReactions::Flag::f_can_see_list
			: MTPDmessageReactions::Flag()),
		MTP_vector<MTPReactionCount>(std::move(results)),
		withRecent
			? MTP_vector<MTPMessagePeerReaction>(std::move(recent))
			: MTPVector<MTPMessagePeerReaction>(),
		MTPVector<MTPMessageReactor>()));
}

MTPMessageReactions deserializeReactions(const std::vector<char> &serialized) {
	return deserializeObject<MTPMessageReactions>(serialized);
}

std::vector<char> serializeReplyMarkup(not_null<HistoryItem*> item) {
	const auto markup = item->inlineReplyMarkup();
	if (!markup || markup->data.rows.empty()) {
		return {};
	}
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << qint32(1)
		<< quint32(uint32(markup->data.flags.value()))
		<< markup->data.placeholder
		<< qint32(markup->data.rows.size());
	for (const auto &row : markup->data.rows) {
		stream << qint32(row.size());
		for (const auto &button : row) {
			stream << quint8(button.type)
				<< button.text
				<< button.forwardText
				<< button.data
				<< qint64(button.buttonId)
				<< quint64(button.visual.iconId)
				<< quint8(button.visual.color);
		}
	}
	return std::vector<char>(bytes.constData(), bytes.constData() + bytes.size());
}

HistoryMessageMarkupData deserializeReplyMarkup(const std::vector<char> &serialized) {
	auto result = HistoryMessageMarkupData();
	if (serialized.empty()) {
		return result;
	}
	auto bytes = QByteArray(serialized.data(), qsizetype(serialized.size()));
	auto stream = QDataStream(&bytes, QIODevice::ReadOnly);
	stream.setVersion(QDataStream::Qt_5_15);
	auto version = qint32();
	auto flags = quint32();
	auto placeholder = QString();
	auto rowsCount = qint32();
	stream >> version >> flags >> placeholder >> rowsCount;
	if (stream.status() != QDataStream::Ok
		|| version != 1
		|| rowsCount <= 0
		|| rowsCount > 1000) {
		return result;
	}
	using Button = HistoryMessageMarkupButton;
	auto rows = std::vector<std::vector<Button>>();
	for (auto i = 0; i != rowsCount; ++i) {
		auto count = qint32();
		stream >> count;
		if (stream.status() != QDataStream::Ok || count < 0 || count > 1000) {
			return result;
		}
		auto row = std::vector<Button>();
		for (auto j = 0; j != count; ++j) {
			auto type = quint8();
			auto text = QString();
			auto forwardText = QString();
			auto data = QByteArray();
			auto buttonId = qint64();
			auto iconId = quint64();
			auto color = quint8();
			stream >> type >> text >> forwardText >> data >> buttonId >> iconId >> color;
			if (stream.status() != QDataStream::Ok
				|| type >= uint8(Button::Type::kCount)
				|| color > uint8(Button::Color::Success)) {
				return result;
			}
			row.emplace_back(
				Button::Type(type),
				text,
				Button::Visual{ DocumentId(iconId), Button::Color(color) },
				data,
				forwardText,
				buttonId);
		}
		if (!row.empty()) {
			rows.push_back(std::move(row));
		}
	}
	if (rows.empty()) {
		return result;
	}
	result.rows = std::move(rows);
	result.flags = ReplyMarkupFlags::from_raw(flags) & ~ReplyMarkupFlag::IsNull;
	result.placeholder = placeholder;
	return result;
}

std::pair<std::string, std::vector<char>> serializeTextWithEntities(not_null<HistoryItem*> item) {
	if (item->emptyText()) {
		return std::make_pair("", std::vector<char>());
	}
	auto textWithEntities = item->originalText();


	std::vector<char> entities;
	if (!textWithEntities.entities.empty()) {
		const auto mtpEntities = Api::EntitiesToMTP(
			&item->history()->session(),
			textWithEntities.entities,
			Api::ConvertOption::WithLocal);

		entities = serializeObject(mtpEntities);
	}

	return std::make_pair(textWithEntities.text.toStdString(), entities);
}

MTPVector<MTPMessageEntity> deserializeTextWithEntities(std::vector<char> serialized) {
	auto result = tryDeserializeObject<MTPVector<MTPMessageEntity>>(serialized);
	return result ? std::move(*result) : MTP_vector<MTPMessageEntity>();
}

std::vector<char> serializeSavableMedia(const MTPMessageMedia &media) {
	return media.match([](const MTPDmessageMediaPhoto &data) -> std::vector<char> {
		const auto photo = data.vphoto();
		if (!photo) {
			return {};
		}
		const auto video = data.vvideo();
		const auto flags = data.vflags().v & ~MTPDmessageMediaPhoto::Flag::f_ttl_seconds;
		return serializeObject(MTPMessageMedia(MTP_messageMediaPhoto(
			MTP_flags(flags),
			*photo,
			MTP_int(0),
			video ? MTPDocument(*video) : MTPDocument())));
	}, [](const MTPDmessageMediaDocument &data) -> std::vector<char> {
		const auto document = data.vdocument();
		if (!document) {
			return {};
		}
		const auto alt = data.valt_documents();
		const auto cover = data.vvideo_cover();
		const auto timestamp = data.vvideo_timestamp();
		const auto flags = data.vflags().v & ~MTPDmessageMediaDocument::Flag::f_ttl_seconds;
		return serializeObject(MTPMessageMedia(MTP_messageMediaDocument(
			MTP_flags(flags),
			*document,
			alt ? MTPVector<MTPDocument>(*alt) : MTPVector<MTPDocument>(),
			cover ? MTPPhoto(*cover) : MTPPhoto(),
			timestamp ? MTP_int(timestamp->v) : MTP_int(0),
			MTP_int(0))));
	}, [](const MTPDmessageMediaEmpty &) -> std::vector<char> {
		return {};
	}, [](const MTPDmessageMediaUnsupported &) -> std::vector<char> {
		return {};
	}, [&](const auto &) {
		return serializeObject(media);
	});
}

std::optional<MTPMessageMedia> tryDeserializeBareMedia(
		const std::vector<char> &serialized) {
	if (serialized.empty() || serialized.size() % sizeof(mtpPrime) != 0) {
		return std::nullopt;
	}
	auto buffer = mtpBuffer(serialized.size() / sizeof(mtpPrime));
	memcpy(buffer.data(), serialized.data(), serialized.size());

	for (const auto type : {
		mtpc_messageMediaPhoto,
		mtpc_messageMediaDocument,
	}) {
		auto from = static_cast<const mtpPrime*>(buffer.data());
		const auto end = from + buffer.size();
		auto data = MTPmessageMedia();
		if (data.read(from, end, type) && from == end) {
			return MTPMessageMedia(data);
		}
	}
	return std::nullopt;
}

MTPMessageMedia deserializeMedia(const std::vector<char> &serialized) {
	auto result = tryDeserializeObject<MTPMessageMedia>(serialized);
	if (!result) {
		result = tryDeserializeBareMedia(serialized);
	}
	if (!result) {
		if (const auto document = tryDeserializeObject<MTPDocument>(serialized)) {
			return MTP_messageMediaDocument(
				MTP_flags(MTPDmessageMediaDocument::Flag::f_document),
				*document,
				MTPVector<MTPDocument>(),
				MTPPhoto(),
				MTP_int(0),
				MTP_int(0));
		}
		if (const auto photo = tryDeserializeObject<MTPPhoto>(serialized)) {
			return MTP_messageMediaPhoto(
				MTP_flags(MTPDmessageMediaPhoto::Flag::f_photo),
				*photo,
				MTP_int(0),
				MTPDocument());
		}
		if (!serialized.empty()) {
			LOG(("AyuMapper: Failed to deserialize saved media"));
		}
		return MTP_messageMediaEmpty();
	}
	const auto valid = result->match([](const MTPDmessageMediaPhoto &data) {
		return data.vphoto() != nullptr;
	}, [](const MTPDmessageMediaDocument &data) {
		return data.vdocument() != nullptr;
	}, [](const MTPDmessageMediaGeo &) {
		return true;
	}, [](const MTPDmessageMediaVenue &) {
		return true;
	}, [](const MTPDmessageMediaContact &) {
		return true;
	}, [](const MTPDmessageMediaDice &) {
		return true;
	}, [](const MTPDmessageMediaPoll &) {
		return true;
	}, [](const MTPDmessageMediaToDo &) {
		return true;
	}, [](const auto &) {
		return false;
	});
	return valid ? std::move(*result) : MTP_messageMediaEmpty();
}

int mapItemFlagsToMTPFlags(not_null<HistoryItem*> item) {
	int flags = 0;

	const auto thread = item->topic()
							? reinterpret_cast<Data::Thread*>(item->topic())
							: item->history();
	if (item->unread(thread)) {
		flags |= kMessageFlagUnread;
	}

	if (item->out()) {
		flags |= kMessageFlagOut;
	}

	if (item->Get<HistoryMessageForwarded>()) {
		flags |= kMessageFlagForwarded;
	}

	if (item->Get<HistoryMessageReply>()) {
		flags |= kMessageFlagReply;
	}

	if (item->mentionsMe()) {
		flags |= kMessageFlagMention;
	}

	if (item->hasUnreadMediaFlag()) {
		flags |= kMessageFlagContentUnread;
	}

	if (item->definesReplyKeyboard()) {
		flags |= kMessageFlagHasMarkup;
	}

	if (!item->originalText().entities.empty()) {
		flags |= kMessageFlagHasEntities;
	}

	if (item->displayFrom()) {
		// todo: maybe wrong
		flags |= kMessageFlagHasFromId;
	}

	if (item->media()) {
		flags |= kMessageFlagHasMedia;
	}

	if (item->hasViews()) {
		flags |= kMessageFlagHasViews;
	}

	if (item->viaBot()) {
		flags |= kMessageFlagHasBotId;
	}

	if (item->isSilent()) {
		flags |= kMessageFlagIsSilent;
	}

	if (item->isPost()) {
		flags |= kMessageFlagIsPost;
	}

	if (item->Get<HistoryMessageEdited>()) {
		flags |= kMessageFlagEdited;
	}

	if (item->Get<HistoryMessageSigned>()) {
		flags |= kMessageFlagHasPostAuthor;
	}

	if (item->groupId()) {
		flags |= kMessageFlagIsGrouped;
	}

	if (item->isScheduled()) {
		flags |= kMessageFlagFromScheduled;
	}

	if (!item->reactions().empty()) {
		flags |= kMessageFlagHasReactions;
	}

	if (item->hideEditedBadge()) {
		flags |= kMessageFlagHideEdit;
	}

	if (item->hasPossibleRestrictions()) {
		flags |= kMessageFlagRestricted;
	}

	if (item->repliesCount() > 0) {
		flags |= kMessageFlagHasReplies;
	}

	if (item->isPinned()) {
		flags |= kMessageFlagIsPinned;
	}

	if (item->ttlDestroyAt() > 0) {
		flags |= kMessageFlagHasTTL;
	}

	if (item->invertMedia()) {
		flags |= kMessageFlagInvertMedia;
	}

	if (item->savedFromSender()) {
		// todo: maybe wrong
		flags |= kMessageFlagHasSavedPeer;
	}

	return flags;
}

}
