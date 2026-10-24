// This is the source code of freshGram.
#include "ayu/data/fresh_legacy_copy.h"

#include "ayu/data/fresh_migration_v4.h"
#include "ayu/libs/sqlite/sqlite3.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <string>
#include <vector>

namespace FreshLegacyCopy {
namespace {

using Clock = std::chrono::steady_clock;

struct Column {
	std::string name;
	std::string type;
	bool notNull = false;
	bool primaryKey = false;
};

[[nodiscard]] std::string Quote(const std::string &name) {
	return "\"" + name + "\"";
}

[[nodiscard]] std::vector<Column> ColumnsOf(
		sqlite3 *db,
		const std::string &schema,
		const std::string &table) {
	auto result = std::vector<Column>();
	sqlite3_stmt *statement = nullptr;
	const auto sql = "PRAGMA " + schema + ".table_info(" + Quote(table) + ")";
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
		return result;
	}
	while (sqlite3_step(statement) == SQLITE_ROW) {
		auto column = Column();
		column.name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
		const auto type = sqlite3_column_text(statement, 2);
		column.type = type ? reinterpret_cast<const char*>(type) : "";
		column.notNull = sqlite3_column_int(statement, 3) != 0;
		column.primaryKey = sqlite3_column_int(statement, 5) != 0;
		result.push_back(std::move(column));
	}
	sqlite3_finalize(statement);
	return result;
}

[[nodiscard]] std::vector<std::string> TablesOf(sqlite3 *db, const std::string &schema) {
	auto result = std::vector<std::string>();
	sqlite3_stmt *statement = nullptr;
	const auto sql = "SELECT name FROM " + schema + ".sqlite_master WHERE type = 'table' "
		"AND name NOT LIKE 'sqlite_%' ORDER BY name";
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
		return result;
	}
	while (sqlite3_step(statement) == SQLITE_ROW) {
		result.push_back(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
	}
	sqlite3_finalize(statement);
	return result;
}

// What a column that is required gets when the old row has nothing for it.
[[nodiscard]] std::string DefaultFor(const std::string &declared) {
	auto type = declared;
	std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
		return char(std::toupper(c));
	});
	if (type.find("BLOB") != std::string::npos) {
		return "X''";
	} else if (type.find("CHAR") != std::string::npos
		|| type.find("TEXT") != std::string::npos
		|| type.find("CLOB") != std::string::npos) {
		return "''";
	}
	return "0";
}

} // namespace

Result Run(sqlite3 *db, const char *schemaName) {
	const auto started = Clock::now();
	const auto schema = std::string(schemaName);
	auto result = Result();
	const auto messages = FreshMigrationV4::Run(db, schemaName);
	if (!messages.ok) {
		result.error = messages.error;
		return result;
	}
	result.deleted = messages.deleted;
	result.edited = messages.edited;
	const auto skipped = std::set<std::string>{
		"DeletedMessage",
		"EditedMessage",
		"SchemaVersion",
	};
	for (const auto &table : TablesOf(db, schema)) {
		if (skipped.contains(table)) {
			continue;
		}
		const auto target = ColumnsOf(db, "main", table);
		if (target.empty()) {
			continue;
		}
		const auto source = ColumnsOf(db, schema, table);
		auto has = std::set<std::string>();
		for (const auto &column : source) {
			has.insert(column.name);
		}
		auto names = std::string();
		auto values = std::string();
		for (const auto &column : target) {
			const auto name = Quote(column.name);
			auto value = std::string();
			if (has.contains(column.name)) {
				value = (column.notNull && !column.primaryKey)
					? "COALESCE(" + name + ", " + DefaultFor(column.type) + ")"
					: name;
			} else if (column.notNull && !column.primaryKey) {
				value = DefaultFor(column.type);
			} else {
				value = "NULL";
			}
			names += (names.empty() ? "" : ", ") + name;
			values += (values.empty() ? "" : ", ") + value;
		}
		const auto sql = "INSERT INTO main." + Quote(table) + " (" + names + ") SELECT " + values
			+ " FROM " + schema + "." + Quote(table);
		char *error = nullptr;
		if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
			result.error = "Failed to copy " + table + ": " + (error ? error : sqlite3_errmsg(db));
			sqlite3_free(error);
			return result;
		}
		result.others += sqlite3_changes64(db);
	}
	result.ok = true;
	result.milliseconds = std::chrono::duration<double, std::milli>(
		Clock::now() - started).count();
	return result;
}

} // namespace FreshLegacyCopy
