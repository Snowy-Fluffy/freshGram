#include "ayu/secret/secret_vault.h"

#include "core/application.h"
#include "main/main_domain.h"
#include "mtproto/mtproto_auth_key.h"
#include "storage/storage_domain.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <array>
#include <cstring>

namespace FreshSecret::Vault {
namespace {

constexpr uint8_t kMagic[4] = { 'F', 'G', 'V', '1' };
constexpr uint8_t kMagicBound[4] = { 'F', 'G', 'V', '2' };
constexpr auto kNonceSize = 12;
constexpr auto kTagSize = 16;
constexpr auto kHeaderSize = 4 + kNonceSize + kTagSize;

[[nodiscard]] bool DeriveKey(std::array<uint8_t, 32> &key) {
	const auto local = Core::App().domain().local().localKey();
	if (!local) {
		return false;
	}
	const auto data = local->data();
	auto context = SHA256_CTX();
	SHA256_Init(&context);
	static const char kLabel[] = "freshGram/secret-chats/vault/v1";
	SHA256_Update(&context, kLabel, sizeof(kLabel) - 1);
	SHA256_Update(&context, data.data(), data.size());
	SHA256_Final(key.data(), &context);
	return true;
}

} // namespace

bool Available() {
	return Core::App().domain().local().localKey() != nullptr;
}

bool IsSealed(const Bytes &data) {
	return data.size() >= kHeaderSize
		&& (std::memcmp(data.data(), kMagic, sizeof(kMagic)) == 0
			|| std::memcmp(
				data.data(),
				kMagicBound,
				sizeof(kMagicBound)) == 0);
}

bool IsBound(const Bytes &data) {
	return data.size() >= kHeaderSize
		&& std::memcmp(
			data.data(),
			kMagicBound,
			sizeof(kMagicBound)) == 0;
}

Bytes Seal(const Bytes &plain, const std::string &context) {
	auto key = std::array<uint8_t, 32>();
	if (!DeriveKey(key)) {
		return {};
	}
	auto result = Bytes(kHeaderSize + plain.size());
	std::memcpy(result.data(), kMagicBound, sizeof(kMagicBound));
	RandomBytes(result.data() + 4, kNonceSize);
	const auto cipher = EVP_CIPHER_CTX_new();
	if (!cipher) {
		return {};
	}
	auto ok = EVP_EncryptInit_ex(
		cipher,
		EVP_aes_256_gcm(),
		nullptr,
		key.data(),
		result.data() + 4) == 1;
	auto length = 0;
	if (ok && !context.empty()) {
		ok = EVP_EncryptUpdate(
			cipher,
			nullptr,
			&length,
			reinterpret_cast<const uint8_t*>(context.data()),
			int(context.size())) == 1;
	}
	if (ok && !plain.empty()) {
		ok = EVP_EncryptUpdate(
			cipher,
			result.data() + kHeaderSize,
			&length,
			plain.data(),
			int(plain.size())) == 1;
	}
	auto finalLength = 0;
	if (ok) {
		auto tail = std::array<uint8_t, 16>();
		ok = EVP_EncryptFinal_ex(cipher, tail.data(), &finalLength) == 1;
	}
	if (ok) {
		ok = EVP_CIPHER_CTX_ctrl(
			cipher,
			EVP_CTRL_GCM_GET_TAG,
			kTagSize,
			result.data() + 4 + kNonceSize) == 1;
	}
	EVP_CIPHER_CTX_free(cipher);
	if (!ok) {
		return {};
	}
	return result;
}

bool Open(
		const Bytes &sealed,
		Bytes &plain,
		const std::string &context) {
	if (!IsSealed(sealed)) {
		return false;
	}
	auto key = std::array<uint8_t, 32>();
	if (!DeriveKey(key)) {
		return false;
	}
	const auto bound = IsBound(sealed);
	const auto size = sealed.size() - kHeaderSize;
	plain.assign(size, 0);
	const auto cipher = EVP_CIPHER_CTX_new();
	if (!cipher) {
		return false;
	}
	auto ok = EVP_DecryptInit_ex(
		cipher,
		EVP_aes_256_gcm(),
		nullptr,
		key.data(),
		sealed.data() + 4) == 1;
	auto length = 0;
	if (ok) {
		auto tag = std::array<uint8_t, kTagSize>();
		std::memcpy(tag.data(), sealed.data() + 4 + kNonceSize, kTagSize);
		ok = EVP_CIPHER_CTX_ctrl(
			cipher,
			EVP_CTRL_GCM_SET_TAG,
			kTagSize,
			tag.data()) == 1;
	}
	if (ok && bound && !context.empty()) {
		ok = EVP_DecryptUpdate(
			cipher,
			nullptr,
			&length,
			reinterpret_cast<const uint8_t*>(context.data()),
			int(context.size())) == 1;
	}
	if (ok && size > 0) {
		ok = EVP_DecryptUpdate(
			cipher,
			plain.data(),
			&length,
			sealed.data() + kHeaderSize,
			int(size)) == 1;
	}
	if (ok) {
		auto tail = std::array<uint8_t, 16>();
		auto finalLength = 0;
		ok = EVP_DecryptFinal_ex(cipher, tail.data(), &finalLength) == 1;
	}
	EVP_CIPHER_CTX_free(cipher);
	if (!ok) {
		plain.clear();
		return false;
	}
	return true;
}

bool SealToFile(
		const QString &path,
		const Bytes &plain,
		const std::string &context) {
	const auto sealed = Seal(plain, context);
	if (sealed.empty()) {
		return false;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	const auto written = file.write(
		reinterpret_cast<const char*>(sealed.data()),
		qint64(sealed.size()));
	return (written == qint64(sealed.size())) && file.commit();
}

bool OpenFromFile(
		const QString &path,
		Bytes &plain,
		const std::string &context,
		bool *legacy) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto raw = file.readAll();
	file.close();
	const auto sealed = Bytes(raw.begin(), raw.end());
	if (legacy) {
		*legacy = !IsBound(sealed);
	}
	return Open(sealed, plain, context);
}

} // namespace FreshSecret::Vault
