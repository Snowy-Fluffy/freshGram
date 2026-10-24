// This file is part of freshGram.
#pragma once

#include "storage/details/storage_passcode_kdf.h"

namespace FreshFakePasscode {

constexpr auto kMaxEntries = 8;

struct Actions {
	bool clearCache = false;
	bool clearCacheDb = false;
	bool clearCacheMedia = false;
	bool clearProxies = false;
	bool logoutDb = false;
	bool logoutMedia = false;
	bool deleteAll = false;
	QString command;
	std::vector<int> logoutAccounts;
};

struct Entry {
	int id = 0;
	QString name;
	QString passcode;
	bool unlocks = true;
	Actions actions;
};

struct Data {
	bool clearCacheOnLock = false;
	std::vector<Entry> list;
};

enum class AddResult : uchar {
	Ok,
	Empty,
	TooMany,
	TooShort,
	WrongPasscode,
	SameAsReal,
	Duplicate,
	Failed,
};

struct Match {
	int id = 0;
	QByteArray realPasscode;
	Actions actions;

	[[nodiscard]] bool unlocks() const {
		return !realPasscode.isEmpty();
	}
};

struct KdfInput {
	QByteArray salt;
	Storage::PasscodeKdf kdf;
	std::vector<QByteArray> blobs;
};
[[nodiscard]] KdfInput MatchKdfInput();
[[nodiscard]] std::optional<Match> TryMatch(const QByteArray &passcode);
// Thread safe, the key is derived from the salt and the KDF of the input.
[[nodiscard]] std::optional<Match> MatchWithKey(
	const KdfInput &input,
	const MTP::AuthKeyPtr &key);

// The blobs are made on a thread when the real passcode changes.
struct RebuildTask {
	bool remove = false;
	bool needed = false;
	bool ok = true;
	QByteArray realPasscode;
	QByteArray salt;
	Storage::PasscodeKdf kdf;
	std::vector<Entry> entries;
	std::vector<QByteArray> blobs;
};
[[nodiscard]] RebuildTask PrepareRebuild(
	const QByteArray &realPasscode,
	const Storage::PasscodeKdf &kdf);
void ComputeRebuild(RebuildTask &task);
void ApplyRebuild(const RebuildTask &task);

void SetPending(int id);
void RunPending();
void RunSilent(const Actions &actions);

void OnLock();

[[nodiscard]] const Data &Current();
[[nodiscard]] AddResult Add(
	QString name,
	QString passcode,
	const QString &realPasscode,
	bool unlocks,
	Actions actions);
void Update(int id, QString name, Actions actions);
void Remove(int id);
void SetClearCacheOnLock(bool value);

void RealPasscodeChanged(const QByteArray &passcode);

} // namespace FreshFakePasscode
