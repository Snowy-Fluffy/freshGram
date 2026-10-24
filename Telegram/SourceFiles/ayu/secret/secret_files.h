#pragma once

#include "ayu/secret/secret_crypto.h"

namespace FreshSecret {

struct FileKey {
	Bytes key;
	Bytes iv;
};

[[nodiscard]] FileKey GenerateFileKey();
[[nodiscard]] int32_t FileFingerprint(const Bytes &key, const Bytes &iv);

[[nodiscard]] Bytes EncryptFile(
	const Bytes &plain,
	const Bytes &key,
	const Bytes &iv);
[[nodiscard]] bool DecryptFile(
	const Bytes &encrypted,
	const Bytes &key,
	const Bytes &iv,
	int64_t size,
	Bytes &plain);

} // namespace FreshSecret
