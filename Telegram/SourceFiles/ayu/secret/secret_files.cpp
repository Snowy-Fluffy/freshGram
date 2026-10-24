#include "ayu/secret/secret_files.h"

#include <openssl/aes.h>
#include <openssl/md5.h>

#include <cstring>

namespace FreshSecret {

FileKey GenerateFileKey() {
	auto result = FileKey();
	result.key = RandomVector(32);
	result.iv = RandomVector(32);
	return result;
}

int32_t FileFingerprint(const Bytes &key, const Bytes &iv) {
	auto joined = Bytes();
	joined.insert(joined.end(), key.begin(), key.end());
	joined.insert(joined.end(), iv.begin(), iv.end());
	uint8_t digest[16];
	MD5(joined.data(), joined.size(), digest);
	int32_t first = 0;
	int32_t second = 0;
	std::memcpy(&first, digest, 4);
	std::memcpy(&second, digest + 4, 4);
	return first ^ second;
}

Bytes EncryptFile(const Bytes &plain, const Bytes &key, const Bytes &iv) {
	const auto padded = (plain.size() + 15) & ~size_t(15);
	auto input = Bytes(padded);
	std::memcpy(input.data(), plain.data(), plain.size());
	if (padded > plain.size()) {
		RandomBytes(input.data() + plain.size(), padded - plain.size());
	}
	AES_KEY aes;
	AES_set_encrypt_key(key.data(), 256, &aes);
	uint8_t vector[32];
	std::memcpy(vector, iv.data(), 32);
	auto output = Bytes(padded);
	if (padded) {
		AES_ige_encrypt(input.data(), output.data(), padded, &aes, vector, AES_ENCRYPT);
	}
	return output;
}

bool DecryptFile(
		const Bytes &encrypted,
		const Bytes &key,
		const Bytes &iv,
		int64_t size,
		Bytes &plain) {
	if (key.size() != 32
		|| iv.size() != 32
		|| encrypted.size() % 16 != 0
		|| size < 0
		|| size_t(size) > encrypted.size()) {
		return false;
	}
	AES_KEY aes;
	AES_set_decrypt_key(key.data(), 256, &aes);
	uint8_t vector[32];
	std::memcpy(vector, iv.data(), 32);
	plain.resize(encrypted.size());
	if (!encrypted.empty()) {
		AES_ige_encrypt(encrypted.data(), plain.data(), encrypted.size(), &aes, vector, AES_DECRYPT);
	}
	plain.resize(size_t(size));
	return true;
}

} // namespace FreshSecret
