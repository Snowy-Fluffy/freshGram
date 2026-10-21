#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace fgpl {

using PublicKey = std::array<uint8_t, 32>;
using PrivateSeed = std::array<uint8_t, 32>;
using SignatureBytes = std::array<uint8_t, 64>;

[[nodiscard]] bool Ed25519Verify(
	const PublicKey &key,
	std::span<const uint8_t> message,
	const SignatureBytes &signature);

[[nodiscard]] std::optional<PublicKey> Ed25519PublicKey(
	const PrivateSeed &seed);

[[nodiscard]] std::optional<SignatureBytes> Ed25519Sign(
	const PrivateSeed &seed,
	std::span<const uint8_t> message);

[[nodiscard]] std::string Fingerprint(const PublicKey &key);

} // namespace fgpl
