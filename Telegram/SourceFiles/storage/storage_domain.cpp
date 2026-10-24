/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/storage_domain.h"

#include "ayu/features/fake_passcode/fake_passcode.h"
#include "core/version.h"
#include "storage/details/storage_file_utilities.h"
#include "storage/serialize_common.h"
#include "mtproto/mtproto_config.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "base/random.h"
#include "ayu/ayu_settings.h"
#include "ayu/data/fresh_vault.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"

namespace Storage {
namespace {

using namespace details;

[[nodiscard]] QString BaseGlobalPath() {
	return cWorkingDir() + u"tdata/"_q;
}

[[nodiscard]] QString ComputeKeyName(const QString &dataName) {
	// We dropped old test authorizations when migrated to multi auth.
	//return "key_" + dataName + (cTestMode() ? "[test]" : "");
	return "key_" + dataName;
}

} // namespace

Domain::Domain(not_null<Main::Domain*> owner, const QString &dataName)
: _owner(owner)
, _dataName(dataName) {
}

Domain::~Domain() = default;

StartResult Domain::start(
		const QByteArray &passcode,
		const PrecomputedPasscodeKey &precomputed) {
	const auto modern = startModern(passcode, precomputed);
	if (modern == StartModernResult::Success) {
		if (_oldVersion < AppVersion) {
			writeAccounts();
		}
		return StartResult::Success;
	} else if (modern == StartModernResult::IncorrectPasscode) {
		return StartResult::IncorrectPasscode;
	} else if (modern == StartModernResult::KeyUnavailable) {
		return StartResult::KeyUnavailable;
	} else if (modern == StartModernResult::Failed) {
		// startModern() may have already read the local key before failing.
		_localKey = nullptr;
		_passcodeKey = nullptr;
		_passcodeKeySalt = QByteArray();
		_passcodeKeyEncrypted = QByteArray();
		_passcodeKdf = PasscodeKdf();
		_hasLocalPasscode = false;
		startFromScratch();
		return StartResult::Success;
	}
	auto legacy = std::make_unique<Main::Account>(_owner, _dataName, 0);
	const auto result = legacy->legacyStart(passcode);
	if (result == StartResult::Success) {
		_oldVersion = legacy->local().oldMapVersion();
		startWithSingleAccount(passcode, std::move(legacy));
	}
	return result;
}

void Domain::startAdded(
		not_null<Main::Account*> account,
		std::unique_ptr<MTP::Config> config) {
	Expects(_localKey != nullptr);

	account->prepareToStartAdded(_localKey);
	account->start(std::move(config));
}

void Domain::startWithSingleAccount(
		const QByteArray &passcode,
		std::unique_ptr<Main::Account> account) {
	Expects(account != nullptr);

	if (auto localKey = account->local().peekLegacyLocalKey()) {
		_localKey = std::move(localKey);
		FreshVault::SetKey(_localKey);
		AyuSettings::attachVault();
		[[maybe_unused]] const auto encrypted = encryptLocalKey(passcode);
		account->start(nullptr);
	} else {
		generateLocalKey();
		account->start(account->prepareToStart(_localKey));
	}
	_owner->accountAddedInStorage(Main::Domain::AccountWithIndex{
		.account = std::move(account)
	});
	writeAccounts();
}

void Domain::generateLocalKey() {
	Expects(_localKey == nullptr);
	Expects(_passcodeKeySalt.isEmpty());
	Expects(_passcodeKeyEncrypted.isEmpty());

	auto pass = QByteArray(MTP::AuthKey::kSize, Qt::Uninitialized);
	auto salt = QByteArray(LocalEncryptSaltSize, Qt::Uninitialized);
	base::RandomFill(pass.data(), pass.size());
	base::RandomFill(salt.data(), salt.size());
	_localKey = CreateLocalKey(pass, salt);
	FreshVault::SetKey(_localKey);
	AyuSettings::attachVault();

	[[maybe_unused]] const auto encrypted = encryptLocalKey(QByteArray());
}

bool Domain::encryptLocalKey(const QByteArray &passcode) {
	const auto kdf = passcode.isEmpty() ? PasscodeKdf() : _passcodeKdf;
	auto salt = QByteArray(LocalEncryptSaltSize, Qt::Uninitialized);
	base::RandomFill(salt.data(), salt.size());
	auto key = DerivePasscodeKey(passcode, salt, kdf);
	if (!key) {
		LOG(("App Error: could not derive the passcode key."));
		return false;
	}
	applyPasscodeKey(passcode, salt, kdf, std::move(key));
	return true;
}

void Domain::applyPasscodeKey(
		const QByteArray &passcode,
		const QByteArray &salt,
		const PasscodeKdf &kdf,
		MTP::AuthKeyPtr key) {
	Expects(key != nullptr);

	_passcodeKdf = passcode.isEmpty() ? PasscodeKdf() : kdf;
	_passcodeKeySalt = salt;
	_passcodeKey = std::move(key);

	EncryptedDescriptor passKeyData(MTP::AuthKey::kSize);
	_localKey->write(passKeyData.stream);
	_passcodeKeyEncrypted = PrepareEncrypted(passKeyData, _passcodeKey);
	_hasLocalPasscode = !passcode.isEmpty();
}

Domain::StartModernResult Domain::startModern(
		const QByteArray &passcode,
		const PrecomputedPasscodeKey &precomputed) {
	const auto name = ComputeKeyName(_dataName);

	FileReadDescriptor keyData;
	if (!ReadFile(keyData, name, BaseGlobalPath())) {
		return StartModernResult::Empty;
	}
	LOG(("App Info: reading accounts info..."));

	QByteArray salt, keyEncrypted, infoEncrypted, kdfData;
	keyData.stream >> salt >> keyEncrypted >> infoEncrypted;
	if (!keyData.stream.atEnd()) {
		keyData.stream >> kdfData;
	}
	if (!CheckStreamStatus(keyData.stream)) {
		return StartModernResult::Failed;
	}

	if (salt.size() != LocalEncryptSaltSize) {
		LOG(("App Error: bad salt in info file, size: %1").arg(salt.size()));
		return StartModernResult::Failed;
	}
	const auto kdf = DeserializePasscodeKdf(kdfData);
	if (!kdf) {
		// WHY: an unknown KDF must neither wipe the data like a broken file
		// does nor count as a wrong passcode.
		LOG(("App Error: unknown passcode KDF in info file."));
		return StartModernResult::KeyUnavailable;
	}
	_passcodeKey = precomputed.matches(salt, *kdf)
		? precomputed.key
		: DerivePasscodeKey(passcode, salt, *kdf);
	if (!_passcodeKey) {
		LOG(("App Error: could not derive the passcode key."));
		return StartModernResult::KeyUnavailable;
	}

	EncryptedDescriptor keyInnerData, info;
	if (!DecryptLocal(keyInnerData, keyEncrypted, _passcodeKey)) {
		LOG(("App Info: could not decrypt pass-protected key from info file, "
			"maybe bad password..."));
		return StartModernResult::IncorrectPasscode;
	}
	auto key = Serialize::read<MTP::AuthKey::Data>(keyInnerData.stream);
	if (keyInnerData.stream.status() != QDataStream::Ok
		|| !keyInnerData.stream.atEnd()) {
		LOG(("App Error: could not read pass-protected key from info file"));
		return StartModernResult::Failed;
	}
	_localKey = std::make_shared<MTP::AuthKey>(key);
	FreshVault::SetKey(_localKey);
	AyuSettings::attachVault();

	_passcodeKeyEncrypted = keyEncrypted;
	_passcodeKeySalt = salt;
	_passcodeKdf = *kdf;
	_hasLocalPasscode = !passcode.isEmpty();

	if (!DecryptLocal(info, infoEncrypted, _localKey)) {
		LOG(("App Error: could not decrypt info."));
		return StartModernResult::Failed;
	}
	LOG(("App Info: reading encrypted info..."));
	auto count = qint32();
	info.stream >> count;
	if (count <= 0 || count > Main::Domain::kPremiumMaxAccounts) {
		LOG(("App Error: bad accounts count: %1").arg(count));
		return StartModernResult::Failed;
	}

	_oldVersion = keyData.version;

	auto tried = base::flat_set<int>();
	auto sessions = base::flat_set<uint64>();
	auto active = 0;
	for (auto i = 0; i != count; ++i) {
		auto index = qint32();
		info.stream >> index;
		if (index >= 0
			&& index < Main::Domain::kPremiumMaxAccounts
			&& tried.emplace(index).second) {
			auto account = std::make_unique<Main::Account>(
				_owner,
				_dataName,
				index);
			auto config = account->prepareToStart(_localKey);
			const auto sessionId = account->willHaveSessionUniqueId(
				config.get());
			if (!sessions.contains(sessionId)
				&& (sessionId != 0 || (sessions.empty() && i + 1 == count))) {
				if (sessions.empty()) {
					active = index;
				}
				account->start(std::move(config));
				_owner->accountAddedInStorage({
					.index = index,
					.account = std::move(account)
				});
				sessions.emplace(sessionId);
			}
		}
	}
	if (sessions.empty()) {
		LOG(("App Error: no accounts read."));
		return StartModernResult::Failed;
	}

	if (!info.stream.atEnd()) {
		info.stream >> active;
	}
	_owner->activateFromStorage(active);

	Ensures(!sessions.empty());
	return StartModernResult::Success;
}

void Domain::writeAccounts() {
	Expects(!_owner->accounts().empty());

	const auto path = BaseGlobalPath();
	if (!QDir().exists(path)) {
		QDir().mkpath(path);
	}

	FileWriteDescriptor key(ComputeKeyName(_dataName), path);
	key.writeData(_passcodeKeySalt);
	key.writeData(_passcodeKeyEncrypted);

	const auto &list = _owner->accounts();

	auto keySize = sizeof(qint32) + sizeof(qint32) * list.size();

	EncryptedDescriptor keyData(keySize);
	keyData.stream << qint32(list.size());
	for (const auto &[index, account] : list) {
		keyData.stream << qint32(index);
	}
	keyData.stream << qint32(_owner->activeForStorage());
	key.writeEncrypted(keyData, _localKey);

	if (const auto kdf = SerializePasscodeKdf(_passcodeKdf); !kdf.isEmpty()) {
		key.writeData(kdf);
	}
}

void Domain::startFromScratch() {
	startWithSingleAccount(
		QByteArray(),
		std::make_unique<Main::Account>(_owner, _dataName, 0));
}

bool Domain::checkPasscode(const QByteArray &passcode) const {
	return checkPasscodeResult(passcode) == PasscodeCheck::Correct;
}

PasscodeCheck Domain::checkPasscodeResult(const QByteArray &passcode) const {
	Expects(!_passcodeKeySalt.isEmpty());

	return checkPasscodeKey({
		.salt = _passcodeKeySalt,
		.kdf = _passcodeKdf,
		.key = DerivePasscodeKey(passcode, _passcodeKeySalt, _passcodeKdf),
	});
}

PasscodeCheck Domain::checkPasscodeKey(
		const PrecomputedPasscodeKey &precomputed) const {
	Expects(_passcodeKey != nullptr);

	if (!precomputed.key) {
		return PasscodeCheck::Failed;
	} else if (!precomputed.matches(_passcodeKeySalt, _passcodeKdf)) {
		return PasscodeCheck::Wrong;
	}
	return precomputed.key->equals(_passcodeKey)
		? PasscodeCheck::Correct
		: PasscodeCheck::Wrong;
}

bool Domain::setPasscode(const QByteArray &passcode) {
	Expects(!_passcodeKeySalt.isEmpty());
	Expects(_localKey != nullptr);

	if (!encryptLocalKey(passcode)) {
		return false;
	}
	writeAccounts();
	FreshFakePasscode::RealPasscodeChanged(passcode);
	finishPasscodeChange();
	return true;
}

void Domain::finishPasscodeChange() {
	_passcodeKeyChanged.fire({});
}

void Domain::setPasscodeAsync(
		const QByteArray &passcode,
		const PasscodeKdf &kdf,
		Fn<void(bool)> done) {
	Expects(_localKey != nullptr);

	const auto useKdf = passcode.isEmpty() ? PasscodeKdf() : kdf;
	auto salt = QByteArray(LocalEncryptSaltSize, Qt::Uninitialized);
	base::RandomFill(salt.data(), salt.size());
	auto fake = std::make_shared<FreshFakePasscode::RebuildTask>(
		FreshFakePasscode::PrepareRebuild(passcode, useKdf));
	crl::async([=, weak = base::make_weak(this)] {
		auto key = DerivePasscodeKey(passcode, salt, useKdf);
		if (key) {
			FreshFakePasscode::ComputeRebuild(*fake);
		}
		crl::on_main([=, key = std::move(key)]() mutable {
			const auto strong = weak.get();
			if (!strong || !key || !fake->ok) {
				done(false);
				return;
			}
			strong->applyPasscodeKey(passcode, salt, useKdf, std::move(key));
			strong->writeAccounts();
			FreshFakePasscode::ApplyRebuild(*fake);
			strong->finishPasscodeChange();
			done(true);
		});
	});
}

auto Domain::passcodeKdfInput() const -> std::optional<PasscodeKdfInput> {
	if (_localKey) {
		return PasscodeKdfInput{ _passcodeKeySalt, _passcodeKdf };
	}
	FileReadDescriptor keyData;
	if (!ReadFile(keyData, ComputeKeyName(_dataName), BaseGlobalPath())) {
		return std::nullopt;
	}
	QByteArray salt, keyEncrypted, infoEncrypted, kdfData;
	keyData.stream >> salt >> keyEncrypted >> infoEncrypted;
	if (!keyData.stream.atEnd()) {
		keyData.stream >> kdfData;
	}
	if (keyData.stream.status() != QDataStream::Ok
		|| salt.size() != LocalEncryptSaltSize) {
		return std::nullopt;
	}
	const auto kdf = DeserializePasscodeKdf(kdfData);
	if (!kdf) {
		return std::nullopt;
	}
	return PasscodeKdfInput{ salt, *kdf };
}

int Domain::oldVersion() const {
	return _oldVersion;
}

void Domain::clearOldVersion() {
	_oldVersion = 0;
}

rpl::producer<> Domain::localPasscodeChanged() const {
	return _passcodeKeyChanged.events();
}

bool Domain::hasLocalPasscode() const {
	return _hasLocalPasscode;
}

} // namespace Storage
