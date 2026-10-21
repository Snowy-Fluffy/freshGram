#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fgpl {

using Digest = std::array<uint8_t, 32>;

[[nodiscard]] Digest Sha256(std::span<const uint8_t> data);

[[nodiscard]] Digest HmacSha256(
	std::span<const uint8_t> key,
	std::span<const uint8_t> data);

[[nodiscard]] bool ConstantTimeEqual(
	std::span<const uint8_t> a,
	std::span<const uint8_t> b);

} // namespace fgpl
