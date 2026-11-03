#include "check.h"
#include "fgpl/database.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

using namespace fgpl;
namespace fs = std::filesystem;

[[nodiscard]] fs::path TempDirectory(const char *name) {
	const auto path = fs::temp_directory_path() / name;
	fs::remove_all(path);
	return path;
}

[[nodiscard]] cbor::Value Text(std::string_view text) {
	return cbor::Value::FromText(std::string(text));
}

[[nodiscard]] cbor::Value Int(int64_t number) {
	return cbor::Value::FromInt(number);
}

[[nodiscard]] Result<std::vector<SqlResult>, SqlError> Run(
		Database &database,
		std::string sql,
		std::vector<cbor::Value> params = {},
		SqlLimits limits = {}) {
	return database.run({ SqlStatement{ std::move(sql), std::move(params) } }, limits, false);
}

[[nodiscard]] std::string CodeOf(const Result<std::vector<SqlResult>, SqlError> &result) {
	return result.ok() ? std::string() : result.error().code;
}

void TestRowsAndParameters() {
	const auto directory = TempDirectory("fgpl-sql-rows");
	auto database = SqliteDatabase(directory / "a.sqlite");
	CHECK(Run(database, "CREATE TABLE log (id INTEGER PRIMARY KEY, text TEXT, data BLOB, score REAL)").ok());
	const auto inserted = Run(database, "INSERT INTO log (text, data, score) VALUES (?, ?, ?)", {
		Text("hello"), cbor::Value::FromBytes({ 1, 2, 3 }), cbor::Value::FromFloat(1.5),
	});
	CHECK(inserted.ok());
	if (inserted.ok()) {
		CHECK(inserted.value()[0].changes == 1);
		CHECK(inserted.value()[0].lastId == 1);
	}
	CHECK(Run(database, "INSERT INTO log (text) VALUES (?)", { cbor::Value() }).ok());
	const auto selected = Run(database, "SELECT id, text, data, score FROM log ORDER BY id");
	CHECK(selected.ok());
	if (selected.ok()) {
		const auto &rows = selected.value()[0].rows;
		CHECK(selected.value()[0].columns.size() == 4);
		CHECK(rows.size() == 2);
		CHECK(*rows[0][0].asInt() == 1);
		CHECK(*rows[0][1].asText() == "hello");
		CHECK(*rows[0][2].asBytes() == cbor::Bytes({ 1, 2, 3 }));
		CHECK(*rows[0][3].asFloat() == 1.5);
		CHECK(rows[1][1].isNull());
	}
	CHECK(CodeOf(Run(database, "SELECT ?", {})) == "invalid");
	CHECK(CodeOf(Run(database, "SELECT 1", { Int(1) })) == "invalid");
	CHECK(!Run(database, "SELEC").ok());
	CHECK(!Run(database, "").ok());
	CHECK(CodeOf(Run(database, "SELECT 1; SELECT 2")) == "invalid");
	CHECK(Run(database, "SELECT 1; -- done").ok());
	CHECK(CodeOf(Run(database, "INSERT INTO log (id) VALUES (1)")) == "invalid");
	fs::remove_all(directory);
}

