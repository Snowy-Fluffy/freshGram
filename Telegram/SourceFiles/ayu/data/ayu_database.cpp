// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/data/ayu_database.h"

#include "ayu/ayu_settings.h"
#include "ayu/data/fresh_database_backup.h"
#include "ayu/data/fresh_database_report.h"
#include "ayu/data/fresh_encrypted_vfs.h"
#include "ayu/data/fresh_legacy_copy.h"
#include "ayu/data/fresh_migration_v4.h"
#include "ayu/data/fresh_vault.h"
#include "ayu/utils/fresh_storage_migration.h"
#include "ayu/data/entities.h"
#include "ayu/data/message_rows.h"
#include "ayu/data/message_schema.h"
#include "ayu/data/messages_storage.h"
#include "ayu/libs/sqlite/sqlite3.h"
#include "ayu/libs/sqlite/sqlite_orm.h"
#include "ayu/utils/id_search.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>

#include <atomic>
#include <chrono>
#include <mutex>
#include <system_error>
#include <thread>

namespace {

sqlite3 *MemoryHandle = nullptr;

} // namespace

using namespace sqlite_orm;
auto storage = make_storage(
	"./tdata/freshdata.sqlite",
	make_table<SchemaVersion>(
		"SchemaVersion",
		make_column("id", &SchemaVersion::id, primary_key()),
		make_column("version", &SchemaVersion::version)
	),
	FreshMessageSchema::DeletedKeyIndex(),
	FreshMessageSchema::DeletedTopicIndex(),
	FreshMessageSchema::EditedKeyIndex(),
	FreshMessageSchema::DeletedTable(),
	FreshMessageSchema::EditedTable(),
	make_table<DeletedDialog>(
		"DeletedDialog",
		make_column("fakeId", &DeletedDialog::fakeId, primary_key().autoincrement()),
		make_column("userId", &DeletedDialog::userId),
		make_column("dialogId", &DeletedDialog::dialogId),
		make_column("peerId", &DeletedDialog::peerId),
		make_column("folderId", &DeletedDialog::folderId),
		make_column("topMessage", &DeletedDialog::topMessage),
		make_column("lastMessageDate", &DeletedDialog::lastMessageDate),
		make_column("flags", &DeletedDialog::flags),
		make_column("entityCreateDate", &DeletedDialog::entityCreateDate)
	),
	make_table<KeptDialog>(
		"KeptDialog",
		make_column("fakeId", &KeptDialog::fakeId, primary_key().autoincrement()),
		make_column("userId", &KeptDialog::userId),
		make_column("dialogId", &KeptDialog::dialogId),
		make_column("kind", &KeptDialog::kind),
		make_column("title", &KeptDialog::title),
		make_column("username", &KeptDialog::username),
		make_column("accessHash", &KeptDialog::accessHash),
		make_column("folderId", &KeptDialog::folderId),
		make_column("lastMessageDate", &KeptDialog::lastMessageDate),
		make_column("lost", &KeptDialog::lost)
	),
	make_table<DeletedMarkup>(
		"DeletedMarkup",
		make_column("fakeId", &DeletedMarkup::fakeId, primary_key().autoincrement()),
		make_column("userId", &DeletedMarkup::userId),
		make_column("dialogId", &DeletedMarkup::dialogId),
		make_column("messageId", &DeletedMarkup::messageId),
		make_column("markup", &DeletedMarkup::markup),
		make_column("entityCreateDate", &DeletedMarkup::entityCreateDate)
	),
	make_table<DeletedRank>(
		"DeletedRank",
		make_column("fakeId", &DeletedRank::fakeId, primary_key().autoincrement()),
		make_column("userId", &DeletedRank::userId),
		make_column("dialogId", &DeletedRank::dialogId),
		make_column("messageId", &DeletedRank::messageId),
		make_column("rank", &DeletedRank::rank),
		make_column("entityCreateDate", &DeletedRank::entityCreateDate)
	),
	make_table<ComposeStashRow>(
		"ComposeStash",
		make_column("fakeId", &ComposeStashRow::fakeId, primary_key().autoincrement()),
		make_column("userId", &ComposeStashRow::userId),
		make_column("dialogId", &ComposeStashRow::dialogId),
		make_column("topicRootId", &ComposeStashRow::topicRootId),
		make_column("monoforumPeerId", &ComposeStashRow::monoforumPeerId),
		make_column("payload", &ComposeStashRow::payload),
		make_column("entityCreateDate", &ComposeStashRow::entityCreateDate)
	),
	make_table<DeletedExtra>(
		"DeletedExtra",
		make_column("fakeId", &DeletedExtra::fakeId, primary_key().autoincrement()),
		make_column("userId", &DeletedExtra::userId),
		make_column("dialogId", &DeletedExtra::dialogId),
		make_column("messageId", &DeletedExtra::messageId),
		make_column("reactions", &DeletedExtra::reactions),
		make_column("repliesCount", &DeletedExtra::repliesCount),
		make_column("commentsChannelId", &DeletedExtra::commentsChannelId),
		make_column("commentsRootId", &DeletedExtra::commentsRootId),
		make_column("commentsReadTill", &DeletedExtra::commentsReadTill),
		make_column("commentsMaxId", &DeletedExtra::commentsMaxId),
		make_column("repliers", &DeletedExtra::repliers),
		make_column("entityCreateDate", &DeletedExtra::entityCreateDate)
	),
	make_table<KeptTopic>(
		"KeptTopic",
		make_column("fakeId", &KeptTopic::fakeId, primary_key().autoincrement()),
		make_column("userId", &KeptTopic::userId),
		make_column("dialogId", &KeptTopic::dialogId),
		make_column("rootId", &KeptTopic::rootId),
		make_column("title", &KeptTopic::title),
		make_column("colorId", &KeptTopic::colorId),
		make_column("iconId", &KeptTopic::iconId),
		make_column("creatorId", &KeptTopic::creatorId),
		make_column("date", &KeptTopic::date),
		make_column("flags", &KeptTopic::flags)
	),
	make_index("idx_known_user_userId_peerId",
			   column<KnownUser>(&KnownUser::userId),
			   column<KnownUser>(&KnownUser::peerId)),
	make_index("idx_secret_chat_userId_chatId",
			   column<SecretChatRow>(&SecretChatRow::userId),
			   column<SecretChatRow>(&SecretChatRow::chatId)),
	make_index("idx_secret_message_userId_chatId_randomId",
			   column<SecretMessageRow>(&SecretMessageRow::userId),
			   column<SecretMessageRow>(&SecretMessageRow::chatId),
			   column<SecretMessageRow>(&SecretMessageRow::randomId)),
	make_table<LocalPin>(
		"LocalPin",
		make_column("fakeId", &LocalPin::fakeId, primary_key().autoincrement()),
		make_column("userId", &LocalPin::userId),
		make_column("peerId", &LocalPin::peerId),
		make_column("position", &LocalPin::position)
	),
	make_table<KnownUser>(
		"KnownUser",
		make_column("fakeId", &KnownUser::fakeId, primary_key().autoincrement()),
		make_column("userId", &KnownUser::userId),
		make_column("peerId", &KnownUser::peerId),
		make_column("accessHash", &KnownUser::accessHash),
		make_column("firstName", &KnownUser::firstName),
		make_column("lastName", &KnownUser::lastName),
		make_column("username", &KnownUser::username),
		make_column("updatedAt", &KnownUser::updatedAt)
	),
	make_table<SecretChatRow>(
		"SecretChat",
		make_column("fakeId", &SecretChatRow::fakeId, primary_key().autoincrement()),
		make_column("userId", &SecretChatRow::userId),
		make_column("chatId", &SecretChatRow::chatId),
		make_column("accessHash", &SecretChatRow::accessHash),
		make_column("peerUserId", &SecretChatRow::peerUserId),
		make_column("creator", &SecretChatRow::creator),
		make_column("state", &SecretChatRow::state),
		make_column("keyData", &SecretChatRow::keyData),
		make_column("fingerprint", &SecretChatRow::fingerprint),
		make_column("myIn", &SecretChatRow::myIn),
		make_column("myOut", &SecretChatRow::myOut),
		make_column("hisIn", &SecretChatRow::hisIn),
		make_column("hisLayer", &SecretChatRow::hisLayer),
		make_column("date", &SecretChatRow::date),
		make_column("lastDate", &SecretChatRow::lastDate),
		make_column("unread", &SecretChatRow::unread)
	),
	make_table<SecretMessageRow>(
		"SecretMessage",
		make_column("fakeId", &SecretMessageRow::fakeId, primary_key().autoincrement()),
		make_column("userId", &SecretMessageRow::userId),
		make_column("chatId", &SecretMessageRow::chatId),
		make_column("randomId", &SecretMessageRow::randomId),
		make_column("outgoing", &SecretMessageRow::outgoing),
		make_column("date", &SecretMessageRow::date),
		make_column("kind", &SecretMessageRow::kind),
		make_column("seqIn", &SecretMessageRow::seqIn),
		make_column("seqOut", &SecretMessageRow::seqOut),
		make_column("text", &SecretMessageRow::text),
		make_column("payload", &SecretMessageRow::payload)
	),
	make_table<PeekedStatusRow>(
		"PeekedStatus",
		make_column("fakeId", &PeekedStatusRow::fakeId, primary_key().autoincrement()),
		make_column("userId", &PeekedStatusRow::userId),
		make_column("targetId", &PeekedStatusRow::targetId),
		make_column("kind", &PeekedStatusRow::kind),
		make_column("time", &PeekedStatusRow::time),
		make_column("checkedAt", &PeekedStatusRow::checkedAt)
	),
	make_table<PeekRestoreRow>(
		"PeekRestore",
		make_column("fakeId", &PeekRestoreRow::fakeId, primary_key().autoincrement()),
		make_column("userId", &PeekRestoreRow::userId),
		make_column("option", &PeekRestoreRow::option),
		make_column("flags", &PeekRestoreRow::flags),
		make_column("always", &PeekRestoreRow::always),
		make_column("never", &PeekRestoreRow::never)
	),
	make_table<SecretStateRow>(
		"SecretState",
		make_column("fakeId", &SecretStateRow::fakeId, primary_key().autoincrement()),
		make_column("userId", &SecretStateRow::userId),
		make_column("qts", &SecretStateRow::qts),
		make_column("date", &SecretStateRow::date)
	),
	make_table<RegexFilter>(
		"RegexFilter",
		make_column("id", &RegexFilter::id, primary_key()),
		make_column("text", &RegexFilter::text),
		make_column("enabled", &RegexFilter::enabled),
		make_column("reversed", &RegexFilter::reversed),
		make_column("caseInsensitive", &RegexFilter::caseInsensitive),
		make_column("dialogId", &RegexFilter::dialogId)
	),
	make_table<RegexFilterGlobalExclusion>(
		"RegexFilterGlobalExclusion",
		make_column("fakeId", &RegexFilterGlobalExclusion::fakeId, primary_key().autoincrement()),
		make_column("dialogId", &RegexFilterGlobalExclusion::dialogId),
		make_column("filterId", &RegexFilterGlobalExclusion::filterId)
	),
	make_table<SpyMessageRead>(
		"SpyMessageRead",
		make_column("fakeId", &SpyMessageRead::fakeId, primary_key().autoincrement()),
		make_column("userId", &SpyMessageRead::userId),
		make_column("dialogId", &SpyMessageRead::dialogId),
		make_column("messageId", &SpyMessageRead::messageId),
		make_column("entityCreateDate", &SpyMessageRead::entityCreateDate)
	),
	make_table<SpyMessageContentsRead>(
		"SpyMessageContentsRead",
		make_column("fakeId", &SpyMessageContentsRead::fakeId, primary_key().autoincrement()),
		make_column("userId", &SpyMessageContentsRead::userId),
		make_column("dialogId", &SpyMessageContentsRead::dialogId),
		make_column("messageId", &SpyMessageContentsRead::messageId),
		make_column("entityCreateDate", &SpyMessageContentsRead::entityCreateDate)
	)
);

