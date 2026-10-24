// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/data/local_pins.h"

#include "ayu/data/ayu_database.h"
#include "data/data_folder.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "main/main_session.h"

#include <algorithm>
#include <map>
#include <vector>

namespace FreshLocalPins {
namespace {

std::map<ID, std::vector<PeerId>> State;
bool Applying = false;

[[nodiscard]] ID UserId(not_null<Data::Session*> owner) {
	return ID(owner->session().userId().bare & PeerId::kChatTypeMask);
}

[[nodiscard]] std::vector<PeerId> &Pins(not_null<Data::Session*> owner) {
	const auto userId = UserId(owner);
	auto i = State.find(userId);
	if (i == end(State)) {
		auto list = std::vector<PeerId>();
		for (const auto id : AyuDatabase::getLocalPins(userId)) {
			if (const auto peerId = DeserializePeerId(quint64(id))) {
				list.push_back(peerId);
			}
		}
		i = State.emplace(userId, std::move(list)).first;
	}
	return i->second;
}

void Save(not_null<Data::Session*> owner) {
	auto list = std::vector<ID>();
	for (const auto peerId : Pins(owner)) {
		list.push_back(ID(SerializePeerId(peerId)));
	}
	AyuDatabase::saveLocalPins(UserId(owner), list);
}

} // namespace

bool isLocal(Dialogs::Key key) {
	const auto history = key.history();
	if (!history) {
		return false;
	}
	const auto &list = Pins(&history->owner());
	return std::find(list.begin(), list.end(), history->peer->id) != list.end();
}

int count(
		not_null<const Data::Session*> owner,
		Data::Folder *folder) {
	auto result = 0;
	for (const auto &key : owner->pinnedChatsOrder(folder)) {
		if (isLocal(key)) {
			++result;
		}
	}
	return result;
}

void pin(not_null<History*> history) {
	const auto owner = &history->owner();
	auto &list = Pins(owner);
	const auto id = history->peer->id;
	if (std::find(list.begin(), list.end(), id) == list.end()) {
		list.push_back(id);
	}
	Applying = true;
	owner->setPinnedFromEntryList(history, true);
	Applying = false;
	owner->notifyPinnedDialogsOrderUpdated();
	Save(owner);
}

bool unpin(not_null<History*> history) {
	const auto owner = &history->owner();
	auto &list = Pins(owner);
	const auto i = std::find(list.begin(), list.end(), history->peer->id);
	if (i == list.end()) {
		return false;
	}
	list.erase(i);
	owner->setChatPinned(history, FilterId(), false);
	Save(owner);
	return true;
}

void forget(not_null<Data::Session*> owner, PeerId peerId) {
	if (Applying) {
		return;
	}
	auto &list = Pins(owner);
	const auto i = std::find(list.begin(), list.end(), peerId);
	if (i != list.end()) {
		list.erase(i);
		Save(owner);
	}
}

void reapply(not_null<Data::Session*> owner) {
	auto changed = false;
	Applying = true;
	for (const auto peerId : Pins(owner)) {
		const auto history = owner->historyLoaded(peerId);
		if (history
			&& history->folderKnown()
			&& !history->isPinnedDialog(FilterId())) {
			owner->setPinnedFromEntryList(history, true);
			changed = true;
		}
	}
	Applying = false;
	if (changed) {
		owner->notifyPinnedDialogsOrderUpdated();
	}
}

void saveOrder(not_null<Data::Session*> owner) {
	auto &list = Pins(owner);
	auto ordered = std::vector<PeerId>();
	const auto collect = [&](Data::Folder *folder) {
		for (const auto &key : owner->pinnedChatsOrder(folder)) {
			if (const auto history = key.history()) {
				const auto id = history->peer->id;
				if (std::find(list.begin(), list.end(), id) != list.end()) {
					ordered.push_back(id);
				}
			}
		}
	};
	collect(nullptr);
	if (const auto archive = owner->folderLoaded(Data::Folder::kId)) {
		collect(archive);
	}
	for (const auto id : list) {
		if (std::find(ordered.begin(), ordered.end(), id) == ordered.end()) {
			ordered.push_back(id);
		}
	}
	list = std::move(ordered);
	Save(owner);
}

} // namespace FreshLocalPins
