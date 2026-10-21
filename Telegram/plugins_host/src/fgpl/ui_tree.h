#pragma once

#include "fgpl/cbor.h"
#include "fgpl/result.h"

#include <string>

namespace fgpl::ui {

constexpr auto kMaxItems = size_t(64);
constexpr auto kMaxLabel = size_t(120);
constexpr auto kMaxText = size_t(1000);
constexpr auto kMaxValue = size_t(1000);
constexpr auto kMaxOptions = size_t(16);

[[nodiscard]] bool SafeText(const std::string &text, size_t maxBytes, bool allowNewlines);

[[nodiscard]] Result<cbor::Value, std::string> ValidatePage(
	const cbor::Value &page);

} // namespace fgpl::ui