namespace {

std::recursive_mutex DatabaseMutex;
std::atomic<bool> DatabaseReady = false;
std::atomic<int> PurgeDays = 0;
bool FinishLegacyMigrationPending = false;

constexpr auto kBusyTimeoutMs = 5000;
constexpr auto kSqliteBusy = 5;
constexpr auto kSqliteLocked = 6;
constexpr auto kSqliteCorrupt = 11;
constexpr auto kSqliteNotADatabase = 26;
constexpr auto kLatestVersion = 4;
constexpr auto kQueueDelay = crl::time(1500);
constexpr auto kQueueAttempts = 3;
constexpr auto kPurgeChunk = 2000;
constexpr auto kPurgeEach = crl::time(6 * 60 * 60 * 1000);

[[nodiscard]] QString snapshotPath() {
	return "./tdata/freshdata.enc";
}

[[nodiscard]] QString legacyPath() {
	return "./tdata/ayudata.db";
}

struct QueuedEdit {
	EditedMessage message;
	int maxRevisions = 0;
};

// What waits for the next write, the writes are put together to save the time of the disk.
struct QueuedWrites {
	std::vector<DeletedMessage> deleted;
	std::vector<DeletedExtra> extras;
	std::vector<DeletedMarkup> markups;
	std::vector<DeletedRank> ranks;
	std::vector<QueuedEdit> edited;
	int attempts = 0;

	[[nodiscard]] bool empty() const {
		return deleted.empty()
			&& extras.empty()
			&& markups.empty()
			&& ranks.empty()
			&& edited.empty();
	}
};

std::mutex QueueMutex;
QueuedWrites Queue;
std::atomic<bool> QueueFilled = false;
std::atomic<bool> QueueScheduled = false;
std::unique_ptr<base::Timer> QueueTimer;
std::unique_ptr<base::Timer> PurgeTimer;

void drainQueuedLocked();
void flushQueuedLater();
void trimRevisions(const EditedMessage &message, int maxRevisions);

template<typename Callback>
void enqueue(Callback &&callback) {
	if (!DatabaseReady.load()) {
		return;
	}
	auto schedule = false;
	{
		const auto lock = std::lock_guard(QueueMutex);
		callback(Queue);
		QueueFilled = true;
		schedule = !QueueScheduled.exchange(true);
	}
	if (schedule) {
		flushQueuedLater();
	}
}

template<typename Value>
void prependTo(std::vector<Value> &to, std::vector<Value> &&from) {
	from.insert(
		from.end(),
		std::make_move_iterator(to.begin()),
		std::make_move_iterator(to.end()));
	to = std::move(from);
}

// The writes that failed go back before the new ones, a few times only.
void requeue(QueuedWrites &&batch) {
	if (++batch.attempts >= kQueueAttempts) {
		LOG(("[freshGram] Database: dropped %1 deleted and %2 edited messages after %3 attempts.")
			.arg(batch.deleted.size())
			.arg(batch.edited.size())
			.arg(batch.attempts));
		return;
	}
	auto schedule = false;
	{
		const auto lock = std::lock_guard(QueueMutex);
		prependTo(Queue.deleted, std::move(batch.deleted));
		prependTo(Queue.extras, std::move(batch.extras));
		prependTo(Queue.markups, std::move(batch.markups));
		prependTo(Queue.ranks, std::move(batch.ranks));
		prependTo(Queue.edited, std::move(batch.edited));
		Queue.attempts = std::max(Queue.attempts, batch.attempts);
		QueueFilled = true;
		schedule = !QueueScheduled.exchange(true);
	}
	if (schedule) {
		flushQueuedLater();
	}
}

[[nodiscard]] std::vector<DeletedMessage> toMessages(const std::vector<DeletedMessageRow> &rows) {
	auto result = std::vector<DeletedMessage>();
	result.reserve(rows.size());
	for (const auto &row : rows) {
		result.push_back(FreshMessageRows::FromRow(row));
	}
	return result;
}

[[nodiscard]] std::vector<EditedMessage> toMessages(const std::vector<EditedMessageRow> &rows) {
	auto result = std::vector<EditedMessage>();
	result.reserve(rows.size());
	for (const auto &row : rows) {
		result.push_back(FreshMessageRows::FromRow(row));
	}
	return result;
}

template<typename Result, typename Callback>
Result run(const char *what, Result fallback, Callback &&callback) {
	std::lock_guard lock(DatabaseMutex);
	if (!DatabaseReady) {
		return fallback;
	}
	try {
		drainQueuedLocked();
		Result result = callback();
		return result;
	} catch (const std::exception &ex) {
		LOG(("[freshGram] Database: failed to %1: %2").arg(QString::fromUtf8(what), QString::fromUtf8(ex.what())));
	} catch (...) {
		LOG(("[freshGram] Database: failed to %1.").arg(QString::fromUtf8(what)));
	}
	return fallback;
}

template<typename Callback>
void runVoid(const char *what, Callback &&callback) {
	run<bool>(what, false, [&] {
		callback();
		return true;
	});
}

template<typename Callback>
void inTransaction(Callback &&callback) {
	storage.begin_transaction();
	try {
		callback();
		storage.commit();
	} catch (...) {
		try {
			storage.rollback();
		} catch (...) {
		}
		throw;
	}
}

// Gives the free pages of the file back to the disk, a part of them at a time.
void reclaimSpace() {
	if (MemoryHandle) {
		sqlite3_exec(MemoryHandle, "PRAGMA incremental_vacuum(4096)", nullptr, nullptr, nullptr);
	}
}

// WHY: the deleted pages are already overwritten by secure_delete, the
// whole file does not need a VACUUM that blocks for long.
void reclaimAllSpace() {
	if (MemoryHandle) {
		sqlite3_exec(MemoryHandle, "PRAGMA incremental_vacuum", nullptr, nullptr, nullptr);
	}
}

void flushQueuedLater() {
	crl::on_main([] {
		if (!QueueTimer) {
			QueueTimer = std::make_unique<base::Timer>([] {
				crl::async([] {
					AyuDatabase::flush();
				});
			});
		}
		QueueTimer->callOnce(kQueueDelay);
	});
}

void drainQueuedLocked() {
	if (!QueueFilled.load()) {
		return;
	}
	auto batch = QueuedWrites();
	{
		const auto lock = std::lock_guard(QueueMutex);
		batch = std::move(Queue);
		Queue = QueuedWrites();
		QueueFilled = false;
		QueueScheduled = false;
	}
	if (batch.empty() || !DatabaseReady.load()) {
		return;
	}
	try {
		inTransaction([&] {
			for (const auto &message : batch.deleted) {
				const auto exists = storage.count<DeletedMessageRow>(
					where(
						column<DeletedMessageRow>(&DeletedMessageRow::userId) == message.userId and
						column<DeletedMessageRow>(&DeletedMessageRow::dialogId) == message.dialogId and
						column<DeletedMessageRow>(&DeletedMessageRow::messageId) == message.messageId
					)
				) > 0;
				if (!exists) {
					auto row = FreshMessageRows::ToRow(message);
					row.fakeId = 0;
					storage.insert(row);
				}
			}
			for (const auto &extra : batch.extras) {
				storage.remove_all<DeletedExtra>(
					where(
						column<DeletedExtra>(&DeletedExtra::userId) == extra.userId and
						column<DeletedExtra>(&DeletedExtra::dialogId) == extra.dialogId and
						column<DeletedExtra>(&DeletedExtra::messageId) == extra.messageId
					)
				);
				storage.insert(extra);
			}
			for (const auto &markup : batch.markups) {
				storage.remove_all<DeletedMarkup>(
					where(
						column<DeletedMarkup>(&DeletedMarkup::userId) == markup.userId and
						column<DeletedMarkup>(&DeletedMarkup::dialogId) == markup.dialogId and
						column<DeletedMarkup>(&DeletedMarkup::messageId) == markup.messageId
					)
				);
				storage.insert(markup);
			}
			for (const auto &rank : batch.ranks) {
				storage.remove_all<DeletedRank>(
					where(
						column<DeletedRank>(&DeletedRank::userId) == rank.userId and
						column<DeletedRank>(&DeletedRank::dialogId) == rank.dialogId and
						column<DeletedRank>(&DeletedRank::messageId) == rank.messageId
					)
				);
				storage.insert(rank);
			}
			for (const auto &edit : batch.edited) {
				const auto &message = edit.message;
				const auto saved = storage.count<EditedMessageRow>(
					where(
						column<EditedMessageRow>(&EditedMessageRow::userId) == message.userId and
						column<EditedMessageRow>(&EditedMessageRow::dialogId) == message.dialogId and
						column<EditedMessageRow>(&EditedMessageRow::messageId) == message.messageId
					));
				auto row = FreshMessageRows::ToRow(message);
				row.fakeId = 0;
				storage.insert(row);
				if (edit.maxRevisions && saved >= edit.maxRevisions) {
					trimRevisions(message, edit.maxRevisions);
				}
			}
		});
	} catch (const std::exception &ex) {
		LOG(("[freshGram] Database: failed to save the queued messages: %1").arg(QString::fromUtf8(ex.what())));
		requeue(std::move(batch));
	} catch (...) {
		LOG(("[freshGram] Database: failed to save the queued messages."));
		requeue(std::move(batch));
	}
}

// The original text stays, the oldest edits after it make room for the new ones.
void trimRevisions(const EditedMessage &message, int maxRevisions) {
	const auto ids = storage.select(
		&EditedMessageRow::fakeId,
		where(
			column<EditedMessageRow>(&EditedMessageRow::userId) == message.userId and
			column<EditedMessageRow>(&EditedMessageRow::dialogId) == message.dialogId and
			column<EditedMessageRow>(&EditedMessageRow::messageId) == message.messageId
		),
		order_by(&EditedMessageRow::fakeId));
	const auto keep = std::max(maxRevisions, 1);
	if (int(ids.size()) <= keep) {
		return;
	}
	const auto removeTill = int(ids.size()) - (keep - 1);
	for (auto i = 1; i < removeTill; ++i) {
		storage.remove<EditedMessageRow>(ids[i]);
	}
}

[[nodiscard]] double MsSince(std::chrono::steady_clock::time_point from) {
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - from).count();
}

