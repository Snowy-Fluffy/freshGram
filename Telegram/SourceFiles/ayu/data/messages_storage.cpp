// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/data/deleted_restore.h"
#include "ayu/secret/secret_peer.h"
#include "ayu/data/messages_storage.h"

#include "ayu/ayu_settings.h"
#include "ayu/data/ayu_database.h"
#include "ayu/utils/ayu_mapper.h"
#include "ayu/utils/telegram_helpers.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/file_location.h"
#include "logs.h"
#include "crl/crl_on_main.h"
#include "data/data_cloud_file.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_forum_topic.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_reply_markup.h"
#include "main/main_session.h"
#include "storage/cache/storage_cache_database.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <cstdlib>
#include <optional>

namespace AyuMessages {

namespace {

[[nodiscard]] int64 MaxCachedDocumentSize() {
	return int64(AyuSettings::getInstance().deletedMediaMaxSizeMb())
		* 1024 * 1024;
}

constexpr auto kPhotoSaveAttempts = 60;
constexpr auto kLongSaveAttempts = 900;

struct PhotoSaveTask {
	std::shared_ptr<Data::PhotoMedia> media;
	QString path;
	base::Timer timer;
	int attempts = 0;
	int maxAttempts = 0;
};

struct DocumentSaveTask {
	std::shared_ptr<Data::DocumentMedia> media;
	QString path;
	base::Timer timer;
	int attempts = 0;
	int maxAttempts = 0;
};

std::vector<DeletedMessage> PendingDeleted;
std::vector<DeletedExtra> PendingExtras;
std::vector<DeletedMarkup> PendingMarkups;
bool FlushScheduled = false;
std::vector<std::unique_ptr<PhotoSaveTask>> PhotoSaveTasks;
std::vector<std::unique_ptr<DocumentSaveTask>> DocumentSaveTasks;

[[nodiscard]] QString SavedMediaDirectory() {
	return QStringLiteral("./tdata/ayu_media");
}

[[nodiscard]] QString SavedMediaPath(
		ID userId,
		ID dialogId,
		int messageId) {
	return QString("%1/%2_%3_%4.bin")
		.arg(SavedMediaDirectory())
		.arg(userId)
		.arg(dialogId)
		.arg(messageId);
}

[[nodiscard]] QString TtlMediaPath(QString path) {
	path.chop(4);
	return path + QStringLiteral(".ttl");
}

[[nodiscard]] QString SavedMediaPath(not_null<HistoryItem*> item) {
	return SavedMediaPath(
		storageUserId(item->history()->peer),
		getDialogIdFromPeer(item->history()->peer),
		item->id.bare);
}

bool WritePhotoBytes(
		const std::shared_ptr<Data::PhotoMedia> &media,
		const QString &path) {
	if (!media->loaded()) {
		return false;
	}
	const auto bytes = media->imageBytes(Data::PhotoSize::Large);
	if (!bytes.isEmpty()) {
		QDir().mkpath(QFileInfo(path).absolutePath());
		auto file = QFile(path);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(bytes);
		}
	}
	return true;
}

void SavePhotoBytes(
		not_null<PhotoData*> photo,
		FullMsgId origin,
		const QString &path,
		int maxAttempts) {
	if (QFile::exists(path)) {
		return;
	}
	auto media = photo->createMediaView();
	media->wanted(Data::PhotoSize::Large, origin);
	if (WritePhotoBytes(media, path)) {
		return;
	}
	auto task = std::make_unique<PhotoSaveTask>();
	const auto raw = task.get();
	raw->media = std::move(media);
	raw->path = path;
	raw->maxAttempts = maxAttempts;
	raw->timer.setCallback([=] {
		++raw->attempts;
		if (WritePhotoBytes(raw->media, raw->path)
			|| raw->attempts >= raw->maxAttempts) {
			raw->timer.cancel();
			crl::on_main([=] {
				PhotoSaveTasks.erase(
					std::remove_if(
						PhotoSaveTasks.begin(),
						PhotoSaveTasks.end(),
						[=](const auto &task) { return task.get() == raw; }),
					PhotoSaveTasks.end());
			});
		}
	});
	raw->timer.callEach(1000);
	PhotoSaveTasks.push_back(std::move(task));
}

[[nodiscard]] QByteArray DocumentBytes(
		const std::shared_ptr<Data::DocumentMedia> &media) {
	auto bytes = media->bytes();
	if (!bytes.isEmpty()) {
		return bytes;
	}
	const auto &location = media->owner()->location(true);
	if (location.accessEnable()) {
		auto file = QFile(location.name());
		if (file.size() <= MaxCachedDocumentSize()
			&& file.open(QIODevice::ReadOnly)) {
			bytes = file.readAll();
		}
		location.accessDisable();
	}
	return bytes;
}

bool WriteDocumentBytes(
		const std::shared_ptr<Data::DocumentMedia> &media,
		const QString &path) {
	if (!media->loaded()) {
		return false;
	}
	const auto bytes = DocumentBytes(media);
	if (!bytes.isEmpty()) {
		QDir().mkpath(QFileInfo(path).absolutePath());
		auto file = QFile(path);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(bytes);
		}
	}
	return true;
}

