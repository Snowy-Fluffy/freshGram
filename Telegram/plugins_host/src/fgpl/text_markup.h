#pragma once

#include "fgpl/result.h"
#include "fgpl/text_entity.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fgpl {

enum class TextFormat : uint8_t {
	Plain,
	Markdown,
	Html,
};

// A text without markup and the entities that the markup meant.
struct MarkedText {
	std::string text;
	std::vector<TextEntity> entities;
};

[[nodiscard]] std::optional<TextFormat> TextFormatNamed(std::string_view name);

// Plain text comes back unchanged; an error says what is wrong with the markup.
[[nodiscard]] Result<MarkedText, std::string> ParseMarkup(
	TextFormat format,
	std::string_view source);

// The number of UTF-16 units of a UTF-8 text, as entity offsets count.
[[nodiscard]] int64_t Utf16Length(std::string_view text);

} // namespace fgpl
