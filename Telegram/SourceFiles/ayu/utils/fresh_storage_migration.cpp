// This file is part of freshGram.
#include "ayu/utils/fresh_storage_migration.h"

#include "logs.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>

#include <utility>
#include <vector>

namespace FreshStorage {
namespace {

constexpr auto kOldDataPrefix = "ayudata";
constexpr auto kNewDataPrefix = "freshdata";

struct Rename {
	const char *from = nullptr;
	const char *to = nullptr;
};

constexpr Rename kDatabaseFiles[] = {
	{ "ayudata.sqlite", "freshdata.sqlite" },
	{ "ayudata.sqlite-journal", "freshdata.sqlite-journal" },
	{ "ayudata.sqlite.dirty", "freshdata.sqlite.dirty" },
	{ "ayudata.sqlite.broken", "freshdata.sqlite.broken" },
	{ "ayudata.enc", "freshdata.enc" },
	{ "ayudata_premigration_v4.enc", "freshdata_premigration_v4.enc" },
};

constexpr Rename kFiles[] = {
	{ "ayu_accounts.enc", "fresh_accounts.enc" },
	{ "ayu_db_report.txt", "fresh_db_report.txt" },
};

constexpr Rename kDirectories[] = {
	{ "ayu_media", "fresh_media" },
	{ "ayu_media_tmp", "fresh_media_tmp" },
	{ "ayu_secret", "fresh_secret" },
	{ "ayu_secret_tmp", "fresh_secret_tmp" },
	{ "ayu_backups", "fresh_backups" },
};

constexpr Rename kStoredFolders[] = {
	{ "tdata/ayu_media/", "tdata/fresh_media/" },
	{ "tdata/ayu_secret/", "tdata/fresh_secret/" },
};

bool Started = false;
bool DatabaseMoved = true;

[[nodiscard]] QString Tdata() {
	return cWorkingDir() + u"tdata/"_q;
}

[[nodiscard]] QString NewDataName(const QString &name) {
	return QString::fromLatin1(kNewDataPrefix)
		+ name.mid(int(qstrlen(kOldDataPrefix)));
}

void MoveDatabase() {
	const auto root = Tdata();
	auto moved = std::vector<Rename>();
	for (const auto &entry : kDatabaseFiles) {
		const auto from = root + entry.from;
		if (!QFile::exists(from)) {
			continue;
		} else if (QFile::exists(root + entry.to)) {
			LOG(("freshGram: %1 is already there, the old one is kept.")
				.arg(entry.to));
			continue;
		} else if (QFile::rename(from, root + entry.to)) {
			moved.push_back(entry);
			continue;
		}
		LOG(("freshGram: could not move %1, the database stays closed.")
			.arg(entry.from));
		for (const auto &done : moved) {
			QFile::rename(root + done.to, root + done.from);
		}
		DatabaseMoved = false;
		return;
	}
}

void MoveDataCopies(const QString &directory, const QRegularExpression &mask) {
	const auto dir = QDir(directory);
	const auto names = dir.entryList(
		{ QString::fromLatin1(kOldDataPrefix) + u"_*"_q },
		QDir::Files);
	for (const auto &name : names) {
		if (!mask.match(name).hasMatch()) {
			continue;
		}
		const auto to = dir.filePath(NewDataName(name));
		if (QFile::exists(to) || !QFile::rename(dir.filePath(name), to)) {
			LOG(("freshGram: could not move %1.").arg(name));
		}
	}
}

void MoveFile(const Rename &entry) {
	const auto from = Tdata() + entry.from;
	const auto to = Tdata() + entry.to;
	if (!QFile::exists(from)) {
		return;
	} else if (QFile::exists(to) || !QFile::rename(from, to)) {
		LOG(("freshGram: could not move %1.").arg(entry.from));
	}
}

void MergeDirectory(const QString &from, const QString &to) {
	if (!QDir(from).exists()) {
		return;
	} else if (!QDir(to).exists() && QDir().rename(from, to)) {
		return;
	}
	QDir().mkpath(to);
	const auto entries = QDir(from).entryInfoList(
		QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
	for (const auto &info : entries) {
		const auto target = to + '/' + info.fileName();
		if (info.isDir()) {
			MergeDirectory(info.absoluteFilePath(), target);
		} else if (!QFile::exists(target)) {
			QFile::rename(info.absoluteFilePath(), target);
		}
	}
	if (!QDir().rmdir(from)) {
		LOG(("freshGram: some files are left in %1.").arg(from));
	}
}

} // namespace

void MigrateLegacyNames() {
	if (std::exchange(Started, true)) {
		return;
	}
	MoveDatabase();
	static const auto copies = QRegularExpression(
		u"^ayudata_(\\d+\\.(sqlite|enc)(-journal)?|import_failed_.+)$"_q);
	MoveDataCopies(Tdata(), copies);
	for (const auto &entry : kFiles) {
		MoveFile(entry);
	}
	for (const auto &entry : kDirectories) {
		MergeDirectory(Tdata() + entry.from, Tdata() + entry.to);
	}
	static const auto backups = QRegularExpression(u"^ayudata_.+$"_q);
	MoveDataCopies(Tdata() + u"fresh_backups"_q, backups);
}

bool DatabaseMigrated() {
	return DatabaseMoved;
}

QString CurrentPath(const QString &path) {
	for (const auto &entry : kStoredFolders) {
		const auto from = QString::fromLatin1(entry.from);
		const auto index = path.indexOf(from);
		if (index < 0) {
			continue;
		}
		auto moved = path;
		moved.replace(index, from.size(), QString::fromLatin1(entry.to));
		return (QFile::exists(moved) || !QFile::exists(path)) ? moved : path;
	}
	return path;
}

std::string CurrentPath(const std::string &path) {
	return CurrentPath(QString::fromStdString(path)).toStdString();
}

} // namespace FreshStorage
