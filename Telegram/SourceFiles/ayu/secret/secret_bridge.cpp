#include "ayu/secret/secret_bridge.h"

#include "ayu/secret/secret_peer.h"
#include "ayu/secret/secret_policy.h"
#include "ayu/ayu_settings.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/file_location.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_send_action.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/image/image_location_factory.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"
#include "window/notifications_manager.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtGui/QImage>
#include <QtGui/QImageReader>

#include <openssl/sha.h>

#include <cstring>

namespace FreshSecret {
namespace {

constexpr auto kHistoryLimit = 150;

struct DocumentRef {
	not_null<Main::Session*> session;
	int chatId = 0;
	int64_t randomId = 0;
};

[[nodiscard]] std::map<DocumentId, DocumentRef> &Documents() {
	static auto result = std::map<DocumentId, DocumentRef>();
	return result;
}

[[nodiscard]] uint64 LocalMediaId(
		uint8_t kind,
		int chatId,
		int64_t randomId) {
	static const auto salt = RandomVector(32);
	auto input = Bytes(salt);
	input.push_back(kind);
	for (auto i = 0; i != 4; ++i) {
		input.push_back(uint8_t((uint32_t(chatId) >> (i * 8)) & 0xFF));
	}
	for (auto i = 0; i != 8; ++i) {
		input.push_back(uint8_t((uint64_t(randomId) >> (i * 8)) & 0xFF));
	}
	uint8_t digest[SHA256_DIGEST_LENGTH];
	SHA256(input.data(), input.size(), digest);
	auto result = uint64(0);
	std::memcpy(&result, digest, sizeof(result));
	result &= 0x7FFFFFFFFFFFFFFFULL;
	return result ? result : 1;
}

[[nodiscard]] QString Qs(const std::string &value) {
	return QString::fromStdString(value);
}

[[nodiscard]] QByteArray ToArray(const Bytes &bytes) {
	return QByteArray(
		reinterpret_cast<const char*>(bytes.data()),
		int(bytes.size()));
}

[[nodiscard]] ::EntityType TextEntityType(int type) {
	switch (EntityType(type)) {
	case EntityType::Mention: return ::EntityType::Mention;
	case EntityType::Hashtag: return ::EntityType::Hashtag;
	case EntityType::BotCommand: return ::EntityType::BotCommand;
	case EntityType::Url: return ::EntityType::Url;
	case EntityType::Email: return ::EntityType::Email;
	case EntityType::Bold: return ::EntityType::Bold;
	case EntityType::Italic: return ::EntityType::Italic;
	case EntityType::Code: return ::EntityType::Code;
	case EntityType::Pre: return ::EntityType::Pre;
	case EntityType::TextUrl: return ::EntityType::CustomUrl;
	case EntityType::Phone: return ::EntityType::Phone;
	case EntityType::Cashtag: return ::EntityType::Cashtag;
	case EntityType::BankCard: return ::EntityType::BankCard;
	case EntityType::Underline: return ::EntityType::Underline;
	case EntityType::Strike: return ::EntityType::StrikeOut;
	case EntityType::Blockquote: return ::EntityType::Blockquote;
	case EntityType::Spoiler: return ::EntityType::Spoiler;
	default: return ::EntityType::Invalid;
	}
}

[[nodiscard]] TextWithEntities BuildText(const MessageData &data) {
	auto result = TextWithEntities();
	const auto &media = data.media;
	const auto append = [&](const QString &line, const QString &url) {
		if (line.isEmpty()) {
			return;
		}
		if (!result.text.isEmpty()) {
			result.text += '\n';
		}
		const auto offset = result.text.size();
		result.text += line;
		if (!url.isEmpty()) {
			result.entities.push_back(EntityInText(
				::EntityType::CustomUrl,
				offset,
				line.size(),
				url));
		}
	};
	const auto geo = QString(
		"https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=16/%1/%2")
		.arg(media.latitude, 0, 'f', 6)
		.arg(media.longitude, 0, 'f', 6);
	switch (media.type) {
	case MediaType::Location:
		append(tr::fresh_SecretMediaLocation(tr::now), geo);
		append(
			QString("%1, %2")
				.arg(media.latitude, 0, 'f', 5)
				.arg(media.longitude, 0, 'f', 5),
			QString());
		break;
	case MediaType::Venue:
		append(Qs(media.title), geo);
		append(Qs(media.address), QString());
		break;
	case MediaType::Contact:
		append(
			(Qs(media.firstName) + " " + Qs(media.lastName)).trimmed(),
			QString());
		append(Qs(media.phone), QString());
		break;
	case MediaType::WebPage:
		append(Qs(media.url), Qs(media.url));
		break;
	case MediaType::Unsupported:
		append(tr::fresh_SecretMediaUnsupported(tr::now), QString());
		break;
	default:
		break;
	}
	auto text = Qs(data.text);
	auto entities = data.entities;
	if (text.isEmpty()) {
		text = Qs(media.caption);
		entities.clear();
	}
	if (text.isEmpty()) {
		return result;
	}
	auto shift = 0;
	if (!result.text.isEmpty()) {
		result.text += '\n';
		shift = result.text.size();
	}
	result.text += text;
	for (const auto &entity : entities) {
		const auto type = TextEntityType(entity.type);
		if (type == ::EntityType::Invalid
			|| entity.length <= 0
			|| entity.offset < 0
			|| entity.offset > text.size()
			|| entity.length > text.size() - entity.offset) {
			continue;
		}
		result.entities.push_back(EntityInText(
			type,
			shift + entity.offset,
			entity.length,
			Qs(entity.extra)));
	}
	return result;
}

[[nodiscard]] bool NeedsFile(const MessageData &data) {
	switch (data.media.type) {
	case MediaType::Photo:
	case MediaType::Video:
	case MediaType::Document:
	case MediaType::Voice:
	case MediaType::Audio:
	case MediaType::Sticker:
	case MediaType::Animation:
	case MediaType::External:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] bool HasStoredFile(const MessageData &data) {
	return !data.media.path.empty()
		&& QFile::exists(Qs(data.media.path));
}

[[nodiscard]] ImageWithLocation InMemoryImage(
		const QByteArray &bytes,
		int width,
		int height) {
	if (bytes.isEmpty() || width <= 0 || height <= 0) {
		return ImageWithLocation();
	}
	return ImageWithLocation{
		.location = ImageLocation(
			DownloadLocation{ InMemoryLocation{ bytes } },
			width,
			height),
		.bytes = bytes,
		.bytesCount = int(bytes.size()),
	};
}

[[nodiscard]] ImageWithLocation ThumbOf(const Media &media) {
	if (media.thumb.empty()) {
		return ImageWithLocation();
	}
	const auto bytes = ToArray(media.thumb);
	const auto image = QImage::fromData(bytes);
	if (image.isNull()) {
		return ImageWithLocation();
	}
	return Images::FromImageInMemory(image, "JPG", bytes);
}

[[nodiscard]] QVector<MTPDocumentAttribute> AttributesOf(
		const Media &media) {
	auto result = QVector<MTPDocumentAttribute>();
	if (!media.fileName.empty()) {
		result.push_back(MTP_documentAttributeFilename(
			MTP_string(Qs(media.fileName))));
	}
	switch (media.type) {
	case MediaType::Voice:
		result.push_back(MTP_documentAttributeAudio(
			MTP_flags(MTPDdocumentAttributeAudio::Flag::f_voice
				| (media.waveform.empty()
					? MTPDdocumentAttributeAudio::Flag()
					: MTPDdocumentAttributeAudio::Flag::f_waveform)),
			MTP_int(media.duration),
			MTPstring(),
			MTPstring(),
			MTP_bytes(ToArray(media.waveform))));
		break;
	case MediaType::Audio:
		result.push_back(MTP_documentAttributeAudio(
			MTP_flags(0),
			MTP_int(media.duration),
			MTP_string(Qs(media.title)),
			MTPstring(),
			MTPbytes()));
		break;
	case MediaType::Video:
	case MediaType::Animation:
		if (media.type == MediaType::Animation
			&& Qs(media.mime).startsWith("image/")) {
			result.push_back(MTP_documentAttributeImageSize(
				MTP_int(media.width),
				MTP_int(media.height)));
			result.push_back(MTP_documentAttributeAnimated());
			break;
		}
		result.push_back(MTP_documentAttributeVideo(
			MTP_flags(MTPDdocumentAttributeVideo::Flag::f_supports_streaming
				| (media.round
					? MTPDdocumentAttributeVideo::Flag::f_round_message
					: MTPDdocumentAttributeVideo::Flag())),
			MTP_double(media.duration),
			MTP_int(media.width),
			MTP_int(media.height),
			MTPint(),
			MTPdouble(),
			MTPstring()));
		if (media.type == MediaType::Animation || media.animated) {
			result.push_back(MTP_documentAttributeAnimated());
		}
		break;
	case MediaType::Sticker:
		result.push_back(MTP_documentAttributeImageSize(
			MTP_int((media.width > 0) ? media.width : 512),
			MTP_int((media.height > 0) ? media.height : 512)));
		result.push_back(MTP_documentAttributeSticker(
			MTP_flags(0),
			MTP_string(Qs(media.emoji)),
			MTP_inputStickerSetEmpty(),
			MTPMaskCoords()));
		break;
	default:
		break;
	}
	return result;
}

} // namespace

Bridge::Bridge(
	not_null<Main::Session*> session,
	not_null<Manager*> manager)
: _session(session)
, _manager(manager) {
	_manager->changes(
	) | rpl::on_next([=] {
		if (!_refreshing) {
			refreshAll();
		}
	}, _lifetime);
	_manager->messageChanges(
	) | rpl::on_next([=](int chatId) {
		if (!_refreshing) {
			refreshChat(chatId);
		}
	}, _lifetime);
}

Bridge::~Bridge() {
	auto &documents = Documents();
	for (auto i = documents.begin(); i != documents.end();) {
		if (i->second.session == _session) {
			i = documents.erase(i);
		} else {
			++i;
		}
	}
}

void Bridge::registerDocument(
		DocumentId id,
		int chatId,
		int64_t randomId) {
	Documents().insert_or_assign(id, DocumentRef{
		.session = _session,
		.chatId = chatId,
		.randomId = randomId,
	});
}

void Bridge::forgetDocuments(int chatId) {
	auto &documents = Documents();
	for (auto i = documents.begin(); i != documents.end();) {
		if (i->second.session == _session && i->second.chatId == chatId) {
			i = documents.erase(i);
		} else {
			++i;
		}
	}
}

void Bridge::refreshAll() {
	if (!Enabled()) {
		removeAll();
		return;
	}
	auto alive = std::set<int>();
	for (const auto &info : _manager->chats()) {
		alive.insert(info.id);
		refreshChat(info.id);
	}
	auto dead = std::vector<int>();
	for (const auto &[chatId, binding] : _bindings) {
		if (!alive.contains(chatId)) {
			dead.push_back(chatId);
		}
	}
	for (const auto chatId : dead) {
		removeChat(chatId);
	}
	if (!_ready) {
		_ready = true;
		for (const auto &[chatId, binding] : _bindings) {
			if (binding.history) {
				Core::App().notifications().clearFromHistory(binding.history);
			}
		}
	}
}

void Bridge::refreshChat(int chatId) {
	if (!Enabled()) {
		return;
	}
	const auto info = _manager->chat(chatId);
	if (!info) {
		removeChat(chatId);
		return;
	}
	_refreshing = true;
	const auto guard = gsl::finally([&] { _refreshing = false; });
	auto &binding = ensureBinding(*info);
	if (_manager->revision(chatId) != binding.revision) {
		syncMessages(binding, *info);
	}
	const auto typing = info->typing && (info->state == ChatState::Ready);
	if (typing && !binding.typing) {
		_session->data().sendActionManager().registerFor(
			binding.history,
			MsgId(0),
			binding.user,
			MTP_sendMessageTypingAction(),
			base::unixtime::now());
	}
	binding.typing = typing;
}

Bridge::Binding &Bridge::ensureBinding(const ChatInfo &info) {
	auto &binding = _bindings[info.id];
	binding.chatId = info.id;
	auto &owner = _session->data();
	const auto bare = PeerBareForChat(info.id);
	const auto name = info.title;
	if (!binding.user) {
		auto user = owner.userLoaded(UserId(bare));
		if (!user) {
			user = owner.processUser(MTP_user(
				MTP_flags(MTPDuser::Flag::f_first_name
					| MTPDuser::Flag::f_status),
				MTP_long(bare),
				MTPlong(),
				MTP_string(name),
				MTPstring(),
				MTPstring(),
				MTPstring(),
				MTP_userProfilePhotoEmpty(),
				MTP_userStatusRecently(MTP_flags(0)),
				MTPint(),
				MTPVector<MTPRestrictionReason>(),
				MTPstring(),
				MTPstring(),
				MTPEmojiStatus(),
				MTPVector<MTPUsername>(),
				MTPRecentStory(),
				MTPPeerColor(),
				MTPPeerColor(),
				MTPint(),
				MTPlong(),
				MTPlong(),
				MTPlong()));
		}
		binding.user = user;
		user->setCallsStatus(UserData::CallsStatus::Disabled);
		user->setIsBlocked(false);
		user->addFlags(UserDataFlag::MessageMoneyRestrictionsKnown);
		user->setBarSettings(PeerBarSettings());
	}
	if (!binding.real) {
		binding.real = owner.user(UserId(info.peerUserId));
		const auto chatId = info.id;
		_session->changes().peerUpdates(
			not_null<PeerData*>(binding.real),
			Data::PeerUpdate::Flag::Name | Data::PeerUpdate::Flag::Photo
		) | rpl::on_next([=] {
			if (!_refreshing) {
				refreshChat(chatId);
			}
			if (const auto i = _bindings.find(chatId); i != _bindings.end()) {
				_session->changes().peerUpdated(
					i->second.user,
					Data::PeerUpdate::Flag::Photo);
			}
		}, binding.lifetime);
	}
	if (binding.user->notify().settingsUnknown()) {
		binding.user->notify().resetToDefault();
	}
	if (binding.user->name() != name) {
		binding.user->setName(name, QString(), QString(), QString());
	}
	if (!binding.history) {
		binding.history = owner.history(binding.user);
		if (!binding.history->folderKnown()) {
			binding.history->clearFolder();
		}
		if (!binding.history->unreadCountKnown()) {
			binding.history->setUnreadCount(
				(info.state == ChatState::Requested) ? 1 : 0);
		}
		binding.history->markLoadedAtTop();
	}
	if (!binding.history->chatListTimeId()) {
		binding.history->setChatListTimeId(
			info.lastDate ? info.lastDate : info.date);
	}
	return binding;
}

void Bridge::promptRequest(int chatId) {
	const auto info = _manager->chat(chatId);
	const auto i = _bindings.find(chatId);
	if (!info
		|| i == _bindings.end()
		|| info->state != ChatState::Requested
		|| i->second.asked) {
		return;
	}
	const auto window = Core::App().activePrimaryWindow();
	if (!window) {
		return;
	}
	i->second.asked = true;
	const auto session = _session;
	auto box = Ui::MakeConfirmBox({
		.text = tr::fresh_SecretChatRequestAsk(
			tr::now,
			lt_name,
			info->title),
		.confirmed = [=](Fn<void()> close) {
			Get(session).accept(chatId);
			close();
		},
		.cancelled = [=](Fn<void()> close) {
			Get(session).end(chatId);
			close();
		},
		.confirmText = tr::fresh_SecretChatAccept(),
		.cancelText = tr::fresh_SecretChatDecline(),
		.strictCancel = true,
	});
	box->boxClosing(
	) | rpl::on_next([=] {
		Get(session).bridge().promptClosed(chatId);
	}, box->lifetime());
	window->show(std::move(box));
}

void Bridge::openChat(int chatId) {
	const auto i = _bindings.find(chatId);
	if (i == _bindings.end() || !i->second.history) {
		return;
	}
	const auto history = i->second.history;
	if (const auto window = Core::App().windowForShowingHistory(
			history->peer)) {
		if (const auto controller = window->sessionController()) {
			controller->showPeerHistory(history);
		}
	}
}

void Bridge::promptClosed(int chatId) {
	const auto i = _bindings.find(chatId);
	if (i != _bindings.end()) {
		i->second.asked = false;
	}
}

void Bridge::syncMessages(Binding &binding, const ChatInfo &info) {
	binding.revision = _manager->revision(info.id);
	auto messages = _manager->messages(info.id, kHistoryLimit);
	if (int(messages.size()) > kHistoryLimit) {
		messages.erase(
			messages.begin(),
			messages.end() - kHistoryLimit);
	}
	auto &owner = _session->data();
	auto present = std::set<int64_t>();
	auto unreadLeft = info.unread;
	auto unreadFrom = int(messages.size());
	for (auto i = int(messages.size()) - 1; i >= 0 && unreadLeft > 0; --i) {
		const auto &message = messages[i];
		if (message.outgoing || message.special) {
			continue;
		}
		unreadFrom = i;
		--unreadLeft;
	}
	for (auto i = 0; i != int(messages.size()); ++i) {
		const auto &message = messages[i];
		present.insert(message.randomId);
		const auto existing = binding.items.find(message.randomId);
		if (existing == binding.items.end()) {
			const auto unread = !message.outgoing
				&& !message.special
				&& (i >= unreadFrom);
			createItem(binding, info, message, unread);
			continue;
		}
		const auto item = owner.message(binding.user, existing->second);
		if (!item) {
			continue;
		}
		if (message.outgoing && message.state == DeliveryState::Read) {
			item->setFreshSecretRead();
		}
		if (message.deleted && !item->isDeleted()) {
			item->setDeleted();
		}
	}
	auto gone = std::vector<int64_t>();
	for (const auto &[randomId, msgId] : binding.items) {
		if (!present.contains(randomId)
			&& !binding.requested.contains(randomId)) {
			gone.push_back(randomId);
		}
	}
	for (const auto randomId : gone) {
		const auto msgId = binding.items[randomId];
		binding.items.erase(randomId);
		_byMsg.erase(msgId);
		if (const auto item = owner.message(binding.user, msgId)) {
			item->destroy();
		}
	}
	owner.sendHistoryChangeNotifications();
}

HistoryItem *Bridge::createItem(
		Binding &binding,
		const ChatInfo &info,
		const MessageData &message,
		bool unread) {
	auto &owner = _session->data();
	const auto history = binding.history;
	if (message.media.type != MediaType::None
		&& NeedsFile(message)
		&& !HasStoredFile(message)) {
		if (!message.outgoing
			&& message.media.fileId
			&& binding.requested.emplace(message.randomId).second) {
			_manager->downloadMedia(info.id, message.randomId);
			return nullptr;
		} else if (_manager->progress(info.id, message.randomId) >= 0.) {
			return nullptr;
		}
	}
	const auto id = owner.nextLocalMessageId();
	const auto self = _session->userPeerId();
	const auto fakeId = binding.user->id;
	auto flags = MessageFlags(MessageFlag::Local);
	if (message.outgoing) {
		flags |= MessageFlag::Outgoing | MessageFlag::HasFromId;
	} else {
		flags |= MessageFlag::HasFromId;
		if (unread) {
			flags |= MessageFlag::ClientSideUnread;
		}
	}
	auto replyTo = FullReplyTo();
	if (message.replyTo) {
		const auto i = binding.items.find(message.replyTo);
		if (i != binding.items.end()) {
			replyTo.messageId = FullMsgId(fakeId, i->second);
			flags |= MessageFlag::HasReplyInfo;
		}
	}
	auto fields = HistoryItemCommonFields{
		.id = id,
		.flags = flags,
		.from = message.outgoing ? self : fakeId,
		.replyTo = replyTo,
		.date = message.date,
	};
	HistoryItem *result = nullptr;
	if (message.special) {
		fields.flags = MessageFlags(MessageFlag::Local);
		if (_ready
			&& (message.special == kSpecialEnded
				|| message.special == kSpecialRequest)) {
			fields.flags |= MessageFlag::ClientSideUnread;
		}
		fields.from = PeerId();
		fields.replyTo = FullReplyTo();
		const auto service = history->makeMessage(
			std::move(fields),
			PreparedServiceText{
				.text = TextWithEntities{ Qs(message.text) },
			});
		result = history->addNewLocalMessage(service);
	} else if (message.media.type == MediaType::Photo
		&& HasStoredFile(message)) {
		const auto bytes = _manager->readFile(info.id, message.randomId);
		if (bytes.isEmpty()) {
			result = history->addNewLocalMessage(
				std::move(fields),
				TextWithEntities{ tr::fresh_SecretMediaPhotoFailed(tr::now) },
				MTP_messageMediaEmpty());
		} else {
			auto width = message.media.width;
			auto height = message.media.height;
			if (width <= 0 || height <= 0) {
				auto buffer = QBuffer();
				buffer.setData(bytes);
				buffer.open(QIODevice::ReadOnly);
				const auto size = QImageReader(&buffer).size();
				width = size.width();
				height = size.height();
			}
			const auto photoId = PhotoId(LocalMediaId(1, info.id, message.randomId));
			const auto thumb = ThumbOf(message.media);
			const auto photo = owner.photo(
				photoId,
				uint64(0),
				QByteArray(),
				message.date,
				0,
				false,
				QByteArray(),
				ImageWithLocation(),
				thumb.location.valid() ? thumb : ImageWithLocation(),
				InMemoryImage(bytes, width, height),
				ImageWithLocation(),
				ImageWithLocation(),
				crl::time(0));
			const auto caption = BuildText(message);
			if (message.ttl > 0) {
				result = history->addNewLocalMessage(
					std::move(fields),
					caption,
					MTP_messageMediaPhoto(
						MTP_flags(MTPDmessageMediaPhoto::Flag::f_photo
							| MTPDmessageMediaPhoto::Flag::f_ttl_seconds),
						MTP_photo(
							MTP_flags(0),
							MTP_long(photoId),
							MTP_long(0),
							MTP_bytes(),
							MTP_int(message.date),
							MTP_vector<MTPPhotoSize>(),
							MTPVector<MTPVideoSize>(),
							MTP_int(0)),
						MTP_int(message.ttl),
						MTPDocument()));
			} else {
				result = history->addNewLocalMessage(
					std::move(fields),
					photo,
					caption);
			}
		}
	} else if (message.media.type != MediaType::None
		&& NeedsFile(message)
		&& message.media.type != MediaType::Photo
		&& HasStoredFile(message)) {
		const auto &media = message.media;
		const auto docId = DocumentId(LocalMediaId(2, info.id, message.randomId));
		auto mime = Qs(media.mime);
		if (mime.isEmpty()) {
			mime = (media.type == MediaType::Sticker)
				? QString("image/webp")
				: QString("application/octet-stream");
		}
		registerDocument(docId, info.id, message.randomId);
		const auto document = owner.document(
			docId,
			uint64(0),
			QByteArray(),
			message.date,
			AttributesOf(media),
			mime,
			InlineImageLocation(),
			ThumbOf(media),
			ImageWithLocation(),
			false,
			0,
			media.size);
		if (media.type == MediaType::Voice
			|| (media.type == MediaType::Video && media.round)) {
			if (!message.outgoing && unread) {
				fields.flags |= MessageFlag::MediaIsUnread;
			}
		}
		result = history->addNewLocalMessage(
			std::move(fields),
			document,
			BuildText(message));
	} else if (message.media.type != MediaType::None
		&& NeedsFile(message)) {
		result = history->addNewLocalMessage(
			std::move(fields),
			TextWithEntities{ tr::fresh_SecretMediaUnavailable(tr::now) },
			MTP_messageMediaEmpty());
	} else if (message.media.type == MediaType::Location
		|| message.media.type == MediaType::Venue) {
		result = history->addNewLocalMessage(
			std::move(fields),
			BuildText(message),
			MTP_messageMediaGeo(MTP_geoPoint(
				MTP_flags(0),
				MTP_double(message.media.longitude),
				MTP_double(message.media.latitude),
				MTP_long(0),
				MTPint())));
	} else {
		result = history->addNewLocalMessage(
			std::move(fields),
			BuildText(message),
			MTP_messageMediaEmpty());
	}
	if (result) {
		binding.items[message.randomId] = result->id;
		_byMsg[result->id] = { info.id, message.randomId };
		if (message.outgoing && message.state == DeliveryState::Read) {
			result->setFreshSecretRead();
		}
		if (message.deleted) {
			result->setDeleted();
		}
	}
	return result;
}

void Bridge::removeAll() {
	auto &owner = _session->data();
	for (auto &[chatId, binding] : _bindings) {
		forgetDocuments(chatId);
		if (binding.history) {
			for (const auto &[randomId, msgId] : binding.items) {
				if (const auto item = owner.message(binding.user, msgId)) {
					item->destroy();
				}
			}
		}
		binding.items.clear();
		binding.revision = -2;
		if (binding.history) {
			owner.deleteConversationLocally(binding.user);
			binding.history = nullptr;
		}
	}
	_byMsg.clear();
}

void Bridge::removeChat(int chatId) {
	const auto i = _bindings.find(chatId);
	if (i == _bindings.end()) {
		return;
	}
	auto &owner = _session->data();
	const auto user = i->second.user;
	for (const auto &window : _session->windows()) {
		if (window->activeChatCurrent().peer() == user) {
			window->clearSectionStack();
		}
	}
	forgetDocuments(chatId);
	for (const auto &[randomId, msgId] : i->second.items) {
		_byMsg.erase(msgId);
		if (const auto item = owner.message(user, msgId)) {
			item->destroy();
		}
	}
	_bindings.erase(i);
	if (user) {
		owner.deleteConversationLocally(user);
	}
}

std::optional<std::pair<int, int64_t>> Bridge::lookup(
		not_null<const HistoryItem*> item) const {
	const auto i = _byMsg.find(item->id);
	if (i == _byMsg.end()) {
		return std::nullopt;
	}
	return i->second;
}

HistoryItem *Bridge::item(int chatId, int64_t randomId) const {
	const auto i = _bindings.find(chatId);
	if (i == _bindings.end()) {
		return nullptr;
	}
	const auto j = i->second.items.find(randomId);
	return (j != i->second.items.end())
		? _session->data().message(i->second.user, j->second)
		: nullptr;
}

History *Bridge::history(int chatId) const {
	const auto i = _bindings.find(chatId);
	return (i != _bindings.end()) ? i->second.history : nullptr;
}

UserData *Bridge::realUser(int chatId) const {
	const auto i = _bindings.find(chatId);
	return (i != _bindings.end()) ? i->second.real : nullptr;
}

int64_t Bridge::randomIdOf(int chatId, FullMsgId id) const {
	const auto i = _byMsg.find(id.msg);
	if (i == _byMsg.end() || i->second.first != chatId) {
		return 0;
	}
	return i->second.second;
}

std::vector<not_null<HistoryItem*>> Bridge::itemsDeletedByUser(
		const std::vector<not_null<HistoryItem*>> &items) {
	auto byChat = std::map<int, std::vector<int64_t>>();
	for (const auto &item : items) {
		const auto i = _byMsg.find(item->id);
		if (i == _byMsg.end()) {
			continue;
		}
		const auto [chatId, randomId] = i->second;
		byChat[chatId].push_back(randomId);
	}
	_refreshing = true;
	const auto guard = gsl::finally([&] { _refreshing = false; });
	for (const auto &[chatId, ids] : byChat) {
		_manager->deleteMessages(chatId, ids);
		if (const auto b = _bindings.find(chatId); b != _bindings.end()) {
			b->second.revision = _manager->revision(chatId);
		}
	}
	auto result = std::vector<not_null<HistoryItem*>>();
	for (const auto &item : items) {
		const auto i = _byMsg.find(item->id);
		if (i == _byMsg.end()) {
			continue;
		}
		const auto [chatId, randomId] = i->second;
		if (_manager->message(chatId, randomId)) {
			item->setDeleted();
			continue;
		}
		_byMsg.erase(i);
		if (const auto b = _bindings.find(chatId); b != _bindings.end()) {
			b->second.items.erase(randomId);
		}
		result.push_back(item);
	}
	return result;
}

void Bridge::historyClearedByUser(not_null<History*> history) {
	const auto chatId = ChatIdOfPeer(history->peer);
	if (!chatId) {
		return;
	}
	const auto i = _bindings.find(chatId);
	_refreshing = true;
	const auto guard = gsl::finally([&] { _refreshing = false; });
	if (i != _bindings.end()) {
		auto &owner = _session->data();
		const auto user = i->second.user;
		for (const auto &[randomId, msgId] : i->second.items) {
			_byMsg.erase(msgId);
			if (const auto item = owner.message(user, msgId)) {
				item->destroy();
			}
		}
		i->second.items.clear();
		i->second.requested.clear();
	}
	_manager->clearHistory(chatId);
	if (i != _bindings.end()) {
		i->second.revision = _manager->revision(chatId);
		_session->data().sendHistoryChangeNotifications();
	}
}

void Bridge::messagesRead(not_null<History*> history) {
	const auto chatId = ChatIdOfPeer(history->peer);
	if (chatId) {
		_manager->markRead(chatId);
	}
}

void Bridge::mediaOpened(not_null<HistoryItem*> item) {
	const auto i = _byMsg.find(item->id);
	if (i != _byMsg.end()) {
		_manager->openMessage(i->second.first, i->second.second);
	}
}

Bridge &BridgeFor(not_null<Main::Session*> session) {
	return Get(session).bridge();
}

void WatchActiveChat(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	controller->activeChatValue(
	) | rpl::map([](Dialogs::Key key) {
		return ChatIdOfPeer(key.peer());
	}) | rpl::distinct_until_changed(
	) | rpl::on_next([=](int chatId) {
		if (!Enabled()) {
			return;
		}
		auto &manager = Get(session);
		manager.setOpenChat(chatId);
		if (chatId) {
			crl::on_main(controller, [=] {
				if (ChatIdOfPeer(controller->activeChatCurrent().peer()) == chatId) {
					Get(session).bridge().promptRequest(chatId);
				}
			});
		}
	}, controller->lifetime());
}

bool IsSecretDocument(const DocumentData *document) {
	const auto &documents = Documents();
	return !documents.empty() && documents.contains(document->id);
}

QByteArray DocumentBytes(const DocumentData *document) {
	const auto &documents = Documents();
	const auto i = documents.find(document->id);
	if (i == documents.end()) {
		return QByteArray();
	}
	return Get(i->second.session).readFile(
		i->second.chatId,
		i->second.randomId);
}

QString NotificationTitle() {
	return tr::fresh_SecretChatNotifyTitle(tr::now);
}

TextWithEntities NotificationText(not_null<HistoryItem*> item) {
	return item->isService()
		? item->notificationText({})
		: TextWithEntities{ tr::fresh_SecretChatNewMessage(tr::now) };
}

UserData *RealUser(const PeerData *peer) {
	const auto chatId = ChatIdOfPeer(peer);
	return chatId ? BridgeFor(&peer->session()).realUser(chatId) : nullptr;
}

void Start(not_null<Main::Session*> session) {
	AyuSettings::getInstance().secretChatsEnabledValue(
	) | rpl::on_next([=](bool enabled) {
		if (enabled && Enabled()) {
			Get(session).bridge().refreshAll();
		} else if (!enabled) {
			Get(session).bridge().removeAll();
		}
	}, session->lifetime());
}

} // namespace FreshSecret
