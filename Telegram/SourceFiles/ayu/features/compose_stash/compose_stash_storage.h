/*
This file is part of freshGram. The compose stash is ported from Forkgram
(https://github.com/forkgram/tdesktop), based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_drafts.h"

class History;

namespace Data {
struct ComposeStash;
} // namespace Data

namespace FreshComposeStash {

void Persist(
	not_null<History*> history,
	Data::DraftKey key,
	const Data::ComposeStash *stash);

[[nodiscard]] std::unique_ptr<Data::ComposeStash> Restore(
	not_null<History*> history,
	Data::DraftKey key);

} // namespace FreshComposeStash
