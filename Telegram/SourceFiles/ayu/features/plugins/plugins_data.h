// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

class HistoryItem;

namespace fgpl {
class DataProvider;
} // namespace fgpl

namespace FreshPlugins {

[[nodiscard]] fgpl::DataProvider *DataFor(const std::string &account);
void OpenData();
void CloseData();
void ReleaseData();

[[nodiscard]] bool WantsEvents();
[[nodiscard]] bool PluginSending();
void SendMessageEvent(
	const char *type,
	not_null<const HistoryItem*> item,
	const std::string &account,
	MsgId localId = 0,
	bool byPlugin = false);

} // namespace FreshPlugins
