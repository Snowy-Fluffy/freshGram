#include "ayu/data/fresh_database_backup.h"

#include "ayu/ayu_settings.h"
#include "ayu/data/ayu_database.h"
#include "ayu/data/fresh_vault.h"
#include "ayu/libs/sqlite/sqlite3.h"
#include "base/timer.h"
#include "logs.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace FreshDatabaseBackup {
namespace {

constexpr auto kHour = crl::time(60 * 60 * 1000);
constexpr auto kBigBackup = qint64(40) * 1024 * 1024;
constexpr auto kBigBackupCount = 3;
constexpr auto kPurgeDelay = crl::time(5000);

std::atomic<bool> Running = false;
std::mutex BackupMutex;
// Nothing is rotated out until the settings are known.
std::atomic<bool> Configured = false;
std::atomic<int> KeepCount = 5;
std::atomic<int> IntervalHours = 6;
std::unique_ptr<base::Timer> PeriodicTimer;
std::atomic<bool> KeepRichest = true;
std::mutex PendingMutex;
QStringList PendingStatements;
std::atomic<bool> PurgeScheduled = false;
std::unique_ptr<base::Timer> PurgeTimer;

struct Entry {
	QString path;
	qint64 rows = 0;
};

// The database file, its change time tells whether there is a news since the last backup.
[[nodiscard]] QString MainPath() {
	return "./tdata/freshdata.sqlite";
}

// A restored backup is put here and the database is made from it at the start.
[[nodiscard]] QString RestorePath() {
	return "./tdata/freshdata.enc";
}

[[nodiscard]] QString Directory() {
	return "./tdata/fresh_backups";
}

struct Handle {
	sqlite3 *db = nullptr;

