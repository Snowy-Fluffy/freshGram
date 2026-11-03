#pragma once

#include "fgpl/cbor.h"
#include "fgpl/result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace fgpl {

struct SqlStatement {
	std::string sql;
	std::vector<cbor::Value> params;
};

struct SqlResult {
	std::vector<std::string> columns;
	std::vector<std::vector<cbor::Value>> rows;
	int64_t changes = 0;
	int64_t lastId = 0;
	bool truncated = false;
};

struct SqlError {
	std::string code;
	std::string message;
};

struct SqlLimits {
	uint64_t maxBytes = uint64_t(10) << 20;
	size_t maxRows = 1000;
	size_t maxResultBytes = size_t(512) << 10;
	int timeoutMs = 3000;
};

class Database {
public:
	virtual ~Database() = default;

	// Runs the statements one after another; a batch is one transaction that rolls back on an error.
	[[nodiscard]] virtual Result<std::vector<SqlResult>, SqlError> run(
		const std::vector<SqlStatement> &statements,
		const SqlLimits &limits,
		bool atomic) = 0;
	[[nodiscard]] virtual uint64_t usedBytes() = 0;
	virtual void close() = 0;
};

class SqliteDatabase final : public Database {
public:
	explicit SqliteDatabase(std::filesystem::path file);
	~SqliteDatabase() override;

	Result<std::vector<SqlResult>, SqlError> run(
		const std::vector<SqlStatement> &statements,
		const SqlLimits &limits,
		bool atomic) override;
	uint64_t usedBytes() override;
	void close() override;

private:
	[[nodiscard]] bool open(SqlError &error);
	[[nodiscard]] Result<SqlResult, SqlError> runOne(
		const SqlStatement &statement,
		const SqlLimits &limits);

	std::mutex _mutex;
	std::filesystem::path _file;
	sqlite3 *_db = nullptr;
	bool _closed = false;

};

} // namespace fgpl
