#pragma once

#include "fgpl/cbor.h"
#include "fgpl/result.h"

#include <set>
#include <string>

namespace fgpl::ui {

constexpr auto kMaxItems = size_t(64);
constexpr auto kMaxLabel = size_t(120);
constexpr auto kMaxText = size_t(1000);
constexpr auto kMaxValue = size_t(1000);
constexpr auto kMaxOptions = size_t(16);
constexpr auto kMaxLongValue = size_t(4096);
constexpr auto kMaxHint = size_t(300);
constexpr auto kMaxUrl = size_t(512);
constexpr auto kMaxButtons = size_t(3);
constexpr auto kMaxButtonLabel = size_t(40);
constexpr auto kMaxSliderValue = int64_t(1000000000);
constexpr auto kMaxSliderSteps = int64_t(1000);

[[nodiscard]] bool SafeText(const std::string &text, size_t maxBytes, bool allowNewlines);
[[nodiscard]] bool ValidIconName(const std::string &name);
[[nodiscard]] const std::set<std::string> &BuiltinIcons();

[[nodiscard]] Result<cbor::Value, std::string> ValidatePage(
	const cbor::Value &page);

} // namespace fgpl::ui
