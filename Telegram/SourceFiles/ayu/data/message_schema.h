// This is the source code of freshGram.
#pragma once

#include "ayu/data/message_rows.h"
#include "ayu/libs/sqlite/sqlite_orm.h"

namespace FreshMessageSchema {

// The indexes are listed before the tables in the storage, as the other ones.
inline auto DeletedKeyIndex() {
	using namespace sqlite_orm;
	using Row = DeletedMessageRow;
	return make_index(
		"idx_deleted_message_v2_key",
		column<Row>(&Row::userId),
		column<Row>(&Row::dialogId),
		column<Row>(&Row::messageId));
}

inline auto DeletedTopicIndex() {
	using namespace sqlite_orm;
	using Row = DeletedMessageRow;
	return make_index(
		"idx_deleted_message_v2_topic",
		column<Row>(&Row::userId),
		column<Row>(&Row::dialogId),
		column<Row>(&Row::topicId),
		column<Row>(&Row::messageId));
}

inline auto EditedKeyIndex() {
	using namespace sqlite_orm;
	using Row = EditedMessageRow;
	return make_index(
		"idx_edited_message_v2_key",
		column<Row>(&Row::userId),
		column<Row>(&Row::dialogId),
		column<Row>(&Row::messageId));
}

inline auto DeletedTable() {
	using namespace sqlite_orm;
	using Row = DeletedMessageRow;
	return make_table<Row>(
		"DeletedMessageV2",
		make_column("fakeId", &Row::fakeId, primary_key().autoincrement()),
		make_column("userId", &Row::userId),
		make_column("dialogId", &Row::dialogId),
		make_column("messageId", &Row::messageId),
		make_column("topicId", &Row::topicId),
		make_column("fromId", &Row::fromId),
		make_column("groupedId", &Row::groupedId),
		make_column("date", &Row::date),
		make_column("flags", &Row::flags),
		make_column("editDate", &Row::editDate),
		make_column("views", &Row::views),
		make_column("replyMessageId", &Row::replyMessageId),
		make_column("replyTopId", &Row::replyTopId),
		make_column("replyForumTopic", &Row::replyForumTopic),
		make_column("entityCreateDate", &Row::entityCreateDate),
		make_column("text", &Row::text),
		make_column("textEntities", &Row::textEntities),
		make_column("mediaPath", &Row::mediaPath),
		make_column("documentType", &Row::documentType),
		make_column("documentSerialized", &Row::documentSerialized),
		make_column("postAuthor", &Row::postAuthor),
		make_column("extra", &Row::extra));
}

inline auto EditedTable() {
	using namespace sqlite_orm;
	using Row = EditedMessageRow;
	return make_table<Row>(
		"EditedMessageV2",
		make_column("fakeId", &Row::fakeId, primary_key().autoincrement()),
		make_column("userId", &Row::userId),
		make_column("dialogId", &Row::dialogId),
		make_column("messageId", &Row::messageId),
		make_column("topicId", &Row::topicId),
		make_column("fromId", &Row::fromId),
		make_column("groupedId", &Row::groupedId),
		make_column("date", &Row::date),
		make_column("flags", &Row::flags),
		make_column("editDate", &Row::editDate),
		make_column("views", &Row::views),
		make_column("replyMessageId", &Row::replyMessageId),
		make_column("replyTopId", &Row::replyTopId),
		make_column("replyForumTopic", &Row::replyForumTopic),
		make_column("entityCreateDate", &Row::entityCreateDate),
		make_column("text", &Row::text),
		make_column("textEntities", &Row::textEntities),
		make_column("mediaPath", &Row::mediaPath),
		make_column("documentType", &Row::documentType),
		make_column("documentSerialized", &Row::documentSerialized),
		make_column("postAuthor", &Row::postAuthor),
		make_column("extra", &Row::extra));
}

} // namespace FreshMessageSchema