void SaveDocumentBytes(
		not_null<DocumentData*> document,
		not_null<HistoryItem*> item,
		const QString &path,
		int maxAttempts) {
	if (QFile::exists(path)
		|| document->size <= 0
		|| document->size > MaxCachedDocumentSize()) {
		return;
	}
	auto media = document->createMediaView();
	media->automaticLoad(item->fullId(), item);
	if (WriteDocumentBytes(media, path)) {
		return;
	}
	auto task = std::make_unique<DocumentSaveTask>();
	const auto raw = task.get();
	raw->media = std::move(media);
	raw->path = path;
	raw->maxAttempts = maxAttempts;
	raw->timer.setCallback([=] {
		++raw->attempts;
		if (WriteDocumentBytes(raw->media, raw->path)
			|| raw->attempts >= raw->maxAttempts) {
			raw->timer.cancel();
			crl::on_main([=] {
				DocumentSaveTasks.erase(
					std::remove_if(
						DocumentSaveTasks.begin(),
						DocumentSaveTasks.end(),
						[=](const auto &task) { return task.get() == raw; }),
					DocumentSaveTasks.end());
			});
		}
	});
	raw->timer.callEach(1000);
	DocumentSaveTasks.push_back(std::move(task));
}

void flushPendingDeleted() {
	FlushScheduled = false;
	if (PendingDeleted.empty()) {
		return;
	}
	auto batch = std::move(PendingDeleted);
	PendingDeleted.clear();
	auto extras = std::move(PendingExtras);
	PendingExtras.clear();
	AyuDatabase::addDeletedMessages(batch);
	AyuDatabase::addDeletedExtras(extras);
	auto markups = std::move(PendingMarkups);
	PendingMarkups.clear();
	AyuDatabase::addDeletedMarkups(markups);
}

}

template<typename DerivedMessage>
std::vector<AyuMessageBase> convertToBase(const std::vector<DerivedMessage> &messages) {
	std::vector<AyuMessageBase> based;
	based.reserve(messages.size());
	for (const auto &msg : messages) {
		based.push_back(static_cast<AyuMessageBase>(msg));
	}
	return based;
}

