// This is the source code of freshGram.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ayu/data/message_types.h"

// The stored form of a deleted or edited message: the fields that are
// always derivable or never set are not columns, the rare ones go to extra.
// The members are declared in each class, so that the mapping of the tables
// can refer to them through the class that owns the table.
#define FRESH_MESSAGE_ROW_FIELDS \
	ID fakeId; \
	ID userId; \
	ID dialogId; \
	int messageId; \
	ID topicId; \
	ID fromId; \
	ID groupedId; \
	int date; \
	int flags; \
	/* Zero when the message was not edited, the creation time otherwise. */ \
	int editDate; \
	int views; \
	int replyMessageId; \
	int replyTopId; \
	bool replyForumTopic; \
	int entityCreateDate; \
	std::string text; \
	std::vector<char> textEntities; \
	std::string mediaPath; \
	int documentType; \
	std::vector<char> documentSerialized; \
	std::string postAuthor; \
	std::vector<char> extra;

class DeletedMessageRow
{
public:
	FRESH_MESSAGE_ROW_FIELDS
};

class EditedMessageRow
{
public:
	FRESH_MESSAGE_ROW_FIELDS
};

namespace FreshMessageRows {

// The columns that are empty in all the rows written by the application.
struct Extra
{
	// Zero stands for the absolute value of the dialog id.
	ID peerId = 0;
	int fwdFlags = 0;
	ID fwdFromId = 0;
	std::string fwdName;
	int fwdDate = 0;
	std::string fwdPostAuthor;
	int replyFlags = 0;
	ID replyPeerId = 0;
	std::vector<char> replySerialized;
	std::vector<char> replyMarkupSerialized;
	std::string hqThumbPath;
	std::vector<char> thumbsSerialized;
	std::vector<char> documentAttributesSerialized;
	std::string mimeType;
};

// Empty when every field has its default value.
[[nodiscard]] std::vector<char> EncodeExtra(const Extra &extra);
[[nodiscard]] bool DecodeExtra(const std::vector<char> &bytes, Extra &extra);

[[nodiscard]] DeletedMessageRow ToRow(const DeletedMessage &message);
[[nodiscard]] DeletedMessage FromRow(const DeletedMessageRow &row);
[[nodiscard]] EditedMessageRow ToRow(const EditedMessage &message);
[[nodiscard]] EditedMessage FromRow(const EditedMessageRow &row);

} // namespace FreshMessageRows
