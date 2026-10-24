// This is the source code of freshGram.
#include "ayu/data/message_rows.h"

#include "ayu/utils/fresh_storage_migration.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>

namespace FreshMessageRows {
namespace {

constexpr auto kExtraVersion = char(1);

enum Bit : uint32_t {
	kPeerId = 1U << 0,
	kFwdFlags = 1U << 1,
	kFwdFromId = 1U << 2,
	kFwdName = 1U << 3,
	kFwdDate = 1U << 4,
	kFwdPostAuthor = 1U << 5,
	kReplyFlags = 1U << 6,
	kReplyPeerId = 1U << 7,
	kReplySerialized = 1U << 8,
	kReplyMarkupSerialized = 1U << 9,
	kHqThumbPath = 1U << 10,
	kThumbsSerialized = 1U << 11,
	kDocumentAttributesSerialized = 1U << 12,
	kMimeType = 1U << 13,
	kKnown = (1U << 14) - 1,
};

class Writer
{
public:
	void number(int64_t value) {
		for (auto i = 0; i != 8; ++i) {
			_bytes.push_back(char((uint64_t(value) >> (8 * i)) & 0xFF));
		}
	}
	void bytes(const char *data, size_t size) {
		for (auto i = 0; i != 4; ++i) {
			_bytes.push_back(char((uint32_t(size) >> (8 * i)) & 0xFF));
		}
		_bytes.insert(_bytes.end(), data, data + size);
	}
	void text(const std::string &value) {
		bytes(value.data(), value.size());
	}
	void blob(const std::vector<char> &value) {
		bytes(value.data(), value.size());
	}
	std::vector<char> take() {
		return std::move(_bytes);
	}
	void raw(char value) {
		_bytes.push_back(value);
	}

private:
	std::vector<char> _bytes;
};

class Reader
{
public:
	explicit Reader(const std::vector<char> &bytes)
	: _data(bytes.data())
	, _size(bytes.size()) {
	}
	bool number(int64_t &value) {
		if (_size - _position < 8) {
			return false;
		}
		auto result = uint64_t(0);
		for (auto i = 0; i != 8; ++i) {
			result |= uint64_t(uint8_t(_data[_position + i])) << (8 * i);
		}
		_position += 8;
		value = int64_t(result);
		return true;
	}
	bool bytes(std::vector<char> &value) {
		if (_size - _position < 4) {
			return false;
		}
		auto size = uint32_t(0);
		for (auto i = 0; i != 4; ++i) {
			size |= uint32_t(uint8_t(_data[_position + i])) << (8 * i);
		}
		_position += 4;
		if (_size - _position < size) {
			return false;
		}
		value.assign(_data + _position, _data + _position + size);
		_position += size;
		return true;
	}
	bool text(std::string &value) {
		auto buffer = std::vector<char>();
		if (!bytes(buffer)) {
			return false;
		}
		value.assign(buffer.begin(), buffer.end());
		return true;
	}
	bool raw(char &value) {
		if (_position >= _size) {
			return false;
		}
		value = _data[_position++];
		return true;
	}

private:
	const char *_data = nullptr;
	size_t _size = 0;
	size_t _position = 0;
};

[[nodiscard]] ID AbsoluteDialog(ID dialogId) {
	return dialogId < 0 ? -dialogId : dialogId;
}

[[nodiscard]] Extra ExtraOf(const AyuMessageBase &message) {
	auto result = Extra();
	if (message.peerId != AbsoluteDialog(message.dialogId)) {
		result.peerId = message.peerId ? message.peerId : -1;
	}
	result.fwdFlags = message.fwdFlags;
	result.fwdFromId = message.fwdFromId;
	result.fwdName = message.fwdName;
	result.fwdDate = message.fwdDate;
	result.fwdPostAuthor = message.fwdPostAuthor;
	result.replyFlags = message.replyFlags;
	result.replyPeerId = message.replyPeerId;
	result.replySerialized = message.replySerialized;
	result.replyMarkupSerialized = message.replyMarkupSerialized;
	result.hqThumbPath = message.hqThumbPath;
	result.thumbsSerialized = message.thumbsSerialized;
	result.documentAttributesSerialized = message.documentAttributesSerialized;
	result.mimeType = message.mimeType;
	return result;
}

void ApplyExtra(const Extra &extra, AyuMessageBase &message) {
	if (extra.peerId) {
		message.peerId = (extra.peerId == -1) ? 0 : extra.peerId;
	}
	message.fwdFlags = extra.fwdFlags;
	message.fwdFromId = extra.fwdFromId;
	message.fwdName = extra.fwdName;
	message.fwdDate = extra.fwdDate;
	message.fwdPostAuthor = extra.fwdPostAuthor;
	message.replyFlags = extra.replyFlags;
	message.replyPeerId = extra.replyPeerId;
	message.replySerialized = extra.replySerialized;
	message.replyMarkupSerialized = extra.replyMarkupSerialized;
	message.hqThumbPath = extra.hqThumbPath;
	message.thumbsSerialized = extra.thumbsSerialized;
	message.documentAttributesSerialized = extra.documentAttributesSerialized;
	message.mimeType = extra.mimeType;
}

template<typename Row>
void FillRow(const AyuMessageBase &message, Row &row) {
	row.fakeId = message.fakeId;
	row.userId = message.userId;
	row.dialogId = message.dialogId;
	row.messageId = message.messageId;
	row.topicId = message.topicId;
	row.fromId = message.fromId;
	row.groupedId = message.groupedId;
	row.date = message.date;
	row.flags = message.flags;
	row.editDate = (message.editDate == message.entityCreateDate)
		? 0
		: message.editDate;
	row.views = message.views;
	row.replyMessageId = message.replyMessageId;
	row.replyTopId = message.replyTopId;
	row.replyForumTopic = message.replyForumTopic;
	row.entityCreateDate = message.entityCreateDate;
	row.text = message.text;
	row.textEntities = message.textEntities;
	row.mediaPath = message.mediaPath;
	row.documentType = message.documentType;
	row.documentSerialized = message.documentSerialized;
	row.postAuthor = message.postAuthor;
	row.extra = EncodeExtra(ExtraOf(message));
}

template<typename Row>
void FillMessage(const Row &row, AyuMessageBase &message) {
	message.fakeId = row.fakeId;
	message.userId = row.userId;
	message.dialogId = row.dialogId;
	message.groupedId = row.groupedId;
	message.peerId = AbsoluteDialog(row.dialogId);
	message.fromId = row.fromId;
	message.topicId = row.topicId;
	message.messageId = row.messageId;
	message.date = row.date;
	message.flags = row.flags;
	message.editDate = row.editDate ? row.editDate : row.entityCreateDate;
	message.views = row.views;
	message.fwdFlags = 0;
	message.fwdFromId = 0;
	message.fwdDate = 0;
	message.replyFlags = 0;
	message.replyMessageId = row.replyMessageId;
	message.replyPeerId = 0;
	message.replyTopId = row.replyTopId;
	message.replyForumTopic = row.replyForumTopic;
	message.entityCreateDate = row.entityCreateDate;
	message.text = row.text;
	message.textEntities = row.textEntities;
	message.mediaPath = FreshStorage::CurrentPath(row.mediaPath);
	message.documentType = row.documentType;
	message.documentSerialized = row.documentSerialized;
	message.postAuthor = row.postAuthor;
	if (!row.extra.empty()) {
		auto extra = Extra();
		if (DecodeExtra(row.extra, extra)) {
			ApplyExtra(extra, message);
		}
	}
}

} // namespace

std::vector<char> EncodeExtra(const Extra &extra) {
	auto mask = uint32_t(0);
	const auto note = [&](bool set, uint32_t bit) {
		if (set) {
			mask |= bit;
		}
	};
	note(extra.peerId != 0, kPeerId);
	note(extra.fwdFlags != 0, kFwdFlags);
	note(extra.fwdFromId != 0, kFwdFromId);
	note(!extra.fwdName.empty(), kFwdName);
	note(extra.fwdDate != 0, kFwdDate);
	note(!extra.fwdPostAuthor.empty(), kFwdPostAuthor);
	note(extra.replyFlags != 0, kReplyFlags);
	note(extra.replyPeerId != 0, kReplyPeerId);
	note(!extra.replySerialized.empty(), kReplySerialized);
	note(!extra.replyMarkupSerialized.empty(), kReplyMarkupSerialized);
	note(!extra.hqThumbPath.empty(), kHqThumbPath);
	note(!extra.thumbsSerialized.empty(), kThumbsSerialized);
	note(!extra.documentAttributesSerialized.empty(), kDocumentAttributesSerialized);
	note(!extra.mimeType.empty(), kMimeType);
	if (!mask) {
		return {};
	}
	auto writer = Writer();
	writer.raw(kExtraVersion);
	writer.number(mask);
	if (mask & kPeerId) writer.number(extra.peerId);
	if (mask & kFwdFlags) writer.number(extra.fwdFlags);
	if (mask & kFwdFromId) writer.number(extra.fwdFromId);
	if (mask & kFwdName) writer.text(extra.fwdName);
	if (mask & kFwdDate) writer.number(extra.fwdDate);
	if (mask & kFwdPostAuthor) writer.text(extra.fwdPostAuthor);
	if (mask & kReplyFlags) writer.number(extra.replyFlags);
	if (mask & kReplyPeerId) writer.number(extra.replyPeerId);
	if (mask & kReplySerialized) writer.blob(extra.replySerialized);
	if (mask & kReplyMarkupSerialized) writer.blob(extra.replyMarkupSerialized);
	if (mask & kHqThumbPath) writer.text(extra.hqThumbPath);
	if (mask & kThumbsSerialized) writer.blob(extra.thumbsSerialized);
	if (mask & kDocumentAttributesSerialized) writer.blob(extra.documentAttributesSerialized);
	if (mask & kMimeType) writer.text(extra.mimeType);
	return writer.take();
}

bool DecodeExtra(const std::vector<char> &bytes, Extra &extra) {
	extra = Extra();
	if (bytes.empty()) {
		return true;
	}
	auto reader = Reader(bytes);
	auto version = char(0);
	auto mask64 = int64_t(0);
	if (!reader.raw(version) || version != kExtraVersion || !reader.number(mask64)) {
		return false;
	}
	const auto mask = uint32_t(mask64);
	if (mask & ~uint32_t(kKnown)) {
		return false;
	}
	auto number = int64_t(0);
	const auto readInt = [&](uint32_t bit, auto &target) {
		if (!(mask & bit)) {
			return true;
		}
		if (!reader.number(number)) {
			return false;
		}
		target = static_cast<std::remove_reference_t<decltype(target)>>(number);
		return true;
	};
	const auto readText = [&](uint32_t bit, std::string &target) {
		return !(mask & bit) || reader.text(target);
	};
	const auto readBlob = [&](uint32_t bit, std::vector<char> &target) {
		return !(mask & bit) || reader.bytes(target);
	};
	return readInt(kPeerId, extra.peerId)
		&& readInt(kFwdFlags, extra.fwdFlags)
		&& readInt(kFwdFromId, extra.fwdFromId)
		&& readText(kFwdName, extra.fwdName)
		&& readInt(kFwdDate, extra.fwdDate)
		&& readText(kFwdPostAuthor, extra.fwdPostAuthor)
		&& readInt(kReplyFlags, extra.replyFlags)
		&& readInt(kReplyPeerId, extra.replyPeerId)
		&& readBlob(kReplySerialized, extra.replySerialized)
		&& readBlob(kReplyMarkupSerialized, extra.replyMarkupSerialized)
		&& readText(kHqThumbPath, extra.hqThumbPath)
		&& readBlob(kThumbsSerialized, extra.thumbsSerialized)
		&& readBlob(kDocumentAttributesSerialized, extra.documentAttributesSerialized)
		&& readText(kMimeType, extra.mimeType);
}

DeletedMessageRow ToRow(const DeletedMessage &message) {
	auto row = DeletedMessageRow();
	FillRow(message, row);
	return row;
}

DeletedMessage FromRow(const DeletedMessageRow &row) {
	auto message = DeletedMessage();
	FillMessage(row, message);
	return message;
}

EditedMessageRow ToRow(const EditedMessage &message) {
	auto row = EditedMessageRow();
	FillRow(message, row);
	return row;
}

EditedMessage FromRow(const EditedMessageRow &row) {
	auto message = EditedMessage();
	FillMessage(row, message);
	return message;
}

} // namespace FreshMessageRows