void map(not_null<HistoryItem*> item, AyuMessageBase &message) {
	const ID userId = item->history()->owner().session().userId().bare & PeerId::kChatTypeMask;

	message.userId = userId;
	message.dialogId = getDialogIdFromPeer(item->history()->peer);
	message.groupedId = item->groupId().raw();
	message.peerId = item->history()->peer->id.value & PeerId::kChatTypeMask;
	message.fromId = item->from()->id.value & PeerId::kChatTypeMask;
	if (item->topic()) {
		message.topicId = item->topicRootId().bare;
	} else {
		message.topicId = 0;
	}
	message.messageId = item->id.bare;
	message.date = item->date();
	message.flags = AyuMapper::mapItemFlagsToMTPFlags(item);

	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		message.editDate = edited->date;
	} else {
		message.editDate = base::unixtime::now();
	}

	message.views = item->viewsCount();
	message.fwdFlags = 0;
	message.fwdFromId = 0;
	// message.fwdName
	message.fwdDate = 0;
	// message.fwdPostAuthor
	if (const auto msgsigned = item->Get<HistoryMessageSigned>()) {
		message.postAuthor = msgsigned->author.toStdString();
	}
	message.replyFlags = 0;
	message.replyMessageId = 0;
	message.replyPeerId = 0;
	message.replyTopId = 0;
	message.replyForumTopic = false;
	if (const auto reply = item->Get<HistoryMessageReply>();
		reply && !reply->externalPeerId()) {
		message.replyMessageId = int(reply->messageId().bare);
		message.replyTopId = int(reply->topMessageId().bare);
		message.replyForumTopic = reply->topicPost();
	}
	// message.replySerialized
	// message.replyMarkupSerialized
	message.entityCreateDate = base::unixtime::now();

	auto serializedText = AyuMapper::serializeTextWithEntities(item);
	message.text = serializedText.first;
	message.textEntities = serializedText.second;

	// todo: implement mapping
	message.documentSerialized = item->ayuSavedMedia();
	const auto media = item->media();
	const auto mediaFile = SavedMediaPath(item);
	message.mediaPath = ((media && (media->photo() || media->document()))
		|| QFile::exists(mediaFile))
		? mediaFile.toStdString()
		: "/";
	// message.hqThumbPath
	message.documentType = message.documentSerialized.empty() ? 0 : 1;
	// message.documentSerialized
	// message.thumbsSerialized
	// message.documentAttributesSerialized
	// message.mimeType
}

bool isBotMessage(not_null<const HistoryItem*> item) {
	const auto isBot = [](PeerData *peer) {
		const auto user = peer ? peer->asUser() : nullptr;
		return user && user->isBot();
	};
	return isBot(item->history()->peer) || isBot(item->from()) || item->viaBot();
}

void addEditedMessage(not_null<HistoryItem *> item) {
	if (AyuSecret::IsSecretPeer(item->history()->peer)) {
		return;
	}
	EditedMessage message;
	map(item, message);

	if (message.text.empty()) {
		return;
	}

	AyuDatabase::addEditedMessage(
		message,
		AyuSettings::getInstance().maxEditRevisions());
}

ID storageUserId(not_null<PeerData*> peer) {
	return peer->session().userId().bare & PeerId::kChatTypeMask;
}

std::vector<AyuMessageBase> loadEditedMessages(ID userId, ID dialogId, ID messageId, ID minId, ID maxId, int totalLimit) {
	return convertToBase(AyuDatabase::getEditedMessages(userId, dialogId, messageId, minId, maxId, totalLimit));
}

bool hasRevisions(not_null<const HistoryItem*> item) {
	const ID userId = item->history()->owner().session().userId().bare & PeerId::kChatTypeMask;
	const auto dialogId = getDialogIdFromPeer(item->history()->peer);
	const auto msgId = item->id.bare;

	return AyuDatabase::hasRevisions(userId, dialogId, msgId);
}

void cacheDeletedMedia(not_null<HistoryItem*> item) {
	if (AyuSecret::IsSecretPeer(item->history()->peer)) {
		return;
	}
	const auto media = item->media();
	if (!media || !AyuSettings::getInstance().deletedMediaMaxSizeMb()) {
		return;
	}
	const auto origin = item->fullId();
	const auto attempts = media->ttlSeconds()
		? kLongSaveAttempts
		: kPhotoSaveAttempts;
	if (const auto photo = media->photo()) {
		SavePhotoBytes(photo, origin, SavedMediaPath(item), attempts);
	}
	if (const auto document = media->document()) {
		document->loadThumbnail(origin);
		SaveDocumentBytes(document, item, SavedMediaPath(item), attempts);
	}
}

