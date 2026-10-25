/*
This file is part of freshGram, based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Window {
class SessionController;
} // namespace Window

namespace FreshComments {

[[nodiscard]] bool NeedsDiscussionLookup(not_null<HistoryItem*> item);

void ShowForDeletedPost(
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

} // namespace FreshComments
