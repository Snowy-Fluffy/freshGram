// This is the source code of freshGram.
#include "ayu/data/fresh_migration_v4.h"

#include "ayu/data/message_rows.h"
#include "ayu/libs/sqlite/sqlite3.h"

#include <chrono>
#include <set>
#include <string>
#include <vector>

namespace FreshMigrationV4 {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::string Quote(const std::string &name) {
	return "\"" + name + "\"";
}

class Statement
{
public:
	Statement(sqlite3 *db, const std::string &sql) {
		_code = sqlite3_prepare_v2(db, sql.c_str(), -1, &_statement, nullptr);
		if (_code != SQLITE_OK) {
			_error = sqlite3_errmsg(db);
		}
	}
	~Statement() {
		sqlite3_finalize(_statement);
	}
	Statement(const Statement &) = delete;
	Statement &operator=(const Statement &) = delete;

	[[nodiscard]] bool ok() const {
		return _code == SQLITE_OK;
	}
	[[nodiscard]] const std::string &error() const {
		return _error;
	}
	[[nodiscard]] sqlite3_stmt *get() const {
		return _statement;
	}

private:
	sqlite3_stmt *_statement = nullptr;
	int _code = SQLITE_OK;
	std::string _error;
};

[[nodiscard]] bool Exec(sqlite3 *db, const std::string &sql, std::string &error) {
	char *message = nullptr;
	if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
		error = message ? message : sqlite3_errmsg(db);
		sqlite3_free(message);
		return false;
	}
	return true;
}

[[nodiscard]] long long Scalar(sqlite3 *db, const std::string &sql, std::string &error) {
	const auto statement = Statement(db, sql);
	if (!statement.ok()) {
		error = statement.error();
		return -1;
	}
	if (sqlite3_step(statement.get()) != SQLITE_ROW) {
		error = sqlite3_errmsg(db);
		return -1;
	}
	return sqlite3_column_int64(statement.get(), 0);
}

[[nodiscard]] bool TableExists(sqlite3 *db, const std::string &schema, const std::string &name) {
	auto error = std::string();
	return Scalar(
		db,
		"SELECT count(*) FROM " + schema + ".sqlite_master WHERE type = 'table' AND name = '" + name + "'",
		error) == 1;
}

class Columns
{
public:
	Columns(
		sqlite3 *db,
		const std::string &schema,
		const std::string &table,
		const std::string &prefix = std::string())
	: _prefix(prefix) {
		const auto statement = Statement(db, "PRAGMA " + schema + ".table_info(" + Quote(table) + ")");
		while (statement.ok() && sqlite3_step(statement.get()) == SQLITE_ROW) {
			_names.insert(reinterpret_cast<const char*>(
				sqlite3_column_text(statement.get(), 1)));
		}
	}

	[[nodiscard]] bool has(const std::string &name) const {
		return _names.contains(name);
	}
	[[nodiscard]] std::string number(const std::string &name) const {
		return has(name) ? "COALESCE(" + _prefix + Quote(name) + ", 0)" : "0";
	}
	[[nodiscard]] std::string text(const std::string &name) const {
		return has(name) ? "COALESCE(" + _prefix + Quote(name) + ", '')" : "''";
	}
	[[nodiscard]] std::string blob(const std::string &name) const {
		return has(name) ? "COALESCE(" + _prefix + Quote(name) + ", X'')" : "X''";
	}

private:
	std::string _prefix;
	std::set<std::string> _names;
};

// The columns that stay in the compact row, in the order of the target.
[[nodiscard]] std::string CommonSelect(const Columns &c) {
	return c.number("topicId")
		+ ", " + c.number("fromId")
		+ ", " + c.number("groupedId")
		+ ", " + c.number("date")
		+ ", " + c.number("flags")
		+ ", CASE WHEN " + c.number("editDate") + " = " + c.number("entityCreateDate")
			+ " THEN 0 ELSE " + c.number("editDate") + " END"
		+ ", " + c.number("views")
		+ ", " + c.number("replyMessageId")
		+ ", " + c.number("replyTopId")
		+ ", " + c.number("replyForumTopic")
		+ ", " + c.number("entityCreateDate")
		+ ", " + c.text("text")
		+ ", " + c.blob("textEntities")
		+ ", " + c.text("mediaPath")
		+ ", " + c.number("documentType")
		+ ", " + c.blob("documentSerialized")
		+ ", " + c.text("postAuthor")
		+ ", X''";
}