void PutPhotoBytesIntoCache(
		not_null<PhotoData*> photo,
		const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto bytes = file.readAll();
	const auto cacheKey = photo->location(
		Data::PhotoSize::Large).file().cacheKey();
	if (bytes.isEmpty() || !cacheKey) {
		return;
	}
	photo->owner().cache().putIfEmpty(
		cacheKey,
		Storage::Cache::Database::TaggedValue(
			QByteArray(bytes),
			Data::kImageCacheTag));
}

void PutDocumentBytesIntoCache(
		not_null<DocumentData*> document,
		const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto bytes = file.readAll();
	if (bytes.isEmpty()) {
		return;
	}
	document->owner().cache().putIfEmpty(
		document->cacheKey(),
		Storage::Cache::Database::TaggedValue(
			QByteArray(bytes),
			document->cacheTag()));
}

void PutMediaBytesIntoCache(
		not_null<HistoryItem*> item,
		const QString &path) {
	const auto media = item->media();
	if (!media) {
		return;
	}
	if (const auto photo = media->photo()) {
		PutPhotoBytesIntoCache(photo, path);
	} else if (const auto document = media->document()) {
		PutDocumentBytesIntoCache(document, path);
	}
}

void restoreSavedMedia(
		not_null<HistoryItem*> item,
		const AyuMessageBase &message) {
	if (message.mediaPath.empty() || message.mediaPath == "/") {
		return;
	}
	PutMediaBytesIntoCache(item, QString::fromStdString(message.mediaPath));
}

void saveTtlMedia(not_null<HistoryItem*> item) {
	if (AyuSecret::IsSecretPeer(item->history()->peer)) {
		return;
	}
	const auto &saved = item->ayuSavedMedia();
	if (saved.empty()) {
		return;
	}
	const auto path = TtlMediaPath(SavedMediaPath(item));
	if (QFile::exists(path)) {
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(saved.data(), qint64(saved.size()));
		LOG(("Ayu: saved self-destructing media of %1").arg(item->id.bare));
	} else {
		LOG(("Ayu: could not save self-destructing media of %1").arg(item->id.bare));
	}
}

static MTPMessageMedia WithSpoiler(const MTPMessageMedia &media) {
	return media.match([&](const MTPDmessageMediaPhoto &data) {
		const auto photo = data.vphoto();
		if (!photo || (data.vflags().v & MTPDmessageMediaPhoto::Flag::f_spoiler)) {
			return media;
		}
		const auto video = data.vvideo();
		return MTPMessageMedia(MTP_messageMediaPhoto(
			MTP_flags(data.vflags().v | MTPDmessageMediaPhoto::Flag::f_spoiler),
			*photo,
			MTP_int(0),
			video ? MTPDocument(*video) : MTPDocument()));
	}, [&](const MTPDmessageMediaDocument &data) {
		const auto document = data.vdocument();
		if (!document || (data.vflags().v & MTPDmessageMediaDocument::Flag::f_spoiler)) {
			return media;
		}
		const auto alt = data.valt_documents();
		const auto cover = data.vvideo_cover();
		const auto timestamp = data.vvideo_timestamp();
		return MTPMessageMedia(MTP_messageMediaDocument(
			MTP_flags(data.vflags().v | MTPDmessageMediaDocument::Flag::f_spoiler),
			*document,
			alt ? MTPVector<MTPDocument>(*alt) : MTPVector<MTPDocument>(),
			cover ? MTPPhoto(*cover) : MTPPhoto(),
			timestamp ? MTP_int(timestamp->v) : MTP_int(0),
			MTP_int(0)));
	}, [&](const auto &) {
		return media;
	});
}

