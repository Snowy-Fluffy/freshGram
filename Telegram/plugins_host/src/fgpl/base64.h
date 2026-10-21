#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fgpl {

[[nodiscard]] std::string Base64Encode(std::span<const uint8_t> data);

[[nodiscard]] std::optional<std::vector<uint8_t>> Base64Decode(
	std::string_view text);

} // namespace fgpl
