#include "ayu/secret/secret_protocol.h"

#include "ayu/secret/secret_tl.h"

namespace AyuSecret {
namespace {

constexpr uint32_t kVector = 0x1cb5c415U;
constexpr uint32_t kMediaEmpty = 0x089f5c4aU;
constexpr uint32_t kMediaPhoto8 = 0x32798a8cU;
constexpr uint32_t kMediaPhoto = 0xf1fa8d78U;
constexpr uint32_t kMediaVideo8 = 0x4cee6ef3U;
constexpr uint32_t kMediaVideo23 = 0x524a415dU;
constexpr uint32_t kMediaVideo = 0x970c8c0eU;
constexpr uint32_t kMediaDocument8 = 0xb095434bU;
constexpr uint32_t kMediaDocument46 = 0x7afe8ae2U;
constexpr uint32_t kMediaDocument = 0x6abd9782U;
constexpr uint32_t kMediaAudio8 = 0x6080758fU;
constexpr uint32_t kMediaAudio = 0x57e0a9cbU;
constexpr uint32_t kMediaGeo = 0x35480a59U;
constexpr uint32_t kMediaContact = 0x588a0a97U;
constexpr uint32_t kMediaVenue = 0x8a0df56fU;
constexpr uint32_t kMediaWebPage = 0xe50511d8U;
constexpr uint32_t kMediaExternal = 0xfa95b0ddU;

constexpr uint32_t kAttrImageSize = 0x6c37c15cU;
constexpr uint32_t kAttrAnimated = 0x11b58939U;
constexpr uint32_t kAttrSticker23 = 0xfb0a5727U;
constexpr uint32_t kAttrSticker = 0x3a556302U;
constexpr uint32_t kAttrVideo23 = 0x5910cccbU;
constexpr uint32_t kAttrVideo = 0x0ef02ce6U;
constexpr uint32_t kAttrAudio23 = 0x051448e5U;
constexpr uint32_t kAttrAudio45 = 0xded218e0U;
constexpr uint32_t kAttrAudio = 0x9852f9c6U;
constexpr uint32_t kAttrFilename = 0x15590068U;

constexpr uint32_t kStickerSetShortName = 0x861cc8a0U;
constexpr uint32_t kStickerSetEmpty = 0xffb62b95U;

constexpr uint32_t kPhotoSizeEmpty = 0x0e17e23cU;
constexpr uint32_t kPhotoSize = 0x77bfb61bU;
constexpr uint32_t kPhotoCachedSize = 0xe9a734faU;
constexpr uint32_t kFileLocationUnavailable = 0x7c596b46U;
constexpr uint32_t kFileLocation = 0x53d69076U;

struct EntityCtor {
	uint32_t id;
	EntityType type;
};

constexpr EntityCtor kEntities[] = {
	{ 0xbb92ba95U, EntityType::Unknown },
	{ 0xfa04579dU, EntityType::Mention },
	{ 0x6f635b0dU, EntityType::Hashtag },
	{ 0x6cef8ac7U, EntityType::BotCommand },
	{ 0x6ed02538U, EntityType::Url },
	{ 0x64e475c2U, EntityType::Email },
	{ 0xbd610bc9U, EntityType::Bold },
	{ 0x826f8b60U, EntityType::Italic },
	{ 0x28a20571U, EntityType::Code },
	{ 0x73924be0U, EntityType::Pre },
	{ 0x76a6d327U, EntityType::TextUrl },
	{ 0x352dca58U, EntityType::MentionName },
	{ 0x9b69e34bU, EntityType::Phone },
	{ 0x4c4e743fU, EntityType::Cashtag },
	{ 0x761e6af4U, EntityType::BankCard },
	{ 0x9c4e7e8bU, EntityType::Underline },
	{ 0xbf0693d4U, EntityType::Strike },
	{ 0x020df5d0U, EntityType::Blockquote },
	{ 0x32ca960fU, EntityType::Spoiler },
	{ 0xc8cf05f8U, EntityType::CustomEmoji },
};

Bytes ToBytes(const std::vector<uint8_t> &raw) {
	return Bytes(raw.begin(), raw.end());
}

bool ParseEntities(Reader &r, std::vector<Entity> &result) {
	if (r.readUInt() != kVector) {
		return false;
	}
	const auto count = r.readInt();
	if (count < 0 || count > 10000) {
		return false;
	}
	for (auto i = 0; i != count; ++i) {
		const auto constructor = r.readUInt();
		auto entity = Entity();
		auto known = false;
		for (const auto &item : kEntities) {
			if (item.id == constructor) {
				entity.type = int(item.type);
				known = true;
				break;
			}
		}
		if (!known) {
			return false;
		}
		entity.offset = r.readInt();
		entity.length = r.readInt();
		switch (EntityType(entity.type)) {
		case EntityType::Pre:
			entity.extra = r.readString();
			break;
		case EntityType::TextUrl:
			entity.extra = r.readString();
			break;
		case EntityType::MentionName:
			entity.id = r.readInt();
			break;
		case EntityType::CustomEmoji:
			entity.id = r.readLong();
			break;
		default:
			break;
		}
		if (r.failed()) {
			return false;
		}
		result.push_back(std::move(entity));
	}
	return true;
}

bool SkipFileLocation(Reader &r) {
	const auto constructor = r.readUInt();
	if (constructor == kFileLocationUnavailable) {
		r.readLong();
		r.readInt();
		r.readLong();
	} else if (constructor == kFileLocation) {
		r.readInt();
		r.readLong();
		r.readInt();
		r.readLong();
	} else {
		return false;
	}
	return !r.failed();
}

bool SkipPhotoSize(Reader &r) {
	const auto constructor = r.readUInt();
	switch (constructor) {
	case kPhotoSizeEmpty:
		r.readString();
		break;
	case kPhotoSize:
		r.readString();
		if (!SkipFileLocation(r)) {
			return false;
		}
		r.readInt();
		r.readInt();
		r.readInt();
		break;
	case kPhotoCachedSize:
		r.readString();
		if (!SkipFileLocation(r)) {
			return false;
		}
		r.readInt();
		r.readInt();
		r.readBytes();
		break;
	default:
		return false;
	}
	return !r.failed();
}

bool ParseAttributes(Reader &r, Media &media) {
	if (r.readUInt() != kVector) {
		return false;
	}
	const auto count = r.readInt();
	if (count < 0 || count > 100) {
		return false;
	}
	auto voice = false;
	auto audio = false;
	auto sticker = false;
	auto video = false;
	for (auto i = 0; i != count; ++i) {
		const auto constructor = r.readUInt();
		switch (constructor) {
		case kAttrImageSize:
			media.width = r.readInt();
			media.height = r.readInt();
			break;
		case kAttrAnimated:
			media.animated = true;
			break;
		case kAttrSticker23:
			sticker = true;
			break;
		case kAttrSticker: {
			sticker = true;
			media.emoji = r.readString();
			const auto set = r.readUInt();
			if (set == kStickerSetShortName) {
				r.readString();
			} else if (set != kStickerSetEmpty) {
				return false;
			}
		} break;
		case kAttrVideo23:
			video = true;
			media.duration = r.readInt();
			media.width = r.readInt();
			media.height = r.readInt();
			break;
		case kAttrVideo: {
			video = true;
			const auto flags = r.readInt();
			media.round = (flags & 1) != 0;
			media.duration = r.readInt();
			media.width = r.readInt();
			media.height = r.readInt();
		} break;
		case kAttrAudio23:
			audio = true;
			media.duration = r.readInt();
			break;
		case kAttrAudio45:
			audio = true;
			media.duration = r.readInt();
			media.title = r.readString();
			r.readString();
			break;
		case kAttrAudio: {
			audio = true;
			const auto flags = r.readInt();
			voice = (flags & (1 << 10)) != 0;
			media.duration = r.readInt();
			if (flags & 1) {
				media.title = r.readString();
			}
			if (flags & 2) {
				r.readString();
			}
			if (flags & 4) {
				media.waveform = ToBytes(r.readBytes());
			}
		} break;
		case kAttrFilename:
			media.fileName = r.readString();
			break;
		default:
			return false;
		}
		if (r.failed()) {
			return false;
		}
	}
	if (sticker) {
		media.type = MediaType::Sticker;
	} else if (voice) {
		media.type = MediaType::Voice;
	} else if (audio) {
		media.type = MediaType::Audio;
	} else if (media.animated) {
		media.type = MediaType::Animation;
	} else if (video) {
		media.type = MediaType::Video;
	} else {
		media.type = MediaType::Document;
	}
	return true;
}

bool ParseMedia(Reader &r, Media &media) {
	const auto constructor = r.readUInt();
	if (r.failed()) {
		return false;
	}
	const auto readThumb = [&] {
		media.thumb = ToBytes(r.readBytes());
		media.thumbWidth = r.readInt();
		media.thumbHeight = r.readInt();
	};
	const auto readKey = [&] {
		media.key = ToBytes(r.readBytes());
		media.iv = ToBytes(r.readBytes());
	};
	switch (constructor) {
	case kMediaEmpty:
		media.type = MediaType::None;
		return true;
	case kMediaPhoto8:
	case kMediaPhoto:
		media.type = MediaType::Photo;
		readThumb();
		media.width = r.readInt();
		media.height = r.readInt();
		media.size = r.readInt();
		readKey();
		if (constructor == kMediaPhoto) {
			media.caption = r.readString();
		}
		return !r.failed();
	case kMediaVideo8:
	case kMediaVideo23:
	case kMediaVideo:
		media.type = MediaType::Video;
		readThumb();
		media.duration = r.readInt();
		if (constructor != kMediaVideo8) {
			media.mime = r.readString();
		}
		media.width = r.readInt();
		media.height = r.readInt();
		media.size = r.readInt();
		readKey();
		if (constructor == kMediaVideo) {
			media.caption = r.readString();
		}
		return !r.failed();
	case kMediaDocument8:
		media.type = MediaType::Document;
		readThumb();
		media.fileName = r.readString();
		media.mime = r.readString();
		media.size = r.readInt();
		readKey();
		return !r.failed();
	case kMediaDocument46:
	case kMediaDocument:
		readThumb();
		media.mime = r.readString();
		media.size = (constructor == kMediaDocument)
			? r.readLong()
			: r.readInt();
		readKey();
		if (!ParseAttributes(r, media)) {
			return false;
		}
		media.caption = r.readString();
		return !r.failed();
	case kMediaAudio8:
		media.type = MediaType::Voice;
		media.duration = r.readInt();
		media.size = r.readInt();
		readKey();
		return !r.failed();
	case kMediaAudio:
		media.type = MediaType::Voice;
		media.duration = r.readInt();
		media.mime = r.readString();
		media.size = r.readInt();
		readKey();
		return !r.failed();
	case kMediaGeo:
		media.type = MediaType::Location;
		media.latitude = r.readDouble();
		media.longitude = r.readDouble();
		return !r.failed();
	case kMediaContact:
		media.type = MediaType::Contact;
		media.phone = r.readString();
		media.firstName = r.readString();
		media.lastName = r.readString();
		media.contactUserId = r.readInt();
		return !r.failed();
	case kMediaVenue:
		media.type = MediaType::Venue;
		media.latitude = r.readDouble();
		media.longitude = r.readDouble();
		media.title = r.readString();
		media.address = r.readString();
		r.readString();
		r.readString();
		return !r.failed();
	case kMediaWebPage:
		media.type = MediaType::WebPage;
		media.url = r.readString();
		return !r.failed();
	case kMediaExternal: {
		media.type = MediaType::External;
		media.fileId = r.readLong();
		media.accessHash = r.readLong();
		r.readInt();
		media.mime = r.readString();
		media.size = r.readInt();
		if (!SkipPhotoSize(r)) {
			return false;
		}
		media.dcId = r.readInt();
		if (!ParseAttributes(r, media)) {
			return false;
		}
		return !r.failed();
	}
	default:
		return false;
	}
}

bool ParseAction(Reader &r, Inbound &result) {
	const auto constructor = r.readUInt();
	if (r.failed()) {
		return false;
	}
	const auto readIds = [&] {
		if (r.readUInt() != kVector) {
			return false;
		}
		const auto count = r.readInt();
		if (count < 0 || size_t(count) * 8 > r.left()) {
			return false;
		}
		for (auto i = 0; i != count; ++i) {
			result.ids.push_back(r.readLong());
		}
		return !r.failed();
	};
	switch (constructor) {
	case kActionNotifyLayer:
		result.action = ActionKind::NotifyLayer;
		result.actionValue = r.readInt();
		return !r.failed();
	case kActionResend:
		result.action = ActionKind::Resend;
		result.resendStart = r.readInt();
		result.resendEnd = r.readInt();
		return !r.failed();
	case kActionReadMessages:
		result.action = ActionKind::ReadMessages;
		return readIds();
	case kActionDeleteMessages:
		result.action = ActionKind::DeleteMessages;
		return readIds();
	case kActionScreenshotMessages:
		result.action = ActionKind::ScreenshotMessages;
		return readIds();
	case kActionFlushHistory:
		result.action = ActionKind::FlushHistory;
		return true;
	case kActionSetMessageTTL:
		result.action = ActionKind::SetTtl;
		result.actionValue = r.readInt();
		return !r.failed();
	case kActionTyping:
		result.action = ActionKind::Typing;
		result.actionValue = int(r.readUInt());
		return !r.failed();
	case kActionRequestKey:
		result.action = ActionKind::RequestKey;
		result.exchangeId = r.readLong();
		result.value = ToBytes(r.readBytes());
		return !r.failed();
	case kActionAcceptKey:
		result.action = ActionKind::AcceptKey;
		result.exchangeId = r.readLong();
		result.value = ToBytes(r.readBytes());
		result.fingerprint = r.readLong();
		return !r.failed();
	case kActionAbortKey:
		result.action = ActionKind::AbortKey;
		result.exchangeId = r.readLong();
		return !r.failed();
	case kActionCommitKey:
		result.action = ActionKind::CommitKey;
		result.exchangeId = r.readLong();
		result.fingerprint = r.readLong();
		return !r.failed();
	case kActionNoop:
		result.action = ActionKind::Noop;
		return true;
	default:
		result.action = ActionKind::Other;
		return true;
	}
}

bool ParseMessage(Reader &r, Inbound &result) {
	const auto constructor = r.readUInt();
	if (r.failed()) {
		return false;
	}
	switch (constructor) {
	case kDecryptedMessage:
	case kDecryptedMessage46: {
		const auto flags = r.readInt();
		result.randomId = r.readLong();
		result.ttl = r.readInt();
		result.text = r.readString();
		if (r.failed()) {
			return false;
		}
		if (flags & (1 << 9)) {
			if (!ParseMedia(r, result.media)) {
				result.media.type = MediaType::Unsupported;
				return true;
			}
		}
		if (flags & (1 << 7)) {
			if (!ParseEntities(r, result.entities)) {
				result.entities.clear();
				return true;
			}
		}
		if (flags & (1 << 11)) {
			r.readString();
		}
		if (flags & (1 << 3)) {
			result.replyTo = r.readLong();
		}
		return true;
	}
	case kDecryptedMessage23: {
		result.randomId = r.readLong();
		result.ttl = r.readInt();
		result.text = r.readString();
		if (r.failed()) {
			return false;
		}
		ParseMedia(r, result.media);
		return true;
	}
	case 0x1f814f1fU: {
		result.randomId = r.readLong();
		r.readBytes();
		result.text = r.readString();
		if (r.failed()) {
			return false;
		}
		ParseMedia(r, result.media);
		return true;
	}
	case kDecryptedMessageService:
		result.service = true;
		result.randomId = r.readLong();
		return ParseAction(r, result);
	case 0xaa48327dU:
		result.service = true;
		result.randomId = r.readLong();
		r.readBytes();
		return ParseAction(r, result);
	default:
		return false;
	}
}

void WriteEntities(Writer &w, const std::vector<Entity> &entities) {
	w.writeUInt(kVector);
	w.writeInt(int(entities.size()));
	for (const auto &entity : entities) {
		auto constructor = kEntities[0].id;
		for (const auto &item : kEntities) {
			if (int(item.type) == entity.type) {
				constructor = item.id;
				break;
			}
		}
		w.writeUInt(constructor);
		w.writeInt(entity.offset);
		w.writeInt(entity.length);
		switch (EntityType(entity.type)) {
		case EntityType::Pre:
		case EntityType::TextUrl:
			w.writeString(entity.extra);
			break;
		case EntityType::MentionName:
			w.writeInt(int(entity.id));
			break;
		case EntityType::CustomEmoji:
			w.writeLong(entity.id);
			break;
		default:
			break;
		}
	}
}

void WriteBytesField(Writer &w, const Bytes &data) {
	w.writeBytes(data.data(), data.size());
}

void WriteAttributes(Writer &w, const Media &media) {
	auto attributes = Writer();
	auto count = 0;
	if (!media.fileName.empty()) {
		attributes.writeUInt(kAttrFilename);
		attributes.writeString(media.fileName);
		++count;
	}
	const auto videoAnimation = (media.type == MediaType::Animation)
		&& (media.mime.rfind("video/", 0) == 0);
	if (media.width > 0 && media.height > 0
		&& (media.type == MediaType::Document
			|| media.type == MediaType::Sticker
			|| (media.type == MediaType::Animation && !videoAnimation))) {
		attributes.writeUInt(kAttrImageSize);
		attributes.writeInt(media.width);
		attributes.writeInt(media.height);
		++count;
	}
	if (media.type == MediaType::Sticker) {
		attributes.writeUInt(kAttrSticker);
		attributes.writeString(media.emoji);
		attributes.writeUInt(kStickerSetEmpty);
		++count;
	}
	if (media.animated || media.type == MediaType::Animation) {
		attributes.writeUInt(kAttrAnimated);
		++count;
	}
	if (media.type == MediaType::Video || videoAnimation) {
		attributes.writeUInt(kAttrVideo);
		attributes.writeInt(media.round ? 1 : 0);
		attributes.writeInt(media.duration);
		attributes.writeInt(media.width);
		attributes.writeInt(media.height);
		++count;
	}
	if (media.type == MediaType::Voice || media.type == MediaType::Audio) {
		attributes.writeUInt(kAttrAudio);
		const auto voice = (media.type == MediaType::Voice);
		const auto flags = (voice ? (1 << 10) : 0)
			| (media.waveform.empty() ? 0 : 4);
		attributes.writeInt(flags);
		attributes.writeInt(media.duration);
		if (!media.waveform.empty()) {
			WriteBytesField(attributes, media.waveform);
		}
		++count;
	}
	w.writeUInt(kVector);
	w.writeInt(count);
	w.writeRaw(attributes.data());
}

void WriteMedia(Writer &w, const Media &media) {
	switch (media.type) {
	case MediaType::Photo:
		w.writeUInt(kMediaPhoto);
		WriteBytesField(w, media.thumb);
		w.writeInt(media.thumbWidth);
		w.writeInt(media.thumbHeight);
		w.writeInt(media.width);
		w.writeInt(media.height);
		w.writeInt(int(media.size));
		WriteBytesField(w, media.key);
		WriteBytesField(w, media.iv);
		w.writeString(media.caption);
		break;
	case MediaType::Location:
		w.writeUInt(kMediaGeo);
		w.writeDouble(media.latitude);
		w.writeDouble(media.longitude);
		break;
	case MediaType::Contact:
		w.writeUInt(kMediaContact);
		w.writeString(media.phone);
		w.writeString(media.firstName);
		w.writeString(media.lastName);
		w.writeInt(int(media.contactUserId));
		break;
	case MediaType::WebPage:
		w.writeUInt(kMediaWebPage);
		w.writeString(media.url);
		break;
	default:
		w.writeUInt(kMediaDocument46);
		WriteBytesField(w, media.thumb);
		w.writeInt(media.thumbWidth);
		w.writeInt(media.thumbHeight);
		w.writeString(media.mime);
		w.writeInt(int(media.size));
		WriteBytesField(w, media.key);
		WriteBytesField(w, media.iv);
		WriteAttributes(w, media);
		w.writeString(media.caption);
		break;
	}
}

Bytes ServiceMessage(int64_t randomId, const Writer &action) {
	auto writer = Writer();
	writer.writeUInt(kDecryptedMessageService);
	writer.writeLong(randomId);
	writer.writeRaw(action.data());
	return writer.data();
}

Bytes IdsAction(
		uint32_t constructor,
		int64_t randomId,
		const std::vector<int64_t> &ids) {
	auto action = Writer();
	action.writeUInt(constructor);
	action.writeUInt(kVector);
	action.writeInt(int32_t(ids.size()));
	for (const auto id : ids) {
		action.writeLong(id);
	}
	return ServiceMessage(randomId, action);
}

} // namespace

bool ParseLayerObject(const Bytes &object, Inbound &result) {
	auto reader = Reader(object.data(), object.size());
	if (reader.readUInt() != kDecryptedMessageLayer) {
		return false;
	}
	reader.readBytes();
	result.layer = reader.readInt();
	result.inSeqNo = reader.readInt();
	result.outSeqNo = reader.readInt();
	if (reader.failed()) {
		return false;
	}
	return ParseMessage(reader, result);
}

Bytes BuildLayerObject(const Bytes &message, int inSeqNo, int outSeqNo) {
	auto writer = Writer();
	writer.writeUInt(kDecryptedMessageLayer);
	const auto random = RandomVector(31);
	writer.writeBytes(random.data(), random.size());
	writer.writeInt(kLayer);
	writer.writeInt(inSeqNo);
	writer.writeInt(outSeqNo);
	writer.writeRaw(message);
	return writer.data();
}

Bytes BuildMessage(const MessageData &data) {
	auto flags = 0;
	const auto hasMedia = (data.media.type != MediaType::None);
	if (hasMedia) {
		flags |= (1 << 9);
	}
	if (!data.entities.empty()) {
		flags |= (1 << 7);
	}
	if (data.replyTo) {
		flags |= (1 << 3);
	}
	auto writer = Writer();
	writer.writeUInt(kDecryptedMessage);
	writer.writeInt(flags);
	writer.writeLong(data.randomId);
	writer.writeInt(data.ttl);
	writer.writeString(data.text);
	if (hasMedia) {
		WriteMedia(writer, data.media);
	}
	if (!data.entities.empty()) {
		WriteEntities(writer, data.entities);
	}
	if (data.replyTo) {
		writer.writeLong(data.replyTo);
	}
	return writer.data();
}

Bytes BuildNotifyLayer(int64_t randomId, int layer) {
	auto action = Writer();
	action.writeUInt(kActionNotifyLayer);
	action.writeInt(layer);
	return ServiceMessage(randomId, action);
}

Bytes BuildResend(int64_t randomId, int start, int end) {
	auto action = Writer();
	action.writeUInt(kActionResend);
	action.writeInt(start);
	action.writeInt(end);
	return ServiceMessage(randomId, action);
}

Bytes BuildNoop(int64_t randomId) {
	auto action = Writer();
	action.writeUInt(kActionNoop);
	return ServiceMessage(randomId, action);
}

Bytes BuildFlushHistory(int64_t randomId) {
	auto action = Writer();
	action.writeUInt(kActionFlushHistory);
	return ServiceMessage(randomId, action);
}

Bytes BuildSetTtl(int64_t randomId, int ttl) {
	auto action = Writer();
	action.writeUInt(kActionSetMessageTTL);
	action.writeInt(ttl);
	return ServiceMessage(randomId, action);
}

Bytes BuildReadMessages(int64_t randomId, const std::vector<int64_t> &ids) {
	return IdsAction(kActionReadMessages, randomId, ids);
}

Bytes BuildDeleteMessages(int64_t randomId, const std::vector<int64_t> &ids) {
	return IdsAction(kActionDeleteMessages, randomId, ids);
}

Bytes BuildScreenshot(int64_t randomId, const std::vector<int64_t> &ids) {
	return IdsAction(kActionScreenshotMessages, randomId, ids);
}

Bytes BuildRequestKey(
		int64_t randomId,
		int64_t exchangeId,
		const Bytes &gA) {
	auto action = Writer();
	action.writeUInt(kActionRequestKey);
	action.writeLong(exchangeId);
	WriteBytesField(action, gA);
	return ServiceMessage(randomId, action);
}

Bytes BuildAcceptKey(
		int64_t randomId,
		int64_t exchangeId,
		const Bytes &gB,
		int64_t fingerprint) {
	auto action = Writer();
	action.writeUInt(kActionAcceptKey);
	action.writeLong(exchangeId);
	WriteBytesField(action, gB);
	action.writeLong(fingerprint);
	return ServiceMessage(randomId, action);
}

Bytes BuildCommitKey(
		int64_t randomId,
		int64_t exchangeId,
		int64_t fingerprint) {
	auto action = Writer();
	action.writeUInt(kActionCommitKey);
	action.writeLong(exchangeId);
	action.writeLong(fingerprint);
	return ServiceMessage(randomId, action);
}

Bytes BuildAbortKey(int64_t randomId, int64_t exchangeId) {
	auto action = Writer();
	action.writeUInt(kActionAbortKey);
	action.writeLong(exchangeId);
	return ServiceMessage(randomId, action);
}

} // namespace AyuSecret