std::optional<MTPMessageMedia> savedTtlMedia(
		not_null<History*> history,
		MsgId id) {
	const auto path = TtlMediaPath(SavedMediaPath(
		storageUserId(history->peer),
		getDialogIdFromPeer(history->peer),
		id.bare));
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		LOG(("Ayu: no saved self-destructing media for %1").arg(id.bare));
		return std::nullopt;
	}
	const auto bytes = file.readAll();
	if (bytes.isEmpty()) {
		return std::nullopt;
	}
	const auto serialized = std::vector<char>(
		bytes.constData(),
		bytes.constData() + bytes.size());
	auto media = AyuMapper::deserializeMedia(serialized);
	if (media.type() == mtpc_messageMediaEmpty) {
		return std::nullopt;
	}
	return WithSpoiler(media);
}

MTPMessageMedia restoredMedia(const AyuMessageBase &message) {
	auto media = AyuMapper::deserializeMedia(message.documentSerialized);
	const auto path = TtlMediaPath(SavedMediaPath(
		message.userId,
		message.dialogId,
		message.messageId));
	return QFile::exists(path) ? WithSpoiler(media) : media;
}

void restoreTtlBytes(not_null<HistoryItem*> item) {
	PutMediaBytesIntoCache(item, SavedMediaPath(item));
}

std::vector<ID> loadDeletedDialogIds(ID userId) {
	return AyuDatabase::getDeletedDialogIds(userId);
}

std::optional<DeletedExtra> MakeExtra(
		not_null<HistoryItem*> item,
		const AyuMessageBase &message) {
	auto extra = DeletedExtra();
	extra.fakeId = 0;
	extra.userId = message.userId;
	extra.dialogId = message.dialogId;
	extra.messageId = message.messageId;
	extra.reactions = AyuMapper::serializeReactions(item);
	extra.repliesCount = 0;
	extra.commentsChannelId = 0;
	extra.commentsRootId = 0;
	extra.commentsReadTill = 0;
	extra.commentsMaxId = 0;
	extra.entityCreateDate = base::unixtime::now();
	if (const auto views = item->Get<HistoryMessageViews>();
		views && views->commentsMegagroupId) {
		extra.repliesCount = std::max(views->replies.count, 0);
		extra.commentsChannelId = ID(views->commentsMegagroupId.bare);
		extra.commentsRootId = int(views->commentsRootId.bare);
		extra.commentsReadTill = int(views->commentsInboxReadTillId.bare);
		extra.commentsMaxId = int(views->commentsMaxId.bare);
		for (const auto &replier : views->recentRepliers) {
			if (!extra.repliers.empty()) {
				extra.repliers += ',';
			}
			extra.repliers += std::to_string(replier.value);
		}
	}
	if (extra.reactions.empty() && !extra.commentsChannelId) {
		return std::nullopt;
	}
	return extra;
}

void addDeletedMessage(not_null<HistoryItem*> item) {
	if (AyuSecret::IsSecretPeer(item->history()->peer)) {
		return;
	}
	DeletedMessage message;
	map(item, message);

	if (item->isService()) {
		auto text = item->notificationText().text;
		const auto postfix = QString(" (%1)").arg(
			AyuSettings::getInstance().deletedMark());
		if (text.endsWith(postfix)) {
			text.chop(postfix.size());
		}
		if (text.isEmpty()) {
			return;
		}
		message.text = text.toStdString();
		message.textEntities.clear();
		message.documentSerialized.clear();
		message.mediaPath = "/";
		message.documentType = kServiceDocumentType;
	} else if (message.text.empty() && message.documentSerialized.empty() && item->media()) {
		message.text = item->notificationText().text.toStdString();
	}
	if (message.text.empty() && message.documentSerialized.empty()) {
		return;
	}

	if (const auto extra = MakeExtra(item, message)) {
		PendingExtras.push_back(*extra);
	}
	auto markup = AyuMapper::serializeReplyMarkup(item);
	if (!markup.empty()) {
		PendingMarkups.push_back({
			.fakeId = 0,
			.userId = message.userId,
			.dialogId = message.dialogId,
			.messageId = message.messageId,
			.markup = std::move(markup),
			.entityCreateDate = base::unixtime::now(),
		});
	}
	PendingDeleted.push_back(std::move(message));
	AyuRestore::noteDeleted(item->history());
	if (!FlushScheduled) {
		FlushScheduled = true;
		crl::on_main(flushPendingDeleted);
	}
}

