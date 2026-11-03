/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "storage/details/storage_passcode_kdf.h"

namespace MTP {
class Config;
class AuthKey;
using AuthKeyPtr = std::shared_ptr<AuthKey>;
} // namespace MTP

namespace Main {
class Account;
class Domain;
} // namespace Main

namespace Storage {

enum class StartResult : uchar {
	Success,
	IncorrectPasscode,
	IncorrectPasscodeLegacy,
	KeyUnavailable,
};

enum class PasscodeCheck : uchar {
	Correct,
	Wrong,
	Failed,
};

struct PasscodeWrap {
	PasscodeKdf kdf;
	QByteArray salt;
	QByteArray keyEncrypted;
};

class Domain final : public base::has_weak_ptr {
public:
	Domain(not_null<Main::Domain*> owner, const QString &dataName);
	~Domain();

	[[nodiscard]] StartResult start(
		const QByteArray &passcode,
		const PrecomputedPasscodeKey &precomputed = {});
	void startAdded(
		not_null<Main::Account*> account,
		std::unique_ptr<MTP::Config> config);
	void writeAccounts();
	void startFromScratch();

	[[nodiscard]] bool checkPasscode(const QByteArray &passcode) const;
	[[nodiscard]] PasscodeCheck checkPasscodeResult(
		const QByteArray &passcode) const;
	[[nodiscard]] PasscodeCheck checkPasscodeKey(
		const PrecomputedPasscodeKey &precomputed) const;
	[[nodiscard]] bool setPasscode(const QByteArray &passcode);

	// Derives the keys on a thread, nothing changes when it fails.
	void setPasscodeAsync(
		const QByteArray &passcode,
		const PasscodeKdf &kdf,
		Fn<void(bool)> done);

	struct PasscodeKdfInput {
		QByteArray salt;
		PasscodeKdf kdf;
	};
	// Reads the key file when the domain is not started yet.
	[[nodiscard]] std::optional<PasscodeKdfInput> passcodeKdfInput() const;
	[[nodiscard]] PasscodeKdf passcodeKdf() const {
		return _passcodeKdf;
	}

	[[nodiscard]] int oldVersion() const;
	void clearOldVersion();

	[[nodiscard]] rpl::producer<> localPasscodeChanged() const;
	[[nodiscard]] bool hasLocalPasscode() const;
	[[nodiscard]] MTP::AuthKeyPtr localKey() const {
		return _localKey;
	}

private:
	enum class StartModernResult {
		Success,
		IncorrectPasscode,
		KeyUnavailable,
		Failed,
		Empty,
	};

	[[nodiscard]] StartModernResult startModern(
		const QByteArray &passcode,
		const PrecomputedPasscodeKey &precomputed);
	void startWithSingleAccount(
		const QByteArray &passcode,
		std::unique_ptr<Main::Account> account);
	void generateLocalKey();
	[[nodiscard]] bool encryptLocalKey(const QByteArray &passcode);
	void applyPasscodeKey(
		const QByteArray &passcode,
		const QByteArray &salt,
		const PasscodeKdf &kdf,
		MTP::AuthKeyPtr key);
	void finishPasscodeChange();

	const not_null<Main::Domain*> _owner;
	const QString _dataName;

	MTP::AuthKeyPtr _localKey;
	MTP::AuthKeyPtr _passcodeKey;
	QByteArray _passcodeKeySalt;
	QByteArray _passcodeKeyEncrypted;
	PasscodeKdf _passcodeKdf;
	quint32 _keyGeneration = 0;
	std::optional<PasscodeWrap> _keptWrap;
	bool _keyFileOutdated = false;
	int _oldVersion = 0;

	bool _hasLocalPasscode = false;
	rpl::event_stream<> _passcodeKeyChanged;

};

} // namespace Storage
