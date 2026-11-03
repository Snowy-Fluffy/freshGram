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

// "KD2\0", the key file trailer of Telegram Desktop since 7.3.
constexpr auto kKeyDataMagic = quint32(0x4B443200);
constexpr auto kKeyDataFormatVersion = quint32(2);
constexpr auto kMaxWrapCount = quint32(2);
constexpr auto kWrapSaltMinSize = 8;

struct KeyFile {
	QByteArray salt;
	QByteArray keyEncrypted;
	QByteArray infoEncrypted;
	PasscodeKdf kdf;
	std::optional<PasscodeWrap> wrap;
	quint32 generation = 0;
	bool wrapped = false;
};

enum class ReadKeyFileResult {
	Success,
	UnknownKdf,
	Failed,
};

[[nodiscard]] QString BaseGlobalPath() {
	return cWorkingDir() + u"tdata/"_q;
}

[[nodiscard]] QString ComputeKeyName(const QString &dataName) {
	// We dropped old test authorizations when migrated to multi auth.
	//return "key_" + dataName + (cTestMode() ? "[test]" : "");
	return "key_" + dataName;
}

[[nodiscard]] QByteArray RandomSalt() {
	auto result = QByteArray(LocalEncryptSaltSize, Qt::Uninitialized);
	base::RandomFill(result.data(), result.size());
	return result;
}

[[nodiscard]] ReadKeyFileResult ReadWrappedTrailer(
		const QByteArray &trailer,
		KeyFile &file) {
	auto stream = QDataStream(trailer);
	stream.setVersion(QDataStream::Qt_5_1);
	auto magic = quint32();
	auto version = quint32();
	auto count = quint32();
	stream >> magic >> version >> file.generation >> count;
	if (stream.status() != QDataStream::Ok
		|| magic != kKeyDataMagic
		|| version > kKeyDataFormatVersion) {
		return ReadKeyFileResult::UnknownKdf;
	} else if (count > kMaxWrapCount) {
		LOG(("App Error: bad key data wrap count: %1").arg(count));
		return ReadKeyFileResult::Failed;
	}
	file.wrapped = true;
	auto unknown = false;
	for (auto i = quint32(); i != count; ++i) {
		auto kind = quint32();
		auto generation = quint32();
		auto wrap = PasscodeWrap();
		stream
			>> kind
			>> generation
			>> wrap.kdf.memoryKiB
			>> wrap.kdf.iterations
			>> wrap.kdf.lanes
			>> wrap.salt
			>> wrap.keyEncrypted;
		if (!CheckStreamStatus(stream)
			|| wrap.salt.size() < kWrapSaltMinSize) {
			return ReadKeyFileResult::Failed;
		} else if (generation != file.generation) {
			continue;
		} else if (file.wrap) {
			LOG(("App Error: duplicate key data wrap generation."));
			return ReadKeyFileResult::Failed;
		}
		wrap.kdf.type = PasscodeKdfType(kind);
		if (kind == quint32(PasscodeKdfType::Pbkdf2)
			|| kind > quint32(PasscodeKdfType::Scrypt)
			|| !PasscodeKdfValid(wrap.kdf)) {
			unknown = true;
		}
		file.wrap = std::move(wrap);
	}
	return unknown
		? ReadKeyFileResult::UnknownKdf
		: ReadKeyFileResult::Success;
}

[[nodiscard]] ReadKeyFileResult ReadKeyFile(
		QDataStream &stream,
		KeyFile &file) {
	auto trailer = QByteArray();
	stream >> file.salt >> file.keyEncrypted >> file.infoEncrypted;
	if (!stream.atEnd()) {
		stream >> trailer;
	}
	if (!CheckStreamStatus(stream)) {
		return ReadKeyFileResult::Failed;
	} else if (file.salt.size() != LocalEncryptSaltSize) {
		LOG(("App Error: bad salt in info file, size: %1"
			).arg(file.salt.size()));
		return ReadKeyFileResult::Failed;
	} else if (trailer.isEmpty()) {
		return ReadKeyFileResult::Success;
	} else if (const auto kdf = DeserializePasscodeKdf(trailer)) {
		file.kdf = *kdf;
		return ReadKeyFileResult::Success;
	}
	return ReadWrappedTrailer(trailer, file);
}

[[nodiscard]] QByteArray SerializeWrappedTrailer(
		quint32 generation,
		const PasscodeWrap &wrap) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream
		<< kKeyDataMagic
		<< kKeyDataFormatVersion
		<< generation
		<< quint32(1)
		<< quint32(wrap.kdf.type)
		<< generation
		<< wrap.kdf.memoryKiB
		<< wrap.kdf.iterations
		<< wrap.kdf.lanes
		<< wrap.salt
		<< wrap.keyEncrypted;
	return result;
}

