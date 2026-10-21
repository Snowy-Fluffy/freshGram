// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

class HistoryItem;

namespace fgpl {
class DataProvider;
} // namespace fgpl

namespace AyuPlugins {

[[nodiscard]] fgpl::DataProvider *DataFor(const std::string &account);
void CloseData();
void ReleaseData();

[[nodiscard]] bool WantsEvents();
void SendMessageEvent(
	const char *type,
	not_null<const HistoryItem*> item,
	const std::string &account);

} // namespace AyuPlugins
