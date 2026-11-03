// This is the source code of freshGram.
#pragma once

namespace MTP {
class AuthKey;
using AuthKeyPtr = std::shared_ptr<AuthKey>;
} // namespace MTP

namespace Storage {

enum class PasscodeKdfType : uchar {
	Pbkdf2 = 0,
	Argon2id = 1,
	Scrypt = 2, // memoryKiB holds N, iterations r and lanes p
};

enum class PasscodeKdfLevel : uchar {
	Standard,
	Strong,
	Maximum,
};

struct PasscodeKdf {
	PasscodeKdfType type = PasscodeKdfType::Pbkdf2;
	quint32 memoryKiB = 0;
	quint32 iterations = 0;
	quint32 lanes = 0;

	friend inline bool operator==(
		const PasscodeKdf &,
		const PasscodeKdf &) = default;
};

[[nodiscard]] bool PasscodeKdfArgon2Supported();
[[nodiscard]] bool PasscodeKdfValid(const PasscodeKdf &kdf);
[[nodiscard]] PasscodeKdf PasscodeKdfFor(PasscodeKdfLevel level);
[[nodiscard]] PasscodeKdfLevel PasscodeKdfLevelOf(const PasscodeKdf &kdf);

// Empty for the standard PBKDF2, so old files keep their layout.
[[nodiscard]] QByteArray SerializePasscodeKdf(const PasscodeKdf &kdf);
[[nodiscard]] std::optional<PasscodeKdf> DeserializePasscodeKdf(
	const QByteArray &data);

// Thread safe. Returns nullptr when the KDF is not available.
[[nodiscard]] MTP::AuthKeyPtr DerivePasscodeKey(
	const QByteArray &passcode,
	const QByteArray &salt,
	const PasscodeKdf &kdf);

// A key derived on a thread for the salt and the KDF it was made with.
struct PrecomputedPasscodeKey {
	QByteArray salt;
	PasscodeKdf kdf;
	MTP::AuthKeyPtr key;

	[[nodiscard]] bool matches(
			const QByteArray &otherSalt,
			const PasscodeKdf &otherKdf) const {
		return key && (salt == otherSalt) && (kdf == otherKdf);
	}
};

} // namespace Storage