[[nodiscard]] QString sqlitePath() {
	return "./tdata/freshdata.sqlite";
}

[[nodiscard]] QString dirtyPath() {
	return "./tdata/freshdata.sqlite.dirty";
}

[[nodiscard]] QString brokenPath() {
	return "./tdata/freshdata.sqlite.broken";
}

// Only the newest copy put aside after a damage is kept.
void pruneMovedCopies() {
	static const auto moved = QRegularExpression(
		u"^freshdata_\\d+\\.(sqlite|enc)$"_q);
	auto list = QDir("./tdata").entryInfoList(
		QStringList() << "freshdata_*",
		QDir::Files,
		QDir::Time);
	for (const auto &extension : { u"sqlite"_q, u"enc"_q }) {
		auto kept = false;
		for (const auto &info : list) {
			if (!moved.match(info.fileName()).hasMatch()
				|| info.suffix() != extension) {
				continue;
			} else if (!kept) {
				kept = true;
				continue;
			}
			QFile::remove(info.absoluteFilePath());
			QFile::remove(info.absoluteFilePath() + "-journal");
		}
	}
}

void renameDatabaseFiles(const QString &suffix, bool withMainFile) {
	for (const auto &extension : {QString(), QString("-shm"), QString("-wal")}) {
		const auto from = legacyPath() + extension;
		if (QFile::exists(from)) {
			QFile::rename(from, QString("./tdata/ayudata_%1.db%2").arg(suffix, extension));
		}
	}
	if (QFile::exists(snapshotPath())) {
		QFile::rename(snapshotPath(), QString("./tdata/freshdata_%1.enc").arg(suffix));
	}
	if (withMainFile) {
		for (const auto &extension : {QString(), QString("-journal")}) {
			const auto from = sqlitePath() + extension;
			if (QFile::exists(from)) {
				QFile::rename(from, QString("./tdata/freshdata_%1.sqlite%2").arg(suffix, extension));
			}
		}
		QFile::remove(dirtyPath());
	}
	pruneMovedCopies();
}

void copyDatabaseFiles(const QString &suffix) {
	if (QFile::exists(snapshotPath())) {
		QFile::copy(snapshotPath(), QString("./tdata/freshdata_%1.enc").arg(suffix));
	}
}

// The copy of the snapshot before the compact tables is only needed for a while.
void pruneMigrationBackup() {
	const auto path = QString("./tdata/freshdata_premigration_v4.enc");
	const auto info = QFileInfo(path);
	if (info.exists() && info.lastModified().daysTo(QDateTime::currentDateTime()) >= 14) {
		QFile::remove(path);
	}
}

[[noreturn]] void throwSqlite(int code) {
	throw std::system_error(
		std::error_code(code, sqlite_orm::get_sqlite_error_category()));
}

struct Statement {
	sqlite3_stmt *raw = nullptr;

	Statement(const Statement &) = delete;
	Statement &operator=(const Statement &) = delete;
	Statement(const char *sql) {
		if (sqlite3_prepare_v2(MemoryHandle, sql, -1, &raw, nullptr) != SQLITE_OK) {
			throwSqlite(sqlite3_extended_errcode(MemoryHandle));
		}
	}
	~Statement() {
		sqlite3_finalize(raw);
	}
	Statement &bind(int index, sqlite3_int64 value) {
		sqlite3_bind_int64(raw, index, value);
		return *this;
	}
	[[nodiscard]] bool next() {
		const auto code = sqlite3_step(raw);
		if (code == SQLITE_ROW) {
			return true;
		} else if (code != SQLITE_DONE) {
			throwSqlite(sqlite3_extended_errcode(MemoryHandle));
		}
		return false;
	}
	[[nodiscard]] sqlite3_int64 value(int column) const {
		return sqlite3_column_int64(raw, column);
	}
	int run() {
		while (next()) {
		}
		return sqlite3_changes(MemoryHandle);
	}
};

[[nodiscard]] QString ListOf(const std::vector<ID> &ids) {
	auto result = QStringList();
	result.reserve(int(ids.size()));
	for (const auto id : ids) {
		result.push_back(QString::number(id));
	}
	return result.join(',');
}

[[nodiscard]] bool DeleteSavedFiles() {
	return AyuSettings::getInstance().deleteSavedFiles();
}

void sealLegacyCopies(const QString &directory, const QString &pattern) {
	for (const auto &info : QDir(directory).entryInfoList(QStringList() << pattern, QDir::Files)) {
		if (const auto plain = FreshVault::ReadFile(info.absoluteFilePath())) {
			auto name = info.completeBaseName();
			if (name.startsWith(u"ayudata_"_q)) {
				name = u"freshdata_"_q + name.mid(8);
			}
			const auto target = info.dir().filePath(name + ".enc");
			if (FreshVault::WriteSealedFile(target, *plain)) {
				QFile::remove(info.absoluteFilePath());
				QFile::remove(info.absoluteFilePath() + "-wal");
				QFile::remove(info.absoluteFilePath() + "-shm");
			}
		}
	}
}

void finishLegacyMigration() {
	for (const auto &extension : {QString(), QString("-shm"), QString("-wal")}) {
		QFile::remove(legacyPath() + extension);
	}
	sealLegacyCopies("./tdata", "ayudata_*.db");
	sealLegacyCopies("./tdata/fresh_backups", "freshdata_*.db");
}

}

namespace AyuMigrations {

void migrateToV1(decltype(storage) &storage) {
	// drop RegexFilter table as we've added primary_key()
	try {
		storage.drop_table_if_exists("RegexFilter");
		LOG(("Migration to V1 successful."));
	} catch (const std::exception &ex) {
		LOG(("Migration to V1 failed: %1").arg(ex.what()));
	}
}

void migrateToV2(decltype(storage) &) {
	// The duplicates are dropped when the compact tables are filled in V4.
	LOG(("Migration to V2 successful."));
}

void migrateToV3(decltype(storage) &storage) {
	storage.remove_all<KeptDialog>(
		where(column<KeptDialog>(&KeptDialog::lost) == 0));
	LOG(("Migration to V3 successful."));
}

void migrateToV4(decltype(storage) &) {
	// The compact tables are created and filled when the database file is made.
	const auto result = FreshMigrationV4::Run(MemoryHandle);
	if (!result.ok) {
		LOG(("Migration to V4 failed: %1").arg(QString::fromStdString(result.error)));
		throw std::runtime_error(result.error);
	}
}

}

void runMigrations(decltype(storage) &storage) {
	const std::map<int, Fn<void(decltype(storage) &)>> migrations = {
		{1, AyuMigrations::migrateToV1},
		{2, AyuMigrations::migrateToV2},
		{3, AyuMigrations::migrateToV3},
		{4, AyuMigrations::migrateToV4},
	};

	int currentVersion = 0;
	try {
		if (auto versionRow = storage.get_pointer<SchemaVersion>(1)) {
			currentVersion = versionRow->version;
		} else {
			storage.insert(SchemaVersion{1, 0});
		}
	} catch (...) {
		LOG(("No SchemaVersion, assuming 0"));
		storage.insert(SchemaVersion{1, 0});
	}

	if (currentVersion >= kLatestVersion) {
		LOG(("Database is ok"));
		return;
	}

	LOG(("Database version: %1. Latest version: %2.").arg(currentVersion).arg(kLatestVersion));

	for (int v = currentVersion + 1; v <= kLatestVersion; ++v) {
		if (migrations.contains(v)) {
			try {
				LOG(("Migration for version: %1").arg(v));
				storage.begin_transaction();

				migrations.at(v)(storage);

				storage.update_all(set(c(&SchemaVersion::version) = v), where(c(&SchemaVersion::id) == 1));
				storage.commit();
				LOG(("Applied migration for version: %1.").arg(v));
			} catch (...) {
				try {
					storage.rollback();
				} catch (...) {
				}
				LOG(("Failed to apply migration for version: %1.").arg(v));
				AyuDatabase::backupCurrentDatabase();

				return;
			}
		}
	}
}

