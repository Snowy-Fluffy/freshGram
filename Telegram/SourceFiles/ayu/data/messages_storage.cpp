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
#include "ayu/data/fresh_vault.h"
#include "ayu/utils/ayu_mapper.h"
#include "ayu/utils/telegram_helpers.h"
#include "base/flat_set.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/file_location.h"
#include "logs.h"
#include "crl/crl_async.h"
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
#include "history/history_item_edition.h"
#include "iv/iv_rich_page.h"
#include "history/history_item_reply_markup.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/cache/storage_cache_database.h"
#include "storage/file_download.h"
#include "ui/toast/toast.h"

#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>

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
std::vector<DeletedRank> PendingRanks;
bool FlushScheduled = false;
std::vector<std::unique_ptr<PhotoSaveTask>> PhotoSaveTasks;
std::vector<std::unique_ptr<DocumentSaveTask>> DocumentSaveTasks;

[[nodiscard]] QString SavedMediaDirectory() {
	return QStringLiteral("./tdata/fresh_media");
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
	auto bytes = media->imageBytes(Data::PhotoSize::Large);
	if (bytes.isEmpty()) {
		// WHY: a sent photo keeps only the decoded image taken from the upload.
		if (const auto image = media->image(Data::PhotoSize::Large)) {
			auto buffer = QBuffer(&bytes);
			image->original().save(&buffer, "JPG", 87);
		}
	}
	if (!bytes.isEmpty()) {
		QDir().mkpath(QFileInfo(path).absolutePath());
		return FreshVault::WriteFile(path, bytes);
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
		return FreshVault::WriteFile(path, bytes);
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
	auto ranks = std::move(PendingRanks);
	PendingRanks.clear();
	AyuDatabase::addDeletedRanks(ranks);
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
	message.documentSerialized = item->freshSavedMedia();
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

namespace {

[[nodiscard]] bool MediaChanged(
		not_null<HistoryItem*> item,
		const MTPMessageMedia *media) {
	const auto old = item->media();
	const auto oldPhoto = old ? old->photo() : nullptr;
	const auto oldDocument = old ? old->document() : nullptr;
	auto newPhotoId = uint64(0);
	auto newDocumentId = uint64(0);
	if (media) {
		media->match([&](const MTPDmessageMediaPhoto &data) {
			if (const auto photo = data.vphoto()) {
				photo->match([&](const MTPDphoto &data) {
					newPhotoId = uint64(data.vid().v);
				}, [](const auto &) {
				});
			}
		}, [&](const MTPDmessageMediaDocument &data) {
			if (const auto document = data.vdocument()) {
				document->match([&](const MTPDdocument &data) {
					newDocumentId = uint64(data.vid().v);
				}, [](const auto &) {
				});
			}
		}, [](const auto &) {
		});
	}
	return (oldPhoto ? uint64(oldPhoto->id) : uint64(0)) != newPhotoId
		|| (oldDocument ? uint64(oldDocument->id) : uint64(0)) != newDocumentId;
}

} // namespace

bool shouldSaveEdit(
		not_null<HistoryItem*> item,
		const HistoryMessageEdition &edit) {
	const auto &settings = AyuSettings::getInstance();
	const auto peer = item->history()->peer;
	if (item->out() && !settings.saveEditedOwn()) {
		return false;
	} else if (peer->isUser() && !settings.saveEditedPrivate()) {
		return false;
	} else if ((peer->isChat() || peer->isMegagroup())
		&& !settings.saveEditedGroups()) {
		return false;
	} else if (peer->isChannel()
		&& !peer->isMegagroup()
		&& !settings.saveEditedChannels()) {
		return false;
	}
	if (settings.saveEditedText()) {
		const auto &was = item->originalText();
		const auto same = edit.richPage
			? (Iv::FlattenRichPageSummary(edit.richPage) == was)
			: (edit.textWithEntities == was);
		if (!same && !was.empty()) {
			return true;
		}
	}
	if (settings.saveEditedMarkup()
		&& !edit.useSameMarkup
		&& (AyuMapper::serializeReplyMarkup(item)
			!= AyuMapper::serializeReplyMarkup(edit.replyMarkup))) {
		return true;
	}
	return settings.saveEditedMedia() && MediaChanged(item, edit.mtpMedia);
}

void addEditedMessage(not_null<HistoryItem *> item) {
	if (FreshSecret::IsSecretPeer(item->history()->peer)) {
		return;
	}
	EditedMessage message;
	map(item, message);
	if (AyuSettings::getInstance().saveEditedMarkup()) {
		message.replyMarkupSerialized = AyuMapper::serializeReplyMarkup(item);
	}

	if (message.text.empty()
		&& message.documentSerialized.empty()
		&& message.replyMarkupSerialized.empty()) {
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
	if (FreshSecret::IsSecretPeer(item->history()->peer)) {
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
	const auto read = FreshVault::ReadFile(path);
	if (!read) {
		return;
	}
	const auto &bytes = *read;
	const auto cacheKey = photo->location(
		Data::PhotoSize::Large).file().cacheKey();
	if (bytes.isEmpty() || !cacheKey || bytes.size() > Storage::kMaxFileInMemory) {
		return;
	}
	photo->owner().cache().putIfEmpty(
		cacheKey,
		Storage::Cache::Database::TaggedValue(
			QByteArray(bytes),
			Data::kImageCacheTag));
}

namespace {

constexpr auto kOpenedFileLifetime = 30 * 60 * crl::time(1000);
constexpr auto kOpenedMediaIdle = 60 * crl::time(1000);
constexpr auto kOpenedCheckEach = 30 * crl::time(1000);

struct LargeDocument {
	DocumentId id = 0;
	QString path;
	std::shared_ptr<Data::DocumentMedia> media;
	QString openedPath;
	crl::time used = 0;
};

struct OpenedFile {
	QString path;
	crl::time created = 0;
};

base::flat_map<not_null<DocumentData*>, LargeDocument> LargeDocuments;
base::flat_set<DocumentId> PendingDecrypts;
std::vector<OpenedFile> OpenedFiles;
std::unique_ptr<base::Timer> OpenedTimer;

[[nodiscard]] QString LegacyOpenedMediaDirectory() {
	return QStringLiteral("./tdata/fresh_media_tmp");
}

// Played or shown inside the app, so the bytes stay in memory only.
[[nodiscard]] bool OpensInside(not_null<DocumentData*> document) {
	return document->isVideoFile()
		|| document->isAnimation()
		|| document->isAudioFile()
		|| document->isVoiceMessage()
		|| document->isVideoMessage()
		|| document->isImage();
}

[[nodiscard]] QString OpenedTarget(
		const QString &path,
		not_null<DocumentData*> document) {
	auto suffix = QFileInfo(document->filename()).suffix();
	suffix.remove(QRegularExpression(u"[^A-Za-z0-9]"_q));
	return QDir(openedMediaDirectory()).absoluteFilePath(
		QFileInfo(path).completeBaseName()
		+ (suffix.isEmpty() ? QString() : ('.' + suffix.left(16))));
}

[[nodiscard]] bool WriteOpenedFile(
		const QString &target,
		const QByteArray &bytes) {
	const auto directory = openedMediaDirectory();
	if (!QDir().mkpath(directory)) {
		return false;
	}
	QFile::setPermissions(
		directory,
		QFileDevice::ReadOwner
			| QFileDevice::WriteOwner
			| QFileDevice::ExeOwner);
	auto file = QSaveFile(target);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	return (file.write(bytes) == bytes.size()) && file.commit();
}

void CheckOpened() {
	const auto now = crl::now();
	for (auto &[document, entry] : LargeDocuments) {
		if (entry.media
			&& entry.media.use_count() == 1
			&& now - entry.used >= kOpenedMediaIdle) {
			entry.media = nullptr;
		}
	}
	OpenedFiles.erase(ranges::remove_if(OpenedFiles, [&](
			const OpenedFile &file) {
		// A file that is still busy on Windows is tried again later.
		return (now - file.created >= kOpenedFileLifetime)
			&& FreshVault::WipeFile(file.path);
	}), end(OpenedFiles));
	const auto busy = !OpenedFiles.empty() || ranges::any_of(
		LargeDocuments,
		[](const auto &pair) { return pair.second.media != nullptr; });
	if (!busy && OpenedTimer) {
		OpenedTimer->cancel();
	}
}

void WatchOpened() {
	if (!OpenedTimer) {
		OpenedTimer = std::make_unique<base::Timer>(CheckOpened);
	}
	if (!OpenedTimer->isActive()) {
		OpenedTimer->callEach(kOpenedCheckEach);
	}
}

void WipeOpenedFiles(bool wait) {
	auto paths = QStringList();
	for (const auto &info : QDir(openedMediaDirectory()).entryInfoList(
			QDir::Files | QDir::Hidden)) {
		paths.push_back(info.absoluteFilePath());
	}
	for (const auto &info : QDir(LegacyOpenedMediaDirectory()).entryInfoList(
			QDir::Files | QDir::Hidden)) {
		paths.push_back(info.absoluteFilePath());
	}
	OpenedFiles.clear();
	const auto wipe = [=] {
		for (const auto &path : paths) {
			FreshVault::WipeFile(path);
		}
		QDir(LegacyOpenedMediaDirectory()).removeRecursively();
	};
	if (wait) {
		wipe();
	} else {
		crl::async(wipe);
	}
}

} // namespace

QString openedMediaDirectory() {
	static const auto result = [] {
		// WHY: every copy of the app has its own folder, so one does not
		// wipe the files that another one has opened.
		const auto hash = QCryptographicHash::hash(
			QDir(cWorkingDir()).absolutePath().toUtf8(),
			QCryptographicHash::Sha256).toHex().left(12);
		return QDir(QStandardPaths::writableLocation(
			QStandardPaths::TempLocation)).absoluteFilePath(
				u"freshgram-media-"_q + QString::fromLatin1(hash));
	}();
	return result;
}

void PutDocumentBytesIntoCache(
		not_null<DocumentData*> document,
		const QString &path) {
	if (QFileInfo(path).size() > Storage::kMaxFileInMemory) {
		auto &entry = LargeDocuments[document];
		if (entry.id != document->id || entry.path != path) {
			entry = LargeDocument{ .id = document->id, .path = path };
		}
		return;
	}
	const auto read = FreshVault::ReadFile(path);
	if (!read || read->isEmpty()) {
		return;
	}
	const auto &bytes = *read;
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
	if (FreshSecret::IsSecretPeer(item->history()->peer)) {
		return;
	}
	const auto &saved = item->freshSavedMedia();
	if (saved.empty()) {
		return;
	}
	const auto path = TtlMediaPath(SavedMediaPath(item));
	if (QFile::exists(path)) {
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	if (FreshVault::WriteFile(path, QByteArray(saved.data(), qsizetype(saved.size())))) {
		LOG(("freshGram: saved self-destructing media of %1").arg(item->id.bare));
	} else {
		LOG(("freshGram: could not save self-destructing media of %1").arg(item->id.bare));
	}
}

static MTPMessageMedia WithTtl(const MTPMessageMedia &media) {
	const auto seconds = MTP_int(int(Data::kTimeToLiveSingleView));
	return media.match([&](const MTPDmessageMediaPhoto &data) {
		const auto photo = data.vphoto();
		if (!photo) {
			return media;
		}
		const auto video = data.vvideo();
		return MTPMessageMedia(MTP_messageMediaPhoto(
			MTP_flags(data.vflags().v | MTPDmessageMediaPhoto::Flag::f_ttl_seconds),
			*photo,
			seconds,
			video ? MTPDocument(*video) : MTPDocument()));
	}, [&](const MTPDmessageMediaDocument &data) {
		const auto document = data.vdocument();
		if (!document) {
			return media;
		}
		const auto alt = data.valt_documents();
		const auto cover = data.vvideo_cover();
		const auto timestamp = data.vvideo_timestamp();
		return MTPMessageMedia(MTP_messageMediaDocument(
			MTP_flags(data.vflags().v | MTPDmessageMediaDocument::Flag::f_ttl_seconds),
			*document,
			alt ? MTPVector<MTPDocument>(*alt) : MTPVector<MTPDocument>(),
			cover ? MTPPhoto(*cover) : MTPPhoto(),
			timestamp ? MTP_int(timestamp->v) : MTP_int(0),
			seconds));
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
	const auto read = FreshVault::ReadFile(path);
	if (!read) {
		LOG(("freshGram: no saved self-destructing media for %1").arg(id.bare));
		return std::nullopt;
	}
	const auto &bytes = *read;
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
	return WithTtl(media);
}

MTPMessageMedia restoredMedia(const AyuMessageBase &message) {
	auto media = AyuMapper::deserializeMedia(message.documentSerialized);
	const auto path = TtlMediaPath(SavedMediaPath(
		message.userId,
		message.dialogId,
		message.messageId));
	return QFile::exists(path) ? WithTtl(media) : media;
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
	if (FreshSecret::IsSecretPeer(item->history()->peer)) {
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
	if (const auto rank = item->fromRank(); !rank.isEmpty()) {
		PendingRanks.push_back({
			.fakeId = 0,
			.userId = message.userId,
			.dialogId = message.dialogId,
			.messageId = message.messageId,
			.rank = rank.toStdString(),
			.entityCreateDate = base::unixtime::now(),
		});
	}
	PendingDeleted.push_back(std::move(message));
	FreshRestore::noteDeleted(item->history());
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
		int limit,
		bool withMedia) {
	return convertToBase(AyuDatabase::searchDeletedMessages(
		userId,
		dialogId,
		topicId,
		fromId,
		query,
		limit,
		withMedia));
}

std::vector<DeletedExtra> loadDeletedExtras(ID userId, ID dialogId) {
	return AyuDatabase::getDeletedExtras(userId, dialogId);
}

std::vector<DeletedMarkup> loadDeletedMarkups(ID userId, ID dialogId) {
	return AyuDatabase::getDeletedMarkups(userId, dialogId);
}

std::vector<DeletedRank> loadDeletedRanks(ID userId, ID dialogId) {
	return AyuDatabase::getDeletedRanks(userId, dialogId);
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
	const auto dialogId = getDialogIdFromPeer(peer);
	AyuDatabase::removeDeletedMessage(userId, dialogId, item->id.bare);
	if (AyuSettings::getInstance().deleteSavedFiles()) {
		removeSavedFiles({ { userId, dialogId, ID(item->id.bare) } });
	}
}

void removeSavedFiles(const std::vector<SavedFileKey> &keys) {
	for (const auto &key : keys) {
		const auto path = SavedMediaPath(
			key.userId,
			key.dialogId,
			int(key.messageId));
		for (const auto &file : { path, TtlMediaPath(path) }) {
			if (QFile::exists(file)) {
				FreshVault::WipeFile(file);
			}
		}
	}
}

void clearAllDeleted() {
	flushPendingDeleted();
	AyuDatabase::clearAllDeleted();
	QDir(SavedMediaDirectory()).removeRecursively();
}

void clearDeletedDatabase() {
	flushPendingDeleted();
	AyuDatabase::clearAllDeleted();
	AyuDatabase::flush();
}

void clearSavedMedia() {
	QDir(SavedMediaDirectory()).removeRecursively();
}

void clearUserDatabase(ID userId) {
	flushPendingDeleted();
	AyuDatabase::clearUserData(userId);
}

void clearUserMedia(ID userId) {
	const auto dir = QDir(SavedMediaDirectory());
	for (const auto &name : dir.entryList(
			QStringList() << (QString::number(userId) + "_*"),
			QDir::Files)) {
		FreshVault::WipeFile(dir.absoluteFilePath(name));
	}
}

void clearOpenedMedia() {
	LargeDocuments.clear();
	PendingDecrypts.clear();
	OpenedTimer = nullptr;
	WipeOpenedFiles(true);
}

void releaseOpenedMedia() {
	for (auto &[document, entry] : LargeDocuments) {
		entry.media = nullptr;
		entry.openedPath = QString();
	}
	WipeOpenedFiles(false);
}

// WHY: the cache takes up to 10 MB per value. A bigger media is decrypted
// into memory on the click, another file into a private temporary folder.
bool prepareLargeMedia(not_null<DocumentData*> document) {
	const auto i = LargeDocuments.find(document);
	if (i == LargeDocuments.end() || i->second.id != document->id) {
		return false;
	}
	auto &entry = i->second;
	if (entry.media && !entry.media->bytes().isEmpty()) {
		entry.used = crl::now();
		return false;
	} else if (!entry.openedPath.isEmpty()
		&& QFile::exists(entry.openedPath)
		&& !document->filepath(true).isEmpty()) {
		return false;
	}
	const auto id = document->id;
	if (!PendingDecrypts.emplace(id).second) {
		return true;
	}
	Ui::Toast::Show(tr::fresh_DeletedMediaDecrypting(tr::now));
	const auto path = entry.path;
	const auto inside = OpensInside(document);
	const auto target = inside ? QString() : OpenedTarget(path, document);
	crl::async([=, session = &document->session()] {
		auto read = FreshVault::ReadFile(path);
		auto done = read && !read->isEmpty();
		if (done && !inside) {
			done = WriteOpenedFile(target, *read);
			read = std::nullopt;
		}
		crl::on_main([=] { PendingDecrypts.remove(id); });
		crl::on_main(session, [=, read = std::move(read)] {
			const auto i = LargeDocuments.find(document);
			if (i == LargeDocuments.end() || i->second.id != id) {
				if (!target.isEmpty()) {
					FreshVault::WipeFile(target);
				}
				return;
			} else if (!done) {
				Ui::Toast::Show(tr::fresh_DeletedMediaDecryptFailed(tr::now));
				return;
			}
			auto &entry = i->second;
			if (inside) {
				entry.media = document->createMediaView();
				entry.media->setBytes(*read);
				entry.used = crl::now();
			} else {
				entry.openedPath = target;
				OpenedFiles.push_back({ target, crl::now() });
				document->setLocation(Core::FileLocation(target));
			}
			WatchOpened();
			session->data().requestDocumentViewRepaint(document);
		});
	});
	return true;
}

void clearEverything() {
	clearOpenedMedia();
	flushPendingDeleted();
	AyuDatabase::clearEverything();
	clearSavedMedia();
	QDir(u"./tdata/fresh_secret"_q).removeRecursively();
	QDir(u"./tdata/fresh_secret_tmp"_q).removeRecursively();
}

void clearDeletedMessages(not_null<PeerData*> peer, ID topicId) {
	flushPendingDeleted();
	const ID userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	const auto dialogId = getDialogIdFromPeer(peer);
	auto keys = std::vector<SavedFileKey>();
	if (AyuSettings::getInstance().deleteSavedFiles()) {
		for (const auto messageId : AyuDatabase::getDeletedMessageIds(
				userId,
				dialogId,
				topicId)) {
			keys.push_back({ userId, dialogId, messageId });
		}
	}
	AyuDatabase::clearDeletedMessages(userId, dialogId, topicId);
	if (!keys.empty()) {
		crl::async([=] { removeSavedFiles(keys); });
	}
}

}