void TestRowLimit() {
	const auto directory = TempDirectory("fgpl-sql-limit");
	auto database = SqliteDatabase(directory / "a.sqlite");
	CHECK(Run(database, "CREATE TABLE n (v INTEGER)").ok());
	CHECK(Run(database, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 50) INSERT INTO n SELECT x FROM c").ok());
	auto limits = SqlLimits();
	limits.maxRows = 10;
	const auto selected = Run(database, "SELECT v FROM n", {}, limits);
	CHECK(selected.ok());
	if (selected.ok()) {
		CHECK(selected.value()[0].rows.size() == 10);
		CHECK(selected.value()[0].truncated);
	}
	fs::remove_all(directory);
}

void TestForbiddenStatements() {
	const auto directory = TempDirectory("fgpl-sql-forbidden");
	auto database = SqliteDatabase(directory / "a.sqlite");
	CHECK(CodeOf(Run(database, "ATTACH DATABASE ':memory:' AS other")) == "denied");
	CHECK(CodeOf(Run(database, "ATTACH DATABASE '" + (directory / "b.sqlite").generic_string() + "' AS other")) == "denied");
	CHECK(!fs::exists(directory / "b.sqlite"));
	CHECK(CodeOf(Run(database, "PRAGMA journal_mode=DELETE")) == "denied");
	CHECK(CodeOf(Run(database, "PRAGMA max_page_count=1000000")) == "denied");
	CHECK(CodeOf(Run(database, "PRAGMA writable_schema=1")) == "denied");
	CHECK(Run(database, "PRAGMA user_version=3").ok());
	const auto version = Run(database, "PRAGMA user_version");
	CHECK(version.ok() && *version.value()[0].rows[0][0].asInt() == 3);
	CHECK(CodeOf(Run(database, "VACUUM")) == "denied");
	CHECK(CodeOf(Run(database, "/* x */ vacuum INTO '" + (directory / "c.sqlite").generic_string() + "'")) == "denied");
	CHECK(CodeOf(Run(database, "-- x\nVACUUM")) == "denied");
	CHECK(CodeOf(Run(database, "\fVACUUM temp")) == "denied");
	CHECK(CodeOf(Run(database, "\vVACUUM")) == "denied");
	CHECK(CodeOf(Run(database, "CREATE TEMP TABLE t (v)")) == "denied");
	CHECK(CodeOf(Run(database, "CREATE TEMP TABLE IF NOT EXISTS t (v)")) == "denied");
	CHECK(CodeOf(Run(database, "CREATE VIEW v AS SELECT 1")) != "");
	CHECK(!Run(database, "CREATE VIEW v AS SELECT 1").ok());
	CHECK(CodeOf(Run(database, "SELECT * FROM dbstat")) == "denied");
	CHECK(CodeOf(Run(database, "SELECT * FROM sqlite_dbpage")) != "");
	CHECK(!fs::exists(directory / "c.sqlite"));
	CHECK(!Run(database, "SELECT load_extension('x')").ok());
	CHECK(CodeOf(Run(database, "BEGIN")) == "denied");
	CHECK(CodeOf(Run(database, "SAVEPOINT a")) == "denied");
	CHECK(Run(database, "SELECT 1").ok());
	CHECK(CodeOf(Run(database, "CREATE VIRTUAL TABLE t USING dbstat")) == "denied");
	fs::remove_all(directory);
}

void TestBatchRollsBack() {
	const auto directory = TempDirectory("fgpl-sql-batch");
	auto database = SqliteDatabase(directory / "a.sqlite");
	CHECK(Run(database, "CREATE TABLE t (v INTEGER PRIMARY KEY)").ok());
	const auto failed = database.run({
		SqlStatement{ "INSERT INTO t VALUES (1)", {} },
		SqlStatement{ "INSERT INTO t VALUES (2)", {} },
		SqlStatement{ "INSERT INTO t VALUES (1)", {} },
	}, {}, true);
	CHECK(!failed.ok());
	const auto count = Run(database, "SELECT COUNT(*) FROM t");
	CHECK(count.ok() && *count.value()[0].rows[0][0].asInt() == 0);
	const auto done = database.run({
		SqlStatement{ "INSERT INTO t VALUES (1)", {} },
		SqlStatement{ "INSERT INTO t VALUES (2)", {} },
	}, {}, true);
	CHECK(done.ok() && done.value().size() == 2);
	const auto after = Run(database, "SELECT COUNT(*) FROM t");
	CHECK(after.ok() && *after.value()[0].rows[0][0].asInt() == 2);
	fs::remove_all(directory);
}

void TestQuotaAndTimeout() {
	const auto directory = TempDirectory("fgpl-sql-quota");
	auto database = SqliteDatabase(directory / "a.sqlite");
	auto limits = SqlLimits();
	limits.maxBytes = uint64_t(64) << 10;
	CHECK(Run(database, "CREATE TABLE blobs (v BLOB)", {}, limits).ok());
	auto full = false;
	for (auto i = 0; i != 100 && !full; ++i) {
		const auto result = Run(
			database,
			"INSERT INTO blobs VALUES (?)",
			{ cbor::Value::FromBytes(cbor::Bytes(8192, 1)) },
			limits);
		full = !result.ok() && CodeOf(result) == "quota";
	}
	CHECK(full);
	CHECK(database.usedBytes() <= limits.maxBytes + 8192);
	CHECK(Run(database, "DELETE FROM blobs", {}, limits).ok());

	auto slow = SqlLimits();
	slow.timeoutMs = 50;
	const auto spin = Run(
		database,
		"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT COUNT(*) FROM c",
		{},
		slow);
	CHECK(CodeOf(spin) == "limit");
	CHECK(Run(database, "SELECT 1").ok());
	fs::remove_all(directory);
}

void TestMemoryAndSize() {
	const auto directory = TempDirectory("fgpl-sql-memory");
	auto database = SqliteDatabase(directory / "a.sqlite");
	auto wide = std::string("SELECT zeroblob(1000000), zeroblob(1000000)");
	CHECK(CodeOf(Run(database, wide)) == "limit");
	CHECK(CodeOf(Run(database, "SELECT zeroblob(16000000)")) == "limit");
	auto columns = std::string("SELECT 1");
	for (auto i = 0; i != 70; ++i) {
		columns += ", 1";
	}
	CHECK(!Run(database, columns).ok());
	CHECK(Run(database, "SELECT zeroblob(1000)").ok());

	const auto started = std::chrono::steady_clock::now();
	const auto sorted = Run(
		database,
		"SELECT x FROM (WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c) SELECT zeroblob(500000) AS x FROM c) ORDER BY x");
	CHECK(CodeOf(sorted) == "limit");
	CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
	CHECK(Run(database, "SELECT 1").ok());
	fs::remove_all(directory);
}

void TestCloseInterrupts() {
	const auto directory = TempDirectory("fgpl-sql-close");
	auto database = SqliteDatabase(directory / "a.sqlite");
	CHECK(Run(database, "SELECT 1").ok());
	auto slow = SqlLimits();
	slow.timeoutMs = 60000;
	auto code = std::string();
	auto worker = std::thread([&] {
		code = CodeOf(Run(
			database,
			"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT COUNT(*) FROM c",
			{},
			slow));
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	const auto started = std::chrono::steady_clock::now();
	database.close();
	worker.join();
	CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
	CHECK(code == "limit");
	CHECK(CodeOf(Run(database, "SELECT 1")) == "unavailable");
	fs::remove_all(directory);
}

void TestFullTextSearch() {
	const auto directory = TempDirectory("fgpl-sql-fts");
	auto database = SqliteDatabase(directory / "a.sqlite");
	CHECK(Run(database, "CREATE VIRTUAL TABLE docs USING fts5(body)").ok());
	CHECK(Run(database, "INSERT INTO docs (body) VALUES (?)", { Text("the quick brown fox") }).ok());
	CHECK(Run(database, "INSERT INTO docs (body) VALUES (?)", { Text("lazy dogs") }).ok());
	const auto found = Run(database, "SELECT body FROM docs WHERE docs MATCH ?", { Text("quick") });
	CHECK(found.ok() && found.value()[0].rows.size() == 1);
	fs::remove_all(directory);
}

void TestPersistsAndRecovers() {
	const auto directory = TempDirectory("fgpl-sql-persist");
	const auto file = directory / "a.sqlite";
	{
		auto database = SqliteDatabase(file);
		CHECK(Run(database, "CREATE TABLE t (v TEXT)").ok());
		CHECK(Run(database, "INSERT INTO t VALUES ('kept')").ok());
	}
	{
		auto database = SqliteDatabase(file);
		const auto rows = Run(database, "SELECT v FROM t");
		CHECK(rows.ok() && *rows.value()[0].rows[0][0].asText() == "kept");
	}
	for (const auto &suffix : { "-wal", "-shm" }) {
		fs::remove(fs::path(file) += suffix);
	}
	{
		auto stream = std::ofstream(file, std::ios::binary | std::ios::trunc);
		stream << std::string(8192, 'x');
	}
	{
		auto database = SqliteDatabase(file);
		CHECK(Run(database, "CREATE TABLE t (v TEXT)").ok());
		CHECK(fs::exists(fs::path(file) += ".corrupt"));
	}
	auto closed = SqliteDatabase(directory / "b.sqlite");
	closed.close();
	CHECK(CodeOf(Run(closed, "SELECT 1")) == "unavailable");
	fs::remove_all(directory);
}

} // namespace

int main() {
	TestRowsAndParameters();
	TestRowLimit();
	TestForbiddenStatements();
	TestBatchRollsBack();
	TestQuotaAndTimeout();
	TestMemoryAndSize();
	TestCloseInterrupts();
	TestFullTextSearch();
	TestPersistsAndRecovers();
	return Failures() ? 1 : 0;
}