namespace AyuDatabase {

void moveCurrentDatabase() {
	// An open database file stays where it is, only the files to import move.
	renameDatabaseFiles(QString::number(base::unixtime::now()), !storage.is_opened());
}

void backupCurrentDatabase() {
	copyDatabaseFiles(QString("backup_%1").arg(base::unixtime::now()));
}

namespace {

constexpr auto kCacheMinimum = qint64(16) * 1024 * 1024;
constexpr auto kCacheMaximum = qint64(128) * 1024 * 1024;

// The cache follows the size of the file, a small database stays in memory.
[[nodiscard]] int cacheKilobytes() {
	const auto size = QFileInfo(sqlitePath()).size();
	return int(std::clamp(size / 4, kCacheMinimum, kCacheMaximum) / 1024);
}

void setupConnection(sqlite3 *db) {
	MemoryHandle = db;
	const auto exec = [&](const char *sql) {
		sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
	};
	// The page size and the reserved bytes are fixed when the file is created.
	exec("PRAGMA page_size = 4096");
	auto reserve = FreshEncryptedVfs::kReserveBytes;
	sqlite3_file_control(db, "main", SQLITE_FCNTL_RESERVE_BYTES, &reserve);
	exec("PRAGMA auto_vacuum = INCREMENTAL");
	exec("PRAGMA journal_mode = TRUNCATE");
	exec("PRAGMA synchronous = NORMAL");
	exec("PRAGMA locking_mode = EXCLUSIVE");
	exec("PRAGMA mmap_size = 0");
	exec("PRAGMA secure_delete = ON");
	exec("PRAGMA temp_store = MEMORY");
	const auto cache = QString("PRAGMA cache_size = -%1").arg(cacheKilobytes()).toUtf8();
	exec(cache.constData());
}

void registerEncryptedFiles() {
	unsigned char key[32];
	if (!FreshVault::DeriveKey("database-v2", key, sizeof(key))
		|| !FreshEncryptedVfs::Register(key, sizeof(key), sqlitePath().toUtf8().constData())) {
		throw std::runtime_error("The database key is unavailable.");
	}
}

// Opens the file the way the storage will, so a wrong key or damage is seen early.
void checkMainFile() {
	const auto info = QFileInfo(sqlitePath());
	if (!info.exists() || !info.size()) {
		return;
	}
	const auto utf8 = sqlitePath().toUtf8();
	sqlite3 *db = nullptr;
	if (sqlite3_open_v2(utf8.constData(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
		const auto code = db ? sqlite3_extended_errcode(db) : SQLITE_CANTOPEN;
		sqlite3_close(db);
		throwSqlite(code);
	}
	sqlite3_busy_timeout(db, kBusyTimeoutMs);
	auto code = sqlite3_exec(db, "SELECT count(*) FROM sqlite_master", nullptr, nullptr, nullptr);
	if (code == SQLITE_OK && QFile::exists(dirtyPath())) {
		// The last session did not finish properly, look at the whole file.
		sqlite3_stmt *statement = nullptr;
		code = sqlite3_prepare_v2(db, "PRAGMA quick_check(1)", -1, &statement, nullptr);
		if (code == SQLITE_OK) {
			code = sqlite3_step(statement);
			const auto text = (code == SQLITE_ROW)
				? sqlite3_column_text(statement, 0)
				: nullptr;
			code = (text && QByteArray(reinterpret_cast<const char*>(text)) == "ok")
				? SQLITE_OK
				: kSqliteCorrupt;
		}
		sqlite3_finalize(statement);
	}
	sqlite3_close(db);
	if (code != SQLITE_OK) {
		throwSqlite(code);
	}
}

void execOrThrow(sqlite3 *db, const char *sql) {
	char *error = nullptr;
	const auto code = sqlite3_exec(db, sql, nullptr, nullptr, &error);
	if (code != SQLITE_OK) {
		const auto message = error ? std::string(error) : std::string(sqlite3_errmsg(db));
		sqlite3_free(error);
		throw std::runtime_error(message);
	}
}

// Fills the new file from the attached old database in one transaction.
// The transaction is deferred: an immediate one locks every attached schema
// for writing, and the read-only old snapshot refuses that.
void copyAttached(sqlite3 *db) {
	execOrThrow(db, "BEGIN");
	const auto result = FreshLegacyCopy::Run(db, "legacy");
	if (!result.ok) {
		sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
		throw std::runtime_error(result.error);
	}
	try {
		const auto version = QString("INSERT OR REPLACE INTO SchemaVersion (id, version) VALUES (1, %1)")
			.arg(kLatestVersion).toUtf8();
		execOrThrow(db, version.constData());
		execOrThrow(db, "COMMIT");
	} catch (...) {
		sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
		throw;
	}
	LOG(("[freshGram] Database: imported %1 deleted and %2 edited messages and %3 other rows in %4 ms.")
		.arg(result.deleted)
		.arg(result.edited)
		.arg(result.others)
		.arg(result.milliseconds, 0, 'f', 0));
}

void importSnapshot() {
	const auto started = std::chrono::steady_clock::now();
	const auto plain = FreshVault::ReadFile(snapshotPath(), true);
	if (!plain || plain->isEmpty()) {
		throwSqlite(kSqliteNotADatabase);
	}
	execOrThrow(MemoryHandle, "ATTACH DATABASE ':memory:' AS legacy");
	const auto size = sqlite3_int64(plain->size());
	// The old image is read in place, nothing of it is changed or copied.
	const auto code = sqlite3_deserialize(
		MemoryHandle,
		"legacy",
		reinterpret_cast<unsigned char*>(const_cast<char*>(plain->constData())),
		size,
		size,
		SQLITE_DESERIALIZE_READONLY);
	if (code != SQLITE_OK) {
		sqlite3_exec(MemoryHandle, "DETACH DATABASE legacy", nullptr, nullptr, nullptr);
		throwSqlite(code);
	}
	try {
		copyAttached(MemoryHandle);
	} catch (...) {
		sqlite3_exec(MemoryHandle, "DETACH DATABASE legacy", nullptr, nullptr, nullptr);
		throw;
	}
	sqlite3_exec(MemoryHandle, "DETACH DATABASE legacy", nullptr, nullptr, nullptr);
	LOG(("[freshGram] Database: moved the snapshot, %1 MB, in %2 ms.")
		.arg(size / 1048576.0, 0, 'f', 1)
		.arg(MsSince(started), 0, 'f', 0));
	// The old image is kept for a while in case something is wrong with the new file.
	QFile::remove("./tdata/freshdata_premigration_v4.enc");
	QFile::rename(snapshotPath(), "./tdata/freshdata_premigration_v4.enc");
}

void importLegacyFile() {
	const auto started = std::chrono::steady_clock::now();
	{
		// The old file may keep its changes in the log, this puts them together.
		sqlite3 *legacy = nullptr;
		const auto path = legacyPath().toUtf8();
		if (sqlite3_open_v2(path.constData(), &legacy, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
			const auto code = legacy ? sqlite3_extended_errcode(legacy) : SQLITE_CANTOPEN;
			sqlite3_close(legacy);
			throwSqlite(code);
		}
		sqlite3_busy_timeout(legacy, kBusyTimeoutMs);
		sqlite3_exec(legacy, "PRAGMA journal_mode = DELETE", nullptr, nullptr, nullptr);
		sqlite3_close(legacy);
	}
	const auto attach = QString("ATTACH DATABASE '%1' AS legacy")
		.arg(legacyPath().replace('\'', "''")).toUtf8();
	execOrThrow(MemoryHandle, attach.constData());
	try {
		copyAttached(MemoryHandle);
	} catch (...) {
		sqlite3_exec(MemoryHandle, "DETACH DATABASE legacy", nullptr, nullptr, nullptr);
		throw;
	}
	sqlite3_exec(MemoryHandle, "DETACH DATABASE legacy", nullptr, nullptr, nullptr);
	LOG(("[freshGram] Database: imported the legacy database in %1 ms.").arg(MsSince(started), 0, 'f', 0));
	FinishLegacyMigrationPending = true;
}

// An import that fails must not leave the database unusable in every session:
// the old file is kept aside untouched and the new one starts empty. A damaged
// file is still reported to the caller, which restores a backup.
template <typename Import>
void importOrKeepAside(Import &&import, const QString &path) {
	try {
		import();
	} catch (const std::system_error &) {
		throw;
	} catch (const std::exception &ex) {
		const auto aside = QString("./tdata/freshdata_import_failed_%1.%2")
			.arg(base::unixtime::now())
			.arg(QFileInfo(path).suffix());
		LOG(("[freshGram] Database: failed to import %1, it is kept as %2: %3")
			.arg(path, aside, QString::fromUtf8(ex.what())));
		if (!QFile::rename(path, aside)) {
			throw;
		}
		storage.insert(SchemaVersion{1, kLatestVersion});
	}
}

void prepareStorage() {
	registerEncryptedFiles();
	if (!storage.is_opened()) {
		if (QFile::exists(brokenPath())) {
			QFile::remove(brokenPath());
			LOG(("[freshGram] Database: the file was marked as damaged."));
			throwSqlite(kSqliteCorrupt);
		}
		checkMainFile();
		storage.on_open = setupConnection;
		storage.open_forever();
	}
	if (!MemoryHandle) {
		throw std::runtime_error("The database handle is missing.");
	}
	storage.sync_schema(true);

	// A database without the version was just made or the import did not finish.
	auto fresh = false;
	try {
		fresh = !storage.get_pointer<SchemaVersion>(1);
	} catch (...) {
		fresh = true;
	}
	if (fresh) {
		if (QFile::exists(snapshotPath())) {
			importOrKeepAside(importSnapshot, snapshotPath());
		} else if (QFile::exists(legacyPath())) {
			importOrKeepAside(importLegacyFile, legacyPath());
		} else {
			storage.insert(SchemaVersion{1, kLatestVersion});
		}
	} else {
		runMigrations(storage);
	}
	storage.sync_schema(true);
}

void touchDirtyMarker(bool dirty) {
	if (dirty) {
		auto file = QFile(dirtyPath());
		if (file.open(QIODevice::WriteOnly)) {
			file.close();
		}
	} else {
		QFile::remove(dirtyPath());
	}
}

}

void initialize() {
	std::lock_guard lock(DatabaseMutex);
	const auto &domain = Core::App().domain();
	const auto key = domain.started() ? domain.local().localKey() : nullptr;
	if (!key) {
		LOG(("[freshGram] Database: the local key is unavailable."));
		return;
	}
	FreshVault::SetKey(key);
	if (DatabaseReady) {
		return;
	}
	FreshStorage::MigrateLegacyNames();
	if (!FreshStorage::DatabaseMigrated()) {
		LOG(("[freshGram] Database is unavailable, messages will not be saved in this session."));
		return;
	}

	auto retriedBusy = false;
	auto resetCorrupted = false;
	auto restoreIndex = 0;
	while (true) {
		try {
			prepareStorage();
			DatabaseReady = true;
			touchDirtyMarker(true);
			if (FinishLegacyMigrationPending) {
				FinishLegacyMigrationPending = false;
				finishLegacyMigration();
			}
			crl::async([] {
				FreshVault::SealFilesIn("./tdata/fresh_media");
				pruneMigrationBackup();
				[[maybe_unused]] const auto created = FreshDatabaseBackup::create();
			});
			FreshDatabaseBackup::startPeriodic();
			if (const auto days = PurgeDays.load()) {
				purgeOlderThan(days);
			}
			if (!PurgeTimer) {
				PurgeTimer = std::make_unique<base::Timer>([] {
					if (const auto days = PurgeDays.load()) {
						purgeOlderThan(days);
					}
				});
				PurgeTimer->callEach(kPurgeEach);
			}
			return;
		} catch (const std::system_error &ex) {
			const auto code = ex.code().value() & 0xFF;
			const auto sqliteError = (ex.code().category() == sqlite_orm::get_sqlite_error_category());
			LOG(("[freshGram] Database initialization failed (%1): %2").arg(code).arg(ex.what()));
			if (sqliteError && (code == kSqliteBusy || code == kSqliteLocked) && !retriedBusy) {
				retriedBusy = true;
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				continue;
			}
			if (sqliteError && (code == kSqliteCorrupt || code == kSqliteNotADatabase)) {
				if (storage.is_opened()) {
					// WHY: an open file cannot be put aside, it is
					// replaced from a backup at the next start.
					auto marker = QFile(brokenPath());
					if (marker.open(QIODevice::WriteOnly)) {
						marker.close();
					}
					break;
				}
				moveCurrentDatabase();
				if (FreshDatabaseBackup::restore(restoreIndex)) {
					continue;
				} else if (!resetCorrupted) {
					resetCorrupted = true;
					continue;
				}
			}
			break;
		} catch (const std::exception &ex) {
			LOG(("[freshGram] Database initialization failed: %1").arg(ex.what()));
			break;
		}
	}

	LOG(("[freshGram] Database is unavailable, messages will not be saved in this session."));
}

void purgeOlderThan(int days) {
	if (days <= 0) {
		PurgeDays = 0;
		return;
	}
	PurgeDays = days;
	const auto files = DeleteSavedFiles();
	crl::async([=] {
		const auto cutoff = int(base::unixtime::now() - int64(days) * 86400);
		const auto chunk = QString::number(kPurgeChunk);
		auto removed = 0;
		// WHY: small parts let the other queries in between the deletions.
		while (true) {
			auto keys = std::vector<AyuMessages::SavedFileKey>();
			const auto deleted = run<int>("purge old deleted messages", 0, [&] {
				auto ids = std::vector<ID>();
				auto select = Statement(QString(
					"SELECT fakeId, userId, dialogId, messageId FROM DeletedMessageV2 "
					"WHERE entityCreateDate < ?1 LIMIT %1").arg(chunk).toUtf8().constData());
				select.bind(1, cutoff);
				while (select.next()) {
					ids.push_back(select.value(0));
					keys.push_back({ select.value(1), select.value(2), select.value(3) });
				}
				if (ids.empty()) {
					return 0;
				}
				const auto list = ListOf(ids);
				inTransaction([&] {
					for (const auto &table : { "DeletedExtra", "DeletedMarkup", "DeletedRank" }) {
						Statement(QString(
							"DELETE FROM %1 WHERE (userId, dialogId, messageId) IN "
							"(SELECT userId, dialogId, messageId FROM DeletedMessageV2 WHERE fakeId IN (%2))"
						).arg(table, list).toUtf8().constData()).run();
					}
					Statement(QString("DELETE FROM DeletedMessageV2 WHERE fakeId IN (%1)")
						.arg(list).toUtf8().constData()).run();
				});
				return int(ids.size());
			});
			if (files && !keys.empty()) {
				AyuMessages::removeSavedFiles(keys);
			}
			removed += deleted;
			if (deleted < kPurgeChunk) {
				break;
			}
		}
		for (const auto &table : {
				"EditedMessageV2",
				"DeletedExtra",
				"DeletedMarkup",
				"DeletedRank" }) {
			const auto sql = QString(
				"DELETE FROM %1 WHERE rowid IN "
				"(SELECT rowid FROM %1 WHERE entityCreateDate < ?1 LIMIT %2)"
			).arg(table, chunk).toUtf8();
			while (true) {
				const auto count = run<int>("purge old rows", 0, [&] {
					return Statement(sql.constData()).bind(1, cutoff).run();
				});
				removed += count;
				if (count < kPurgeChunk) {
					break;
				}
			}
		}
		runVoid("reclaim the purged space", [] {
			reclaimSpace();
		});
		if (files && removed) {
			FreshDatabaseBackup::purgeLater(FreshDatabaseBackup::OlderThan(cutoff));
		}
	});
}

void addEditedMessage(const EditedMessage &message, int maxRevisions) {
	enqueue([&](QueuedWrites &queue) {
		queue.edited.push_back({ message, maxRevisions });
	});
}

std::vector<EditedMessage> getEditedMessages(ID userId, ID dialogId, ID messageId, ID minId, ID maxId, int totalLimit) {
	return run<std::vector<EditedMessage>>("load edited messages", {}, [&] {
		return toMessages(storage.get_all<EditedMessageRow>(
			where(
				column<EditedMessageRow>(&EditedMessageRow::userId) == userId and
				column<EditedMessageRow>(&EditedMessageRow::dialogId) == dialogId and
				column<EditedMessageRow>(&EditedMessageRow::messageId) == messageId and
				(column<EditedMessageRow>(&EditedMessageRow::fakeId) > minId or minId == 0) and
				(column<EditedMessageRow>(&EditedMessageRow::fakeId) < maxId or maxId == 0)
			),
			order_by(column<EditedMessageRow>(&EditedMessageRow::fakeId)).desc(),
			limit(totalLimit)
		));
	});
}

bool hasRevisions(ID userId, ID dialogId, ID messageId) {
	return run<bool>("check edited messages", false, [&] {
		return !storage.select(
			columns(column<EditedMessageRow>(&EditedMessageRow::messageId)),
			where(
				column<EditedMessageRow>(&EditedMessageRow::userId) == userId and
				column<EditedMessageRow>(&EditedMessageRow::dialogId) == dialogId and
				column<EditedMessageRow>(&EditedMessageRow::messageId) == messageId
			),
			limit(1)
		).empty();
	});
}

void addDeletedMessages(const std::vector<DeletedMessage> &messages) {
	if (messages.empty()) {
		return;
	}
	enqueue([&](QueuedWrites &queue) {
		queue.deleted.insert(queue.deleted.end(), messages.begin(), messages.end());
	});
}

void addDeletedExtras(const std::vector<DeletedExtra> &extras) {
	if (extras.empty()) {
		return;
	}
	enqueue([&](QueuedWrites &queue) {
		queue.extras.insert(queue.extras.end(), extras.begin(), extras.end());
	});
}

void addDeletedMarkups(const std::vector<DeletedMarkup> &markups) {
	if (markups.empty()) {
		return;
	}
	enqueue([&](QueuedWrites &queue) {
		queue.markups.insert(queue.markups.end(), markups.begin(), markups.end());
	});
}

void addDeletedRanks(const std::vector<DeletedRank> &ranks) {
	if (ranks.empty()) {
		return;
	}
	enqueue([&](QueuedWrites &queue) {
		queue.ranks.insert(queue.ranks.end(), ranks.begin(), ranks.end());
	});
}

std::vector<DeletedRank> getDeletedRanks(ID userId, ID dialogId) {
	return run<std::vector<DeletedRank>>("load deleted ranks", {}, [&] {
		return storage.get_all<DeletedRank>(
			where(
				column<DeletedRank>(&DeletedRank::userId) == userId and
				column<DeletedRank>(&DeletedRank::dialogId) == dialogId
			));
	});
}

std::vector<DeletedMarkup> getDeletedMarkups(ID userId, ID dialogId) {
	return run<std::vector<DeletedMarkup>>("load deleted markups", {}, [&] {
		return storage.get_all<DeletedMarkup>(
			where(
				column<DeletedMarkup>(&DeletedMarkup::userId) == userId and
				column<DeletedMarkup>(&DeletedMarkup::dialogId) == dialogId
			));
	});
}

std::vector<DeletedExtra> getDeletedExtras(ID userId, ID dialogId) {
	return run<std::vector<DeletedExtra>>("load deleted extras", {}, [&] {
		return storage.get_all<DeletedExtra>(
			where(
				column<DeletedExtra>(&DeletedExtra::userId) == userId and
				column<DeletedExtra>(&DeletedExtra::dialogId) == dialogId
			));
	});
}

void addDeletedMessage(const DeletedMessage &message) {
	addDeletedMessages({message});
}

std::vector<DeletedMessage> getDeletedMessages(ID userId, ID dialogId, ID topicId, ID minId, ID maxId, int totalLimit, const std::string &searchQuery) {
	return run<std::vector<DeletedMessage>>("load deleted messages", {}, [&] {
		if (searchQuery.empty()) {
			return toMessages(storage.get_all<DeletedMessageRow>(
				where(
					column<DeletedMessageRow>(&DeletedMessageRow::userId) == userId and
					column<DeletedMessageRow>(&DeletedMessageRow::dialogId) == dialogId and
					(column<DeletedMessageRow>(&DeletedMessageRow::topicId) == topicId or topicId == 0) and
					(column<DeletedMessageRow>(&DeletedMessageRow::messageId) > minId or minId == 0) and
					(column<DeletedMessageRow>(&DeletedMessageRow::messageId) < maxId or maxId == 0)
				),
				order_by(column<DeletedMessageRow>(&DeletedMessageRow::messageId)).desc(),
				limit(totalLimit)
			));
		}

		std::string escaped;
		escaped.reserve(searchQuery.size());
		for (const auto c : searchQuery) {
			if (c == '%' || c == '_' || c == '\\') {
				escaped += '\\';
			}
			escaped += c;
		}
		const auto pattern = "%" + escaped + "%";
		const auto idQuery = FreshIdSearch::Parse(
			QString::fromStdString(searchQuery).trimmed());
		const auto senderId = idQuery.valid() ? ID(idQuery.id) : ID(-1);
		return toMessages(storage.get_all<DeletedMessageRow>(
			where(
				column<DeletedMessageRow>(&DeletedMessageRow::userId) == userId and
				column<DeletedMessageRow>(&DeletedMessageRow::dialogId) == dialogId and
				(column<DeletedMessageRow>(&DeletedMessageRow::topicId) == topicId or topicId == 0) and
				(column<DeletedMessageRow>(&DeletedMessageRow::messageId) > minId or minId == 0) and
				(column<DeletedMessageRow>(&DeletedMessageRow::messageId) < maxId or maxId == 0) and
				(like(column<DeletedMessageRow>(&DeletedMessageRow::text), pattern, "\\") or
				column<DeletedMessageRow>(&DeletedMessageRow::fromId) == senderId)
			),
			order_by(column<DeletedMessageRow>(&DeletedMessageRow::messageId)).desc(),
			limit(totalLimit)
		));
	});
}

std::vector<DeletedMessage> searchDeletedMessages(ID userId, ID dialogId, ID topicId, ID fromId, const std::string &searchQuery, int totalLimit, bool withMedia) {
	return run<std::vector<DeletedMessage>>("search deleted messages", {}, [&] {
		std::string escaped;
		escaped.reserve(searchQuery.size());
		for (const auto c : searchQuery) {
			if (c == '%' || c == '_' || c == '\\') {
				escaped += '\\';
			}
			escaped += c;
		}
		const auto pattern = "%" + escaped + "%";
		return toMessages(storage.get_all<DeletedMessageRow>(
			where(
				column<DeletedMessageRow>(&DeletedMessageRow::userId) == userId and
				(column<DeletedMessageRow>(&DeletedMessageRow::dialogId) == dialogId or dialogId == 0) and
				(column<DeletedMessageRow>(&DeletedMessageRow::topicId) == topicId or topicId == 0) and
				(column<DeletedMessageRow>(&DeletedMessageRow::fromId) == fromId or fromId == 0) and
				(column<DeletedMessageRow>(&DeletedMessageRow::documentType) == 1 or !withMedia) and
				like(column<DeletedMessageRow>(&DeletedMessageRow::text), pattern, "\\")
			),
			order_by(column<DeletedMessageRow>(&DeletedMessageRow::date)).desc(),
			limit(totalLimit)
		));
	});
}

bool hasDeletedMessages(ID userId, ID dialogId, ID topicId) {
	return run<bool>("check deleted messages", false, [&] {
		return !storage.select(
			columns(column<DeletedMessageRow>(&DeletedMessageRow::dialogId)),
			where(
				column<DeletedMessageRow>(&DeletedMessageRow::userId) == userId and
				column<DeletedMessageRow>(&DeletedMessageRow::dialogId) == dialogId and
				(column<DeletedMessageRow>(&DeletedMessageRow::topicId) == topicId or topicId == 0)
			),
			limit(1)
		).empty();
	});
}

std::vector<ID> getDeletedDialogIds(ID userId) {
	return run<std::vector<ID>>("load deleted dialogs", {}, [&] {
		// Hops over the dialogs by the key index instead of reading every message.
		constexpr auto sql = "WITH RECURSIVE dialogs(id) AS ("
			"SELECT min(dialogId) FROM DeletedMessageV2 WHERE userId = ?1 "
			"UNION ALL "
			"SELECT (SELECT min(dialogId) FROM DeletedMessageV2 "
			"WHERE userId = ?1 AND dialogId > dialogs.id) FROM dialogs WHERE id IS NOT NULL"
			") SELECT id FROM dialogs WHERE id IS NOT NULL";
		auto result = std::vector<ID>();
		sqlite3_stmt *statement = nullptr;
		if (sqlite3_prepare_v2(MemoryHandle, sql, -1, &statement, nullptr) != SQLITE_OK) {
			throwSqlite(sqlite3_extended_errcode(MemoryHandle));
		}
		sqlite3_bind_int64(statement, 1, userId);
		while (sqlite3_step(statement) == SQLITE_ROW) {
			result.push_back(sqlite3_column_int64(statement, 0));
		}
		const auto code = sqlite3_finalize(statement);
		if (code != SQLITE_OK) {
			throwSqlite(code);
		}
		return result;
	});
}

void removeDeletedMessage(ID userId, ID dialogId, ID messageId) {
	runVoid("remove deleted message", [&] {
		storage.remove_all<DeletedExtra>(
			where(
				column<DeletedExtra>(&DeletedExtra::userId) == userId and
				column<DeletedExtra>(&DeletedExtra::dialogId) == dialogId and
				column<DeletedExtra>(&DeletedExtra::messageId) == messageId
			)
		);
		storage.remove_all<DeletedMarkup>(
			where(
				column<DeletedMarkup>(&DeletedMarkup::userId) == userId and
				column<DeletedMarkup>(&DeletedMarkup::dialogId) == dialogId and
				column<DeletedMarkup>(&DeletedMarkup::messageId) == messageId
			)
		);
		storage.remove_all<DeletedRank>(
			where(
				column<DeletedRank>(&DeletedRank::userId) == userId and
				column<DeletedRank>(&DeletedRank::dialogId) == dialogId and
				column<DeletedRank>(&DeletedRank::messageId) == messageId
			)
		);
		storage.remove_all<DeletedMessageRow>(
			where(
				column<DeletedMessageRow>(&DeletedMessageRow::userId) == userId and
				column<DeletedMessageRow>(&DeletedMessageRow::dialogId) == dialogId and
				column<DeletedMessageRow>(&DeletedMessageRow::messageId) == messageId
			)
		);
	});
	if (DeleteSavedFiles()) {
		FreshDatabaseBackup::purgeLater(
			FreshDatabaseBackup::OfMessage(userId, dialogId, messageId));
	}
}

void clearDeletedMessages(ID userId, ID dialogId, ID topicId) {
	runVoid("clear deleted messages", [&] {
		inTransaction([&] {
			// WHY: the rows of a topic are found by the messages they belong to.
			for (const auto &table : { "DeletedExtra", "DeletedMarkup", "DeletedRank" }) {
				const auto sql = QString(topicId
					? "DELETE FROM %1 WHERE userId = ?1 AND dialogId = ?2 AND messageId IN "
						"(SELECT messageId FROM DeletedMessageV2 "
						"WHERE userId = ?1 AND dialogId = ?2 AND topicId = ?3)"
					: "DELETE FROM %1 WHERE userId = ?1 AND dialogId = ?2").arg(table).toUtf8();
				auto statement = Statement(sql.constData());
				statement.bind(1, userId).bind(2, dialogId);
				if (topicId) {
					statement.bind(3, topicId);
				}
				statement.run();
			}
			storage.remove_all<DeletedMessageRow>(
				where(
					column<DeletedMessageRow>(&DeletedMessageRow::userId) == userId and
					column<DeletedMessageRow>(&DeletedMessageRow::dialogId) == dialogId and
					(column<DeletedMessageRow>(&DeletedMessageRow::topicId) == topicId or topicId == 0)
				)
			);
		});
		reclaimSpace();
	});
	if (DeleteSavedFiles()) {
		FreshDatabaseBackup::purgeLater(
			FreshDatabaseBackup::OfDialog(userId, dialogId, topicId));
	}
}

std::vector<ID> getDeletedMessageIds(ID userId, ID dialogId, ID topicId) {
	return run<std::vector<ID>>("load deleted message ids", {}, [&] {
		const auto ids = storage.select(
			&DeletedMessageRow::messageId,
			where(
				column<DeletedMessageRow>(&DeletedMessageRow::userId) == userId and
				column<DeletedMessageRow>(&DeletedMessageRow::dialogId) == dialogId and
				(column<DeletedMessageRow>(&DeletedMessageRow::topicId) == topicId or topicId == 0)
			));
		return std::vector<ID>(ids.begin(), ids.end());
	});
}

void clearAllDeleted() {
	runVoid("clear all deleted", [&] {
		inTransaction([&] {
			storage.remove_all<DeletedExtra>();
			storage.remove_all<DeletedMarkup>();
			storage.remove_all<DeletedRank>();
			storage.remove_all<DeletedMessageRow>();
			storage.remove_all<DeletedDialog>();
			storage.remove_all<KeptTopic>();
			storage.remove_all<KeptDialog>();
		});
		reclaimSpace();
	});
	FreshDatabaseBackup::removeAll();
}

template<typename Callback>
void forEachUserTable(Callback &&callback) {
	callback(std::type_identity<DeletedExtra>());
	callback(std::type_identity<DeletedMarkup>());
	callback(std::type_identity<DeletedRank>());
	callback(std::type_identity<DeletedMessageRow>());
	callback(std::type_identity<EditedMessageRow>());
	callback(std::type_identity<DeletedDialog>());
	callback(std::type_identity<KeptTopic>());
	callback(std::type_identity<KeptDialog>());
	callback(std::type_identity<KnownUser>());
	callback(std::type_identity<LocalPin>());
	callback(std::type_identity<ComposeStashRow>());
	callback(std::type_identity<SecretMessageRow>());
	callback(std::type_identity<SecretChatRow>());
	callback(std::type_identity<SecretStateRow>());
	callback(std::type_identity<PeekedStatusRow>());
	callback(std::type_identity<PeekRestoreRow>());
	callback(std::type_identity<SpyMessageRead>());
	callback(std::type_identity<SpyMessageContentsRead>());
}

void clearUserData(ID userId) {
	runVoid("clear user data", [&] {
		inTransaction([&] {
			forEachUserTable([&](auto tag) {
				using Row = typename decltype(tag)::type;
				storage.remove_all<Row>(
					where(column<Row>(&Row::userId) == userId));
			});
		});
		reclaimAllSpace();
	});
	FreshDatabaseBackup::purgeUser(userId);
	flush();
}

void clearEverything() {
	runVoid("clear everything", [&] {
		inTransaction([&] {
			forEachUserTable([&](auto tag) {
				using Row = typename decltype(tag)::type;
				storage.remove_all<Row>();
			});
		});
		reclaimAllSpace();
	});
	FreshDatabaseBackup::removeAll();
	flush();
}

bool exportCompressed(const QString &path, qint64 &rows) {
	if (!FreshVault::HasKey()) {
		return false;
	}
	auto data = static_cast<unsigned char*>(nullptr);
	auto size = sqlite3_int64();
	{
		std::lock_guard lock(DatabaseMutex);
		if (!DatabaseReady || !MemoryHandle) {
			return false;
		}
		drainQueuedLocked();
		data = sqlite3_serialize(MemoryHandle, "main", &size, 0);
		rows = 0;
		try {
			rows = storage.count<DeletedMessageRow>()
				+ storage.count<EditedMessageRow>();
		} catch (...) {
		}
	}
	if (!data) {
		return false;
	}
	// The copy is packed and sealed by blocks outside of the lock.
	const auto ok = FreshVault::WriteCompressedFile(path, data, size);
	sqlite3_free(data);
	return ok;
}

std::string sizeReport() {
	auto copy = static_cast<unsigned char*>(nullptr);
	auto size = sqlite3_int64();
	auto copied = 0.;
	{
		std::lock_guard lock(DatabaseMutex);
		if (!DatabaseReady || !MemoryHandle) {
			return "The database is not ready.\n";
		}
		drainQueuedLocked();
		const auto started = std::chrono::steady_clock::now();
		copy = sqlite3_serialize(MemoryHandle, "main", &size, 0);
		copied = MsSince(started);
	}
	if (!copy) {
		return "The database copy failed.\n";
	}
	sqlite3 *report = nullptr;
	if (sqlite3_open_v2(":memory:", &report, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
		sqlite3_close(report);
		sqlite3_free(copy);
		return "The report database failed to open.\n";
	}
	const auto code = sqlite3_deserialize(
		report,
		"main",
		copy,
		size,
		size,
		SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
	if (code != SQLITE_OK) {
		sqlite3_close(report);
		return "The report database failed to load.\n";
	}
	auto options = FreshDatabaseReport::Options();
	options.measureSerialize = false;
	auto result = FreshDatabaseReport::Build(report, options);
	sqlite3_close(report);
	result += "== Snapshot ==\n";
	result += QString("  sqlite3_serialize copy: %1 MB in %2 ms (the sealing and writing come on top)\n")
		.arg(size / 1048576.0, 0, 'f', 1)
		.arg(copied, 0, 'f', 1)
		.toStdString();
	return result;
}

void saveSizeReport(std::function<void(bool)> done) {
	if (!AyuSettings::getInstance().enableDebugLogging()) {
		done(false);
		return;
	}
	crl::async([=] {
		const auto text = sizeReport();
		auto file = QFile("./tdata/fresh_db_report.txt");
		const auto ok = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
			&& file.write(text.data(), qint64(text.size())) == qint64(text.size());
		crl::on_main([=] {
			done(ok);
		});
	});
}

void flush() {
	std::lock_guard lock(DatabaseMutex);
	if (DatabaseReady) {
		drainQueuedLocked();
	}
}

void finish() {
	QueueTimer = nullptr;
	PurgeTimer = nullptr;
	flush();
	if (DatabaseReady) {
		touchDirtyMarker(false);
	}
}

void saveKeptDialog(const KeptDialog &dialog) {
	runVoid("save kept dialog", [&] {
		inTransaction([&] {
			storage.remove_all<KeptDialog>(
				where(
					column<KeptDialog>(&KeptDialog::userId) == dialog.userId and
					column<KeptDialog>(&KeptDialog::dialogId) == dialog.dialogId
				)
			);
			storage.insert(dialog);
		});
	});
}

void saveKeptTopics(const std::vector<KeptTopic> &topics) {
	if (topics.empty()) {
		return;
	}
	runVoid("save kept topics", [&] {
		inTransaction([&] {
			for (const auto &topic : topics) {
				storage.remove_all<KeptTopic>(
					where(
						column<KeptTopic>(&KeptTopic::userId) == topic.userId and
						column<KeptTopic>(&KeptTopic::dialogId) == topic.dialogId and
						column<KeptTopic>(&KeptTopic::rootId) == topic.rootId
					)
				);
				storage.insert(topic);
			}
		});
	});
}

std::vector<KeptTopic> getKeptTopics(ID userId, ID dialogId) {
	return run<std::vector<KeptTopic>>("load kept topics", {}, [&] {
		return storage.get_all<KeptTopic>(
			where(
				column<KeptTopic>(&KeptTopic::userId) == userId and
				column<KeptTopic>(&KeptTopic::dialogId) == dialogId
			));
	});
}

std::vector<KeptTopic> getKeptTopicsFor(ID userId) {
	return run<std::vector<KeptTopic>>("load all kept topics", {}, [&] {
		return storage.get_all<KeptTopic>(
			where(column<KeptTopic>(&KeptTopic::userId) == userId));
	});
}

void removeKeptTopic(ID userId, ID dialogId, ID rootId) {
	runVoid("remove kept topic", [&] {
		storage.remove_all<KeptTopic>(
			where(
				column<KeptTopic>(&KeptTopic::userId) == userId and
				column<KeptTopic>(&KeptTopic::dialogId) == dialogId and
				column<KeptTopic>(&KeptTopic::rootId) == rootId
			)
		);
	});
}

void removeKeptTopics(ID userId, ID dialogId) {
	runVoid("remove kept topics", [&] {
		storage.remove_all<KeptTopic>(
			where(
				column<KeptTopic>(&KeptTopic::userId) == userId and
				column<KeptTopic>(&KeptTopic::dialogId) == dialogId
			)
		);
	});
}

void syncKeptDialogs(const std::vector<KeptDialog> &dialogs) {
	if (dialogs.empty()) {
		return;
	}
	runVoid("sync kept dialogs", [&] {
		inTransaction([&] {
			for (const auto &dialog : dialogs) {
				const auto lost = storage.count<KeptDialog>(
					where(
						column<KeptDialog>(&KeptDialog::userId) == dialog.userId and
						column<KeptDialog>(&KeptDialog::dialogId) == dialog.dialogId and
						column<KeptDialog>(&KeptDialog::lost) == 1
					));
				if (lost) {
					continue;
				}
				storage.remove_all<KeptDialog>(
					where(
						column<KeptDialog>(&KeptDialog::userId) == dialog.userId and
						column<KeptDialog>(&KeptDialog::dialogId) == dialog.dialogId
					)
				);
				storage.insert(dialog);
			}
		});
	});
}

std::vector<KeptDialog> getKeptDialogs(ID userId) {
	return run<std::vector<KeptDialog>>("load kept dialogs", {}, [&] {
		return storage.get_all<KeptDialog>(
			where(column<KeptDialog>(&KeptDialog::userId) == userId));
	});
}

void removeKeptDialog(ID userId, ID dialogId) {
	runVoid("remove kept dialog", [&] {
		storage.remove_all<KeptDialog>(
			where(
				column<KeptDialog>(&KeptDialog::userId) == userId and
				column<KeptDialog>(&KeptDialog::dialogId) == dialogId
			)
		);
	});
}

std::vector<ID> getLocalPins(ID userId) {
	return run<std::vector<ID>>("load local pins", {}, [&] {
		auto result = std::vector<ID>();
		const auto rows = storage.get_all<LocalPin>(
			where(column<LocalPin>(&LocalPin::userId) == userId),
			order_by(column<LocalPin>(&LocalPin::position)));
		result.reserve(rows.size());
		for (const auto &row : rows) {
			result.push_back(row.peerId);
		}
		return result;
	});
}

void saveLocalPins(ID userId, const std::vector<ID> &peers) {
	runVoid("save local pins", [&] {
		inTransaction([&] {
			storage.remove_all<LocalPin>(
				where(column<LocalPin>(&LocalPin::userId) == userId));
			auto position = 0;
			for (const auto peerId : peers) {
				storage.insert(LocalPin{ 0, userId, peerId, position++ });
			}
		});
	});
}

void saveComposeStash(const ComposeStashRow &row) {
	runVoid("save compose stash", [&] {
		inTransaction([&] {
			storage.remove_all<ComposeStashRow>(
				where(
					column<ComposeStashRow>(&ComposeStashRow::userId) == row.userId and
					column<ComposeStashRow>(&ComposeStashRow::dialogId) == row.dialogId and
					column<ComposeStashRow>(&ComposeStashRow::topicRootId) == row.topicRootId and
					column<ComposeStashRow>(&ComposeStashRow::monoforumPeerId) == row.monoforumPeerId
				)
			);
			storage.insert(row);
		});
	});
}

std::optional<ComposeStashRow> getComposeStash(
		ID userId,
		ID dialogId,
		ID topicRootId,
		ID monoforumPeerId) {
	return run<std::optional<ComposeStashRow>>("load compose stash", std::nullopt, [&] {
		const auto rows = storage.get_all<ComposeStashRow>(
			where(
				column<ComposeStashRow>(&ComposeStashRow::userId) == userId and
				column<ComposeStashRow>(&ComposeStashRow::dialogId) == dialogId and
				column<ComposeStashRow>(&ComposeStashRow::topicRootId) == topicRootId and
				column<ComposeStashRow>(&ComposeStashRow::monoforumPeerId) == monoforumPeerId
			),
			limit(1));
		return rows.empty()
			? std::optional<ComposeStashRow>()
			: std::optional<ComposeStashRow>(rows.front());
	});
}

void removeComposeStash(
		ID userId,
		ID dialogId,
		ID topicRootId,
		ID monoforumPeerId) {
	runVoid("remove compose stash", [&] {
		storage.remove_all<ComposeStashRow>(
			where(
				column<ComposeStashRow>(&ComposeStashRow::userId) == userId and
				column<ComposeStashRow>(&ComposeStashRow::dialogId) == dialogId and
				column<ComposeStashRow>(&ComposeStashRow::topicRootId) == topicRootId and
				column<ComposeStashRow>(&ComposeStashRow::monoforumPeerId) == monoforumPeerId
			)
		);
	});
}

void saveKnownUsers(const std::vector<KnownUser> &users) {
	if (users.empty()) {
		return;
	}
	runVoid("save known users", [&] {
		inTransaction([&] {
			for (const auto &user : users) {
				const auto same = [&](const KnownUser &row) {
					return row.accessHash == user.accessHash
						&& row.firstName == user.firstName
						&& row.lastName == user.lastName
						&& row.username == user.username;
				};
				const auto sameUser = where(
					column<KnownUser>(&KnownUser::userId) == user.userId and
					column<KnownUser>(&KnownUser::peerId) == user.peerId);
				const auto existing = storage.get_all<KnownUser>(sameUser, limit(1));
				if (!existing.empty() && same(existing.front())) {
					continue;
				}
				storage.remove_all<KnownUser>(sameUser);
				storage.insert(user);
			}
		});
	});
}

std::optional<KnownUser> getKnownUser(ID userId, ID peerId) {
	return run<std::optional<KnownUser>>("load known user", std::nullopt, [&] {
		auto rows = storage.get_all<KnownUser>(
			where(
				column<KnownUser>(&KnownUser::userId) == userId and
				column<KnownUser>(&KnownUser::peerId) == peerId
			),
			limit(1));
		return rows.empty()
			? std::optional<KnownUser>()
			: std::optional<KnownUser>(std::move(rows.front()));
	});
}

void saveSecretChat(const SecretChatRow &chat) {
	runVoid("save secret chat", [&] {
		inTransaction([&] {
			storage.remove_all<SecretChatRow>(
				where(
					column<SecretChatRow>(&SecretChatRow::userId) == chat.userId and
					column<SecretChatRow>(&SecretChatRow::chatId) == chat.chatId
				)
			);
			storage.insert(chat);
		});
	});
}

std::vector<SecretChatRow> getSecretChats(ID userId) {
	return run<std::vector<SecretChatRow>>("load secret chats", {}, [&] {
		return storage.get_all<SecretChatRow>(
			where(column<SecretChatRow>(&SecretChatRow::userId) == userId));
	});
}

void removeSecretChat(ID userId, int chatId) {
	runVoid("remove secret chat", [&] {
		inTransaction([&] {
			storage.remove_all<SecretChatRow>(
				where(
					column<SecretChatRow>(&SecretChatRow::userId) == userId and
					column<SecretChatRow>(&SecretChatRow::chatId) == chatId
				)
			);
			storage.remove_all<SecretMessageRow>(
				where(
					column<SecretMessageRow>(&SecretMessageRow::userId) == userId and
					column<SecretMessageRow>(&SecretMessageRow::chatId) == chatId
				)
			);
		});
	});
}

bool addSecretMessage(const SecretMessageRow &message) {
	return run<bool>("save secret message", false, [&] {
		const auto exists = storage.count<SecretMessageRow>(
			where(
				column<SecretMessageRow>(&SecretMessageRow::userId) == message.userId and
				column<SecretMessageRow>(&SecretMessageRow::chatId) == message.chatId and
				column<SecretMessageRow>(&SecretMessageRow::randomId) == message.randomId
			)
		) > 0;
		if (exists) {
			return false;
		}
		storage.insert(message);
		return true;
	});
}

void updateSecretMessage(const SecretMessageRow &message) {
	runVoid("update secret message", [&] {
		storage.update_all(
			set(
				assign(&SecretMessageRow::kind, message.kind),
				assign(&SecretMessageRow::outgoing, message.outgoing),
				assign(&SecretMessageRow::seqIn, message.seqIn),
				assign(&SecretMessageRow::seqOut, message.seqOut),
				assign(&SecretMessageRow::date, message.date),
				assign(&SecretMessageRow::text, message.text),
				assign(&SecretMessageRow::payload, message.payload)
			),
			where(
				column<SecretMessageRow>(&SecretMessageRow::userId) == message.userId and
				column<SecretMessageRow>(&SecretMessageRow::chatId) == message.chatId and
				column<SecretMessageRow>(&SecretMessageRow::randomId) == message.randomId
			)
		);
	});
}

std::vector<SecretMessageRow> getSecretMessages(ID userId, int chatId) {
	return run<std::vector<SecretMessageRow>>("load secret messages", {}, [&] {
		return storage.get_all<SecretMessageRow>(
			where(
				column<SecretMessageRow>(&SecretMessageRow::userId) == userId and
				column<SecretMessageRow>(&SecretMessageRow::chatId) == chatId
			),
			order_by(column<SecretMessageRow>(&SecretMessageRow::fakeId)).asc()
		);
	});
}

void removeSecretMessage(ID userId, int chatId, ID randomId) {
	runVoid("remove secret message", [&] {
		storage.remove_all<SecretMessageRow>(
			where(
				column<SecretMessageRow>(&SecretMessageRow::userId) == userId and
				column<SecretMessageRow>(&SecretMessageRow::chatId) == chatId and
				column<SecretMessageRow>(&SecretMessageRow::randomId) == randomId
			)
		);
	});
}

void clearSecretMessages(ID userId, int chatId) {
	runVoid("clear secret messages", [&] {
		storage.remove_all<SecretMessageRow>(
			where(
				column<SecretMessageRow>(&SecretMessageRow::userId) == userId and
				column<SecretMessageRow>(&SecretMessageRow::chatId) == chatId
			)
		);
	});
}

void savePeekedStatus(const PeekedStatusRow &row) {
	runVoid("save peeked status", [&] {
		inTransaction([&] {
			storage.remove_all<PeekedStatusRow>(
				where(
					column<PeekedStatusRow>(&PeekedStatusRow::userId) == row.userId and
					column<PeekedStatusRow>(&PeekedStatusRow::targetId) == row.targetId
				)
			);
			storage.insert(row);
		});
	});
}

std::vector<PeekedStatusRow> getPeekedStatus(ID userId, ID targetId) {
	return run<std::vector<PeekedStatusRow>>("load peeked status", {}, [&] {
		return storage.get_all<PeekedStatusRow>(
			where(
				column<PeekedStatusRow>(&PeekedStatusRow::userId) == userId and
				column<PeekedStatusRow>(&PeekedStatusRow::targetId) == targetId
			)
		);
	});
}

void savePeekRestore(const PeekRestoreRow &row) {
	runVoid("save peek restore", [&] {
		inTransaction([&] {
			storage.remove_all<PeekRestoreRow>(
				where(column<PeekRestoreRow>(&PeekRestoreRow::userId) == row.userId));
			storage.insert(row);
		});
	});
}

void saveSecretState(const SecretStateRow &row) {
	runVoid("save secret state", [&] {
		inTransaction([&] {
			storage.remove_all<SecretStateRow>(
				where(column<SecretStateRow>(&SecretStateRow::userId) == row.userId));
			storage.insert(row);
		});
	});
}

std::vector<SecretStateRow> getSecretState(ID userId) {
	return run<std::vector<SecretStateRow>>("load secret state", {}, [&] {
		return storage.get_all<SecretStateRow>(
			where(column<SecretStateRow>(&SecretStateRow::userId) == userId));
	});
}

std::vector<PeekRestoreRow> getPeekRestore(ID userId) {
	return run<std::vector<PeekRestoreRow>>("load peek restore", {}, [&] {
		return storage.get_all<PeekRestoreRow>(
			where(column<PeekRestoreRow>(&PeekRestoreRow::userId) == userId));
	});
}

void clearPeekRestore(ID userId) {
	runVoid("clear peek restore", [&] {
		storage.remove_all<PeekRestoreRow>(
			where(column<PeekRestoreRow>(&PeekRestoreRow::userId) == userId));
	});
}

template<typename T>
std::vector<T> getAllT() {
	return run<std::vector<T>>("load all", {}, [&] {
		return storage.get_all<T>();
	});
}

std::vector<RegexFilter> getAllRegexFilters() {
	return getAllT<RegexFilter>();
}

std::vector<RegexFilterGlobalExclusion> getAllFiltersExclusions() {
	return getAllT<RegexFilterGlobalExclusion>();
}

std::vector<RegexFilter> getExcludedByDialogId(ID dialogId) {
	return run<std::vector<RegexFilter>>("load excluded filters", {}, [&] {
		return storage.get_all<RegexFilter>(
			where(in(&RegexFilter::id,
					 storage.select(columns(&RegexFilterGlobalExclusion::filterId),
									where(is_equal(&RegexFilterGlobalExclusion::dialogId, dialogId))
					 )
			))
		);
	});
}

int getCount() {
	return run<int>("count filters", 0, [&] {
		return storage.count<RegexFilter>();
	});
}

RegexFilter getById(std::vector<char> id) {
	return run<RegexFilter>("load filter", RegexFilter{}, [&] {
		return storage.get<RegexFilter>(
			where(column<RegexFilter>(&RegexFilter::id) == std::move(id))
		);
	});
}

std::vector<RegexFilter> getShared() {
	return run<std::vector<RegexFilter>>("load shared filters", {}, [&] {
		return storage.get_all<RegexFilter>(
			where(is_null(column<RegexFilter>(&RegexFilter::dialogId)))
		);
	});
}

std::vector<RegexFilter> getByDialogId(ID dialogId) {
	return run<std::vector<RegexFilter>>("load dialog filters", {}, [&] {
		return storage.get_all<RegexFilter>(
			where(column<RegexFilter>(&RegexFilter::dialogId) == dialogId)
		);
	});
}

void addRegexFilter(const RegexFilter &filter) {
	runVoid("save regex filter", [&] {
		inTransaction([&] {
			storage.replace(filter); // we're using replace as we set std::vector<char> as primary key
		});
	});
}

void addRegexExclusion(const RegexFilterGlobalExclusion &exclusion) {
	runVoid("save regex filter exclusion", [&] {
		inTransaction([&] {
			storage.insert(exclusion);
		});
	});
}

void updateRegexFilter(const RegexFilter &filter) {
	runVoid("update regex filter", [&] {
		storage.update_all(
			set(
				c(&RegexFilter::text) = filter.text,
				c(&RegexFilter::enabled) = filter.enabled,
				c(&RegexFilter::reversed) = filter.reversed,
				c(&RegexFilter::caseInsensitive) = filter.caseInsensitive,
				c(&RegexFilter::dialogId) = filter.dialogId
			),
			where(c(&RegexFilter::id) == filter.id)
		);
	});
}

void deleteFilter(const std::vector<char> &id) {
	runVoid("delete regex filter", [&] {
		storage.remove_all<RegexFilter>(
			where(column<RegexFilter>(&RegexFilter::id) == id)
		);
	});
}

void deleteExclusionsByFilterId(const std::vector<char> &id) {
	runVoid("delete regex filter exclusions", [&] {
		storage.remove_all<RegexFilterGlobalExclusion>(
			where(column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::filterId) == id)
		);
	});
}

void deleteExclusion(ID dialogId, std::vector<char> filterId) {
	runVoid("delete regex filter exclusion", [&] {
		storage.remove_all<RegexFilterGlobalExclusion>(
			where(column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::filterId) == filterId and
				column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::dialogId) == dialogId
			)
		);
	});
}

void deleteAllFilters() {
	runVoid("delete all regex filters", [&] {
		storage.remove_all<RegexFilter>();
	});
}

void deleteAllExclusions() {
	runVoid("delete all regex filter exclusions", [&] {
		storage.remove_all<RegexFilterGlobalExclusion>();
	});
}

bool hasFilters() {
	return run<bool>("check regex filters", false, [&] {
		return !storage.select(
			columns(column<RegexFilter>(&RegexFilter::id)),
			limit(1)
		).empty();
	});
}

bool hasPerDialogFilters() {
	return run<bool>("check per dialog filters", false, [&] {
		return
			!storage.select(
				columns(column<RegexFilter>(&RegexFilter::id)),
				where(is_not_null(column<RegexFilter>(&RegexFilter::dialogId))),
				limit(1)
			).empty() ||
			!storage.select(
				columns(column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::fakeId)),
				limit(1)
			).empty();
	});
}

}
