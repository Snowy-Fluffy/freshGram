#include "fgpl/database.h"

#include "fgpl/broker.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
#include <cstring>
#include <system_error>

namespace fgpl {
namespace {

constexpr auto kPageSize = 4096;
constexpr auto kMaxBlobLength = 1 << 20;
constexpr auto kMaxColumns = 64;
constexpr auto kMaxStatementMemory = int64_t(64) << 20;
constexpr auto kProgressOps = 100;

struct Deadline {
	std::chrono::steady_clock::time_point end;
	const std::atomic<bool> *stop = nullptr;
	int64_t memory = 0;
};

[[nodiscard]] bool Same(const char *a, const char *b) {
	return a && b && sqlite3_stricmp(a, b) == 0;
}

[[nodiscard]] bool AllowedPragma(const char *name) {
	static constexpr const char *kAllowed[] = {
		"user_version", "table_info", "table_xinfo", "table_list", "index_list",
		"index_info", "index_xinfo", "foreign_key_list", "foreign_keys", "data_version",
	};
	for (const auto allowed : kAllowed) {
		if (Same(name, allowed)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool AllowedFunction(const char *name) {
	static constexpr const char *kDenied[] = {
		"load_extension", "readfile", "writefile", "edit", "fts3_tokenizer",
	};
	for (const auto denied : kDenied) {
		if (Same(name, denied)) {
			return false;
		}
	}
	return true;
}

int Authorize(
		void *,
		int action,
		const char *first,
		const char *second,
		const char *,
		const char *) {
	switch (action) {
	case SQLITE_SELECT:
	case SQLITE_INSERT:
	case SQLITE_UPDATE:
	case SQLITE_DELETE:
	case SQLITE_RECURSIVE:
	case SQLITE_CREATE_TABLE:
	case SQLITE_CREATE_INDEX:
	case SQLITE_CREATE_TRIGGER:
	case SQLITE_DROP_TABLE:
	case SQLITE_DROP_INDEX:
	case SQLITE_DROP_TRIGGER:
	case SQLITE_ALTER_TABLE:
	case SQLITE_REINDEX:
	case SQLITE_ANALYZE:
	case SQLITE_SAVEPOINT:
		return SQLITE_OK;
	case SQLITE_READ:
		return (Same(first, "dbstat")
			|| Same(first, "sqlite_dbpage")
			|| Same(first, "sqlite_stmt")) ? SQLITE_DENY : SQLITE_OK;
	case SQLITE_CREATE_VTABLE:
	case SQLITE_DROP_VTABLE:
		return Same(second, "fts5") ? SQLITE_OK : SQLITE_DENY;
	case SQLITE_PRAGMA:
		return AllowedPragma(first) ? SQLITE_OK : SQLITE_DENY;
	case SQLITE_FUNCTION:
		return AllowedFunction(second) ? SQLITE_OK : SQLITE_DENY;
	}
	return SQLITE_DENY;
}

int Progress(void *data) {
	const auto deadline = static_cast<Deadline*>(data);
	return (std::chrono::steady_clock::now() > deadline->end
		|| (deadline->stop && deadline->stop->load())
		|| sqlite3_memory_used() - deadline->memory > kMaxStatementMemory) ? 1 : 0;
}

[[nodiscard]] SqlError Failure(sqlite3 *db, int code) {
	auto message = std::string(db ? sqlite3_errmsg(db) : sqlite3_errstr(code));
	switch (code & 0xff) {
	case SQLITE_FULL:
		return { api::error::kQuota, "the database is full" };
	case SQLITE_INTERRUPT:
		return { api::error::kLimit, "the statement took too long" };
	case SQLITE_TOOBIG:
	case SQLITE_NOMEM:
		return { api::error::kLimit, std::move(message) };
	case SQLITE_AUTH:
		return { api::error::kDenied, std::move(message) };
	case SQLITE_ERROR:
	case SQLITE_CONSTRAINT:
	case SQLITE_RANGE:
	case SQLITE_MISMATCH:
		return { api::error::kInvalid, std::move(message) };
	}
	return { api::error::kFailed, std::move(message) };
}

// Skips spaces, comments and semicolons.
[[nodiscard]] const char *SkipFiller(const char *p) {
	while (*p) {
		if (std::strchr(" \t\r\n\f\v;", *p)) {
			++p;
		} else if (p[0] == '-' && p[1] == '-') {
			while (*p && *p != '\n') {
				++p;
			}
		} else if (p[0] == '/' && p[1] == '*') {
			p += 2;
			while (*p && !(p[0] == '*' && p[1] == '/')) {
				++p;
			}
			p += *p ? 2 : 0;
		} else {
			break;
		}
	}
	return p;
}

[[nodiscard]] bool StartsAllowed(const char *sql) {
	static constexpr const char *kAllowed[] = {
		"select", "insert", "update", "delete", "with", "values", "replace",
		"create", "drop", "alter", "reindex", "analyze", "pragma",
	};
	const auto p = SkipFiller(sql);
	auto length = size_t(0);
	while (std::isalpha(static_cast<unsigned char>(p[length]))) {
		++length;
	}
	for (const auto word : kAllowed) {
		if (std::strlen(word) == length && sqlite3_strnicmp(p, word, int(length)) == 0) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] std::string PathText(const std::filesystem::path &path) {
	const auto text = path.u8string();
	return std::string(text.begin(), text.end());
}

int Exec(sqlite3 *db, const char *sql) {
	return sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

[[nodiscard]] int64_t PragmaInt(sqlite3 *db, const char *sql) {
	auto statement = static_cast<sqlite3_stmt*>(nullptr);
	if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
		return -1;
	}
	const auto result = (sqlite3_step(statement) == SQLITE_ROW)
		? sqlite3_column_int64(statement, 0)
		: -1;
	sqlite3_finalize(statement);
	return result;
}

class Trusted {
public:
	explicit Trusted(sqlite3 *db) : _db(db) {
		sqlite3_set_authorizer(_db, nullptr, nullptr);
	}
	~Trusted() {
		sqlite3_set_authorizer(_db, Authorize, nullptr);
	}

private:
	sqlite3 *_db = nullptr;

};

[[nodiscard]] int Bind(sqlite3_stmt *statement, int index, const cbor::Value &value) {
	switch (value.type()) {
	case cbor::Value::Type::Null:
		return sqlite3_bind_null(statement, index);
	case cbor::Value::Type::Bool:
		return sqlite3_bind_int(statement, index, *value.asBool() ? 1 : 0);
	case cbor::Value::Type::Int:
		return sqlite3_bind_int64(statement, index, *value.asInt());
	case cbor::Value::Type::Float:
		return sqlite3_bind_double(statement, index, *value.asFloat());
	case cbor::Value::Type::Text: {
		const auto &text = *value.asText();
		return sqlite3_bind_text64(
			statement,
			index,
			text.data(),
			text.size(),
			SQLITE_TRANSIENT,
			SQLITE_UTF8);
	}
	case cbor::Value::Type::Bytes: {
		const auto &bytes = *value.asBytes();
		return sqlite3_bind_blob64(
			statement,
			index,
			bytes.data(),
			bytes.size(),
			SQLITE_TRANSIENT);
	}
	default:
		return SQLITE_MISMATCH;
	}
}

[[nodiscard]] size_t RowBytes(sqlite3_stmt *statement, int columns) {
	auto result = size_t(0);
	for (auto i = 0; i != columns; ++i) {
		const auto type = sqlite3_column_type(statement, i);
		result += (type == SQLITE_TEXT || type == SQLITE_BLOB)
			? size_t(sqlite3_column_bytes(statement, i)) + 5
			: 9;
	}
	return result;
}

class ProgressGuard {
public:
	ProgressGuard(sqlite3 *db, Deadline *deadline) : _db(db) {
		sqlite3_progress_handler(_db, kProgressOps, Progress, deadline);
	}
	~ProgressGuard() {
		sqlite3_progress_handler(_db, 0, nullptr, nullptr);
	}

private:
	sqlite3 *_db = nullptr;

};

[[nodiscard]] cbor::Value Column(sqlite3_stmt *statement, int index) {
	switch (sqlite3_column_type(statement, index)) {
	case SQLITE_INTEGER:
		return cbor::Value::FromInt(sqlite3_column_int64(statement, index));
	case SQLITE_FLOAT:
		return cbor::Value::FromFloat(sqlite3_column_double(statement, index));
	case SQLITE_TEXT: {
		const auto data = reinterpret_cast<const char*>(
			sqlite3_column_text(statement, index));
		auto text = std::string(data, size_t(sqlite3_column_bytes(statement, index)));
		if (cbor::IsValidUtf8(text)) {
			return cbor::Value::FromText(std::move(text));
		}
		return cbor::Value::FromBytes(cbor::Bytes(text.begin(), text.end()));
	}
	case SQLITE_BLOB: {
		const auto data = static_cast<const uint8_t*>(sqlite3_column_blob(statement, index));
		const auto length = size_t(sqlite3_column_bytes(statement, index));
		return cbor::Value::FromBytes(data
			? cbor::Bytes(data, data + length)
			: cbor::Bytes());
	}
	}
	return cbor::Value();
}

} // namespace

SqliteDatabase::SqliteDatabase(std::filesystem::path file)
: _file(std::move(file)) {
}

SqliteDatabase::~SqliteDatabase() {
	close();
}

bool SqliteDatabase::open(SqlError &error) {
	if (_db) {
		return true;
	} else if (_closed) {
		error = { api::error::kUnavailable, "the database is closed" };
		return false;
	}
	auto filesystemError = std::error_code();
	std::filesystem::create_directories(_file.parent_path(), filesystemError);
	for (auto attempt = 0; attempt != 2; ++attempt) {
		const auto code = sqlite3_open_v2(
			PathText(_file).c_str(),
			&_db,
			SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
			nullptr);
		if (code == SQLITE_OK) {
			sqlite3_busy_timeout(_db, 1000);
			sqlite3_db_config(_db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
			sqlite3_db_config(_db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
			sqlite3_db_config(_db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
			sqlite3_db_config(_db, SQLITE_DBCONFIG_ENABLE_FKEY, 1, nullptr);
			sqlite3_db_config(_db, SQLITE_DBCONFIG_DQS_DML, 0, nullptr);
			sqlite3_db_config(_db, SQLITE_DBCONFIG_DQS_DDL, 0, nullptr);
			sqlite3_db_config(_db, SQLITE_DBCONFIG_ENABLE_VIEW, 0, nullptr);
			sqlite3_limit(_db, SQLITE_LIMIT_LENGTH, kMaxBlobLength);
			sqlite3_limit(_db, SQLITE_LIMIT_SQL_LENGTH, 1 << 16);
			sqlite3_limit(_db, SQLITE_LIMIT_VARIABLE_NUMBER, 256);
			sqlite3_limit(_db, SQLITE_LIMIT_ATTACHED, 0);
			sqlite3_limit(_db, SQLITE_LIMIT_COLUMN, kMaxColumns);
			sqlite3_limit(_db, SQLITE_LIMIT_COMPOUND_SELECT, 50);
			sqlite3_limit(_db, SQLITE_LIMIT_EXPR_DEPTH, 100);
			sqlite3_limit(_db, SQLITE_LIMIT_LIKE_PATTERN_LENGTH, 1000);
			sqlite3_limit(_db, SQLITE_LIMIT_TRIGGER_DEPTH, 10);
			sqlite3_limit(_db, SQLITE_LIMIT_FUNCTION_ARG, 32);
			const auto probe = PragmaInt(_db, "PRAGMA schema_version");
			if (probe >= 0) {
				Exec(_db, "PRAGMA journal_mode=WAL");
				Exec(_db, "PRAGMA synchronous=NORMAL");
				Exec(_db, "PRAGMA temp_store=MEMORY");
				Exec(_db, "PRAGMA journal_size_limit=4194304");
				sqlite3_set_authorizer(_db, Authorize, nullptr);
				return true;
			}
		}
		const auto reason = (code != SQLITE_OK || !_db) ? code : sqlite3_errcode(_db);
		const auto damaged = (reason & 0xff) == SQLITE_NOTADB
			|| (reason & 0xff) == SQLITE_CORRUPT;
		error = Failure(_db, reason);
		sqlite3_close(_db);
		_db = nullptr;
		if (!damaged || attempt) {
			return false;
		}
		std::filesystem::rename(
			_file,
			std::filesystem::path(_file) += ".corrupt",
			filesystemError);
		std::filesystem::remove(std::filesystem::path(_file) += "-wal", filesystemError);
		std::filesystem::remove(std::filesystem::path(_file) += "-shm", filesystemError);
	}
	return false;
}

Result<std::vector<SqlResult>, SqlError> SqliteDatabase::run(
		const std::vector<SqlStatement> &statements,
		const SqlLimits &limits,
		bool atomic) {
	const auto lock = std::scoped_lock(_mutex);
	auto error = SqlError();
	if (_stop.load()) {
		return SqlError{ api::error::kUnavailable, "the database is closed" };
	} else if (!open(error)) {
		return error;
	}
	{
		const auto trusted = Trusted(_db);
		const auto pages = std::max<int64_t>(
			int64_t(limits.maxBytes / kPageSize),
			PragmaInt(_db, "PRAGMA page_count"));
		Exec(_db, ("PRAGMA max_page_count=" + std::to_string(pages)).c_str());
		if (atomic && Exec(_db, "BEGIN IMMEDIATE") != SQLITE_OK) {
			return Failure(_db, sqlite3_errcode(_db));
		}
	}
	auto deadline = Deadline{
		std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.timeoutMs),
		&_stop,
		sqlite3_memory_used(),
	};
	const auto progress = ProgressGuard(_db, &deadline);
	auto results = std::vector<SqlResult>();
	for (const auto &statement : statements) {
		auto one = _stop.load()
			? Result<SqlResult, SqlError>(SqlError{ api::error::kUnavailable, "the database is closed" })
			: runOne(statement, limits);
		if (!one.ok()) {
			if (atomic) {
				const auto trusted = Trusted(_db);
				Exec(_db, "ROLLBACK");
			}
			return one.error();
		}
		results.push_back(std::move(one.value()));
	}
	if (atomic) {
		const auto trusted = Trusted(_db);
		if (Exec(_db, "COMMIT") != SQLITE_OK) {
			auto failure = Failure(_db, sqlite3_errcode(_db));
			Exec(_db, "ROLLBACK");
			return failure;
		}
	} else if (!sqlite3_get_autocommit(_db)) {
		const auto trusted = Trusted(_db);
		Exec(_db, "ROLLBACK");
		return SqlError{ api::error::kInvalid, "transactions are managed with batch" };
	}
	return results;
}

Result<SqlResult, SqlError> SqliteDatabase::runOne(
		const SqlStatement &statement,
		const SqlLimits &limits) {
	if (statement.sql.empty()
		|| statement.sql.find('\0') != std::string::npos
		|| !StartsAllowed(statement.sql.c_str())) {
		return SqlError{ api::error::kDenied, "this statement is not allowed" };
	}
	auto prepared = static_cast<sqlite3_stmt*>(nullptr);
	auto tail = static_cast<const char*>(nullptr);
	if (const auto code = sqlite3_prepare_v2(
			_db,
			statement.sql.c_str(),
			int(statement.sql.size() + 1),
			&prepared,
			&tail); code != SQLITE_OK) {
		return Failure(_db, code);
	} else if (!prepared) {
		return SqlError{ api::error::kInvalid, "the statement is empty" };
	} else if (*SkipFiller(tail)) {
		sqlite3_finalize(prepared);
		return SqlError{ api::error::kInvalid, "only one statement can be run at a time" };
	} else if (size_t(sqlite3_bind_parameter_count(prepared)) != statement.params.size()) {
		sqlite3_finalize(prepared);
		return SqlError{ api::error::kInvalid, "the number of parameters does not match" };
	}
	for (auto i = 0; i != int(statement.params.size()); ++i) {
		if (const auto code = Bind(prepared, i + 1, statement.params[i]); code != SQLITE_OK) {
			auto failure = Failure(_db, code);
			sqlite3_finalize(prepared);
			return failure;
		}
	}

	auto result = SqlResult();
	const auto columns = sqlite3_column_count(prepared);
	for (auto i = 0; i != columns; ++i) {
		result.columns.emplace_back(sqlite3_column_name(prepared, i));
	}
	const auto before = sqlite3_total_changes64(_db);
	auto size = size_t(0);
	auto code = SQLITE_OK;
	while ((code = sqlite3_step(prepared)) == SQLITE_ROW) {
		const auto bytes = RowBytes(prepared, columns);
		if (result.rows.size() >= limits.maxRows
			|| (!result.rows.empty() && size + bytes > limits.maxResultBytes)) {
			result.truncated = true;
			break;
		} else if (bytes > limits.maxResultBytes) {
			sqlite3_finalize(prepared);
			return SqlError{ api::error::kLimit, "the row is too large" };
		}
		size += bytes;
		auto row = std::vector<cbor::Value>();
		row.reserve(columns);
		for (auto i = 0; i != columns; ++i) {
			row.push_back(Column(prepared, i));
		}
		result.rows.push_back(std::move(row));
	}
	if (code != SQLITE_ROW && code != SQLITE_DONE) {
		auto failure = Failure(_db, code);
		sqlite3_finalize(prepared);
		return failure;
	}
	sqlite3_finalize(prepared);
	result.changes = sqlite3_total_changes64(_db) - before;
	result.lastId = sqlite3_last_insert_rowid(_db);
	return result;
}

uint64_t SqliteDatabase::usedBytes() {
	const auto lock = std::scoped_lock(_mutex);
	if (_db) {
		const auto trusted = Trusted(_db);
		const auto pages = PragmaInt(_db, "PRAGMA page_count");
		return pages > 0 ? uint64_t(pages) * kPageSize : 0;
	}
	auto error = std::error_code();
	const auto size = std::filesystem::file_size(_file, error);
	return error ? 0 : uint64_t(size);
}

void SqliteDatabase::close() {
	_stop = true;
	const auto lock = std::scoped_lock(_mutex);
	_closed = true;
	if (_db) {
		sqlite3_close(_db);
		_db = nullptr;
	}
}

} // namespace fgpl
