#include "fgpl/base64.h"

namespace fgpl {
namespace {

constexpr auto kAlphabet =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] int Value(char ch) {
	if (ch >= 'A' && ch <= 'Z') {
		return ch - 'A';
	} else if (ch >= 'a' && ch <= 'z') {
		return ch - 'a' + 26;
	} else if (ch >= '0' && ch <= '9') {
		return ch - '0' + 52;
	} else if (ch == '+') {
		return 62;
	} else if (ch == '/') {
		return 63;
	}
	return -1;
}

} // namespace

std::string Base64Encode(std::span<const uint8_t> data) {
	auto result = std::string();
	result.reserve((data.size() + 2) / 3 * 4);
	for (auto i = size_t(0); i < data.size(); i += 3) {
		const auto remaining = data.size() - i;
		const auto a = uint32_t(data[i]);
		const auto b = remaining > 1 ? uint32_t(data[i + 1]) : 0;
		const auto c = remaining > 2 ? uint32_t(data[i + 2]) : 0;
		const auto word = (a << 16) | (b << 8) | c;
		result.push_back(kAlphabet[(word >> 18) & 63]);
		result.push_back(kAlphabet[(word >> 12) & 63]);
		result.push_back(remaining > 1 ? kAlphabet[(word >> 6) & 63] : '=');
		result.push_back(remaining > 2 ? kAlphabet[word & 63] : '=');
	}
	return result;
}

std::optional<std::vector<uint8_t>> Base64Decode(std::string_view text) {
	if (text.size() % 4 != 0) {
		return std::nullopt;
	}
	auto result = std::vector<uint8_t>();
	result.reserve(text.size() / 4 * 3);
	for (auto i = size_t(0); i < text.size(); i += 4) {
		const auto last = (i + 4 == text.size());
		auto padding = 0;
		auto word = uint32_t(0);
		for (auto k = 0; k != 4; ++k) {
			const auto ch = text[i + size_t(k)];
			if (ch == '=') {
				if (!last || k < 2) {
					return std::nullopt;
				}
				++padding;
				word <<= 6;
				continue;
			}
			if (padding != 0) {
				return std::nullopt;
			}
			const auto value = Value(ch);
			if (value < 0) {
				return std::nullopt;
			}
			word = (word << 6) | uint32_t(value);
		}
		if (padding == 2 && (word & 0xFFFF) != 0) {
			return std::nullopt;
		} else if (padding == 1 && (word & 0xFF) != 0) {
			return std::nullopt;
		}
		result.push_back(uint8_t(word >> 16));
		if (padding < 2) {
			result.push_back(uint8_t(word >> 8));
		}
		if (padding < 1) {
			result.push_back(uint8_t(word));
		}
	}
	return result;
}

} // namespace fgpl