std::vector<AyuMessageBase> loadDeletedMessages(
		ID userId,
		ID dialogId,
		ID topicId,
		ID minId,
		ID maxId,
		int totalLimit,
		const std::string &searchQuery) {
	return convertToBase(AyuDatabase::getDeletedMessages(userId, dialogId, topicId, minId, maxId, totalLimit, searchQuery));
}

void flushPending() {
	flushPendingDeleted();
}

std::vector<AyuMessageBase> searchDeletedMessages(
		ID userId,
		ID dialogId,
		ID topicId,
		ID fromId,
		const std::string &query,
		int limit) {
	return convertToBase(AyuDatabase::searchDeletedMessages(
		userId,
		dialogId,
		topicId,
		fromId,
		query,
		limit));
}

std::vector<DeletedExtra> loadDeletedExtras(ID userId, ID dialogId) {
	return AyuDatabase::getDeletedExtras(userId, dialogId);
}

std::vector<DeletedMarkup> loadDeletedMarkups(ID userId, ID dialogId) {
	return AyuDatabase::getDeletedMarkups(userId, dialogId);
}

void restoreExtra(not_null<HistoryItem*> item, const DeletedExtra &extra) {
	if (!extra.reactions.empty()) {
		const auto reactions = AyuMapper::deserializeReactions(
			extra.reactions);
		item->updateReactions(&reactions);
	}
	if (!extra.commentsChannelId) {
		return;
	}
	const auto channelId = ChannelId(uint64(extra.commentsChannelId));
	auto data = HistoryMessageRepliesData();
	data.isNull = false;
	data.repliesCount = extra.repliesCount;
	data.channelId = channelId;
	data.readMaxId = MsgId(extra.commentsReadTill);
	data.maxId = MsgId(extra.commentsMaxId);
	auto start = size_t(0);
	while (start < extra.repliers.size()) {
		auto end = extra.repliers.find(',', start);
		if (end == std::string::npos) {
			end = extra.repliers.size();
		}
		const auto value = std::strtoull(
			extra.repliers.substr(start, end - start).c_str(),
			nullptr,
			10);
		if (value) {
			data.recentRepliers.push_back(PeerId(value));
		}
		start = end + 1;
	}
	item->setReplies(std::move(data));
	if (extra.commentsRootId) {
		item->setCommentsItemId(FullMsgId(
			peerFromChannel(channelId),
			MsgId(extra.commentsRootId)));
	}
}

bool hasDeletedMessages(not_null<PeerData*> peer, ID topicId) {
	flushPendingDeleted();
	return AyuDatabase::hasDeletedMessages(storageUserId(peer), getDialogIdFromPeer(peer), topicId);
}

void removeDeletedMessage(not_null<HistoryItem*> item) {
	flushPendingDeleted();
	const auto peer = item->history()->peer;
	const ID userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	AyuDatabase::removeDeletedMessage(userId, getDialogIdFromPeer(peer), item->id.bare);
}

void clearAllDeleted() {
	flushPendingDeleted();
	AyuDatabase::clearAllDeleted();
	QDir(SavedMediaDirectory()).removeRecursively();
}

void clearDeletedMessages(not_null<PeerData*> peer, ID topicId) {
	flushPendingDeleted();
	const ID userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	AyuDatabase::clearDeletedMessages(userId, getDialogIdFromPeer(peer), topicId);
}

}
