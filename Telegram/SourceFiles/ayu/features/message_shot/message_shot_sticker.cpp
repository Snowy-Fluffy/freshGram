#include "ayu/features/message_shot/message_shot_sticker.h"

#include "lang_auto.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "ayu/ui/boxes/edit_mark_box.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "window/window_session_controller.h"

#include <QBuffer>

namespace AyuFeatures::MessageShot::Sticker {
namespace {

constexpr auto kSide = 512;
constexpr auto kMaxBytes = 512 * 1024;
constexpr auto kMyStickersLimit = 100;
constexpr auto kFirstQuality = 95;
constexpr auto kLastQuality = 35;
constexpr auto kQualityStep = 10;

struct Pack {
	uint64 id = 0;
	uint64 accessHash = 0;
	QString title;
};

struct UploadState {
	rpl::lifetime lifetime;
	Fn<void(std::optional<MTPInputDocument>)> done;
};

[[nodiscard]] QVector<MTPDocumentAttribute> StickerAttributes(
		QSize size,
		const QString &emoji) {
	return {
		MTP_documentAttributeImageSize(
			MTP_int(size.width()),
			MTP_int(size.height())),
		MTP_documentAttributeSticker(
			MTP_flags(0),
			MTP_string(emoji),
			MTP_inputStickerSetEmpty(),
			MTPMaskCoords()),
		MTP_documentAttributeFilename(MTP_string(u"sticker.webp"_q)),
	};
}

[[nodiscard]] std::optional<MTPInputDocument> ReadInputDocument(
		const MTPMessageMedia &media) {
	auto result = std::optional<MTPInputDocument>();
	media.match([&](const MTPDmessageMediaDocument &data) {
		const auto document = data.vdocument();
		if (!document) {
			return;
		}
		document->match([&](const MTPDdocument &data) {
			result = MTP_inputDocument(
				data.vid(),
				data.vaccess_hash(),
				data.vfile_reference());
		}, [](const auto &) {
		});
	}, [](const auto &) {
	});
	return result;
}

void UploadSticker(
		not_null<Main::Session*> session,
		const QByteArray &webp,
		const QString &emoji,
		Fn<void(std::optional<MTPInputDocument>)> done) {
	const auto size = QImage::fromData(webp).size();
	if (size.isEmpty()) {
		done(std::nullopt);
		return;
	}
	const auto id = base::RandomValue<DocumentId>();
	const auto attributes = StickerAttributes(size, emoji);
	auto ready = MakePreparedFile({
		.id = id,
		.type = SendMediaType::ThemeFile,
	});
	ready->filename = u"sticker.webp"_q;
	ready->filemime = u"image/webp"_q;
	ready->content = webp;
	ready->filesize = webp.size();
	ready->setFileData(webp);
	ready->document = MTP_document(
		MTP_flags(0),
		MTP_long(id),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(base::unixtime::now()),
		MTP_string("image/webp"),
		MTP_long(webp.size()),
		MTP_vector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(session->mainDcId()),
		MTP_vector<MTPDocumentAttribute>(attributes));

	const auto uploadId = FullMsgId(
		session->userPeerId(),
		session->data().nextLocalMessageId());
	const auto state = std::make_shared<UploadState>();
	state->done = std::move(done);

	session->uploader().documentFailed(
	) | rpl::on_next([=](const FullMsgId &failed) {
		if (failed != uploadId) {
			return;
		}
		const auto keep = state;
		auto finish = std::move(keep->done);
		keep->lifetime.destroy();
		finish(std::nullopt);
	}, state->lifetime);

	session->uploader().documentReady(
	) | rpl::on_next([=](const Storage::UploadedMedia &data) {
		if (data.fullId != uploadId) {
			return;
		}
		const auto keep = state;
		auto finish = std::move(keep->done);
		const auto api = &session->api();
		auto request = MTPmessages_UploadMedia(
			MTP_flags(0),
			MTPstring(),
			session->user()->input(),
			MTP_inputMediaUploadedDocument(
				MTP_flags(0),
				data.info.file,
				MTPInputFile(),
				MTP_string("image/webp"),
				MTP_vector<MTPDocumentAttribute>(attributes),
				MTP_vector<MTPInputDocument>(),
				MTPInputPhoto(),
				MTP_int(0),
				MTP_int(0)));
		keep->lifetime.destroy();

		api->request(
			std::move(request)
		).done([=](const MTPMessageMedia &result) {
			finish(ReadInputDocument(result));
		}).fail([=] {
			finish(std::nullopt);
		}).send();
	}, state->lifetime);

	session->uploader().upload(uploadId, ready);
}

void FetchOwnPacks(
		not_null<Main::Session*> session,
		Fn<void(std::vector<Pack>)> done,
		Fn<void()> fail) {
	session->api().request(MTPmessages_GetMyStickers(
		MTP_long(0),
		MTP_int(kMyStickersLimit)
	)).done([=](const MTPmessages_MyStickers &result) {
		auto packs = std::vector<Pack>();
		result.match([&](const MTPDmessages_myStickers &data) {
			for (const auto &covered : data.vsets().v) {
				const auto &set = covered.match([](
						const auto &data) -> const MTPStickerSet & {
					return data.vset();
				});
				set.match([&](const MTPDstickerSet &data) {
					if (data.is_masks() || data.is_emojis()) {
						return;
					}
					packs.push_back({
						.id = data.vid().v,
						.accessHash = data.vaccess_hash().v,
						.title = qs(data.vtitle()),
					});
				});
			}
		});
		done(std::move(packs));
	}).fail([=] {
		fail();
	}).send();
}

void Finish(
		not_null<Window::SessionController*> controller,
		const Fn<void(bool)> &done,
		bool success) {
	controller->showToast(success
		? tr::fresh_MessageShotStickerAdded(tr::now)
		: tr::fresh_MessageShotStickerFailed(tr::now));
	done(success);
}

void AddToExisting(
		not_null<Window::SessionController*> controller,
		const QByteArray &webp,
		const QString &emoji,
		Pack pack,
		Fn<void(bool)> done) {
	const auto session = &controller->session();
	UploadSticker(session, webp, emoji, [=](std::optional<MTPInputDocument> input) {
		if (!input) {
			Finish(controller, done, false);
			return;
		}
		session->api().request(MTPstickers_AddStickerToSet(
			MTP_inputStickerSetID(
				MTP_long(pack.id),
				MTP_long(pack.accessHash)),
			MTP_inputStickerSetItem(
				MTP_flags(0),
				*input,
				MTP_string(emoji),
				MTPMaskCoords(),
				MTPstring())
		)).done([=] {
			Finish(controller, done, true);
		}).fail([=] {
			Finish(controller, done, false);
		}).send();
	});
}

void CreatePack(
		not_null<Window::SessionController*> controller,
		const QByteArray &webp,
		const QString &emoji,
		const QString &title,
		Fn<void(bool)> done) {
	const auto session = &controller->session();
	UploadSticker(session, webp, emoji, [=](std::optional<MTPInputDocument> input) {
		if (!input) {
			Finish(controller, done, false);
			return;
		}
		const auto shortName = u"fresh_"_q
			+ QString::number(base::RandomValue<uint64>(), 16);
		session->api().request(MTPstickers_CreateStickerSet(
			MTP_flags(0),
			session->user()->inputUser(),
			MTP_string(title),
			MTP_string(shortName),
			MTPInputDocument(),
			MTP_vector<MTPInputStickerSetItem>(1, MTP_inputStickerSetItem(
				MTP_flags(0),
				*input,
				MTP_string(emoji),
				MTPMaskCoords(),
				MTPstring())),
			MTPstring()
		)).done([=] {
			Finish(controller, done, true);
		}).fail([=] {
			Finish(controller, done, false);
		}).send();
	});
}

void AskNewPackTitle(
		not_null<Window::SessionController*> controller,
		const QByteArray &webp,
		const QString &emoji,
		Fn<void(bool)> done) {
	controller->show(Box<EditMarkBox>(
		tr::fresh_MessageShotStickerPackTitle(),
		QString(),
		QString(),
		[=](const QString &title) {
			const auto trimmed = title.trimmed();
			if (trimmed.isEmpty()) {
				return;
			}
			CreatePack(controller, webp, emoji, trimmed, done);
		}), Ui::LayerOption::KeepOther);
}

} // namespace

QByteArray Encode(QImage shot) {
	if (shot.isNull()) {
		return {};
	}
	shot.setDevicePixelRatio(1.);
	auto image = shot.convertToFormat(QImage::Format_ARGB32);
	image = image.scaled(
		kSide,
		kSide,
		Qt::KeepAspectRatio,
		Qt::SmoothTransformation);
	for (auto quality = kFirstQuality;
		quality >= kLastQuality;
		quality -= kQualityStep) {
		auto bytes = QByteArray();
		auto buffer = QBuffer(&bytes);
		if (!image.save(&buffer, "WEBP", quality)) {
			return {};
		}
		if (bytes.size() <= kMaxBytes) {
			return bytes;
		}
	}
	return {};
}

void Send(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		const QByteArray &webp) {
	controller->session().api().sendFile(
		webp,
		SendMediaType::File,
		Api::SendAction(thread));
}

void AddToPack(
		not_null<Window::SessionController*> controller,
		const QByteArray &webp,
		const QString &emoji,
		Fn<void(bool)> done) {
	FetchOwnPacks(&controller->session(), [=](std::vector<Pack> packs) {
		auto options = std::vector<QString>();
		options.reserve(packs.size() + 1);
		for (const auto &pack : packs) {
			options.push_back(pack.title);
		}
		options.push_back(tr::fresh_MessageShotStickerNewPack(tr::now));
		const auto newIndex = int(packs.size());
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			SingleChoiceBox(box, {
				.title = tr::fresh_MessageShotStickerChoosePack(),
				.options = options,
				.initialSelection = newIndex,
				.callback = [=](int index) {
					if (index == newIndex) {
						AskNewPackTitle(controller, webp, emoji, done);
					} else {
						AddToExisting(controller, webp, emoji, packs[index], done);
					}
				},
			});
		}), Ui::LayerOption::KeepOther);
	}, [=] {
		Finish(controller, done, false);
	});
}

} // namespace AyuFeatures::MessageShot::Sticker
