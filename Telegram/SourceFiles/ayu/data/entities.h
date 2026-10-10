// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <string>

using ID = long long;

class AyuMessageBase
{
public:
	ID fakeId;
	ID userId;
	ID dialogId;
	ID groupedId;
	ID peerId;
	ID fromId;
	ID topicId;
	int messageId;
	int date;
	int flags;
	int editDate;
	int views;
	int fwdFlags;
	ID fwdFromId;
	std::string fwdName;
	int fwdDate;
	std::string fwdPostAuthor;
	std::string postAuthor;
	int replyFlags;
	int replyMessageId;
	ID replyPeerId;
	int replyTopId;
	bool replyForumTopic;
	std::vector<char> replySerialized;
	std::vector<char> replyMarkupSerialized;
	int entityCreateDate;
	std::string text;
	std::vector<char> textEntities;
	std::string mediaPath;
	std::string hqThumbPath;
	int documentType;
	std::vector<char> documentSerialized;
	std::vector<char> thumbsSerialized;
	std::vector<char> documentAttributesSerialized;
	std::string mimeType;
};

class DeletedMessage : public AyuMessageBase
{
};

class EditedMessage : public AyuMessageBase
{
};

// Reactions and comments of a deleted message, they are kept apart from
// the main row, so that the table of deleted messages stays compatible.
class DeletedExtra
{
public:
	ID fakeId;
	ID userId;
	ID dialogId;
	int messageId;
	std::vector<char> reactions;
	int repliesCount;
	ID commentsChannelId;
	int commentsRootId;
	int commentsReadTill;
	int commentsMaxId;
	std::string repliers;
	int entityCreateDate;
};

// Inline buttons of a deleted message, kept in their own table.
class DeletedMarkup
{
public:
	ID fakeId;
	ID userId;
	ID dialogId;
	int messageId;
	std::vector<char> markup;
	int entityCreateDate;
};

class DeletedDialog
{
public:
	ID fakeId;
	ID userId;
	ID dialogId;
	ID peerId;
	std::unique_ptr<int> folderId; // nullable
	int topMessage;
	int lastMessageDate;
	int flags;
	int entityCreateDate;
};

class KeptDialog
{
public:
	ID fakeId;
	ID userId;
	ID dialogId;
	int kind;
	std::string title;
	std::string username;
	ID accessHash;
	int folderId;
	int lastMessageDate;
	int lost;
};

class KeptTopic
{
public:
	ID fakeId;
	ID userId;
	ID dialogId;
	ID rootId;
	std::string title;
	int colorId;
	ID iconId;
	ID creatorId;
	int date;
	int flags;
};

class KnownUser
{
public:
	ID fakeId;
	ID userId;
	ID peerId;
	ID accessHash;
	std::string firstName;
	std::string lastName;
	std::string username;
	int updatedAt;
};

class SecretChatRow
{
public:
	ID fakeId;
	ID userId;
	int chatId;
	ID accessHash;
	ID peerUserId;
	int creator;
	int state;
	std::vector<char> keyData;
	ID fingerprint;
	int myIn;
	int myOut;
	int hisIn;
	int hisLayer;
	int date;
	int lastDate;
	int unread;
};

class SecretMessageRow
{
public:
	ID fakeId;
	ID userId;
	int chatId;
	ID randomId;
	int outgoing;
	int date;
	int kind;
	int seqIn;
	int seqOut;
	std::string text;
	std::vector<char> payload;
};

class PeekedStatusRow
{
public:
	ID fakeId;
	ID userId;
	ID targetId;
	int kind;
	int time;
	int checkedAt;
};

class PeekRestoreRow
{
public:
	ID fakeId;
	ID userId;
	int option;
	int flags;
	std::string always;
	std::string never;
};

class SecretStateRow
{
public:
	ID fakeId;
	ID userId;
	int qts;
	int date;
};

class RegexFilter
{
public:
	std::vector<char> id;
	std::string text;
	bool enabled;
	bool reversed;
	bool caseInsensitive;
	std::optional<ID> dialogId; // nullable

	bool operator==(const RegexFilter &other) const {
		return id == other.id &&
			text == other.text &&
			caseInsensitive == other.caseInsensitive &&
			reversed == other.reversed &&
			dialogId == other.dialogId &&
			enabled == other.enabled;
	}
	[[nodiscard]] QJsonObject toJson() const {
		QJsonObject json;
		json["id"] = QString::fromUtf8(id.data());
		json["text"] = QString::fromStdString(text);
		json["enabled"] = enabled;
		json["reversed"] = reversed;
		json["caseInsensitive"] = caseInsensitive;
		if (dialogId.has_value()) {
			json["dialogId"] = dialogId.value();
		}
		return json;
	}
};

class RegexFilterGlobalExclusion
{
public:
	ID fakeId;
	ID dialogId;
	std::vector<char> filterId;

	bool operator==(const RegexFilterGlobalExclusion& other) const {
		return dialogId == other.dialogId && filterId == other.filterId;
	}
};

class SpyMessageRead
{
public:
	ID fakeId;
	ID userId;
	ID dialogId;
	int messageId;
	int entityCreateDate;
};

class SpyMessageContentsRead
{
public:
	ID fakeId;
	ID userId;
	ID dialogId;
	int messageId;
	int entityCreateDate;
};
