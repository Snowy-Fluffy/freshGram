#include "ayu/secret/secret_crypto.h"

#include <openssl/aes.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace FreshSecret {
namespace {

struct Kdf {
	uint8_t aesKey[32];
	uint8_t aesIv[32];
};

Kdf DeriveKdf(const Bytes &key, const uint8_t *msgKey, int x) {
	uint8_t first[16 + 36];
	std::memcpy(first, msgKey, 16);
	std::memcpy(first + 16, key.data() + x, 36);
	uint8_t a[32];
	SHA256(first, sizeof(first), a);

	uint8_t second[36 + 16];
	std::memcpy(second, key.data() + 40 + x, 36);
	std::memcpy(second + 36, msgKey, 16);
	uint8_t b[32];
	SHA256(second, sizeof(second), b);

	auto result = Kdf();
	std::memcpy(result.aesKey, a, 8);
	std::memcpy(result.aesKey + 8, b + 8, 16);
	std::memcpy(result.aesKey + 24, a + 24, 8);
	std::memcpy(result.aesIv, b, 8);
	std::memcpy(result.aesIv + 8, a + 8, 16);
	std::memcpy(result.aesIv + 24, b + 24, 8);
	return result;
}

void ComputeMsgKey(
		const Bytes &key,
		int x,
		const uint8_t *data,
		size_t size,
		uint8_t *msgKey) {
	auto buffer = Bytes(32 + size);
	std::memcpy(buffer.data(), key.data() + 88 + x, 32);
	std::memcpy(buffer.data() + 32, data, size);
	uint8_t large[32];
	SHA256(buffer.data(), buffer.size(), large);
	std::memcpy(msgKey, large + 8, 16);
}

void IgeEncrypt(const Kdf &kdf, const uint8_t *from, uint8_t *to, size_t size) {
	AES_KEY aes;
	AES_set_encrypt_key(kdf.aesKey, 256, &aes);
	uint8_t iv[32];
	std::memcpy(iv, kdf.aesIv, 32);
	AES_ige_encrypt(from, to, size, &aes, iv, AES_ENCRYPT);
}

void IgeDecrypt(const Kdf &kdf, const uint8_t *from, uint8_t *to, size_t size) {
	AES_KEY aes;
	AES_set_decrypt_key(kdf.aesKey, 256, &aes);
	uint8_t iv[32];
	std::memcpy(iv, kdf.aesIv, 32);
	AES_ige_encrypt(from, to, size, &aes, iv, AES_DECRYPT);
}

} // namespace

void RandomBytes(uint8_t *data, size_t size) {
	if (size && RAND_bytes(data, int(size)) != 1) {
		std::abort();
	}
}

Bytes RandomVector(size_t size) {
	auto result = Bytes(size);
	if (size) {
		RandomBytes(result.data(), size);
	}
	return result;
}

int64_t KeyFingerprint(const Bytes &key) {
	uint8_t digest[20];
	SHA1(key.data(), key.size(), digest);
	int64_t result = 0;
	std::memcpy(&result, digest + 12, sizeof(result));
	return result;
}

Bytes KeyVisualHash(const Bytes &key) {
	uint8_t sha1[20];
	SHA1(key.data(), key.size(), sha1);
	uint8_t sha256[32];
	SHA256(key.data(), key.size(), sha256);
	auto result = Bytes(36);
	std::memcpy(result.data(), sha1, 16);
	std::memcpy(result.data() + 16, sha256, 20);
	return result;
}

void UpdateKeyVisualHash(Bytes &hash, const Bytes &key, int layer) {
	if (key.size() != kKeySize || hash.size() == 36) {
		return;
	}
	// PFS rotates working keys, never the saved authentication hash.
	const auto current = KeyVisualHash(key);
	if (hash.empty()) {
		hash.assign(current.begin(), current.begin() + 16);
	}
	if (hash.size() == 16 && layer >= 46) {
		hash.insert(hash.end(), current.begin() + 16, current.end());
	}
}

Bytes PadKey(const Bytes &key) {
	if (key.size() >= kKeySize) {
		return key;
	}
	auto result = Bytes(kKeySize - key.size(), 0);
	result.insert(result.end(), key.begin(), key.end());
	return result;
}

Bytes EncryptPacket(
		const Bytes &key,
		bool creator,
		const Bytes &object) {
	const auto x = creator ? 0 : 8;
	const auto extra = RandomVector(1);
	const auto total = (4 + object.size() + 12 + size_t(extra[0]) + 15)
		& ~size_t(15);
	auto plain = Bytes(total);
	const auto length = int32_t(object.size());
	std::memcpy(plain.data(), &length, 4);
	std::memcpy(plain.data() + 4, object.data(), object.size());
	const auto padStart = 4 + object.size();
	RandomBytes(plain.data() + padStart, total - padStart);

	uint8_t msgKey[16];
	ComputeMsgKey(key, x, plain.data(), plain.size(), msgKey);
	const auto kdf = DeriveKdf(key, msgKey, x);

	auto result = Bytes(8 + 16 + total);
	const auto fingerprint = KeyFingerprint(key);
	std::memcpy(result.data(), &fingerprint, 8);
	std::memcpy(result.data() + 8, msgKey, 16);
	IgeEncrypt(kdf, plain.data(), result.data() + 24, total);
	return result;
}

bool DecryptPacket(
		const Bytes &key,
		bool creator,
		const Bytes &packet,
		Bytes &object) {
	if (key.size() != kKeySize || packet.size() < 8 + 16 + 16) {
		return false;
	}
	int64_t fingerprint = 0;
	std::memcpy(&fingerprint, packet.data(), 8);
	if (fingerprint != KeyFingerprint(key)) {
		return false;
	}
	const auto x = creator ? 8 : 0;
	const auto encrypted = packet.size() - 24;
	const auto size = encrypted - (encrypted & 15);
	if (size < 16) {
		return false;
	}
	const auto kdf = DeriveKdf(key, packet.data() + 8, x);
	auto plain = Bytes(size);
	IgeDecrypt(kdf, packet.data() + 24, plain.data(), size);

	uint8_t expected[16];
	ComputeMsgKey(key, x, plain.data(), plain.size(), expected);
	auto diff = 0;
	for (auto i = 0; i != 16; ++i) {
		diff |= expected[i] ^ packet[8 + i];
	}
	if (diff != 0) {
		return false;
	}
	int32_t length = 0;
	std::memcpy(&length, plain.data(), 4);
	if (length <= 0 || (length & 3) != 0 || size_t(length) + 4 > size) {
		return false;
	}
	const auto padding = size - 4 - size_t(length);
	if (padding < 12 || padding > 1024) {
		return false;
	}
	object.assign(plain.begin() + 4, plain.begin() + 4 + length);
	return true;
}

} // namespace FreshSecret