constexpr auto kCommonTarget =
	"topicId, fromId, groupedId, date, flags, editDate, views, "
	"replyMessageId, replyTopId, replyForumTopic, entityCreateDate, "
	"text, textEntities, mediaPath, documentType, documentSerialized, "
	"postAuthor, extra";

// The columns that the application leaves empty, the row needs extra if any is set.
[[nodiscard]] std::string RareSelect(const Columns &c) {
	return (c.has("peerId") ? c.number("peerId") : "abs(" + c.number("dialogId") + ")")
		+ ", " + c.number("fwdFlags")
		+ ", " + c.number("fwdFromId")
		+ ", " + c.text("fwdName")
		+ ", " + c.number("fwdDate")
		+ ", " + c.text("fwdPostAuthor")
		+ ", " + c.number("replyFlags")
		+ ", " + c.number("replyPeerId")
		+ ", " + c.blob("replySerialized")
		+ ", " + c.blob("replyMarkupSerialized")
		+ ", " + c.text("hqThumbPath")
		+ ", " + c.blob("thumbsSerialized")
		+ ", " + c.blob("documentAttributesSerialized")
		+ ", " + c.text("mimeType");
}

[[nodiscard]] std::string RareCondition(const Columns &c) {
	auto condition = std::string("(");
	const auto add = [&](const std::string &part) {
		if (condition.size() > 1) {
			condition += " OR ";
		}
		condition += part;
	};
	if (c.has("peerId")) {
		add(c.number("peerId") + " != abs(" + c.number("dialogId") + ")");
	}
	for (const auto name : {"fwdFlags", "fwdFromId", "fwdDate", "replyFlags", "replyPeerId"}) {
		if (c.has(name)) {
			add(c.number(name) + " != 0");
		}
	}
	for (const auto name : {"fwdName", "fwdPostAuthor", "hqThumbPath", "mimeType"}) {
		if (c.has(name)) {
			add(c.text(name) + " != ''");
		}
	}
	for (const auto name : {
		"replySerialized",
		"replyMarkupSerialized",
		"thumbsSerialized",
		"documentAttributesSerialized",
	}) {
		if (c.has(name)) {
			add("length(" + c.blob(name) + ") > 0");
		}
	}
	return (condition.size() > 1) ? condition + ")" : "0";
}

[[nodiscard]] std::vector<char> Blob(sqlite3_stmt *statement, int column) {
	const auto data = static_cast<const char*>(sqlite3_column_blob(statement, column));
	const auto size = sqlite3_column_bytes(statement, column);
	return data ? std::vector<char>(data, data + size) : std::vector<char>();
}

[[nodiscard]] std::string Text(sqlite3_stmt *statement, int column) {
	const auto data = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
	const auto size = sqlite3_column_bytes(statement, column);
	return data ? std::string(data, size) : std::string();
}

// Reads the rare columns, which start at the given index of the statement.
[[nodiscard]] FreshMessageRows::Extra ReadExtra(sqlite3_stmt *statement, int from, long long dialogId) {
	auto extra = FreshMessageRows::Extra();
	const auto peerId = sqlite3_column_int64(statement, from + 0);
	const auto absolute = dialogId < 0 ? -dialogId : dialogId;
	if (peerId != absolute) {
		extra.peerId = peerId ? peerId : -1;
	}
	extra.fwdFlags = sqlite3_column_int(statement, from + 1);
	extra.fwdFromId = sqlite3_column_int64(statement, from + 2);
	extra.fwdName = Text(statement, from + 3);
	extra.fwdDate = sqlite3_column_int(statement, from + 4);
	extra.fwdPostAuthor = Text(statement, from + 5);
	extra.replyFlags = sqlite3_column_int(statement, from + 6);
	extra.replyPeerId = sqlite3_column_int64(statement, from + 7);
	extra.replySerialized = Blob(statement, from + 8);
	extra.replyMarkupSerialized = Blob(statement, from + 9);
	extra.hqThumbPath = Text(statement, from + 10);
	extra.thumbsSerialized = Blob(statement, from + 11);
	extra.documentAttributesSerialized = Blob(statement, from + 12);
	extra.mimeType = Text(statement, from + 13);
	return extra;
}