	~Handle() {
		if (db) {
			sqlite3_close(db);
		}
	}
};

[[nodiscard]] bool OpenMemory(Handle &handle, const QByteArray &plain) {
	if (sqlite3_open_v2(
			":memory:",
			&handle.db,
			SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
			nullptr) != SQLITE_OK) {
		return false;
	}
	const auto size = sqlite3_int64(plain.size());
	const auto buffer = static_cast<unsigned char*>(sqlite3_malloc64(size));
	if (!buffer) {
		return false;
	}
	memcpy(buffer, plain.constData(), size_t(size));
	return sqlite3_deserialize(
		handle.db,
		"main",
		buffer,
		size,
		size,
		SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE)
		== SQLITE_OK;
}

[[nodiscard]] bool Exec(sqlite3 *db, const QString &sql) {
	const auto utf8 = sql.toUtf8();
	return sqlite3_exec(db, utf8.constData(), nullptr, nullptr, nullptr)
		== SQLITE_OK;
}

[[nodiscard]] qint64 ScalarInt(sqlite3 *db, const QString &sql) {
	const auto utf8 = sql.toUtf8();
	sqlite3_stmt *statement = nullptr;
	if (sqlite3_prepare_v2(db, utf8.constData(), -1, &statement, nullptr)
		!= SQLITE_OK) {
		return -1;
	}
	auto result = qint64(-1);
	if (sqlite3_step(statement) == SQLITE_ROW) {
		result = sqlite3_column_int64(statement, 0);
	}
	sqlite3_finalize(statement);
	return result;
}

[[nodiscard]] bool QuickCheckOk(sqlite3 *db) {
	sqlite3_stmt *statement = nullptr;
	if (sqlite3_prepare_v2(db, "PRAGMA quick_check(1)", -1, &statement, nullptr)
		!= SQLITE_OK) {
		return false;
	}
	auto ok = false;
	if (sqlite3_step(statement) == SQLITE_ROW) {
		const auto text = sqlite3_column_text(statement, 0);
		ok = text && (QByteArray(reinterpret_cast<const char*>(text)) == "ok");
	}
	sqlite3_finalize(statement);
	return ok;
}

[[nodiscard]] bool TableExists(sqlite3 *db, const QString &name) {
	return ScalarInt(
		db,
		QString("SELECT count(*) FROM sqlite_master "
			"WHERE type='table' AND name='%1'").arg(name)) == 1;
}

[[nodiscard]] bool Verify(const QString &path, qint64 *rows = nullptr) {
	const auto plain = FreshVault::ReadFile(path, true);
	if (!plain || plain->size() < 15 || !plain->startsWith("SQLite format 3")) {
		return false;
	}
	auto handle = Handle();
	if (!OpenMemory(handle, *plain) || !QuickCheckOk(handle.db)) {
		return false;
	}
	auto total = qint64(0);
	for (const auto &table : { "EditedMessage", "DeletedMessage" }) {
		const auto compact = QString(table) + "V2";
		const auto name = TableExists(handle.db, compact)
			? compact
			: QString(table);
		if (!TableExists(handle.db, name)) {
			return false;
		}
		total += std::max<qint64>(
			0,
			ScalarInt(handle.db, QString("SELECT count(*) FROM %1").arg(name)));
	}
	if (rows) {
		*rows = total;
	}
	return true;
}

[[nodiscard]] std::vector<Entry> List() {
	auto names = QDir(Directory()).entryList(
		QStringList() << "freshdata_*.enc",
		QDir::Files,
		QDir::Name);
	std::reverse(names.begin(), names.end());
	auto result = std::vector<Entry>();
	for (const auto &name : names) {
		auto entry = Entry();
		entry.path = Directory() + '/' + name;
		const auto parts = name.chopped(4).split('_');
		if (parts.size() >= 4) {
			entry.rows = parts.back().toLongLong();
		}
		result.push_back(std::move(entry));
	}
	return result;
}

void ScrubSecrets(const QString &path) {
	const auto plain = FreshVault::ReadFile(path, true);
	auto handle = Handle();
	if (!plain || !OpenMemory(handle, *plain)) {
		return;
	}
	auto dirty = false;
	for (const auto &table : { "SecretMessage", "SecretChat", "SecretState" }) {
		if (TableExists(handle.db, table)
			&& ScalarInt(
				handle.db,
				QString("SELECT count(*) FROM %1").arg(table)) != 0) {
			dirty = true;
		}
	}
	if (!dirty) {
		return;
	}
	static_cast<void>(Exec(handle.db, QString("PRAGMA secure_delete = ON")));
	for (const auto &table : { "SecretMessage", "SecretChat", "SecretState" }) {
		if (TableExists(handle.db, table)) {
			static_cast<void>(
				Exec(handle.db, QString("DELETE FROM %1").arg(table)));
		}
	}
	static_cast<void>(Exec(handle.db, QString("VACUUM")));
	auto size = sqlite3_int64();
	const auto data = sqlite3_serialize(handle.db, "main", &size, 0);
	if (data) {
		FreshVault::WriteCompressedFile(path, data, qint64(size));
		sqlite3_free(data);
	}
}

void Rotate() {
	const auto list = List();
	auto richest = -1;
	for (auto i = 0; i != int(list.size()); ++i) {
		if (richest < 0 || list[i].rows > list[richest].rows) {
			richest = i;
		}
	}
	if (!Configured) {
		return;
	}
	// A big database keeps fewer copies, every one of them takes a lot of the disk.
	auto keep = KeepCount.load();
	if (!list.empty() && QFileInfo(list.front().path).size() > kBigBackup) {
		keep = std::min(keep, kBigBackupCount);
	}
	if (!KeepRichest) {
		richest = -1;
	}
	for (auto i = keep; i < int(list.size()); ++i) {
		if (i != richest) {
			QFile::remove(list[i].path);
		} else {
			ScrubSecrets(list[i].path);
		}
	}
}

[[nodiscard]] bool UpToDate() {
	const auto list = List();
	if (list.empty()) {
		return false;
	}
	const auto newest = QFileInfo(list.front().path).lastModified();
	const auto info = QFileInfo(MainPath());
	return !info.exists() || info.lastModified() <= newest;
}

} // namespace

