#include "ayu/secret/secret_send.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "ayu/secret/secret_bridge.h"
#include "ayu/secret/secret_peer.h"
#include "ayu/secret/secret_policy.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtGui/QImage>

namespace FreshSecret {
namespace {

struct Target {
	int chatId = 0;
	Main::Session *session = nullptr;
	int64_t replyTo = 0;
};

[[nodiscard]] std::optional<Target> Resolve(const Api::SendAction &action) {
	const auto peer = action.history->peer;
	if (!IsSecretPeer(peer)) {
		return std::nullopt;
	}
	auto result = Target();
	result.chatId = ChatIdOfPeer(peer);
	result.session = &peer->session();
	if (action.replyTo.messageId) {
		result.replyTo = BridgeFor(result.session).randomIdOf(
			result.chatId,
			action.replyTo.messageId);
	}
	return result;
}

[[nodiscard]] bool Ready(const Target &target) {
	if (!Enabled()) {
		Ui::Toast::Show(tr::fresh_SecretChatsNeedPasscode(tr::now));
		return false;
	}
	const auto info = Get(target.session).chat(target.chatId);
	if (!info || info->state != ChatState::Ready) {
		Ui::Toast::Show(tr::fresh_SecretChatNotReady(tr::now));
		return false;
	}
	return true;
}

void Announce(const Api::SendAction &action) {
	action.history->session().api().sendAction(action);
}

void ObtainBytes(
		not_null<DocumentData*> document,
		Fn<void(QByteArray)> done) {
	const auto media = document->createMediaView();
	const auto origin = document->stickerOrGifOrigin();
	const auto read = [=] {
		auto bytes = media->bytes();
		if (bytes.isEmpty()) {
			const auto path = document->filepath(true);
			if (!path.isEmpty()) {
				auto file = QFile(path);
				if (file.open(QIODevice::ReadOnly)) {
					bytes = file.readAll();
				}
			}
		}
		return bytes;
	};
	if (auto bytes = read(); !bytes.isEmpty()) {
		done(std::move(bytes));
		return;
	}
	if (!document->saveToCache()) {
		Ui::Toast::Show(tr::fresh_SecretNotSupported(tr::now));
		return;
	}
	document->save(origin, QString(), LoadFromCloudOrLocal, true);
	const auto session = &document->session();
	session->downloaderTaskFinished(
	) | rpl::filter([=] {
		return !read().isEmpty();
	}) | rpl::take(1) | rpl::on_next([=] {
		done(read());
	}, session->lifetime());
}

[[nodiscard]] QByteArray PreparedImageBytes(
		const Ui::PreparedFile &file,
		QString &name) {
	if (!file.information) {
		return QByteArray();
	}
	const auto image = std::get_if<Ui::PreparedFileInformation::Image>(
		&file.information->media);
	if (!image) {
		return QByteArray();
	} else if (!image->bytes.isEmpty()) {
		return image->bytes;
	} else if (image->data.isNull()) {
		return QByteArray();
	}
	const auto png = image->data.hasAlphaChannel();
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	buffer.open(QIODevice::WriteOnly);
	image->data.save(&buffer, png ? "PNG" : "JPEG", 94);
	name = png ? u"photo.png"_q : u"photo.jpg"_q;
	return result;
}

[[nodiscard]] TextWithEntities ToEntities(const TextWithTags &tags) {
	return TextWithEntities{
		tags.text,
		TextUtilities::ConvertTextTagsToEntities(tags.tags),
	};
}

} // namespace

bool Reject(not_null<PeerData*> peer) {
	if (!IsSecretPeer(peer)) {
		return false;
	}
	Ui::Toast::Show(tr::fresh_SecretNotSupported(tr::now));
	return true;
}

bool SendDocument(
		const Api::MessageToSend &message,
		not_null<DocumentData*> document) {
	const auto target = Resolve(message.action);
	if (!target) {
		return false;
	}
	if (!Ready(*target)) {
		return true;
	}
	Announce(message.action);
	auto outgoing = OutgoingFile();
	outgoing.replyTo = target->replyTo;
	outgoing.width = document->dimensions.width();
	outgoing.height = document->dimensions.height();
	if (const auto sticker = document->sticker()) {
		outgoing.kind = MediaType::Sticker;
		outgoing.emoji = sticker->alt;
		if (sticker->isLottie()) {
			outgoing.mime = u"application/x-tgsticker"_q;
			outgoing.name = u"sticker.tgs"_q;
		} else if (sticker->isWebm()) {
			outgoing.mime = u"video/webm"_q;
			outgoing.name = u"sticker.webm"_q;
		} else {
			outgoing.mime = u"image/webp"_q;
			outgoing.name = u"sticker.webp"_q;
		}
	} else if (document->isAnimation() && !document->isVideoMessage()) {
		outgoing.kind = MediaType::Animation;
		outgoing.mime = document->mimeString();
		outgoing.name = document->filename();
		outgoing.duration = int(std::max<crl::time>(
			1,
			document->duration() / 1000));
	} else {
		Ui::Toast::Show(tr::fresh_SecretNotSupported(tr::now));
		return true;
	}
	const auto chatId = target->chatId;
	const auto session = target->session;
	ObtainBytes(document, [=](QByteArray bytes) mutable {
		outgoing.bytes = std::move(bytes);
		Get(session).sendFile(chatId, std::move(outgoing));
	});
	return true;
}

bool SendText(const Api::MessageToSend &message) {
	const auto target = Resolve(message.action);
	if (!target) {
		return false;
	}
	if (!Ready(*target)) {
		return true;
	}
	auto text = ToEntities(message.textWithTags);
	if (text.text.trimmed().isEmpty()) {
		return true;
	}
	Announce(message.action);
	Get(target->session).sendText(
		target->chatId,
		std::move(text),
		target->replyTo);
	return true;
}

bool SendFiles(Ui::PreparedList &list, const Api::SendAction &action) {
	const auto target = Resolve(action);
	if (!target) {
		return false;
	}
	if (!Ready(*target)) {
		return true;
	}
	Announce(action);
	auto replyTo = target->replyTo;
	for (auto &file : list.files) {
		auto outgoing = OutgoingFile();
		outgoing.path = file.path;
		outgoing.name = file.displayName;
		if (file.path.isEmpty()) {
			outgoing.bytes = file.content;
			if (outgoing.bytes.isEmpty()) {
				outgoing.bytes = PreparedImageBytes(file, outgoing.name);
			}
		}
		outgoing.caption = file.caption.text;
		outgoing.replyTo = replyTo;
		replyTo = 0;
		Get(target->session).sendFile(target->chatId, std::move(outgoing));
	}
	return true;
}

bool SendBytes(const QByteArray &bytes, const Api::SendAction &action) {
	const auto target = Resolve(action);
	if (!target) {
		return false;
	}
	if (!Ready(*target)) {
		return true;
	}
	Announce(action);
	auto outgoing = OutgoingFile();
	outgoing.bytes = bytes;
	outgoing.replyTo = target->replyTo;
	Get(target->session).sendFile(target->chatId, std::move(outgoing));
	return true;
}

bool SendVoice(
		const QByteArray &bytes,
		const VoiceWaveform &waveform,
		crl::time duration,
		bool video,
		const Api::SendAction &action) {
	const auto target = Resolve(action);
	if (!target) {
		return false;
	}
	if (!Ready(*target)) {
		return true;
	}
	Announce(action);
	auto outgoing = OutgoingFile();
	outgoing.bytes = bytes;
	outgoing.replyTo = target->replyTo;
	outgoing.duration = int(std::max<crl::time>(1, duration / 1000));
	if (video) {
		outgoing.kind = MediaType::Video;
		outgoing.mime = "video/mp4";
		outgoing.name = "video_message.mp4";
		outgoing.round = true;
		outgoing.width = 384;
		outgoing.height = 384;
	} else {
		outgoing.kind = MediaType::Voice;
		outgoing.mime = "audio/ogg";
		outgoing.name = "voice.ogg";
		outgoing.waveform = documentWaveformEncode5bit(waveform);
	}
	Get(target->session).sendFile(target->chatId, std::move(outgoing));
	return true;
}

bool Forward(
		const std::vector<not_null<HistoryItem*>> &items,
		std::vector<not_null<Data::Thread*>> &threads,
		const Api::SendOptions &options) {
	if (items.empty()) {
		return false;
	}
	auto fromSecret = false;
	for (const auto &item : items) {
		fromSecret = fromSecret || item->isFreshSecret();
	}
	auto handled = std::vector<not_null<Data::Thread*>>();
	for (const auto &thread : threads) {
		if (fromSecret || IsSecretPeer(thread->peer())) {
			handled.push_back(thread);
		}
	}
	if (handled.empty()) {
		return false;
	}
	const auto session = &items.front()->history()->session();
	for (const auto &thread : handled) {
		const auto peer = thread->peer();
		const auto toSecret = IsSecretPeer(peer);
		auto action = Api::SendAction(thread, options);
		const auto target = toSecret ? Resolve(action) : std::nullopt;
		if (toSecret && (!target || !Ready(*target))) {
			continue;
		}
		for (const auto &item : items) {
			const auto source = item->isFreshSecret()
				? BridgeFor(session).lookup(item)
				: std::nullopt;
			const auto &text = item->originalText();
			const auto hasMedia = (item->media() != nullptr)
				&& (item->media()->document() || item->media()->photo());
			auto bytes = QByteArray();
			auto name = QString();
			auto mime = QString();
			if (source && hasMedia) {
				auto &manager = Get(session);
				bytes = manager.readFile(source->first, source->second);
				if (const auto data = manager.message(
						source->first,
						source->second)) {
					name = QString::fromStdString(data->media.fileName);
					mime = QString::fromStdString(data->media.mime);
				}
			}
			if (toSecret) {
				if (!bytes.isEmpty()) {
					auto outgoing = OutgoingFile();
					outgoing.bytes = bytes;
					outgoing.name = name;
					outgoing.mime = mime;
					outgoing.caption = text.text;
					Get(target->session).sendFile(
						target->chatId,
						std::move(outgoing));
				} else if (!text.empty()) {
					Get(target->session).sendText(
						target->chatId,
						text,
						0);
				} else if (hasMedia) {
					Ui::Toast::Show(tr::fresh_SecretNotSupported(tr::now));
				}
			} else {
				auto &api = session->api();
				if (!bytes.isEmpty()) {
					api.sendFile(bytes, SendMediaType::File, action);
				}
				if (!text.empty()) {
					auto message = Api::MessageToSend(action);
					message.textWithTags = TextWithTags{
						text.text,
						TextUtilities::ConvertEntitiesToTextTags(
							text.entities),
					};
					api.sendMessage(std::move(message));
				}
			}
		}
	}
	for (const auto &thread : handled) {
		threads.erase(
			std::remove(threads.begin(), threads.end(), thread),
			threads.end());
	}
	return threads.empty();
}

} // namespace FreshSecret
