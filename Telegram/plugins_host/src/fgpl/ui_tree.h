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
constexpr auto kMaxDepth = size_t(4);
constexpr auto kMaxNodes = size_t(256);
constexpr auto kMaxRowItems = size_t(4);
constexpr auto kMaxTabs = size_t(8);
constexpr auto kMaxTableColumns = size_t(6);
constexpr auto kMaxTableRows = size_t(32);
constexpr auto kMaxCell = size_t(120);
constexpr auto kMaxImageBytes = size_t(256 * 1024);
constexpr auto kMaxImageDimension = uint32_t(2048);
constexpr auto kMaxImageSide = int64_t(512);
constexpr auto kMaxRichIcons = size_t(32);
constexpr auto kMaxBadges = size_t(4);
constexpr auto kMaxBadgeTooltip = size_t(80);
constexpr auto kMaxBadgeImageBytes = size_t(64 * 1024);
constexpr auto kMaxBadgeImageDimension = uint32_t(128);

[[nodiscard]] bool SafeText(const std::string &text, size_t maxBytes, bool allowNewlines);
[[nodiscard]] bool ValidIconName(const std::string &name);
// peer:<chat id> or emoji:<custom emoji id>, drawn by the app from its own data.
[[nodiscard]] bool ValidRichIcon(const std::string &name);
[[nodiscard]] bool ValidWindowId(const std::string &id);
[[nodiscard]] const std::set<std::string> &BuiltinIcons();

[[nodiscard]] Result<cbor::Value, std::string> ValidatePage(
	const cbor::Value &page);

[[nodiscard]] Result<cbor::Value, std::string> ValidateBadges(
	const cbor::Value &items);

} // namespace fgpl::ui
