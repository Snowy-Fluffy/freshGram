#pragma once

#include "ayu/secret/secret_crypto.h"

#include <string>
#include <vector>

namespace FreshSecret {

enum class MediaType : int {
	None = 0,
	Photo = 1,
	Video = 2,
	Document = 3,
	Voice = 4,
	Audio = 5,
	Sticker = 6,
	Animation = 7,
	Location = 8,
	Contact = 9,
	Venue = 10,
	WebPage = 11,
	External = 12,
	Unsupported = 13,
};

enum class EntityType : int {
	Unknown = 0,
	Mention = 1,
	Hashtag = 2,
	BotCommand = 3,
	Url = 4,
	Email = 5,
	Bold = 6,
	Italic = 7,
	Code = 8,
	Pre = 9,
	TextUrl = 10,
	MentionName = 11,
	Phone = 12,
	Cashtag = 13,
	BankCard = 14,
	Underline = 15,
	Strike = 16,
	Blockquote = 17,
	Spoiler = 18,
	CustomEmoji = 19,
};

enum class DeliveryState : int {
	Sent = 0,
	Pending = 1,
	Read = 2,
	Failed = 3,
};

struct Entity {
	int type = 0;
	int offset = 0;
	int length = 0;
	std::string extra;
	int64_t id = 0;
};

struct Media {
	MediaType type = MediaType::None;
	int64_t fileId = 0;
	int64_t accessHash = 0;
	int dcId = 0;
	int64_t size = 0;
	Bytes key;
	Bytes iv;
	int width = 0;
	int height = 0;
	int duration = 0;
	std::string mime;
	std::string fileName;
	std::string caption;
	Bytes thumb;
	int thumbWidth = 0;
	int thumbHeight = 0;
	double latitude = 0;
	double longitude = 0;
	std::string title;
	std::string address;
	std::string phone;
	std::string firstName;
	std::string lastName;
	std::string url;
	std::string emoji;
	int64_t contactUserId = 0;
	Bytes waveform;
	bool round = false;
	bool animated = false;
	std::string path;
};

inline constexpr auto kSpecialEnded = 3;
inline constexpr auto kSpecialRequest = 4;

struct MessageData {
	int64_t randomId = 0;
	bool outgoing = false;
	int date = 0;
	int ttl = 0;
	int expiresAt = 0;
	DeliveryState state = DeliveryState::Sent;
	bool opened = false;
	bool deleted = false;
	int special = 0;
	std::string text;
	std::vector<Entity> entities;
	int64_t replyTo = 0;
	Media media;
	Bytes object;
	int seqIn = 0;
	int seqOut = 0;
};

[[nodiscard]] Bytes SerializeMeta(const MessageData &data);
[[nodiscard]] bool ParseMeta(const Bytes &meta, MessageData &data);

} // namespace FreshSecret