bool create() {
	if (!QFile::exists(MainPath()) || UpToDate()) {
		return false;
	}
	auto expected = false;
	if (!Running.compare_exchange_strong(expected, true)) {
		return false;
	}
	const auto guard = gsl::finally([] { Running = false; });
	const auto lock = std::lock_guard(BackupMutex);
	QDir().mkpath(Directory());
	const auto temporary = Directory() + "/export.tmp";
	auto rows = qint64(0);
	if (!AyuDatabase::exportCompressed(temporary, rows)) {
		QFile::remove(temporary);
		return false;
	}
	const auto name = QString("%1/freshdata_%2_%3.enc")
		.arg(Directory())
		.arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss"))
		.arg(rows);
	if (!QFile::rename(temporary, name)) {
		QFile::remove(temporary);
		return false;
	}
	QFile::setPermissions(
		name,
		QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	Rotate();
	return true;
}

bool restore(int &index) {
	const auto list = List();
	for (auto i = index; i < int(list.size()); ++i) {
		if (!Verify(list[i].path)) {
			continue;
		}
		QFile::remove(RestorePath());
		if (QFile::copy(list[i].path, RestorePath())) {
			index = i + 1;
			LOG(("[freshGram] Database restored from '%1'.").arg(list[i].path));
			return true;
		}
	}
	index = int(list.size());
	return false;
}

void removeAll() {
	const auto lock = std::lock_guard(BackupMutex);
	QDir(Directory()).removeRecursively();
	QDir("./tdata/ayu_backups").removeRecursively();
	// Every copy put aside: old snapshots, damaged and failed imports.
	const auto moved = QDir("./tdata").entryList(
		QStringList() << "ayudata_*" << "freshdata_*",
		QDir::Files);
	for (const auto &name : moved) {
		QFile::remove("./tdata/" + name);
	}
	QFile::remove("./tdata/freshdata.sqlite.broken");
	QFile::remove("./tdata/fresh_db_report.txt");
}

namespace {

// Runs the statements on every copy, a copy that cannot be read is
// removed only when asked.
void Rewrite(const QStringList &statements, bool removeUnreadable) {
	const auto lock = std::lock_guard(BackupMutex);
	auto paths = QStringList();
	for (const auto &entry : List()) {
		paths.push_back(entry.path);
	}
	if (QFile::exists(RestorePath())) {
		paths.push_back(RestorePath());
	}
	for (const auto &path : paths) {
		const auto plain = FreshVault::ReadFile(path, true);
		auto handle = Handle();
		if (!plain || !OpenMemory(handle, *plain)) {
			if (removeUnreadable) {
				QFile::remove(path);
			}
			continue;
		}
		static_cast<void>(Exec(handle.db, QString("PRAGMA secure_delete = ON")));
		const auto before = sqlite3_total_changes(handle.db);
		for (const auto &statement : statements) {
			// A table missing in an old copy only fails its own statement.
			static_cast<void>(Exec(handle.db, statement));
		}
		if (sqlite3_total_changes(handle.db) == before) {
			continue;
		}
		static_cast<void>(Exec(handle.db, QString("VACUUM")));
		auto size = sqlite3_int64();
		const auto data = sqlite3_serialize(handle.db, "main", &size, 0);
		if (!data) {
			if (removeUnreadable) {
				QFile::remove(path);
			}
			continue;
		}
		const auto saved = FreshVault::WriteCompressedFile(
			path,
			data,
			qint64(size));
		sqlite3_free(data);
		if (!saved && removeUnreadable) {
			QFile::remove(path);
		}
	}
}

void RunPendingPurge() {
	auto statements = QStringList();
	{
		const auto lock = std::lock_guard(PendingMutex);
		statements = base::take(PendingStatements);
		PurgeScheduled = false;
	}
	if (!statements.isEmpty()) {
		Rewrite(statements, false);
	}
}

[[nodiscard]] QStringList MessageTables() {
	return {
		u"DeletedExtra"_q,
		u"DeletedMarkup"_q,
		u"DeletedRank"_q,
		u"DeletedMessageV2"_q,
		u"DeletedMessage"_q,
	};
}

} // namespace

void purgeUser(long long userId) {
	static const auto kTables = {
		"DeletedExtra", "DeletedMarkup", "DeletedRank", "DeletedMessage",
		"EditedMessage", "DeletedMessageV2", "EditedMessageV2",
		"DeletedDialog", "KeptTopic", "KeptDialog", "KnownUser", "LocalPin",
		"SecretChat", "SecretMessage", "SecretState", "PeekedStatus",
		"PeekRestore", "SpyMessageRead", "SpyMessageContentsRead",
		"ComposeStash",
	};
	auto statements = QStringList();
	for (const auto &table : kTables) {
		statements.push_back(QString(
			"DELETE FROM %1 WHERE userId = %2").arg(table).arg(userId));
	}
	Rewrite(statements, true);
}

void purgeSecrets() {
	crl::async([] {
		Rewrite({
			"DELETE FROM SecretMessage",
			"DELETE FROM SecretChat",
			"DELETE FROM SecretState",
		}, false);
	});
}

void setKeepRichest(bool keep) {
	KeepRichest = keep;
}

void purgeLater(const QStringList &statements) {
	if (statements.isEmpty()) {
		return;
	}
	{
		const auto lock = std::lock_guard(PendingMutex);
		PendingStatements += statements;
	}
	if (PurgeScheduled.exchange(true)) {
		return;
	}
	crl::on_main([] {
		if (!PurgeTimer) {
			PurgeTimer = std::make_unique<base::Timer>([] {
				crl::async(RunPendingPurge);
			});
		}
		PurgeTimer->callOnce(kPurgeDelay);
	});
}

QStringList OfMessage(
		long long userId,
		long long dialogId,
		long long messageId) {
	auto result = QStringList();
	for (const auto &table : MessageTables()) {
		result.push_back(QString(
			"DELETE FROM %1 WHERE userId = %2 AND dialogId = %3 AND messageId = %4"
		).arg(table).arg(userId).arg(dialogId).arg(messageId));
	}
	return result;
}

QStringList OfDialog(
		long long userId,
		long long dialogId,
		long long topicId) {
	auto result = QStringList();
	const auto where = QString("userId = %1 AND dialogId = %2")
		.arg(userId)
		.arg(dialogId);
	for (const auto &table : MessageTables()) {
		const auto messages = table.startsWith(u"DeletedMessage"_q);
		if (!topicId) {
			result.push_back(QString("DELETE FROM %1 WHERE %2").arg(table, where));
		} else if (messages) {
			result.push_back(QString("DELETE FROM %1 WHERE %2 AND topicId = %3")
				.arg(table, where)
				.arg(topicId));
		} else {
			result.push_back(QString(
				"DELETE FROM %1 WHERE %2 AND messageId IN "
				"(SELECT messageId FROM DeletedMessageV2 WHERE %2 AND topicId = %3)"
			).arg(table, where).arg(topicId));
		}
	}
	return result;
}

QStringList OlderThan(int cutoff) {
	auto result = QStringList();
	auto tables = MessageTables();
	tables.push_back(u"EditedMessageV2"_q);
	tables.push_back(u"EditedMessage"_q);
	for (const auto &table : tables) {
		result.push_back(QString("DELETE FROM %1 WHERE entityCreateDate < %2")
			.arg(table)
			.arg(cutoff));
	}
	return result;
}

void startPeriodic() {
	if (PeriodicTimer) {
		return;
	}
	PeriodicTimer = std::make_unique<base::Timer>([] {
		if (!Configured) {
			const auto &settings = AyuSettings::getInstance();
			configure(
				settings.backupKeepCount(),
				settings.backupIntervalHours());
		}
		crl::async([] {
			[[maybe_unused]] const auto created = create();
		});
	});
	PeriodicTimer->callEach(IntervalHours.load() * kHour);
}

void configure(int keepCount, int intervalHours) {
	Configured = true;
	KeepCount = keepCount;
	IntervalHours = intervalHours;
	if (PeriodicTimer) {
		PeriodicTimer->callEach(intervalHours * kHour);
	}
}

} // namespace FreshDatabaseBackup
