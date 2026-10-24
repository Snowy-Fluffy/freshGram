// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ayu_state.h"

#include "ayu/ayu_settings.h"

namespace AyuState {

std::unordered_map<PeerId, std::unordered_set<MsgId>> hiddenMessages;
Main::Session *disableGhostModeOnStoryCloseSession = nullptr;

void hide(PeerId peerId, MsgId messageId) {
	hiddenMessages[peerId].insert(messageId);
}

namespace {

[[nodiscard]] MsgId ServerId(not_null<HistoryItem*> item) {
	const auto original = item->isFreshRestored()
		? item->history()->freshOriginalId(item)
		: MsgId();
	return original ? original : item->id;
}

} // namespace

void hide(not_null<HistoryItem*> item) {
	hide(item->history()->peer->id, ServerId(item));
}

bool isHidden(PeerId peerId, MsgId messageId) {
	const auto it = hiddenMessages.find(peerId);
	if (it != hiddenMessages.end()) {
		return it->second.contains(messageId);
	}
	return false;
}

bool isHidden(not_null<HistoryItem*> item) {
	return isHidden(item->history()->peer->id, ServerId(item));
}

void setDisableGhostModeOnStoryClose(Main::Session *session) {
	disableGhostModeOnStoryCloseSession = session;
}

void disableGhostModeOnStoryClose(Main::Session *session) {
	if (disableGhostModeOnStoryCloseSession != session) {
		return;
	}
	disableGhostModeOnStoryCloseSession = nullptr;
	if (session) {
		AyuSettings::ghost(session).setGhostModeEnabled(false);
	}
}

}