[[nodiscard]] std::optional<PasscodeWrap> WrapToOpen(
		const KeyFile &file,
		bool withPasscode) {
	if (!file.wrapped) {
		return PasscodeWrap{ file.kdf, file.salt, file.keyEncrypted };
	} else if (withPasscode) {
		return file.wrap;
	} else if (file.keyEncrypted.isEmpty()) {
		return std::nullopt;
	}
	return PasscodeWrap{ PasscodeKdf(), file.salt, file.keyEncrypted };
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
		if (_oldVersion < AppVersion || _keyFileOutdated) {
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
	base::RandomFill(pass.data(), pass.size());
	_localKey = CreateLocalKey(pass, RandomSalt());
	FreshVault::SetKey(_localKey);
	AyuSettings::attachVault();

	[[maybe_unused]] const auto encrypted = encryptLocalKey(QByteArray());
}

bool Domain::encryptLocalKey(const QByteArray &passcode) {
	const auto kdf = passcode.isEmpty() ? PasscodeKdf() : _passcodeKdf;
	const auto salt = RandomSalt();
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
	_keptWrap = std::nullopt;
	++_keyGeneration;
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

	auto file = KeyFile();
	const auto read = ReadKeyFile(keyData.stream, file);
	if (read == ReadKeyFileResult::Failed) {
		return StartModernResult::Failed;
	} else if (read == ReadKeyFileResult::UnknownKdf) {
		// WHY: an unknown KDF must neither wipe the data like a broken file
		// does nor count as a wrong passcode.
		LOG(("App Error: unknown passcode KDF in info file."));
		return StartModernResult::KeyUnavailable;
	}
	const auto wrap = WrapToOpen(file, !passcode.isEmpty());
	if (!wrap) {
		LOG(("App Info: no passcode wrap to open the local key with."));
		return StartModernResult::IncorrectPasscode;
	}
	_passcodeKey = precomputed.matches(wrap->salt, wrap->kdf)
		? precomputed.key
		: DerivePasscodeKey(passcode, wrap->salt, wrap->kdf);
	if (!_passcodeKey) {
		LOG(("App Error: could not derive the passcode key."));
		return StartModernResult::KeyUnavailable;
	}

	EncryptedDescriptor keyInnerData, info;
	if (!DecryptLocal(keyInnerData, wrap->keyEncrypted, _passcodeKey)) {
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

	_passcodeKeyEncrypted = wrap->keyEncrypted;
	_passcodeKeySalt = wrap->salt;
	_passcodeKdf = wrap->kdf;
	_hasLocalPasscode = !passcode.isEmpty();
	_keyGeneration = file.generation;
	_keptWrap = _hasLocalPasscode ? std::nullopt : file.wrap;
	_keyFileOutdated = !file.wrapped
		&& (_passcodeKdf.type != PasscodeKdfType::Pbkdf2);

	if (!DecryptLocal(info, file.infoEncrypted, _localKey)) {
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

	// WHY: Telegram Desktop 7.3 rejects a PBKDF2 wrap, while it reads
	// the three blobs alone as a file of its older versions.
	const auto wrapped = (_passcodeKdf.type != PasscodeKdfType::Pbkdf2);
	FileWriteDescriptor key(ComputeKeyName(_dataName), path);
	key.writeData(wrapped ? RandomSalt() : _passcodeKeySalt);
	key.writeData(wrapped ? QByteArray() : _passcodeKeyEncrypted);

	const auto &list = _owner->accounts();

	auto keySize = sizeof(qint32) + sizeof(qint32) * list.size();

	EncryptedDescriptor keyData(keySize);
	keyData.stream << qint32(list.size());
	for (const auto &[index, account] : list) {
		keyData.stream << qint32(index);
	}
	keyData.stream << qint32(_owner->activeForStorage());
	key.writeEncrypted(keyData, _localKey);

	if (wrapped) {
		key.writeData(SerializeWrappedTrailer(_keyGeneration, {
			.kdf = _passcodeKdf,
			.salt = _passcodeKeySalt,
			.keyEncrypted = _passcodeKeyEncrypted,
		}));
	} else if (_keptWrap) {
		key.writeData(SerializeWrappedTrailer(_keyGeneration, *_keptWrap));
	}
	_keyFileOutdated = false;
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
	const auto salt = RandomSalt();
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
	auto file = KeyFile();
	if (ReadKeyFile(keyData.stream, file) != ReadKeyFileResult::Success) {
		return std::nullopt;
	}
	const auto wrap = WrapToOpen(file, true);
	if (!wrap) {
		return std::nullopt;
	}
	return PasscodeKdfInput{ wrap->salt, wrap->kdf };
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
