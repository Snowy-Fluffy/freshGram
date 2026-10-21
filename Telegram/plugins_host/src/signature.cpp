#include "signature.h"

#include "sha256.h"

#include <openssl/evp.h>

#include <memory>

namespace fgpl {
namespace {

struct KeyDeleter {
	void operator()(EVP_PKEY *key) const {
		EVP_PKEY_free(key);
	}
};

struct ContextDeleter {
	void operator()(EVP_MD_CTX *context) const {
		EVP_MD_CTX_free(context);
	}
};

using Key = std::unique_ptr<EVP_PKEY, KeyDeleter>;
using Context = std::unique_ptr<EVP_MD_CTX, ContextDeleter>;

} // namespace

bool Ed25519Verify(
		const PublicKey &key,
		std::span<const uint8_t> message,
		const SignatureBytes &signature) {
	const auto pkey = Key(EVP_PKEY_new_raw_public_key(
		EVP_PKEY_ED25519,
		nullptr,
		key.data(),
		key.size()));
	const auto context = Context(EVP_MD_CTX_new());
	if (!pkey || !context
		|| EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) {
		return false;
	}
	return EVP_DigestVerify(
		context.get(),
		signature.data(),
		signature.size(),
		message.data(),
		message.size()) == 1;
}

std::optional<PublicKey> Ed25519PublicKey(const PrivateSeed &seed) {
	const auto pkey = Key(EVP_PKEY_new_raw_private_key(
		EVP_PKEY_ED25519,
		nullptr,
		seed.data(),
		seed.size()));
	if (!pkey) {
		return std::nullopt;
	}
	auto result = PublicKey();
	auto length = result.size();
	if (EVP_PKEY_get_raw_public_key(pkey.get(), result.data(), &length) != 1
		|| length != result.size()) {
		return std::nullopt;
	}
	return result;
}

std::optional<SignatureBytes> Ed25519Sign(
		const PrivateSeed &seed,
		std::span<const uint8_t> message) {
	const auto pkey = Key(EVP_PKEY_new_raw_private_key(
		EVP_PKEY_ED25519,
		nullptr,
		seed.data(),
		seed.size()));
	const auto context = Context(EVP_MD_CTX_new());
	if (!pkey || !context
		|| EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) {
		return std::nullopt;
	}
	auto result = SignatureBytes();
	auto length = result.size();
	if (EVP_DigestSign(
			context.get(),
			result.data(),
			&length,
			message.data(),
			message.size()) != 1
		|| length != result.size()) {
		return std::nullopt;
	}
	return result;
}

std::string Fingerprint(const PublicKey &key) {
	const auto digest = Sha256(key);
	constexpr auto digits = "0123456789abcdef";
	auto result = std::string();
	for (auto i = 0; i != 16; ++i) {
		if (i != 0 && i % 2 == 0) {
			result.push_back(' ');
		}
		result.push_back(digits[digest[size_t(i)] >> 4]);
		result.push_back(digits[digest[size_t(i)] & 15]);
	}
	return result;
}

} // namespace fgpl