[[nodiscard]] bool WriteExtras(
		sqlite3 *db,
		const std::string &select,
		const std::string &update,
		bool withRevision,
		std::string &error) {
	const auto read = Statement(db, select);
	if (!read.ok()) {
		error = read.error();
		return false;
	}
	const auto write = Statement(db, update);
	if (!write.ok()) {
		error = write.error();
		return false;
	}
	while (sqlite3_step(read.get()) == SQLITE_ROW) {
		const auto userId = sqlite3_column_int64(read.get(), 0);
		const auto dialogId = sqlite3_column_int64(read.get(), 1);
		const auto messageId = sqlite3_column_int64(read.get(), 2);
		const auto revision = sqlite3_column_int64(read.get(), 3);
		const auto extra = FreshMessageRows::EncodeExtra(
			ReadExtra(read.get(), 4, dialogId));
		if (extra.empty()) {
			continue;
		}
		sqlite3_reset(write.get());
		sqlite3_bind_blob(write.get(), 1, extra.data(), int(extra.size()), SQLITE_TRANSIENT);
		sqlite3_bind_int64(write.get(), 2, userId);
		sqlite3_bind_int64(write.get(), 3, dialogId);
		sqlite3_bind_int64(write.get(), 4, messageId);
		if (withRevision) {
			sqlite3_bind_int64(write.get(), 5, revision - 1);
		}
		if (sqlite3_step(write.get()) != SQLITE_DONE) {
			error = sqlite3_errmsg(db);
			return false;
		}
	}
	return true;
}

// Writes the rare columns of the rows into extra, the rows come from the select.
[[nodiscard]] std::string Source(const std::string &schema, const std::string &table) {
	return schema + "." + Quote(table);
}

[[nodiscard]] bool MigrateDeleted(
		sqlite3 *db,
		const std::string &schema,
		long long &count,
		std::string &error) {
	const auto c = Columns(db, schema, "DeletedMessage", "d.");
	const auto source = Source(schema, "DeletedMessage");
	// The last copy of every message and its place in the order of the key.
	if (!Exec(
		db,
		"CREATE TEMP TABLE fresh_keep_deleted (userId INTEGER, dialogId INTEGER, messageId INTEGER, "
		"fid INTEGER, PRIMARY KEY (userId, dialogId, messageId)) WITHOUT ROWID",
		error)
		|| !Exec(
			db,
			"INSERT INTO fresh_keep_deleted SELECT " + c.number("userId") + ", " + c.number("dialogId")
				+ ", " + c.number("messageId") + ", max(d.fakeId) FROM " + source
				+ " d GROUP BY 1, 2, 3",
			error)) {
		return false;
	}
	if (!Exec(
		db,
		std::string("INSERT INTO main.DeletedMessageV2 (userId, dialogId, messageId, ")
			+ kCommonTarget + ") SELECT "
			+ c.number("userId") + ", " + c.number("dialogId") + ", " + c.number("messageId")
			+ ", " + CommonSelect(c)
			+ " FROM temp.fresh_keep_deleted k JOIN " + source + " d ON d.fakeId = k.fid"
			+ " ORDER BY k.userId, k.dialogId, k.messageId",
		error)) {
		return false;
	}
	const auto condition = RareCondition(c);
	if (condition != "0") {
		const auto select = "SELECT d.userId, d.dialogId, d.messageId, 0, "
			+ RareSelect(c)
			+ " FROM " + source + " d WHERE " + condition
			+ " AND d.fakeId IN (SELECT fid FROM temp.fresh_keep_deleted)";
		if (!WriteExtras(
			db,
			select,
			"UPDATE main.DeletedMessageV2 SET extra = ?1 WHERE userId = ?2 AND dialogId = ?3 AND messageId = ?4",
			false,
			error)) {
			return false;
		}
	}
	const auto expected = Scalar(db, "SELECT count(*) FROM temp.fresh_keep_deleted", error);
	count = Scalar(db, "SELECT count(*) FROM main.DeletedMessageV2", error);
	if (!Exec(db, "DROP TABLE temp.fresh_keep_deleted", error)) {
		return false;
	}
	if (expected < 0 || count != expected) {
		error = "The deleted messages do not match: " + std::to_string(count)
			+ " of " + std::to_string(expected);
		return false;
	}
	return true;
}

