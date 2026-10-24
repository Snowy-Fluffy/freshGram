// This is the source code of freshGram.
#include "ayu/data/fresh_database_report.h"

#include "ayu/libs/sqlite/sqlite3.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#if __has_include(<openssl/evp.h>)
#include <openssl/evp.h>
#include <openssl/rand.h>
#define FRESH_REPORT_HAS_OPENSSL 1
#endif

namespace FreshDatabaseReport {
namespace {

using Clock = std::chrono::steady_clock;
using Row = std::vector<std::string>;
using Rows = std::vector<Row>;

constexpr auto kColumnStatsMinRows = 1000;
constexpr auto kPageSize = 4096;

[[nodiscard]] double MsSince(Clock::time_point from) {
	return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

[[nodiscard]] std::string Format(const char *format, ...) {
	char buffer[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

[[nodiscard]] std::string Quote(const std::string &name) {
	auto result = std::string("\"");
	for (const auto c : name) {
		if (c == '"') {
			result += '"';
		}
		result += c;
	}
	result += '"';
	return result;
}

[[nodiscard]] double Mb(int64_t bytes) {
	return double(bytes) / (1024. * 1024.);
}

[[nodiscard]] std::optional<Rows> Query(sqlite3 *db, const std::string &sql) {
	sqlite3_stmt *statement = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
		return std::nullopt;
	}
	auto rows = Rows();
	const auto count = sqlite3_column_count(statement);
	while (sqlite3_step(statement) == SQLITE_ROW) {
		auto row = Row();
		for (auto i = 0; i != count; ++i) {
			const auto text = sqlite3_column_text(statement, i);
			row.push_back(text
				? reinterpret_cast<const char*>(text)
				: std::string());
		}
		rows.push_back(std::move(row));
	}
	const auto code = sqlite3_finalize(statement);
	if (code != SQLITE_OK) {
		return std::nullopt;
	}
	return rows;
}

[[nodiscard]] int64_t Scalar(sqlite3 *db, const std::string &sql, int64_t fallback = 0) {
	const auto rows = Query(db, sql);
	if (!rows || rows->empty() || rows->front().empty()) {
		return fallback;
	}
	return std::atoll(rows->front().front().c_str());
}

struct Timing {
	bool ok = false;
	int64_t rows = 0;
	double first = 0.;
	double average = 0.;
	double worst = 0.;
};

// Runs the statement, touching every column of every row.
[[nodiscard]] Timing Measure(sqlite3 *db, const std::string &sql, int repeats) {
	auto result = Timing();
	sqlite3_stmt *statement = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
		return result;
	}
	const auto count = sqlite3_column_count(statement);
	auto total = 0.;
	for (auto i = 0; i != repeats; ++i) {
		const auto from = Clock::now();
		auto rows = int64_t(0);
		while (sqlite3_step(statement) == SQLITE_ROW) {
			++rows;
			for (auto column = 0; column != count; ++column) {
				sqlite3_column_bytes(statement, column);
			}
		}
		const auto elapsed = MsSince(from);
		sqlite3_reset(statement);
		result.rows = rows;
		if (!i) {
			result.first = elapsed;
		}
		result.worst = std::max(result.worst, elapsed);
		total += elapsed;
	}
	sqlite3_finalize(statement);
	result.average = total / std::max(repeats, 1);
	result.ok = true;
	return result;
}

void AddTiming(
		std::string &out,
		sqlite3 *db,
		const char *name,
		const std::string &sql,
		int repeats) {
	const auto timing = Measure(db, sql, repeats);
	if (!timing.ok) {
		out += Format("  %-34s skipped\n", name);
		return;
	}
	out += Format(
		"  %-34s first %9.3f  avg %9.3f  worst %9.3f ms  (%lld rows, x%d)\n",
		name,
		timing.first,
		timing.average,
		timing.worst,
		(long long)timing.rows,
		repeats);
}

void AddPlan(std::string &out, sqlite3 *db, const char *name, const std::string &sql) {
	out += Format("  %s\n", name);
	const auto plan = Query(db, "EXPLAIN QUERY PLAN " + sql);
	if (!plan) {
		out += "    unavailable\n";
		return;
	}
	for (const auto &row : *plan) {
		if (row.size() >= 4) {
			out += "    " + row[3] + "\n";
		}
	}
}

[[nodiscard]] bool HasColumns(
		sqlite3 *db,
		const std::string &table,
		const std::vector<std::string> &columns) {
	const auto info = Query(db, "PRAGMA table_info(" + Quote(table) + ")");
	if (!info) {
		return false;
	}
	for (const auto &needed : columns) {
		const auto found = std::any_of(info->begin(), info->end(), [&](const Row &row) {
			return row.size() > 1 && row[1] == needed;
		});
		if (!found) {
			return false;
		}
	}
	return true;
}

// The compact tables replace the wide ones after the migration.
[[nodiscard]] std::string TableName(sqlite3 *db, const std::string &base) {
	const auto compact = base + "V2";
	return (Scalar(db, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = '" + compact + "'") > 0)
		? compact
		: base;
}

void AddOverview(std::string &out, sqlite3 *db) {
	const auto pageSize = Scalar(db, "PRAGMA page_size");
	const auto pageCount = Scalar(db, "PRAGMA page_count");
	const auto freePages = Scalar(db, "PRAGMA freelist_count");
	out += "== Overview ==\n";
	out += Format("  sqlite %s\n", sqlite3_libversion());
	out += Format(
		"  pages %lld x %lld B = %.1f MB, free pages %lld (%.1f MB)\n",
		(long long)pageCount,
		(long long)pageSize,
		Mb(pageCount * pageSize),
		(long long)freePages,
		Mb(freePages * pageSize));
	out += Format(
		"  journal_mode %s, auto_vacuum %lld, cache_size %lld\n",
		Query(db, "PRAGMA journal_mode")->at(0).at(0).c_str(),
		(long long)Scalar(db, "PRAGMA auto_vacuum"),
		(long long)Scalar(db, "PRAGMA cache_size"));
	if (HasColumns(db, "SchemaVersion", {"version"})) {
		out += Format(
			"  schema version %lld\n",
			(long long)Scalar(db, "SELECT max(version) FROM SchemaVersion"));
	}
	out += "\n";
}

struct TableInfo {
	std::string name;
	int64_t rows = 0;
};

[[nodiscard]] std::vector<TableInfo> ListTables(sqlite3 *db) {
	auto result = std::vector<TableInfo>();
	const auto names = Query(
		db,
		"SELECT name FROM sqlite_master WHERE type = 'table' "
		"AND name NOT LIKE 'sqlite_%' ORDER BY name");
	if (!names) {
		return result;
	}
	for (const auto &row : *names) {
		result.push_back({
			row[0],
			Scalar(db, "SELECT count(*) FROM " + Quote(row[0])),
		});
	}
	return result;
}

void AddObjects(std::string &out, sqlite3 *db, const std::vector<TableInfo> &tables) {
	out += "== Tables and indexes ==\n";
	const auto stat = Query(
		db,
		"SELECT name, count(*), sum(pgsize), sum(payload), sum(unused) "
		"FROM dbstat GROUP BY name ORDER BY 3 DESC");
	if (!stat) {
		out += "  dbstat is not compiled in, only row counts are available\n";
		for (const auto &table : tables) {
			out += Format("  %-34s %10lld rows\n", table.name.c_str(), (long long)table.rows);
		}
		out += "\n";
		return;
	}
	out += Format(
		"  %-48s %10s %9s %9s %9s %9s\n",
		"name", "rows", "pages", "MB", "payload", "unused");
	for (const auto &row : *stat) {
		auto rows = std::string("-");
		for (const auto &table : tables) {
			if (table.name == row[0]) {
				rows = std::to_string(table.rows);
			}
		}
		out += Format(
			"  %-48s %10s %9lld %9.1f %9.1f %9.1f\n",
			row[0].c_str(),
			rows.c_str(),
			(long long)std::atoll(row[1].c_str()),
			Mb(std::atoll(row[2].c_str())),
			Mb(std::atoll(row[3].c_str())),
			Mb(std::atoll(row[4].c_str())));
	}
	out += "\n";
}

void AddColumns(std::string &out, sqlite3 *db, const TableInfo &table) {
	const auto info = Query(db, "PRAGMA table_info(" + Quote(table.name) + ")");
	if (!info || info->empty()) {
		return;
	}
	auto sql = std::string("SELECT ");
	for (const auto &column : *info) {
		const auto name = Quote(column[1]);
		sql += "count(" + name + "), ";
		sql += "sum(" + name + " IS NOT NULL AND " + name + " != '' AND " + name + " != 0), ";
		sql += "sum(CASE WHEN typeof(" + name + ") IN ('text', 'blob') "
			"THEN length(CAST(" + name + " AS BLOB)) ELSE 0 END), ";
	}
	sql.resize(sql.size() - 2);
	sql += " FROM " + Quote(table.name);
	const auto stats = Query(db, sql);
	if (!stats || stats->empty()) {
		return;
	}
	out += Format("  %s, %lld rows\n", table.name.c_str(), (long long)table.rows);
	out += Format(
		"    %-30s %-10s %12s %12s %10s\n",
		"column", "type", "not null", "non-empty", "bytes MB");
	const auto &values = stats->front();
	for (auto i = size_t(0); i != info->size(); ++i) {
		const auto &column = (*info)[i];
		out += Format(
			"    %-30s %-10s %12lld %12lld %10.2f\n",
			column[1].c_str(),
			column[2].c_str(),
			(long long)std::atoll(values[i * 3].c_str()),
			(long long)std::atoll(values[i * 3 + 1].c_str()),
			Mb(std::atoll(values[i * 3 + 2].c_str())));
	}
}

void AddColumnStats(std::string &out, sqlite3 *db, const std::vector<TableInfo> &tables) {
	out += "== Columns of the big tables ==\n";
	for (const auto &table : tables) {
		if (table.rows >= kColumnStatsMinRows) {
			AddColumns(out, db, table);
		}
	}
	out += "\n";
}

void AddDerived(std::string &out, sqlite3 *db) {
	out += "== Redundancy ==\n";
	if (HasColumns(db, "DeletedMessage", {"userId", "dialogId", "peerId", "editDate", "entityCreateDate", "text"})) {
		const auto rows = Query(
			db,
			"SELECT count(*), "
			"sum(peerId = abs(dialogId)), "
			"sum(editDate = entityCreateDate), "
			"sum(length(text) <= 20), "
			"sum(length(text) <= 100), "
			"avg(length(text)), "
			"count(DISTINCT userId), "
			"count(DISTINCT dialogId) "
			"FROM DeletedMessage");
		if (rows && !rows->empty()) {
			const auto &r = rows->front();
			const auto total = std::max<int64_t>(std::atoll(r[0].c_str()), 1);
			const auto percent = [&](const std::string &value) {
				return 100. * double(std::atoll(value.c_str())) / double(total);
			};
			out += Format("  DeletedMessage rows %lld, users %s, dialogs %s\n",
				(long long)total,
				r[6].c_str(),
				r[7].c_str());
			out += Format("  peerId = |dialogId| in %.1f%% of rows\n", percent(r[1]));
			out += Format("  editDate = entityCreateDate in %.1f%% of rows\n", percent(r[2]));
			out += Format("  text <= 20 chars: %.1f%%, <= 100 chars: %.1f%%, average %.1f\n",
				percent(r[3]),
				percent(r[4]),
				std::atof(r[5].c_str()));
		}
	}
	const auto edited = TableName(db, "EditedMessage");
	if (HasColumns(db, edited, {"userId", "dialogId", "messageId"})) {
		const auto rows = Query(
			db,
			"SELECT count(*), max(c), avg(c), sum(c >= 10), sum(c >= 100) FROM ("
			"SELECT count(*) AS c FROM " + edited + " "
			"GROUP BY userId, dialogId, messageId)");
		if (rows && !rows->empty()) {
			const auto &r = rows->front();
			out += Format(
				"  EditedMessage: %s edited messages, most revisions %s, average %.2f, "
				"with >= 10: %s, with >= 100: %s\n",
				r[0].c_str(),
				r[1].c_str(),
				std::atof(r[2].c_str()),
				r[3].c_str(),
				r[4].c_str());
		}
	}
	if (HasColumns(db, edited, {"userId", "dialogId", "messageId", "text"})) {
		const auto rows = Query(
			db,
			"SELECT count(*), count(*) - count(DISTINCT userId || '/' || dialogId || '/' "
			"|| messageId || '/' || text) FROM " + edited);
		if (rows && !rows->empty()) {
			out += Format(
				"  EditedMessage: %s rows, %s repeat an earlier text of the same message\n",
				rows->front()[0].c_str(),
				rows->front()[1].c_str());
		}
	}
	out += "\n";
}

struct Probe {
	int64_t userId = 0;
	int64_t dialogId = 0;
	int64_t messageId = 0;
	int64_t dialogRows = 0;
};

[[nodiscard]] std::optional<Probe> FindProbe(sqlite3 *db, const std::string &table) {
	if (!HasColumns(db, table, {"userId", "dialogId", "messageId", "text", "date"})) {
		return std::nullopt;
	}
	const auto top = Query(
		db,
		"SELECT userId, dialogId, count(*) AS c FROM " + table +
		" GROUP BY userId, dialogId ORDER BY c DESC LIMIT 1");
	if (!top || top->empty()) {
		return std::nullopt;
	}
	auto probe = Probe();
	probe.userId = std::atoll((*top)[0][0].c_str());
	probe.dialogId = std::atoll((*top)[0][1].c_str());
	probe.dialogRows = std::atoll((*top)[0][2].c_str());
	probe.messageId = Scalar(
		db,
		Format(
			("SELECT messageId FROM " + table + " WHERE userId = %lld AND dialogId = %lld "
			"ORDER BY messageId LIMIT 1 OFFSET %lld").c_str(),
			(long long)probe.userId,
			(long long)probe.dialogId,
			(long long)(probe.dialogRows / 2)));
	return probe;
}

void AddPlansAndTimings(std::string &out, sqlite3 *db) {
	const auto deleted = TableName(db, "DeletedMessage");
	const auto edited = TableName(db, "EditedMessage");
	const auto probe = FindProbe(db, deleted);
	if (!probe) {
		out += "== Queries ==\n  no DeletedMessage table\n\n";
		return;
	}
	const auto user = std::to_string(probe->userId);
	const auto dialog = std::to_string(probe->dialogId);
	const auto message = std::to_string(probe->messageId);
	const auto window = "SELECT * FROM " + deleted + " WHERE userId = " + user
		+ " AND dialogId = " + dialog
		+ " AND messageId > " + message
		+ " ORDER BY messageId DESC LIMIT 50";
	const auto lookup = "SELECT * FROM " + deleted + " WHERE userId = " + user
		+ " AND dialogId = " + dialog
		+ " AND messageId = " + message + " LIMIT 1";
	const auto dialogs = "SELECT DISTINCT dialogId FROM " + deleted + " WHERE userId = " + user;
	const auto searchRare = "SELECT * FROM " + deleted + " WHERE userId = " + user
		+ " AND text LIKE '%zzqqxx%' ESCAPE '\\' ORDER BY date DESC LIMIT 100";
	const auto searchCommon = "SELECT * FROM " + deleted + " WHERE userId = " + user
		+ " AND text LIKE '%e%' ESCAPE '\\' ORDER BY date DESC LIMIT 100";
	const auto searchDialog = "SELECT * FROM " + deleted + " WHERE userId = " + user
		+ " AND dialogId = " + dialog
		+ " AND text LIKE '%zzqqxx%' ESCAPE '\\' ORDER BY date DESC LIMIT 100";
	const auto edits = "SELECT * FROM " + edited + " WHERE userId = " + user
		+ " AND dialogId = " + dialog
		+ " AND messageId = " + message;

	out += "== Query plans ==\n";
	AddPlan(out, db, "dialog window", window);
	AddPlan(out, db, "message lookup", lookup);
	AddPlan(out, db, "dialog list", dialogs);
	AddPlan(out, db, "search everywhere", searchRare);
	AddPlan(out, db, "search in a dialog", searchDialog);
	if (HasColumns(db, edited, {"userId", "dialogId", "messageId"})) {
		AddPlan(out, db, "message revisions", edits);
	}
	out += Format("  (the biggest dialog has %lld rows)\n\n", (long long)probe->dialogRows);

	out += "== Timings (everything is in memory, so this is the warm case) ==\n";
	AddTiming(out, db, "dialog window, 50 rows", window, 30);
	AddTiming(out, db, "message lookup", lookup, 2000);
	AddTiming(out, db, "dialog list", dialogs, 5);
	AddTiming(out, db, "search in the biggest dialog", searchDialog, 3);
	AddTiming(out, db, "search everywhere, no matches", searchRare, 3);
	AddTiming(out, db, "search everywhere, common letter", searchCommon, 3);
	if (HasColumns(db, edited, {"userId", "dialogId", "messageId"})) {
		AddTiming(out, db, "message revisions", edits, 2000);
	}
	out += "\n";
}

void AddSerialize(std::string &out, sqlite3 *db) {
	out += "== Snapshot ==\n";
	const auto from = Clock::now();
	auto size = sqlite3_int64();
	const auto data = sqlite3_serialize(db, "main", &size, 0);
	const auto elapsed = MsSince(from);
	if (!data) {
		out += "  sqlite3_serialize failed\n\n";
		return;
	}
	sqlite3_free(data);
	out += Format(
		"  sqlite3_serialize copy: %.1f MB in %.1f ms (the sealing and writing come on top)\n\n",
		Mb(size),
		elapsed);
}

} // namespace

std::string MeasureCrypto() {
#ifdef FRESH_REPORT_HAS_OPENSSL
	constexpr auto kPages = 20000;
	constexpr auto kNonce = 12;
	constexpr auto kTag = 16;
	auto plain = std::vector<unsigned char>(kPageSize, 0x5a);
	auto cipher = std::vector<unsigned char>(kPageSize);
	auto back = std::vector<unsigned char>(kPageSize);
	unsigned char key[32];
	RAND_bytes(key, sizeof(key));
	unsigned char nonce[kNonce];
	unsigned char tag[kTag];
	const auto context = EVP_CIPHER_CTX_new();
	if (!context) {
		return "  OpenSSL context failed\n";
	}
	auto length = 0;
	auto ok = true;

	auto from = Clock::now();
	for (auto i = 0; i != kPages && ok; ++i) {
		RAND_bytes(nonce, sizeof(nonce));
		ok = EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
			&& EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, kNonce, nullptr) == 1
			&& EVP_EncryptInit_ex(context, nullptr, nullptr, key, nonce) == 1
			&& EVP_EncryptUpdate(context, cipher.data(), &length, plain.data(), kPageSize) == 1
			&& EVP_EncryptFinal_ex(context, cipher.data() + length, &length) == 1
			&& EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, kTag, tag) == 1;
	}
	const auto encrypt = MsSince(from);

	from = Clock::now();
	for (auto i = 0; i != kPages && ok; ++i) {
		ok = EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
			&& EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, kNonce, nullptr) == 1
			&& EVP_DecryptInit_ex(context, nullptr, nullptr, key, nonce) == 1
			&& EVP_DecryptUpdate(context, back.data(), &length, cipher.data(), kPageSize) == 1
			&& EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, kTag, tag) == 1
			&& EVP_DecryptFinal_ex(context, back.data() + length, &length) == 1;
	}
	const auto decrypt = MsSince(from);
	EVP_CIPHER_CTX_free(context);
	if (!ok) {
		return "  AES-256-GCM benchmark failed\n";
	}
	const auto megabytes = Mb(int64_t(kPages) * kPageSize);
	return Format(
		"  AES-256-GCM, 4 KiB pages, random nonce: encrypt %.2f us/page (%.0f MB/s), "
		"decrypt+verify %.2f us/page (%.0f MB/s)\n",
		encrypt * 1000. / kPages,
		megabytes / (encrypt / 1000.),
		decrypt * 1000. / kPages,
		megabytes / (decrypt / 1000.));
#else
	return "  OpenSSL headers are unavailable\n";
#endif
}

std::string Build(sqlite3 *db, const Options &options) {
	auto out = std::string("freshGram database report\n\n");
	if (!db) {
		return out + "no database\n";
	}
	AddOverview(out, db);
	const auto tables = ListTables(db);
	AddObjects(out, db, tables);
	AddColumnStats(out, db, tables);
	AddDerived(out, db);
	AddPlansAndTimings(out, db);
	if (options.measureSerialize) {
		AddSerialize(out, db);
	}
	if (options.measureCrypto) {
		out += "== Crypto ==\n" + MeasureCrypto() + "\n";
	}
	return out;
}

} // namespace FreshDatabaseReport
