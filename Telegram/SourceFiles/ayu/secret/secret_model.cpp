#include "ayu/secret/secret_model.h"

#include "ayu/secret/secret_tl.h"
#include "ayu/utils/fresh_storage_migration.h"

namespace FreshSecret {
namespace {

constexpr uint32_t kMagic = 0x31535941U;

void WriteBytes(Writer &w, const Bytes &data) {
	w.writeBytes(data.data(), data.size());
}

Bytes ReadBytes(Reader &r) {
	const auto raw = r.readBytes();
	return Bytes(raw.begin(), raw.end());
}

void WriteMedia(Writer &w, const Media &m) {
	w.writeInt(int(m.type));
	w.writeLong(m.fileId);
	w.writeLong(m.accessHash);
	w.writeInt(m.dcId);
	w.writeLong(m.size);
	WriteBytes(w, m.key);
	WriteBytes(w, m.iv);
	w.writeInt(m.width);
	w.writeInt(m.height);
	w.writeInt(m.duration);
	w.writeString(m.mime);
	w.writeString(m.fileName);
	w.writeString(m.caption);
	WriteBytes(w, m.thumb);
	w.writeInt(m.thumbWidth);
	w.writeInt(m.thumbHeight);
	w.writeDouble(m.latitude);
	w.writeDouble(m.longitude);
	w.writeString(m.title);
	w.writeString(m.address);
	w.writeString(m.phone);
	w.writeString(m.firstName);
	w.writeString(m.lastName);
	w.writeString(m.url);
	w.writeString(m.emoji);
	w.writeLong(m.contactUserId);
	WriteBytes(w, m.waveform);
	w.writeInt((m.round ? 1 : 0) | (m.animated ? 2 : 0));
	w.writeString(m.path);
}

bool ReadMedia(Reader &r, Media &m) {
	m.type = MediaType(r.readInt());
	m.fileId = r.readLong();
	m.accessHash = r.readLong();
	m.dcId = r.readInt();
	m.size = r.readLong();
	m.key = ReadBytes(r);
	m.iv = ReadBytes(r);
	m.width = r.readInt();
	m.height = r.readInt();
	m.duration = r.readInt();
	m.mime = r.readString();
	m.fileName = r.readString();
	m.caption = r.readString();
	m.thumb = ReadBytes(r);
	m.thumbWidth = r.readInt();
	m.thumbHeight = r.readInt();
	m.latitude = r.readDouble();
	m.longitude = r.readDouble();
	m.title = r.readString();
	m.address = r.readString();
	m.phone = r.readString();
	m.firstName = r.readString();
	m.lastName = r.readString();
	m.url = r.readString();
	m.emoji = r.readString();
	m.contactUserId = r.readLong();
	m.waveform = ReadBytes(r);
	const auto flags = r.readInt();
	m.round = (flags & 1) != 0;
	m.animated = (flags & 2) != 0;
	m.path = FreshStorage::CurrentPath(r.readString());
	return !r.failed();
}

} // namespace

Bytes SerializeMeta(const MessageData &data) {
	auto w = Writer();
	w.writeUInt(kMagic);
	w.writeInt(data.ttl);
	w.writeInt(data.expiresAt);
	w.writeInt(int(data.state));
	w.writeInt((data.opened ? 1 : 0)
		| ((data.special & 0x7FFF) << 1)
		| (data.deleted ? (1 << 16) : 0));
	w.writeLong(data.replyTo);
	w.writeInt(int(data.entities.size()));
	for (const auto &entity : data.entities) {
		w.writeInt(entity.type);
		w.writeInt(entity.offset);
		w.writeInt(entity.length);
		w.writeString(entity.extra);
		w.writeLong(entity.id);
	}
	WriteMedia(w, data.media);
	WriteBytes(w, data.object);
	return w.data();
}

bool ParseMeta(const Bytes &meta, MessageData &data) {
	if (meta.size() < 4) {
		return false;
	}
	auto r = Reader(meta.data(), meta.size());
	if (r.readUInt() != kMagic) {
		data.object = meta;
		return true;
	}
	data.ttl = r.readInt();
	data.expiresAt = r.readInt();
	data.state = DeliveryState(r.readInt());
	const auto flags = r.readInt();
	data.opened = (flags & 1) != 0;
	data.special = (flags >> 1) & 0x7FFF;
	data.deleted = (flags & (1 << 16)) != 0;
	data.replyTo = r.readLong();
	const auto count = r.readInt();
	if (count < 0 || count > 10000) {
		return false;
	}
	data.entities.clear();
	for (auto i = 0; i != count; ++i) {
		auto entity = Entity();
		entity.type = r.readInt();
		entity.offset = r.readInt();
		entity.length = r.readInt();
		entity.extra = r.readString();
		entity.id = r.readLong();
		data.entities.push_back(std::move(entity));
	}
	if (!ReadMedia(r, data.media)) {
		return false;
	}
	data.object = ReadBytes(r);
	return !r.failed();
}

} // namespace FreshSecret