[[nodiscard]] bool MigrateEdited(
		sqlite3 *db,
		const std::string &schema,
		long long &count,
		std::string &error) {
	const auto c = Columns(db, schema, "EditedMessage");
	const auto d = Columns(db, schema, "EditedMessage", "d.");
	const auto source = Source(schema, "EditedMessage");
	// The revisions in the order of the message and of the old identifier.
	if (!Exec(
		db,
		"CREATE TEMP TABLE fresh_keep_edited (userId INTEGER, dialogId INTEGER, messageId INTEGER, "
		"fid INTEGER, PRIMARY KEY (userId, dialogId, messageId, fid)) WITHOUT ROWID",
		error)
		|| !Exec(
			db,
			"INSERT INTO fresh_keep_edited SELECT " + d.number("userId") + ", " + d.number("dialogId")
				+ ", " + d.number("messageId") + ", d.fakeId FROM " + source + " d",
			error)) {
		return false;
	}
	if (!Exec(
		db,
		std::string("INSERT INTO main.EditedMessageV2 (userId, dialogId, messageId, ")
			+ kCommonTarget + ") SELECT "
			+ d.number("userId") + ", " + d.number("dialogId") + ", " + d.number("messageId")
			+ ", " + CommonSelect(d)
			+ " FROM temp.fresh_keep_edited k JOIN " + source + " d ON d.fakeId = k.fid"
			+ " ORDER BY k.userId, k.dialogId, k.messageId, k.fid",
		error)) {
		return false;
	}
	const auto condition = RareCondition(c);
	if (condition != "0") {
		const auto revision = std::string(
			"row_number() OVER (PARTITION BY userId, dialogId, messageId ORDER BY fakeId)");
		const auto select = "SELECT userId, dialogId, messageId, revision, "
			"peerId, fwdFlags, fwdFromId, fwdName, fwdDate, fwdPostAuthor, replyFlags, "
			"replyPeerId, replySerialized, replyMarkupSerialized, hqThumbPath, "
			"thumbsSerialized, documentAttributesSerialized, mimeType FROM (SELECT "
			+ c.number("userId") + " AS userId, " + c.number("dialogId") + " AS dialogId, "
			+ c.number("messageId") + " AS messageId, " + revision + " AS revision, "
			+ (c.has("peerId") ? c.number("peerId") : "abs(" + c.number("dialogId") + ")") + " AS peerId, "
			+ c.number("fwdFlags") + " AS fwdFlags, " + c.number("fwdFromId") + " AS fwdFromId, "
			+ c.text("fwdName") + " AS fwdName, " + c.number("fwdDate") + " AS fwdDate, "
			+ c.text("fwdPostAuthor") + " AS fwdPostAuthor, " + c.number("replyFlags") + " AS replyFlags, "
			+ c.number("replyPeerId") + " AS replyPeerId, " + c.blob("replySerialized") + " AS replySerialized, "
			+ c.blob("replyMarkupSerialized") + " AS replyMarkupSerialized, "
			+ c.text("hqThumbPath") + " AS hqThumbPath, " + c.blob("thumbsSerialized") + " AS thumbsSerialized, "
			+ c.blob("documentAttributesSerialized") + " AS documentAttributesSerialized, "
			+ c.text("mimeType") + " AS mimeType, " + condition + " AS rare FROM " + source + ") WHERE rare = 1";
		if (!WriteExtras(
			db,
			select,
			"UPDATE main.EditedMessageV2 SET extra = ?1 WHERE fakeId = (SELECT fakeId FROM main.EditedMessageV2 "
			"WHERE userId = ?2 AND dialogId = ?3 AND messageId = ?4 ORDER BY fakeId LIMIT 1 OFFSET ?5)",
			true,
			error)) {
			return false;
		}
	}
	const auto expected = Scalar(db, "SELECT count(*) FROM temp.fresh_keep_edited", error);
	count = Scalar(db, "SELECT count(*) FROM main.EditedMessageV2", error);
	if (!Exec(db, "DROP TABLE temp.fresh_keep_edited", error)) {
		return false;
	}
	if (expected < 0 || count != expected) {
		error = "The edited messages do not match: " + std::to_string(count)
			+ " of " + std::to_string(expected);
		return false;
	}
	return true;
}

} // namespace

Result Run(sqlite3 *db, const char *schemaName) {
	const auto started = Clock::now();
	const auto schema = std::string(schemaName ? schemaName : "main");
	auto result = Result();
	const auto hasDeleted = TableExists(db, schema, "DeletedMessage");
	const auto hasEdited = TableExists(db, schema, "EditedMessage");
	if (!hasDeleted && !hasEdited) {
		result.ok = true;
		return result;
	}
	if (hasDeleted && !MigrateDeleted(db, schema, result.deleted, result.error)) {
		return result;
	}
	if (hasEdited && !MigrateEdited(db, schema, result.edited, result.error)) {
		return result;
	}
	if (schema == "main") {
		if (hasDeleted && !Exec(db, "DROP TABLE main.DeletedMessage", result.error)) {
			return result;
		}
		if (hasEdited && !Exec(db, "DROP TABLE main.EditedMessage", result.error)) {
			return result;
		}
	}
	result.ok = true;
	result.migrated = true;
	result.milliseconds = std::chrono::duration<double, std::milli>(
		Clock::now() - started).count();
	return result;
}

} // namespace FreshMigrationV4
